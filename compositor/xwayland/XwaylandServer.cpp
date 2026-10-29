#include "xwayland/XwaylandServer.h"

#include "Server.h"
#include "core/Log.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace dragon {

namespace {

UniqueFd listenOn(const std::string& path, bool abstract)
{
    UniqueFd fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd)
        return {};
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    // Abstract sockets start with a NUL byte and have no file.
    const std::size_t offset = abstract ? 1 : 0;
    if (path.size() + offset >= sizeof(addr.sun_path))
        return {};
    std::memcpy(addr.sun_path + offset, path.data(), path.size());
    const auto len = socklen_t(offsetof(sockaddr_un, sun_path) + offset + path.size() + (abstract ? 0 : 1));
    if (!abstract)
        unlink(path.c_str()); // a stale socket from a crashed server (we hold the lock)
    if (bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), len) != 0 || ::listen(fd.get(), 4096) != 0)
        return {};
    return fd;
}

} // namespace

XwaylandServer::XwaylandServer(Server& server, EventLoop& loop) : m_server(server), m_loop(loop)
{
    m_clientListener.owner = this;
    m_clientListener.listener.notify = clientDestroyed;
}

XwaylandServer::~XwaylandServer()
{
    m_displayFdSource = {};
    if (m_client) {
        wl_list_remove(&m_clientListener.listener.link);
        m_client = nullptr;
    }
    if (m_pid > 0)
        kill(m_pid, SIGTERM); // reaped by the SIGCHLD handler, or by init once we're gone
    releaseDisplay();
}

bool XwaylandServer::claimDisplay(int display)
{
    // The lock file says who owns :N; create it exclusively.
    const std::string lock = std::format("/tmp/.X{}-lock", display);
    UniqueFd fd(open(lock.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444));
    if (!fd) {
        if (errno != EEXIST)
            return false;
        // Stale if its owner is gone.
        char buf[12] = {};
        UniqueFd existing(open(lock.c_str(), O_RDONLY | O_CLOEXEC));
        if (!existing || read(existing.get(), buf, 11) != 11)
            return false;
        const pid_t owner = pid_t(std::strtol(buf, nullptr, 10));
        if (owner <= 0 || kill(owner, 0) == 0 || errno != ESRCH || unlink(lock.c_str()) != 0)
            return false;
        fd = UniqueFd(open(lock.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444));
        if (!fd)
            return false;
    }
    const std::string pid = std::format("{:>10}\n", getpid());
    if (write(fd.get(), pid.data(), pid.size()) != ssize_t(pid.size())) {
        unlink(lock.c_str());
        return false;
    }

    mkdir("/tmp/.X11-unix", 01777);
    const std::string socketPath = std::format("/tmp/.X11-unix/X{}", display);
    struct stat st {};
    if (stat(socketPath.c_str(), &st) == 0) {
        // A socket without a lock (someone else's, or a leftover): don't fight over it.
        unlink(lock.c_str());
        return false;
    }
    m_abstractSocket = listenOn(socketPath, true);
    m_socket = listenOn(socketPath, false);
    if (!m_abstractSocket || !m_socket) {
        m_abstractSocket.reset();
        m_socket.reset();
        unlink(socketPath.c_str());
        unlink(lock.c_str());
        return false;
    }
    m_display = display;
    m_displayName = std::format(":{}", display);
    m_lockPath = lock;
    m_socketPath = socketPath;
    return true;
}

void XwaylandServer::releaseDisplay()
{
    m_lazyAbstract = {};
    m_lazySocket = {};
    m_abstractSocket.reset();
    m_socket.reset();
    if (!m_socketPath.empty())
        unlink(m_socketPath.c_str());
    if (!m_lockPath.empty())
        unlink(m_lockPath.c_str());
    m_socketPath.clear();
    m_lockPath.clear();
    m_display = -1;
}

bool XwaylandServer::listen()
{
    for (int display = 0; display < 64; ++display) {
        if (claimDisplay(display)) {
            log::info("xwayland", "X11 display {} ready (Xwayland starts with the first X client)", m_displayName);
            armLazyStart();
            return true;
        }
    }
    log::error("xwayland", "no free X11 display");
    return false;
}

void XwaylandServer::armLazyStart()
{
    // A connection waiting on either socket means an X client wants a server.
    auto onConnection = [this](int, std::uint32_t) {
        m_loop.addIdle([this] { start(); }); // not from inside the source we're about to remove
    };
    m_lazyAbstract = m_loop.addFd(m_abstractSocket.get(), WL_EVENT_READABLE, onConnection);
    m_lazySocket = m_loop.addFd(m_socket.get(), WL_EVENT_READABLE, onConnection);
}

