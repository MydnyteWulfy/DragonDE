#pragma once

#include "core/Signal.h"
#include "dragon-protocols/wayland.h"

#include <string>
#include <vector>

namespace dragon {

class Seat;

// Clipboard between clients (wl_data_device_manager). The selection belongs
// to a client's wl_data_source; whoever has keyboard focus gets a
// wl_data_offer for it and reads the data straight from the source through a
// pipe. Drag and drop is refused for now.

class DataSource final : public proto::WlDataSource {
public:
    using proto::WlDataSource::WlDataSource;
    static DataSource* from(wl_resource* resource)
    {
        return static_cast<DataSource*>(proto::WlDataSource::fromResource(resource));
    }
    const std::vector<std::string>& mimeTypes() const { return m_mimeTypes; }

protected:
    void handleOffer(std::string_view mimeType) override { m_mimeTypes.emplace_back(mimeType); }
    void handleSetActions(proto::WlDataDeviceManagerDndAction) override {}

private:
    std::vector<std::string> m_mimeTypes;
};

class DataOffer final : public proto::WlDataOffer {
public:
    DataOffer(wl_client* client, std::uint32_t version, std::uint32_t id, DataSource* source);

protected:
    void handleReceive(std::string_view mimeType, std::int32_t fd) override;
    void handleAccept(std::uint32_t, const char*) override {}
    void handleFinish() override {}
    void handleSetActions(proto::WlDataDeviceManagerDndAction, proto::WlDataDeviceManagerDndAction) override {}

private:
    DataSource* m_source;
    Connection m_sourceGone;
};

class DataDevice final : public proto::WlDataDevice {
public:
    DataDevice(wl_client* client, std::uint32_t version, std::uint32_t id, Seat& seat)
        : WlDataDevice(client, version, id), m_seat(seat) {}

protected:
    void handleStartDrag(wl_resource* source, wl_resource* origin, wl_resource* icon, std::uint32_t serial) override;
    void handleSetSelection(wl_resource* source, std::uint32_t serial) override;

private:
    Seat& m_seat;
};

class DataDeviceManager final : public proto::WlDataDeviceManager {
public:
    DataDeviceManager(wl_client* client, std::uint32_t version, std::uint32_t id, Seat& seat)
        : WlDataDeviceManager(client, version, id), m_seat(seat) {}

protected:
    void handleCreateDataSource(std::uint32_t id) override;
    void handleGetDataDevice(std::uint32_t id, wl_resource* seat) override;

private:
    Seat& m_seat;
};

} // namespace dragon
