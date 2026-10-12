#pragma once

#include "core/Geometry.h"
#include "core/Region.h"
#include "core/Signal.h"
#include "dragon-protocols/wayland.h"
#include "protocols/Buffer.h"

#include <memory>
#include <vector>

namespace dragon {

class Server;
class Subsurface;
struct SurfaceState;

// What a surface is for (xdg_toplevel, wl_subsurface, cursor, ...). A surface
// gets at most one role *type* for its lifetime; the role object implementing
// it may be destroyed and replaced by another of the same type.
class SurfaceRole {
public:
    virtual ~SurfaceRole() = default;
    // Before pending state is applied. Return false (after posting a protocol
    // error) to reject the commit.
    virtual bool precommit(const SurfaceState&) { return true; }
    // After the new state became current.
    virtual void commit() {}
};

// wl_callback objects from wl_surface.frame, waiting for "a good time to draw".
class FrameCallbackList {
public:
    void add(proto::WlCallback* callback);
    void append(FrameCallbackList& other);
    // Sends done(msec) to every callback and destroys them.
    void sendDone(std::uint32_t msec);
    bool empty() const { return m_entries.empty(); }

private:
    struct Entry {
        proto::WlCallback* callback;
        Connection gone;
    };
    std::vector<std::unique_ptr<Entry>> m_entries; // unique_ptr: Entry address is captured by `gone`
};

// Double-buffered wl_surface state. `committed` flags say which fields a
// commit carried; unset fields keep their previous value.
struct SurfaceState {
    enum Field : std::uint32_t {
        BufferField = 1 << 0,
        DamageField = 1 << 1,
        OpaqueField = 1 << 2,
        InputField = 1 << 3,
        ScaleField = 1 << 4,
        TransformField = 1 << 5,
        OffsetField = 1 << 6,
    };

    std::uint32_t committed = 0;
    BufferLock buffer;   // may be released early (shm is copied on commit), see hasBuffer
    bool hasBuffer = false; // a buffer is attached, even if its lock was released
    Size bufferSize;
    Point offset;        // accumulated attach/offset delta, consumed by the role
    Region surfaceDamage; // surface-local coordinates
    Region bufferDamage;  // buffer coordinates
    Region opaque;
    Region input = Region::infinite();
    std::int32_t scale = 1;
    proto::WlOutputTransform transform = proto::WlOutputTransform::Normal;
    FrameCallbackList frameCallbacks;

    // Takes everything `next` carries; `next` is left empty for the next commit.
    void absorb(SurfaceState& next);
};

class Surface final : public proto::WlSurface {
public:
    Surface(wl_client* client, std::uint32_t version, std::uint32_t id, Server& server);
    ~Surface() override;

    static Surface* from(wl_resource* resource)
    {
        return static_cast<Surface*>(proto::WlSurface::fromResource(resource));
    }

    Server& server() const { return m_server; }

    const SurfaceState& state() const { return m_current; }
    // The committed buffer, until releaseBuffer(). Null when none is attached.
    ClientBuffer* buffer() const { return m_current.buffer.get(); }
    // Whether the surface shows something (a buffer is attached), even after
    // its buffer was released.
    bool hasContent() const { return m_current.hasBuffer; }
    // Surface-local size: buffer size after transform and scale. Empty without a buffer.
    Size size() const;

    // We copied the contents (wl_shm): give the buffer back to the client now
    // instead of when it's replaced.
    void releaseBuffer() { m_current.buffer.reset(); }
    // Damage was consumed (uploaded / repainted).
    void clearDamage();

    // Consumes the accumulated attach offset (the role moves the window by it).
    Point takeOffset();

    // --- roles ---
    const char* roleName() const { return m_roleName; }
    SurfaceRole* role() const { return m_role; }
    // Sets the role type; false if the surface already has a different one.
    bool setRoleName(const char* name);
    // Installs the object implementing the role; false if one is active.
    bool attachRoleObject(SurfaceRole* role);
    void detachRoleObject(SurfaceRole* role);

