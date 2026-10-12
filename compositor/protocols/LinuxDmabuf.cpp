#include "protocols/LinuxDmabuf.h"

#include "Server.h"
#include "core/Log.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <sys/mman.h>
#include <unistd.h>

namespace dragon {

namespace {

constexpr std::uint32_t kVersion = 4;
constexpr std::uint32_t kMaxPlanes = 4;

using ParamsError = proto::ZwpLinuxBufferParamsV1Error;

// One entry of the shared format table (layout fixed by the protocol).
struct TableEntry {
    std::uint32_t format;
    std::uint32_t padding;
    std::uint64_t modifier;
};
static_assert(sizeof(TableEntry) == 16);

class Params final : public proto::ZwpLinuxBufferParamsV1 {
public:
    Params(wl_client* client, std::uint32_t version, std::uint32_t id, const LinuxDmabuf& dmabuf)
        : ZwpLinuxBufferParamsV1(client, version, id), m_dmabuf(dmabuf) {}

protected:
    void handleAdd(std::int32_t fd, std::uint32_t planeIdx, std::uint32_t offset, std::uint32_t stride,
                   std::uint32_t modifierHi, std::uint32_t modifierLo) override
    {
        UniqueFd owned(fd); // received fds are ours to close
        if (m_used) {
            postError(ParamsError::AlreadyUsed, "params already used to create a buffer");
            return;
        }
        if (planeIdx >= kMaxPlanes) {
            postError(ParamsError::PlaneIdx, "plane index {} out of bounds", planeIdx);
            return;
        }
        auto& plane = m_planes[planeIdx];
        if (plane.set) {
            postError(ParamsError::PlaneSet, "plane {} already set", planeIdx);
            return;
        }
        plane = {std::move(owned), offset, stride, (std::uint64_t(modifierHi) << 32) | modifierLo, true};
    }

    void handleCreate(std::int32_t width, std::int32_t height, std::uint32_t format,
                      proto::ZwpLinuxBufferParamsV1Flags flags) override
    {
        bool posted = false;
        auto buf = build(width, height, format, flags, posted);
        if (posted)
            return;
        if (!buf) {
            sendFailed();
            return;
        }
        // The new wl_buffer is created by us (id 0: libwayland picks a server-side id).
        if (auto* buffer = makeResource<DmabufBuffer>(client(), 0u, std::move(*buf)))
            sendCreated(buffer->resource());
    }

    void handleCreateImmed(std::uint32_t bufferId, std::int32_t width, std::int32_t height, std::uint32_t format,
                           proto::ZwpLinuxBufferParamsV1Flags flags) override
    {
        bool posted = false;
        auto buf = build(width, height, format, flags, posted);
        if (posted)
            return;
        if (!buf) {
            postError(ParamsError::InvalidWlBuffer, "dma-buf import failed");
            return;
        }
        makeResource<DmabufBuffer>(client(), bufferId, std::move(*buf));
    }

private:
    struct Plane {
        UniqueFd fd;
        std::uint32_t offset = 0;
        std::uint32_t stride = 0;
        std::uint64_t modifier = 0;
        bool set = false;
    };

    // Validates and assembles the buffer. Protocol violations post an error
    // (and set `posted`); a valid request we can't import returns nullopt.
    std::optional<DmaBuf> build(std::int32_t width, std::int32_t height, std::uint32_t format,
                                proto::ZwpLinuxBufferParamsV1Flags flags, bool& posted)
    {
        auto fail = [&](ParamsError code, std::string_view what) {
            postError(code, "{}", what);
            posted = true;
            return std::nullopt;
        };
        if (m_used)
            return fail(ParamsError::AlreadyUsed, "params already used to create a buffer");
        m_used = true;

        std::uint32_t planeCount = 0;
        while (planeCount < kMaxPlanes && m_planes[planeCount].set)
            ++planeCount;
        if (planeCount == 0)
            return fail(ParamsError::Incomplete, "no planes added");
        for (std::uint32_t i = planeCount; i < kMaxPlanes; ++i) {
            if (m_planes[i].set)
                return fail(ParamsError::Incomplete, "gap in plane indices");
        }
        if (width <= 0 || height <= 0)
            return fail(ParamsError::InvalidDimensions, "width and height must be positive");
        if (!m_dmabuf.supportsFormat(format))
            return fail(ParamsError::InvalidFormat, "unsupported format");

        const std::uint64_t modifier = m_planes[0].modifier;
        for (std::uint32_t i = 1; i < planeCount; ++i) {
            if (m_planes[i].modifier != modifier)
                return fail(ParamsError::InvalidFormat, "planes have different modifiers");
        }

        // Cheap bounds check where the fd supports seeking (dma-bufs do).
        for (std::uint32_t i = 0; i < planeCount; ++i) {
            const off_t size = lseek(m_planes[i].fd.get(), 0, SEEK_END);
            if (size < 0)
                continue;
            if (std::uint64_t(m_planes[i].offset) >= std::uint64_t(size))
                return fail(ParamsError::OutOfBounds, "plane offset beyond the buffer");
            if (i == 0 && std::uint64_t(m_planes[0].offset) +
                                  std::uint64_t(m_planes[0].stride) * std::uint64_t(height) > std::uint64_t(size))
                return fail(ParamsError::OutOfBounds, "plane 0 extends beyond the buffer");
        }

        if (std::uint32_t(flags) != 0) {
            log::debug("proto", "dmabuf: unsupported flags {:#x}", std::uint32_t(flags));
            return std::nullopt; // y-invert/interlaced: not supported, import "fails"
        }
        if (!m_dmabuf.supports(format, modifier)) {
            log::debug("proto", "dmabuf: format {:#010x} with modifier {:#018x} not advertised", format, modifier);
            return std::nullopt;
        }

        DmaBuf buf{std::uint32_t(width), std::uint32_t(height), format, modifier, {}};
        for (std::uint32_t i = 0; i < planeCount; ++i)
            buf.planes.push_back({std::move(m_planes[i].fd), m_planes[i].offset, m_planes[i].stride});

        if (m_dmabuf.support().validate && !m_dmabuf.support().validate(buf)) {
            log::debug("proto", "dmabuf: renderer rejected {}x{} {:#010x}/{:#018x}", width, height, format, modifier);
            return std::nullopt;
        }
        return buf;
    }

