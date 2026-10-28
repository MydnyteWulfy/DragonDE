#pragma once

#include "core/Signal.h"
#include "dragon-protocols/primary-selection-unstable-v1.h"
#include "dragon-protocols/wayland.h"

#include <string>
#include <vector>

namespace dragon {

class Seat;

// Clipboard between clients (wl_data_device_manager). The selection belongs
// to a client's wl_data_source; whoever has keyboard focus gets a
// wl_data_offer for it and reads the data straight from the source through a
// pipe. Drag and drop is refused for now.

using DndAction = proto::WlDataDeviceManagerDndAction;

class DataSource final : public proto::WlDataSource {
public:
    using proto::WlDataSource::WlDataSource;
    static DataSource* from(wl_resource* resource)
    {
        return static_cast<DataSource*>(proto::WlDataSource::fromResource(resource));
    }
    const std::vector<std::string>& mimeTypes() const { return m_mimeTypes; }
    // What a drag from this source may do (v3+; older sources: copy).
    DndAction dndActions() const { return version() >= 3 ? m_actions : DndAction::Copy; }

protected:
    void handleOffer(std::string_view mimeType) override { m_mimeTypes.emplace_back(mimeType); }
    void handleSetActions(DndAction actions) override { m_actions = actions; }

private:
    std::vector<std::string> m_mimeTypes;
    DndAction m_actions = DndAction::None;
};

// An offer of a selection (clipboard) or of a drag in progress (`dnd`).
class DataOffer final : public proto::WlDataOffer {
public:
    DataOffer(wl_client* client, std::uint32_t version, std::uint32_t id, DataSource* source, bool dnd = false);

    // Drag and drop: what the target agreed to.
    const std::string& acceptedMimeType() const { return m_accepted; }
    DndAction action() const { return m_action; }
    // The drop happened: the target may now read the data and call finish.
    void markDropped() { m_dropped = true; }

protected:
    void handleReceive(std::string_view mimeType, std::int32_t fd) override;
    void handleAccept(std::uint32_t serial, const char* mimeType) override;
    void handleFinish() override;
    void handleSetActions(DndAction actions, DndAction preferred) override;

private:
    void negotiate();

    DataSource* m_source;
    Connection m_sourceGone;
    bool m_dnd;
    bool m_dropped = false;
    std::string m_accepted;
    DndAction m_targetActions = DndAction::None;
    DndAction m_preferred = DndAction::None;
    DndAction m_action = DndAction::None;
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

// Primary selection (middle-click paste): the same model with its own objects.
// Whatever text was last selected is offered; pasting reads it from the source.

class PrimarySource final : public proto::ZwpPrimarySelectionSourceV1 {
public:
    using proto::ZwpPrimarySelectionSourceV1::ZwpPrimarySelectionSourceV1;
    static PrimarySource* from(wl_resource* resource)
    {
        return static_cast<PrimarySource*>(proto::ZwpPrimarySelectionSourceV1::fromResource(resource));
    }
    const std::vector<std::string>& mimeTypes() const { return m_mimeTypes; }

protected:
    void handleOffer(std::string_view mimeType) override { m_mimeTypes.emplace_back(mimeType); }

private:
    std::vector<std::string> m_mimeTypes;
};

class PrimaryOffer final : public proto::ZwpPrimarySelectionOfferV1 {
public:
    PrimaryOffer(wl_client* client, std::uint32_t version, std::uint32_t id, PrimarySource* source);

protected:
    void handleReceive(std::string_view mimeType, std::int32_t fd) override;

private:
    PrimarySource* m_source;
    Connection m_sourceGone;
};

class PrimaryDevice final : public proto::ZwpPrimarySelectionDeviceV1 {
public:
    PrimaryDevice(wl_client* client, std::uint32_t version, std::uint32_t id, Seat& seat)
        : ZwpPrimarySelectionDeviceV1(client, version, id), m_seat(seat) {}

protected:
    void handleSetSelection(wl_resource* source, std::uint32_t serial) override;

private:
    Seat& m_seat;
};

class PrimaryDeviceManager final : public proto::ZwpPrimarySelectionDeviceManagerV1 {
public:
    PrimaryDeviceManager(wl_client* client, std::uint32_t version, std::uint32_t id, Seat& seat)
        : ZwpPrimarySelectionDeviceManagerV1(client, version, id), m_seat(seat) {}

protected:
    void handleCreateSource(std::uint32_t id) override;
    void handleGetDevice(std::uint32_t id, wl_resource* seat) override;

private:
    Seat& m_seat;
};

} // namespace dragon
