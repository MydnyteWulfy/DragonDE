#include "core/FileWatcher.h"

#include "core/Log.h"

#include <cerrno>
#include <cstring>
#include <sys/inotify.h>
#include <wayland-server-core.h>

namespace dragon {

FileWatcher::FileWatcher(EventLoop& loop, std::filesystem::path file) : m_file(std::move(file))
{
    m_fd = UniqueFd(inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
    if (!m_fd) {
        log::warn("core", "inotify: {}", std::strerror(errno));
        return;
    }
    const auto dir = m_file.parent_path();
    m_watch = inotify_add_watch(m_fd.get(), dir.c_str(),
                                IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_MOVED_FROM);
    if (m_watch < 0) {
        log::debug("core", "not watching {}: {}", dir.string(), std::strerror(errno));
        return;
    }
    m_source = loop.addFd(m_fd.get(), WL_EVENT_READABLE, [this](int, std::uint32_t) { onReadable(); });
    // Editors write in several steps; act once things have been quiet briefly.
    m_settle = loop.addTimer([this] { changed.emit(); });
}

void FileWatcher::onReadable()
{
    alignas(inotify_event) char buffer[4096];
    bool relevant = false;
    for (;;) {
        const ssize_t n = ::read(m_fd.get(), buffer, sizeof buffer);
        if (n <= 0)
            break;
        for (ssize_t off = 0; off < n;) {
            const auto* ev = reinterpret_cast<const inotify_event*>(buffer + off);
            if (ev->len > 0 && m_file.filename() == ev->name)
                relevant = true;
            off += ssize_t(sizeof(inotify_event) + ev->len);
        }
    }
    if (relevant)
        m_settle.arm(std::chrono::milliseconds(100));
}

} // namespace dragon
