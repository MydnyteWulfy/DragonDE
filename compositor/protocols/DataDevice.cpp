#include "protocols/DataDevice.h"

#include "core/Log.h"
#include "core/UniqueFd.h"
#include "protocols/Compositor.h"
#include "protocols/Seat.h"

#include <string>

namespace dragon {

DataOffer::DataOffer(wl_client* client, std::uint32_t version, std::uint32_t id, DataSource* source, bool dnd)
    : WlDataOffer(client, version, id), m_source(source), m_dnd(dnd)
{
    if (source)
        m_sourceGone = source->destroyed.connect([this] { m_source = nullptr; });
    // Old targets (< v3) don't negotiate: they always copy.
    if (dnd && version < 3)
        m_targetActions = m_preferred = DndAction::Copy;
}

void DataOffer::handleAccept(std::uint32_t, const char* mimeType)
{
    if (!m_dnd)
        return;
    m_accepted = mimeType ? mimeType : "";
    if (m_source)
        m_source->sendTarget(mimeType); // null: the target won't take it here
}

void DataOffer::handleSetActions(DndAction actions, DndAction preferred)
{
    if (!m_dnd)
        return;
    m_targetActions = actions;
    m_preferred = preferred;
    negotiate();
}

void DataOffer::negotiate()
{
    // Both sides' sets intersected; the target's preference wins if allowed,
    // else copy, move, ask in that order.
    const DndAction common = m_source ? (m_source->dndActions() & m_targetActions) : DndAction::None;
    DndAction chosen = DndAction::None;
    if (proto::hasFlags(common, m_preferred) && m_preferred != DndAction::None)
        chosen = m_preferred;
    else
        for (DndAction a : {DndAction::Copy, DndAction::Move, DndAction::Ask})
            if (chosen == DndAction::None && proto::hasFlags(common, a))
                chosen = a;
    if (chosen == m_action)
        return;
    m_action = chosen;
    sendAction(chosen);
    if (m_source)
        m_source->sendAction(chosen);
}

void DataOffer::handleFinish()
{
    if (m_dnd && m_dropped && m_source)
        m_source->sendDndFinished(); // the source may now delete moved data
}

void DataOffer::handleReceive(std::string_view mimeType, std::int32_t fd)
{
    UniqueFd owned(fd); // libwayland dups it when forwarding; ours to close
    if (m_source)
        m_source->sendSend(std::string(mimeType).c_str(), fd);
}

void DataDevice::handleStartDrag(wl_resource* source, wl_resource* origin, wl_resource* icon, std::uint32_t serial)
{
    Surface* iconSurface = icon ? Surface::from(icon) : nullptr;
    if (iconSurface && (iconSurface->role() || !iconSurface->setRoleName("wl_data_device-icon"))) {
        postError(proto::WlDataDeviceError::Role, "drag icon already has another role");
        return;
    }
    m_seat.startDrag(client(), source ? DataSource::from(source) : nullptr, Surface::from(origin), iconSurface,
                     serial);
}

void DataDevice::handleSetSelection(wl_resource* source, std::uint32_t)
{
    // Serials aren't checked yet: any client can set the clipboard.
    m_seat.setSelection(source ? DataSource::from(source) : nullptr);
}

void DataDeviceManager::handleCreateDataSource(std::uint32_t id)
{
    makeResource<DataSource>(client(), version(), id);
}

void DataDeviceManager::handleGetDataDevice(std::uint32_t id, wl_resource*)
{
    if (auto* device = makeResource<DataDevice>(client(), version(), id, m_seat))
        m_seat.addDataDevice(device);
}

// ------------------------------------------------------- primary selection --

PrimaryOffer::PrimaryOffer(wl_client* client, std::uint32_t version, std::uint32_t id, PrimarySource* source)
    : ZwpPrimarySelectionOfferV1(client, version, id), m_source(source)
{
    if (source)
        m_sourceGone = source->destroyed.connect([this] { m_source = nullptr; });
}

void PrimaryOffer::handleReceive(std::string_view mimeType, std::int32_t fd)
{
    UniqueFd owned(fd);
    if (m_source)
        m_source->sendSend(std::string(mimeType).c_str(), fd);
}

void PrimaryDevice::handleSetSelection(wl_resource* source, std::uint32_t serial)
{
    // Only from real input of the client, so background apps can't overwrite it.
    if (!m_seat.isRecentInputSerial(client(), serial)) {
        log::debug("input", "ignoring primary selection with a stale serial");
        return;
    }
    m_seat.setPrimarySelection(source ? PrimarySource::from(source) : nullptr);
}

void PrimaryDeviceManager::handleCreateSource(std::uint32_t id)
{
    makeResource<PrimarySource>(client(), version(), id);
}

void PrimaryDeviceManager::handleGetDevice(std::uint32_t id, wl_resource*)
{
    if (auto* device = makeResource<PrimaryDevice>(client(), version(), id, m_seat))
        m_seat.addPrimaryDevice(device);
}

} // namespace dragon
