#include "protocols/DataDevice.h"

#include "core/Log.h"
#include "core/UniqueFd.h"
#include "protocols/Seat.h"

#include <string>

namespace dragon {

DataOffer::DataOffer(wl_client* client, std::uint32_t version, std::uint32_t id, DataSource* source)
    : WlDataOffer(client, version, id), m_source(source)
{
    if (source)
        m_sourceGone = source->destroyed.connect([this] { m_source = nullptr; });
}

void DataOffer::handleReceive(std::string_view mimeType, std::int32_t fd)
{
    UniqueFd owned(fd); // libwayland dups it when forwarding; ours to close
    if (m_source)
        m_source->sendSend(std::string(mimeType).c_str(), fd);
}

void DataDevice::handleStartDrag(wl_resource* source, wl_resource*, wl_resource*, std::uint32_t)
{
    // Drag and drop isn't implemented yet: end it right away so the client doesn't wait.
    if (auto* s = DataSource::from(source))
        s->sendCancelled();
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

} // namespace dragon
