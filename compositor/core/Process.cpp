#include "core/Process.h"

#include "core/Log.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

namespace dragon::process {

bool spawn(const std::string& command)
{
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
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSID);

    const char* argv[] = {"/bin/sh", "-c", command.c_str(), nullptr};
    pid_t pid = 0;
    const int err = posix_spawn(&pid, "/bin/sh", nullptr, &attr, const_cast<char* const*>(argv), environ);
    posix_spawnattr_destroy(&attr);

    if (err != 0) {
        log::error("process", "cannot run '{}': {}", command, std::strerror(err));
        return false;
    }
    log::info("process", "started '{}' (pid {})", command, pid);
    return true;
}

void reapChildren()
{
    int status = 0;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        log::debug("process", "pid {} exited ({})", pid, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

} // namespace dragon::process
