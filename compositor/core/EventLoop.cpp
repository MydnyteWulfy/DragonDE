#include "core/EventLoop.h"

#include "core/Log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>
#include <variant>
#include <wayland-server-core.h>

namespace dragon {

struct detail::EventCallback {
    std::variant<EventLoop::FdCallback, std::function<void()>> fn;
};

namespace {

int fdTrampoline(int fd, uint32_t mask, void* data)
{
    auto* cb = static_cast<detail::EventCallback*>(data);
    std::get<EventLoop::FdCallback>(cb->fn)(fd, mask);
    return 0;
}

int timerTrampoline(void* data)
{
    auto* cb = static_cast<detail::EventCallback*>(data);
    std::get<std::function<void()>>(cb->fn)();
    return 0;
}

int signalTrampoline(int, void* data)
{
    return timerTrampoline(data);
}

} // namespace

struct EventLoop::Idle {
    EventLoop* loop;
    wl_event_source* source = nullptr;
    std::function<void()> fn;
};

void idleTrampoline(void* data)
{
    // libwayland removes the source after this returns; forget it first, then run.
    auto* idle = static_cast<EventLoop::Idle*>(data);
    auto fn = std::move(idle->fn);
    std::erase_if(idle->loop->m_idles, [idle](const auto& p) { return p.get() == idle; });
    fn();
}

EventLoop::EventLoop(wl_event_loop* loop) : m_loop(loop) {}

EventLoop::~EventLoop()
{
    for (auto& idle : m_idles)
        wl_event_source_remove(idle->source);
}

EventSource::EventSource() noexcept = default;

EventSource::~EventSource()
{
    if (m_source)
        wl_event_source_remove(m_source);
}

EventSource::EventSource(EventSource&& other) noexcept
    : m_source(std::exchange(other.m_source, nullptr)), m_callback(std::move(other.m_callback)) {}

EventSource& EventSource::operator=(EventSource&& other) noexcept
{
    if (this != &other) {
        if (m_source)
            wl_event_source_remove(m_source);
        m_source = std::exchange(other.m_source, nullptr);
        m_callback = std::move(other.m_callback);
    }
    return *this;
}

void EventSource::arm(std::chrono::milliseconds delay)
{
    if (m_source)
        wl_event_source_timer_update(m_source, int(delay.count()));
}

void EventSource::setMask(std::uint32_t mask)
{
    if (m_source)
        wl_event_source_fd_update(m_source, mask);
}

EventSource EventLoop::addFd(int fd, std::uint32_t mask, FdCallback cb)
{
    EventSource src;
    src.m_callback = std::make_unique<detail::EventCallback>(std::move(cb));
    src.m_source = wl_event_loop_add_fd(m_loop, fd, mask, fdTrampoline, src.m_callback.get());
    if (!src.m_source)
        log::error("core", "wl_event_loop_add_fd({}) failed: {}", fd, std::strerror(errno));
    return src;
}

EventSource EventLoop::addTimer(std::function<void()> cb)
{
    EventSource src;
    src.m_callback = std::make_unique<detail::EventCallback>(std::move(cb));
    src.m_source = wl_event_loop_add_timer(m_loop, timerTrampoline, src.m_callback.get());
    if (!src.m_source)
        log::error("core", "wl_event_loop_add_timer failed: {}", std::strerror(errno));
    return src;
}

EventSource EventLoop::addSignal(int signo, std::function<void()> cb)
{
    EventSource src;
    src.m_callback = std::make_unique<detail::EventCallback>(std::move(cb));
    src.m_source = wl_event_loop_add_signal(m_loop, signo, signalTrampoline, src.m_callback.get());
    if (!src.m_source)
        log::error("core", "wl_event_loop_add_signal({}) failed: {}", signo, std::strerror(errno));
    return src;
}

void EventLoop::addIdle(std::function<void()> cb)
{
    auto idle = std::make_unique<Idle>(Idle{this, nullptr, std::move(cb)});
    idle->source = wl_event_loop_add_idle(m_loop, idleTrampoline, idle.get());
    if (!idle->source) {
        log::error("core", "wl_event_loop_add_idle failed");
        return;
    }
    m_idles.push_back(std::move(idle));
}

int EventLoop::dispatch(int timeoutMs)
{
    // Idle callbacks run first: they may produce output that needs flushing.
    wl_event_loop_dispatch_idle(m_loop);
    beforeSleep.emit();
    return wl_event_loop_dispatch(m_loop, timeoutMs);
}

} // namespace dragon
