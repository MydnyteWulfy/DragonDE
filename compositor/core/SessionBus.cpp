#include "core/SessionBus.h"

#include "core/Log.h"
#include "core/UniqueFd.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace dragon {

std::unique_ptr<SessionBus> SessionBus::start()
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0)
        return nullptr;
    UniqueFd readEnd(fds[0]), writeEnd(fds[1]);

    // dbus-daemon prints its address to fd 3 (a dup of our pipe, so it survives exec).
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, writeEnd.get(), 3);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t empty;
    sigemptyset(&empty);
    posix_spawnattr_setsigmask(&attr, &empty);
    sigset_t defaults;
    sigemptyset(&defaults);
    for (int sig : {SIGINT, SIGTERM, SIGCHLD, SIGPIPE, SIGHUP})
        sigaddset(&defaults, sig);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    const char* argv[] = {"dbus-daemon", "--session", "--nofork", "--nopidfile", "--print-address=3", nullptr};
    pid_t pid = 0;
    const int err = posix_spawnp(&pid, "dbus-daemon", &actions, &attr, const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    writeEnd.reset();
    if (err != 0) {
        log::warn("core", "cannot start dbus-daemon: {}", std::strerror(err));
        return nullptr;
    }

    // Wait (bounded) for the address line.
    std::string address;
    char buf[256];
    pollfd pfd{readEnd.get(), POLLIN, 0};
    while (address.find('\n') == std::string::npos && poll(&pfd, 1, 5000) > 0) {
        const ssize_t n = read(readEnd.get(), buf, sizeof(buf));
        if (n <= 0)
            break;
        address.append(buf, std::size_t(n));
    }
    if (const auto nl = address.find('\n'); nl != std::string::npos)
        address.resize(nl);
    if (address.empty()) {
        log::warn("core", "dbus-daemon didn't report an address");
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        return nullptr;
    }
    log::info("core", "private session bus at {}", address);
    return std::unique_ptr<SessionBus>(new SessionBus(pid, std::move(address)));
}

SessionBus::~SessionBus()
{
    kill(m_pid, SIGTERM);
    waitpid(m_pid, nullptr, 0);
}

} // namespace dragon
