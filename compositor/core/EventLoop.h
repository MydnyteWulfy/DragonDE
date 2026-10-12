#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "core/Signal.h"

struct wl_event_loop;
struct wl_event_source;

namespace dragon {

namespace detail {
struct EventCallback;
}

// RAII owner of a wl_event_source. Removing the source (destroying this)
// also frees its callback.
class EventSource {
public:
    EventSource() noexcept;
    ~EventSource();
    EventSource(EventSource&& other) noexcept;
    EventSource& operator=(EventSource&& other) noexcept;
    EventSource(const EventSource&) = delete;
    EventSource& operator=(const EventSource&) = delete;

    explicit operator bool() const { return m_source != nullptr; }

    // Timers only: fire once after `delay` (0 disarms).
    void arm(std::chrono::milliseconds delay);
    // Fd sources only: change the watched mask (WL_EVENT_READABLE, ...).
    void setMask(std::uint32_t mask);

private:
    friend class EventLoop;

    wl_event_source* m_source = nullptr;
    std::unique_ptr<detail::EventCallback> m_callback;
};

// Thin C++ layer over wl_event_loop. The compositor has exactly one, owned by
// the wl_display; every fd (libinput, DRM, parent Wayland, D-Bus) goes here.
class EventLoop {
public:
    explicit EventLoop(wl_event_loop* loop);
    // Cancels idle callbacks that never ran. Destroy before the wl_event_loop.
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    wl_event_loop* handle() const { return m_loop; }

    using FdCallback = std::function<void(int fd, std::uint32_t mask)>;

    [[nodiscard]] EventSource addFd(int fd, std::uint32_t mask, FdCallback cb);
    [[nodiscard]] EventSource addTimer(std::function<void()> cb);
    // Blocks the signal for the process and delivers it here via signalfd.
    [[nodiscard]] EventSource addSignal(int signo, std::function<void()> cb);
    // One-shot: runs before the loop next sleeps.
    void addIdle(std::function<void()> cb);

    // Emits beforeSleep, then dispatches pending events, waiting up to
    // `timeoutMs` (-1 = forever).
    int dispatch(int timeoutMs = -1);

    // Last chance to flush buffered output (to clients, to a parent
    // compositor) before the loop may block.
    Signal<> beforeSleep;

private:
    struct Idle;
    friend void idleTrampoline(void* data);

    wl_event_loop* m_loop;
    std::vector<std::unique_ptr<Idle>> m_idles;
};

} // namespace dragon
