#pragma once

#include <memory>
#include <string>
#include <sys/types.h>

namespace dragon {

// A private D-Bus session bus (dbus-daemon) for the apps we run. D-Bus
// activated services, xdg-desktop-portal in particular, are started with the
// environment the bus was started in: ours. So the file chooser of an app
// inside DragonDE opens inside DragonDE, even when we're nested in a desktop
// that has its own portals on its own bus.
class SessionBus {
public:
    // Starts dbus-daemon; nullptr if it isn't available.
    static std::unique_ptr<SessionBus> start();
    ~SessionBus(); // stops the bus; activated services exit with it

    const std::string& address() const { return m_address; }

private:
    SessionBus(pid_t pid, std::string address) : m_pid(pid), m_address(std::move(address)) {}

    pid_t m_pid;
    std::string m_address;
};

} // namespace dragon
