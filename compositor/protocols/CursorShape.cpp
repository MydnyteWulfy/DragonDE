#include "protocols/CursorShape.h"

#include "Server.h"
#include "protocols/Seat.h"

namespace dragon {

namespace {

constexpr std::uint32_t kVersion = 2;
constexpr std::uint32_t kLastShape = 36; // v2's all_resize ... zoom_out range ends here

class Device final : public proto::WpCursorShapeDeviceV1 {
public:
    // Null seat: a tablet tool's device (no tablets yet), which stays inert.
    Device(wl_client* client, std::uint32_t version, std::uint32_t id, Seat* seat)
        : WpCursorShapeDeviceV1(client, version, id), m_seat(seat) {}

protected:
    void handleSetShape(std::uint32_t serial, proto::WpCursorShapeDeviceV1Shape shape) override
    {
        const auto value = std::uint32_t(shape);
        if (value < 1 || value > kLastShape) {
            postError(proto::WpCursorShapeDeviceV1Error::InvalidShape, "invalid cursor shape {}", value);
            return;
        }
        if (m_seat)
            m_seat->requestCursor(client(), serial, value);
    }

private:
    Seat* m_seat;
};

class Manager final : public proto::WpCursorShapeManagerV1 {
public:
    Manager(wl_client* client, std::uint32_t version, std::uint32_t id, Seat& seat)
        : WpCursorShapeManagerV1(client, version, id), m_seat(seat) {}

protected:
    void handleGetPointer(std::uint32_t id, wl_resource*) override
    {
        makeResource<Device>(client(), version(), id, &m_seat);
    }
    void handleGetTabletToolV2(std::uint32_t id, wl_resource*) override
    {
        makeResource<Device>(client(), version(), id, nullptr);
    }

private:
    Seat& m_seat;
};

} // namespace

CursorShapeManager::CursorShapeManager(Server& server, Seat& seat)
{
    m_global = Global::create<proto::WpCursorShapeManagerV1>(
        server.display(), kVersion, [&seat](wl_client* client, std::uint32_t version, std::uint32_t id) {
            makeResource<Manager>(client, version, id, seat);
        });
}

CursorShapeManager::~CursorShapeManager() = default;

} // namespace dragon