    // --- subsurface tree ---
    Subsurface* asSubsurface() const;
    // Children and this surface in stacking order, bottom to top (`this` marks
    // where the parent's own content goes).
    const std::vector<Surface*>& stack() const { return m_currentStack; }

    // Fires the frame callbacks of the current state.
    void sendFrameDone(std::uint32_t msec) { m_current.frameCallbacks.sendDone(msec); }

    Signal<> committed;
    // Start of destruction, while state is still readable (`destroyed` fires
    // after members are gone). Roles unmap here.
    Signal<> aboutToBeDestroyed;

protected:
    void handleAttach(wl_resource* buffer, std::int32_t x, std::int32_t y) override;
    void handleDamage(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) override;
    void handleDamageBuffer(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) override;
    void handleFrame(std::uint32_t callback) override;
    void handleSetOpaqueRegion(wl_resource* region) override;
    void handleSetInputRegion(wl_resource* region) override;
    void handleCommit() override;
    void handleSetBufferTransform(proto::WlOutputTransform transform) override;
    void handleSetBufferScale(std::int32_t scale) override;
    void handleOffset(std::int32_t x, std::int32_t y) override;

private:
    friend class Subsurface;

    // Makes `next` current, then applies children's parent-committed state.
    void apply(SurfaceState& next);

    Server& m_server;
    SurfaceState m_pending;
    SurfaceState m_cached; // synchronized subsurfaces: commits waiting for the parent
    bool m_hasCached = false;
    SurfaceState m_current;

    const char* m_roleName = nullptr;
    SurfaceRole* m_role = nullptr;

    std::vector<Surface*> m_pendingStack{this};
    std::vector<Surface*> m_currentStack{this};
};

class Subsurface final : public proto::WlSubsurface, public SurfaceRole {
public:
    Subsurface(wl_client* client, std::uint32_t version, std::uint32_t id, Surface* surface, Surface* parent);
    ~Subsurface() override;

    Surface* surface() const { return m_surface; }
    Surface* parent() const { return m_parent; }
    // Relative to the parent surface; applied on the parent's commit.
    Point position() const { return m_position; }
    // Effectively synchronized: this one, or any ancestor subsurface, is in sync mode.
    bool synchronized() const;

protected:
    void handleSetPosition(std::int32_t x, std::int32_t y) override;
    void handlePlaceAbove(wl_resource* sibling) override;
    void handlePlaceBelow(wl_resource* sibling) override;
    void handleSetSync() override;
    void handleSetDesync() override;

private:
    friend class Surface;

    void parentCommitted();
    void place(wl_resource* sibling, bool above);
    void detach();

    Surface* m_surface;
    Surface* m_parent;
    Point m_pendingPosition;
    Point m_position;
    bool m_sync = true;
    Connection m_surfaceGone;
    Connection m_parentGone;
};

class RegionResource final : public proto::WlRegion {
public:
    using WlRegion::WlRegion;
    static const Region* from(wl_resource* resource);
    const Region& region() const { return m_region; }

protected:
    void handleAdd(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) override;
    void handleSubtract(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) override;

private:
    Region m_region;
};

class CompositorResource final : public proto::WlCompositor {
public:
    CompositorResource(wl_client* client, std::uint32_t version, std::uint32_t id, Server& server)
        : WlCompositor(client, version, id), m_server(server) {}

protected:
    void handleCreateSurface(std::uint32_t id) override;
    void handleCreateRegion(std::uint32_t id) override;

private:
    Server& m_server;
};

class SubcompositorResource final : public proto::WlSubcompositor {
public:
    using WlSubcompositor::WlSubcompositor;

protected:
    void handleGetSubsurface(std::uint32_t id, wl_resource* surface, wl_resource* parent) override;
};

} // namespace dragon
