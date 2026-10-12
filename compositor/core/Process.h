#pragma once

#include <string>

namespace dragon::process {

// Runs `command` through /bin/sh in a new session, detached from us. The child
// gets a clean signal mask and default handlers: the compositor blocks
// SIGINT/SIGTERM/SIGCHLD for its signalfds, and children must not inherit that.
bool spawn(const std::string& command);

// Collects exited children (call on SIGCHLD) so they don't linger as zombies.
void reapChildren();

} // namespace dragon::process
