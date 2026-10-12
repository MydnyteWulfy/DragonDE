#pragma once

#include "core/Geometry.h"

#include <cstdint>
#include <utility>
#include <wayland-server-core.h>

namespace dragon {

// Our view of a client's wl_buffer, whatever created it (wl_shm via
// libwayland, linux-dmabuf via us). Created on first use and kept until the
// wl_buffer is destroyed *and* nothing holds a lock on it.
//
// Locks track "the compositor still needs this buffer's contents"; when the
// last lock goes away the client gets wl_buffer.release and may reuse it.
class ClientBuffer {
public:
    enum class Kind { Shm, DmaBuf };

    // nullptr for buffer types we don't support.
    static ClientBuffer* get(wl_resource* buffer);

    ClientBuffer(const ClientBuffer&) = delete;
    ClientBuffer& operator=(const ClientBuffer&) = delete;

    Kind kind() const { return m_kind; }
    Size size() const { return m_size; }
    std::uint32_t format() const { return m_format; } // DRM fourcc, for every kind

    // Null once the client destroyed the wl_buffer.
    wl_resource* resource() const { return m_resource; }
    // Kind::Shm only; null once destroyed. Wrap reads in wl_shm_buffer_begin/end_access.
    wl_shm_buffer* shmBuffer() const;

    void lock() { ++m_locks; }
    void unlock();

private:
    ClientBuffer(wl_resource* resource, Kind kind, Size size, std::uint32_t format);
    ~ClientBuffer() = default;

    static void handleResourceDestroy(wl_listener* listener, void* data);

    // wl_listener first, so a wl_listener* converts back to Listener*.
    struct Listener {
        wl_listener listener;
        ClientBuffer* owner;
    };

    Listener m_destroyListener{};
    wl_resource* m_resource;
    Kind m_kind;
    Size m_size;
    std::uint32_t m_format;
    int m_locks = 0;
};

// RAII lock on a ClientBuffer (empty = no buffer).
class BufferLock {
public:
    BufferLock() = default;
    explicit BufferLock(ClientBuffer* buffer) : m_buffer(buffer)
    {
        if (m_buffer)
            m_buffer->lock();
    }
    ~BufferLock() { reset(); }

    BufferLock(BufferLock&& other) noexcept : m_buffer(std::exchange(other.m_buffer, nullptr)) {}
    BufferLock& operator=(BufferLock&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_buffer = std::exchange(other.m_buffer, nullptr);
        }
        return *this;
    }
    BufferLock(const BufferLock& other) : BufferLock(other.m_buffer) {}
    BufferLock& operator=(const BufferLock& other)
    {
        if (this != &other)
            *this = BufferLock(other.m_buffer);
        return *this;
    }

    ClientBuffer* get() const { return m_buffer; }
    ClientBuffer* operator->() const { return m_buffer; }
    explicit operator bool() const { return m_buffer != nullptr; }

    void reset()
    {
        if (auto* b = std::exchange(m_buffer, nullptr))
            b->unlock();
    }

private:
    ClientBuffer* m_buffer = nullptr;
};

} // namespace dragon
