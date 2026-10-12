#pragma once

#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace dragon {

// Owning file descriptor: closes on destruction, move-only.
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : m_fd(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(UniqueFd&& other) noexcept : m_fd(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other)
            reset(other.release());
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    int get() const { return m_fd; }
    bool valid() const { return m_fd >= 0; }
    explicit operator bool() const { return valid(); }

    // Gives up ownership without closing.
    int release() { return std::exchange(m_fd, -1); }

    void reset(int fd = -1)
    {
        if (m_fd >= 0)
            ::close(m_fd);
        m_fd = fd;
    }

    // New descriptor for the same file (close-on-exec). Invalid on failure.
    UniqueFd duplicate() const
    {
        return UniqueFd(m_fd >= 0 ? ::fcntl(m_fd, F_DUPFD_CLOEXEC, 0) : -1);
    }

private:
    int m_fd = -1;
};

} // namespace dragon