    const LinuxDmabuf& m_dmabuf;
    std::array<Plane, kMaxPlanes> m_planes;
    bool m_used = false;
};

class DmabufResource final : public proto::ZwpLinuxDmabufV1 {
public:
    DmabufResource(wl_client* client, std::uint32_t version, std::uint32_t id, const LinuxDmabuf& dmabuf)
        : ZwpLinuxDmabufV1(client, version, id), m_dmabuf(dmabuf) {}

protected:
    void handleCreateParams(std::uint32_t id) override { makeResource<Params>(client(), version(), id, m_dmabuf); }

    void handleGetDefaultFeedback(std::uint32_t id) override { feedback(id); }
    // Per-surface feedback (e.g. scanout tranches for fullscreen) comes with the DRM backend.
    void handleGetSurfaceFeedback(std::uint32_t id, wl_resource*) override { feedback(id); }

private:
    void feedback(std::uint32_t id)
    {
        if (auto* fb = makeResource<proto::ZwpLinuxDmabufFeedbackV1>(client(), version(), id))
            m_dmabuf.sendFeedback(*fb);
    }

    const LinuxDmabuf& m_dmabuf;
};

void appendDevice(wl_array& array, dev_t device)
{
    if (void* p = wl_array_add(&array, sizeof(device)))
        std::memcpy(p, &device, sizeof(device));
}

} // namespace

LinuxDmabuf::LinuxDmabuf(Server& server, DmabufSupport support) : m_support(std::move(support))
{
    // The table is shared read-only with every client: write it once, seal it.
    std::vector<TableEntry> table;
    for (const auto& f : m_support.formats)
        table.push_back({f.format, 0, f.modifier});
    m_formatTableSize = std::uint32_t(table.size() * sizeof(TableEntry));

    m_formatTable = UniqueFd(memfd_create("dragon-dmabuf-formats", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (!m_formatTable ||
        write(m_formatTable.get(), table.data(), m_formatTableSize) != ssize_t(m_formatTableSize) ||
        fcntl(m_formatTable.get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) != 0) {
        log::error("proto", "dmabuf: cannot create format table: {}", std::strerror(errno));
    }

    m_global = Global::create<proto::ZwpLinuxDmabufV1>(
        server.display(), kVersion, [this](wl_client* client, std::uint32_t version, std::uint32_t id) {
            auto* r = makeResource<DmabufResource>(client, version, id, *this);
            if (!r || version >= 4)
                return; // v4+: clients ask for feedback instead
            for (const auto& f : m_support.formats) {
                if (version >= 3)
                    r->sendModifier(f.format, std::uint32_t(f.modifier >> 32), std::uint32_t(f.modifier));
                else if (f.modifier == 0) // DRM_FORMAT_MOD_LINEAR: v1/v2 have no modifiers
                    r->sendFormat(f.format);
            }
        });
    log::info("proto", "linux-dmabuf: {} format/modifier pairs", m_support.formats.size());
}

LinuxDmabuf::~LinuxDmabuf() = default;

bool LinuxDmabuf::supportsFormat(std::uint32_t format) const
{
    return std::ranges::any_of(m_support.formats, [format](const auto& f) { return f.format == format; });
}

bool LinuxDmabuf::supports(std::uint32_t format, std::uint64_t modifier) const
{
    return std::ranges::any_of(m_support.formats,
                               [&](const auto& f) { return f.format == format && f.modifier == modifier; });
}

void LinuxDmabuf::sendFeedback(proto::ZwpLinuxDmabufFeedbackV1& fb) const
{
    fb.sendFormatTable(m_formatTable.get(), m_formatTableSize); // libwayland sends a dup

    wl_array device;
    wl_array_init(&device);
    appendDevice(device, m_support.mainDevice);
    fb.sendMainDevice(&device);

    // One tranche: everything, on our render device.
    fb.sendTrancheTargetDevice(&device);
    wl_array indices;
    wl_array_init(&indices);
    for (std::uint16_t i = 0; i < m_support.formats.size(); ++i) {
        if (auto* p = static_cast<std::uint16_t*>(wl_array_add(&indices, sizeof(std::uint16_t))))
            *p = i;
    }
    fb.sendTrancheFormats(&indices);
    fb.sendTrancheFlags(proto::ZwpLinuxDmabufFeedbackV1TrancheFlags{});
    fb.sendTrancheDone();
    fb.sendDone();

    wl_array_release(&indices);
    wl_array_release(&device);
}

} // namespace dragon
