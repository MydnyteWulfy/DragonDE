#pragma once

#include <string>
#include <vector>

namespace dragon::process {

// Runs `command` through /bin/sh in a new session, detached from us. The child
// gets a clean signal mask and default handlers: the compositor blocks
// SIGINT/SIGTERM/SIGCHLD for its signalfds, and children must not inherit that.
// `extraEnv` entries ("NAME=value") are added to (or override) our environment.
bool spawn(const std::string& command, const std::vector<std::string>& extraEnv = {});

// Collects exited children (call on SIGCHLD) so they don't linger as zombies.
void reapChildren();

} // namespace dragon::process