void XwaylandServer::start()
{
    if (m_client || m_display < 0)
        return;
    m_lazyAbstract = {};
    m_lazySocket = {}; // Xwayland listens on these from now on

    int wl[2], wm[2], readyPipe[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wl) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm) != 0 || pipe2(readyPipe, O_CLOEXEC) != 0) {
        log::error("xwayland", "cannot create sockets: {}", std::strerror(errno));
        armLazyStart();
        return;
    }
    UniqueFd wlOurs(wl[0]), wlTheirs(wl[1]), wmOurs(wm[0]), wmTheirs(wm[1]), readyRead(readyPipe[0]),
        readyWrite(readyPipe[1]);

    // Xwayland's fds at fixed numbers 3..7 (dup2 clears close-on-exec). Staged
    // through high numbers first: a source could itself be one of 3..7.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    const int sources[] = {m_abstractSocket.get(), m_socket.get(), readyWrite.get(), wmTheirs.get(), wlTheirs.get()};
    constexpr int kStage = 200;
    for (int i = 0; i < 5; ++i)
        posix_spawn_file_actions_adddup2(&actions, sources[i], kStage + i);
    for (int i = 0; i < 5; ++i) {
        posix_spawn_file_actions_adddup2(&actions, kStage + i, 3 + i);
        posix_spawn_file_actions_addclose(&actions, kStage + i);
    }

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t empty;
    sigemptyset(&empty);
    posix_spawnattr_setsigmask(&attr, &empty);
    sigset_t defaults;
    sigemptyset(&defaults);
    for (int sig : {SIGINT, SIGTERM, SIGCHLD, SIGPIPE, SIGHUP, SIGUSR1})
        sigaddset(&defaults, sig);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    // Our environment with Xwayland's Wayland connection handed over as a socket.
    std::vector<std::string> env{"WAYLAND_SOCKET=7"};
    for (char** e = environ; *e; ++e) {
        if (!std::string_view(*e).starts_with("WAYLAND_SOCKET="))
            env.emplace_back(*e);
    }
    std::vector<char*> envp;
    for (auto& e : env)
        envp.push_back(e.data());
    envp.push_back(nullptr);

    const char* argv[] = {"Xwayland", m_displayName.c_str(), "-rootless", "-listenfd", "3", "-listenfd", "4",
                          "-displayfd", "5", "-wm", "6", nullptr};
    const int err = posix_spawnp(&m_pid, "Xwayland", &actions, &attr, const_cast<char* const*>(argv), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (err != 0) {
        log::error("xwayland", "cannot run Xwayland: {}", std::strerror(err));
        m_pid = 0;
        armLazyStart();
        return;
    }

    m_client = wl_client_create(m_server.display(), wlOurs.release());
    if (!m_client) {
        log::error("xwayland", "cannot create the Wayland client for Xwayland");
        kill(m_pid, SIGTERM);
        m_pid = 0;
        armLazyStart();
        return;
    }
    wl_client_add_destroy_listener(m_client, &m_clientListener.listener);
    started.emit(m_client);
    m_wmFd = std::move(wmOurs);
    m_displayFd = std::move(readyRead);
    m_displayFdSource = m_loop.addFd(m_displayFd.get(), WL_EVENT_READABLE, [this](int, std::uint32_t) {
        m_loop.addIdle([this] { onDisplayReady(); });
        m_displayFdSource.setMask(0);
    });
    log::info("xwayland", "started Xwayland {} (pid {})", m_displayName, m_pid);
}

void XwaylandServer::onDisplayReady()
{
    // Xwayland writes the display number once it accepts clients.
    char buf[16] = {};
    if (!m_displayFd || read(m_displayFd.get(), buf, sizeof(buf) - 1) <= 0) {
        log::error("xwayland", "Xwayland exited before it was ready");
        return; // the client-destroyed listener cleans up
    }
    m_displayFdSource = {};
    m_displayFd.reset();
    log::info("xwayland", "Xwayland {} is ready", m_displayName);
    ready.emit(m_wmFd.release());
}

void XwaylandServer::clientDestroyed(wl_listener* listener, void*)
{
    auto* self = reinterpret_cast<ClientListener*>(listener)->owner;
    wl_list_remove(&listener->link);
    self->m_client = nullptr;
    self->onStopped();
}

void XwaylandServer::onStopped()
{
    log::warn("xwayland", "Xwayland {} stopped; it restarts with the next X client", m_displayName);
    m_pid = 0;
    m_wmFd.reset();
    m_displayFdSource = {};
    m_displayFd.reset();
    stopped.emit();
    if (m_display >= 0)
        armLazyStart();
}

} // namespace dragon
