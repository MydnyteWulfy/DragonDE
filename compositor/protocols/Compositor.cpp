#include "protocols/Compositor.h"

#include "Server.h"
#include "core/Log.h"

#include <algorithm>
#include <cstring>

namespace dragon {

namespace {
constexpr const char* kSubsurfaceRole = "wl_subsurface";

bool isRotated90(proto::WlOutputTransform t)
{
    using T = proto::WlOutputTransform;
    return t == T::_90 || t == T::_270 || t == T::Flipped90 || t == T::Flipped270;
}
} // namespace

// ------------------------------------------------------- FrameCallbackList --

void FrameCallbackList::add(proto::WlCallback* callback)
{
    auto entry = std::make_unique<Entry>();
    auto* raw = entry.get();
    raw->callback = callback;
    raw->gone = callback->destroyed.connect([raw] { raw->callback = nullptr; });
    m_entries.push_back(std::move(entry));
}

void FrameCallbackList::append(FrameCallbackList& other)
{
    for (auto& e : other.m_entries)
        m_entries.push_back(std::move(e));
    other.m_entries.clear();
}

void FrameCallbackList::sendDone(std::uint32_t msec)
{
    auto entries = std::move(m_entries);
    m_entries.clear();
    for (auto& e : entries) {
        if (e->callback) {
            e->callback->sendDone(msec);
            e->callback->destroy(); // wl_callback.done is a destructor event
        }
    }
}

// ------------------------------------------------------------ SurfaceState --

void SurfaceState::absorb(SurfaceState& next)
{
    if (next.committed & BufferField) {
        buffer = std::move(next.buffer);
        hasBuffer = next.hasBuffer;
        bufferSize = next.bufferSize;
    }
    if (next.committed & OffsetField)
        offset = offset + next.offset;
    if (next.committed & DamageField) {
        surfaceDamage.unite(next.surfaceDamage);
        bufferDamage.unite(next.bufferDamage);
    }
    if (next.committed & OpaqueField)
        opaque = next.opaque;
    if (next.committed & InputField)
        input = next.input;
    if (next.committed & ScaleField)
        scale = next.scale;
    if (next.committed & TransformField)
        transform = next.transform;
    frameCallbacks.append(next.frameCallbacks);
    committed |= next.committed;

    next.committed = 0;
    next.buffer.reset();
    next.offset = {};
    next.surfaceDamage.clear();
    next.bufferDamage.clear();
}

// ----------------------------------------------------------------- Surface --

Surface::Surface(wl_client* client, std::uint32_t version, std::uint32_t id, Server& server)
    : WlSurface(client, version, id), m_server(server)
{
}

Surface::~Surface()
{
    aboutToBeDestroyed.emit();

    // Unlink the subsurface tree now, while our members still exist; the
    // `destroyed` signal only fires after they're gone.
    std::vector<Surface*> children;
    for (auto* s : m_pendingStack) {
        if (s != this)
            children.push_back(s);
    }
    for (auto* child : children) {
        if (auto* sub = child->asSubsurface(); sub && sub->parent() == this)
            sub->detach();
    }
    if (auto* sub = asSubsurface())
        sub->detach();
}

Size Surface::size() const
{
    if (!m_current.hasBuffer)
        return {};
    Size s = m_current.bufferSize;
    if (isRotated90(m_current.transform))
        std::swap(s.width, s.height);
    return {s.width / m_current.scale, s.height / m_current.scale};
}

void Surface::clearDamage()
{
    m_current.surfaceDamage.clear();
    m_current.bufferDamage.clear();
}

Point Surface::takeOffset()
{
    return std::exchange(m_current.offset, Point{});
}

bool Surface::setRoleName(const char* name)
{
    if (m_roleName && std::strcmp(m_roleName, name) != 0)
        return false;
    m_roleName = name;
    return true;
}

bool Surface::attachRoleObject(SurfaceRole* role)
{
    if (m_role)
        return false;
    m_role = role;
    return true;
}

void Surface::detachRoleObject(SurfaceRole* role)
{
    if (m_role == role)
        m_role = nullptr;
}

Subsurface* Surface::asSubsurface() const
{
    return dynamic_cast<Subsurface*>(m_role);
}

void Surface::handleAttach(wl_resource* buffer, std::int32_t x, std::int32_t y)
{
    if (x != 0 || y != 0) {
        if (version() >= 5) {
            postError(proto::WlSurfaceError::InvalidOffset, "attach offset must be 0 since v5, use wl_surface.offset");
            return;
        }
        m_pending.offset = {x, y};
        m_pending.committed |= SurfaceState::OffsetField;
    }

    ClientBuffer* cb = nullptr;
    if (buffer) {
        cb = ClientBuffer::get(buffer);
        if (!cb)
            log::warn("proto", "wl_surface.attach: unsupported buffer type, ignoring");
    }
    m_pending.buffer = BufferLock(cb);
    m_pending.hasBuffer = cb != nullptr;
    m_pending.bufferSize = cb ? cb->size() : Size{};
    m_pending.committed |= SurfaceState::BufferField;
}

void Surface::handleDamage(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
{
    m_pending.surfaceDamage.add({x, y, width, height});
    m_pending.committed |= SurfaceState::DamageField;
}

void Surface::handleDamageBuffer(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
{
    m_pending.bufferDamage.add({x, y, width, height});
    m_pending.committed |= SurfaceState::DamageField;
}

void Surface::handleFrame(std::uint32_t callback)
{
    if (auto* cb = makeResource<proto::WlCallback>(client(), 1u, callback))
        m_pending.frameCallbacks.add(cb);
}

void Surface::handleSetOpaqueRegion(wl_resource* region)
{
    const Region* r = RegionResource::from(region);
    m_pending.opaque = r ? *r : Region();
    m_pending.committed |= SurfaceState::OpaqueField;
}

void Surface::handleSetInputRegion(wl_resource* region)
{
    const Region* r = RegionResource::from(region);
    m_pending.input = r ? *r : Region::infinite();
    m_pending.committed |= SurfaceState::InputField;
}

void Surface::handleSetBufferTransform(proto::WlOutputTransform transform)
{
    if (std::uint32_t(transform) > std::uint32_t(proto::WlOutputTransform::Flipped270)) {
        postError(proto::WlSurfaceError::InvalidTransform, "invalid buffer transform {}", std::uint32_t(transform));
        return;
    }
    m_pending.transform = transform;
    m_pending.committed |= SurfaceState::TransformField;
}

void Surface::handleSetBufferScale(std::int32_t scale)
{
    if (scale < 1) {
        postError(proto::WlSurfaceError::InvalidScale, "buffer scale must be >= 1, got {}", scale);
        return;
    }
    m_pending.scale = scale;
    m_pending.committed |= SurfaceState::ScaleField;
}

void Surface::handleOffset(std::int32_t x, std::int32_t y)
{
    m_pending.offset = {x, y};
    m_pending.committed |= SurfaceState::OffsetField;
}

void Surface::handleCommit()
{
    if (m_role && !m_role->precommit(m_pending))
        return;

    if (auto* sub = asSubsurface(); sub && sub->synchronized()) {
        m_cached.absorb(m_pending);
        m_hasCached = true;
        return;
    }
    if (m_hasCached) {
        m_cached.absorb(m_pending);
        m_hasCached = false;
        apply(m_cached);
    } else {
        apply(m_pending);
    }
}

void Surface::apply(SurfaceState& next)
{
    m_current.absorb(next);

    // Subsurface order and positions are double-buffered on the parent.
    m_currentStack = m_pendingStack;
    for (auto* s : std::vector(m_currentStack)) {
        if (s != this) {
            if (auto* sub = s->asSubsurface())
                sub->parentCommitted();
        }
    }

    if (m_role)
        m_role->commit();
    committed.emit();
}

// -------------------------------------------------------------- Subsurface --

Subsurface::Subsurface(wl_client* client, std::uint32_t version, std::uint32_t id, Surface* surface, Surface* parent)
    : WlSubsurface(client, version, id), m_surface(surface), m_parent(parent)
{
    m_surface->attachRoleObject(this);
    // New subsurfaces start on top of their siblings.
    m_parent->m_pendingStack.push_back(surface);
    m_parent->m_currentStack.push_back(surface);

    // ~Surface already detaches us; these only drop the dangling pointers.
    m_surfaceGone = surface->destroyed.connect([this] { m_surface = nullptr; });
    m_parentGone = parent->destroyed.connect([this] { m_parent = nullptr; });
}

Subsurface::~Subsurface()
{
    detach();
}

void Subsurface::detach()
{
    if (m_parent) {
        std::erase(m_parent->m_pendingStack, m_surface);
        std::erase(m_parent->m_currentStack, m_surface);
        m_parent = nullptr;
        m_parentGone.disconnect();
    }
    if (m_surface) {
        m_surface->detachRoleObject(this);
        m_surface = nullptr;
        m_surfaceGone.disconnect();
    }
}

bool Subsurface::synchronized() const
{
    if (m_sync)
        return true;
    if (m_parent) {
        if (auto* parentSub = m_parent->asSubsurface())
            return parentSub->synchronized();
    }
    return false;
}

void Subsurface::parentCommitted()
{
    m_position = m_pendingPosition;
    if (m_surface && synchronized() && m_surface->m_hasCached) {
        m_surface->m_hasCached = false;
        m_surface->apply(m_surface->m_cached);
    }
}

void Subsurface::handleSetPosition(std::int32_t x, std::int32_t y)
{
    m_pendingPosition = {x, y};
}

void Subsurface::handlePlaceAbove(wl_resource* sibling)
{
    place(sibling, true);
}

void Subsurface::handlePlaceBelow(wl_resource* sibling)
{
    place(sibling, false);
}

void Subsurface::place(wl_resource* sibling, bool above)
{
    if (!m_parent || !m_surface)
        return; // inert
    Surface* sib = Surface::from(sibling);
    const bool valid = sib == m_parent ||
                       (sib && sib != m_surface && sib->asSubsurface() && sib->asSubsurface()->parent() == m_parent);
    if (!valid) {
        postError(proto::WlSubsurfaceError::BadSurface, "{} target is neither the parent nor a sibling",
                  above ? "place_above" : "place_below");
        return;
    }
    auto& stack = m_parent->m_pendingStack;
    std::erase(stack, m_surface);
    auto it = std::ranges::find(stack, sib);
    stack.insert(above ? it + 1 : it, m_surface);
}

void Subsurface::handleSetSync()
{
    m_sync = true;
}

void Subsurface::handleSetDesync()
{
    m_sync = false;
    // Leaving sync mode applies whatever was cached.
    if (m_surface && !synchronized() && m_surface->m_hasCached) {
        m_surface->m_hasCached = false;
        m_surface->apply(m_surface->m_cached);
    }
}

// ---------------------------------------------------------- RegionResource --

const Region* RegionResource::from(wl_resource* resource)
{
    auto* r = static_cast<RegionResource*>(proto::WlRegion::fromResource(resource));
    return r ? &r->m_region : nullptr;
}

void RegionResource::handleAdd(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
{
    m_region.add({x, y, width, height});
}

void RegionResource::handleSubtract(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height)
{
    m_region.subtract({x, y, width, height});
}

// --------------------------------------------- compositor / subcompositor --

void CompositorResource::handleCreateSurface(std::uint32_t id)
{
    if (auto* surface = makeResource<Surface>(client(), version(), id, m_server))
        m_server.surfaceCreated.emit(surface);
}

void CompositorResource::handleCreateRegion(std::uint32_t id)
{
    makeResource<RegionResource>(client(), 1u, id);
}

void SubcompositorResource::handleGetSubsurface(std::uint32_t id, wl_resource* surface, wl_resource* parent)
{
    Surface* s = Surface::from(surface);
    Surface* p = Surface::from(parent);
    if (!s || !p)
        return;

    if (s == p) {
        postError(proto::WlSubcompositorError::BadParent, "a surface cannot be its own parent");
        return;
    }
    for (auto* sub = p->asSubsurface(); sub; sub = sub->parent() ? sub->parent()->asSubsurface() : nullptr) {
        if (sub->parent() == s) {
            postError(proto::WlSubcompositorError::BadParent, "parent is a descendant of the surface");
            return;
        }
    }
    if (s->role() || !s->setRoleName(kSubsurfaceRole)) {
        postError(proto::WlSubcompositorError::BadSurface, "surface already has the role {}",
                  s->roleName() ? s->roleName() : "(active role object)");
        return;
    }
    makeResource<Subsurface>(client(), version(), id, s, p);
}

} // namespace dragon
