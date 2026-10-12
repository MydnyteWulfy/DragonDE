#include "protocols/Seat.h"

#include "Server.h"
#include "backend/Backend.h"
#include "core/Log.h"
#include "protocols/Compositor.h"
#include "protocols/DataDevice.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

namespace dragon {

namespace {

constexpr std::uint32_t kSeatVersion = 9;
constexpr std::int32_t kRepeatRate = 25;   // keys per second
constexpr std::int32_t kRepeatDelay = 600; // ms
constexpr const char* kCursorRole = "wl_pointer-cursor";

class PointerResource final : public proto::WlPointer {
public:
    PointerResource(wl_client* client, std::uint32_t version, std::uint32_t id, Seat& seat)
        : WlPointer(client, version, id), m_seat(seat) {}

protected:
    void handleSetCursor(std::uint32_t serial, wl_resource* surface, std::int32_t, std::int32_t) override
    {
        Surface* s = surface ? Surface::from(surface) : nullptr;
        if (s && (s->role() || !s->setRoleName(kCursorRole))) {
            postError(proto::WlPointerError::Role, "cursor surface already has another role");
            return;
        }
        // Null surface: hide the cursor. A surface: the client's own image.
        m_seat.requestCursor(client(), serial, s ? std::nullopt : std::optional<std::uint32_t>(Seat::kHiddenCursor));
    }

private:
    Seat& m_seat;
};

class SeatResource final : public proto::WlSeat {
public:
    SeatResource(wl_client* client, std::uint32_t version, std::uint32_t id, Seat& seat)
        : WlSeat(client, version, id), m_seat(seat) {}

protected:
    void handleGetPointer(std::uint32_t id) override
    {
        if (auto* p = makeResource<PointerResource>(client(), version(), id, m_seat))
            m_seat.addPointer(p);
    }
    void handleGetKeyboard(std::uint32_t id) override
    {
        if (auto* k = makeResource<proto::WlKeyboard>(client(), version(), id))
            m_seat.addKeyboard(k);
    }
    void handleGetTouch(std::uint32_t id) override
    {
        makeResource<proto::WlTouch>(client(), version(), id); // no touch capability: stays silent
    }

private:
    Seat& m_seat;
};

proto::WlPointerAxis axisOf(const PointerAxisEvent& e)
{
    return e.horizontal ? proto::WlPointerAxis::HorizontalScroll : proto::WlPointerAxis::VerticalScroll;
}

} // namespace

// ----------------------------------------------------------------- Tracked --

template <typename T>
void Seat::Tracked<T>::add(T* obj)
{
    objects.push_back(obj);
    std::erase_if(connections, [](const Connection& c) { return !c.connected(); });
    connections.push_back(obj->destroyed.connect([this, obj] { std::erase(objects, obj); }));
}

template <typename T>
std::vector<T*> Seat::Tracked<T>::forClient(wl_client* client) const
{
    std::vector<T*> out;
    for (T* obj : objects) {
        if (obj->client() == client)
            out.push_back(obj);
    }
    return out;
}

// -------------------------------------------------------------------- Seat --

Seat::Seat(Server& server, std::string name) : m_server(server), m_name(std::move(name))
{
    m_xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    // Null names: XKB_DEFAULT_RULES/MODEL/LAYOUT/... from the environment, else US.
    if (xkb_keymap* keymap = xkb_keymap_new_from_names(m_xkbContext, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS))
        applyKeymap(keymap);
    else
        log::error("input", "cannot compile the default keymap");

    m_global = Global::create<proto::WlSeat>(
        server.display(), kSeatVersion, [this](wl_client* client, std::uint32_t version, std::uint32_t id) {
            auto* r = makeResource<SeatResource>(client, version, id, *this);
            if (!r)
                return;
            r->sendCapabilities(proto::WlSeatCapability::Pointer | proto::WlSeatCapability::Keyboard);
            r->sendName(m_name.c_str());
        });
}

Seat::~Seat()
{
    xkb_state_unref(m_xkbState);
    xkb_keymap_unref(m_keymap);
    xkb_context_unref(m_xkbContext);
}

std::uint32_t Seat::nextSerial()
{
    return m_server.nextSerial();
}

// ---------------------------------------------------------------- keyboard --

bool Seat::setKeymap(const std::string& text)
{
    xkb_keymap* keymap = xkb_keymap_new_from_string(m_xkbContext, text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1,
                                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        log::warn("input", "rejected an invalid keymap");
        return false;
    }
    applyKeymap(keymap);
    return true;
}

void Seat::applyKeymap(xkb_keymap* keymap)
{
    xkb_state_unref(m_xkbState);
    xkb_keymap_unref(m_keymap);
    m_keymap = keymap;
    m_xkbState = xkb_state_new(keymap);
    for (std::uint32_t key : m_pressedKeys)
        xkb_state_update_key(m_xkbState, key + 8, XKB_KEY_DOWN);

    // Clients get the keymap as a sealed, read-only memfd (NUL-terminated text).
    char* text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    const std::size_t size = std::strlen(text) + 1;
    UniqueFd fd(memfd_create("dragon-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (!fd || write(fd.get(), text, size) != ssize_t(size) ||
        fcntl(fd.get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) != 0) {
        log::error("input", "cannot share the keymap: {}", std::strerror(errno));
    }
    std::free(text);
    m_keymapFd = std::move(fd);
    m_keymapSize = std::uint32_t(size);

    for (auto* kb : m_keyboards.objects)
        sendKeymap(*kb);
    log::debug("input", "keymap updated ({} bytes)", size);
}

void Seat::sendKeymap(proto::WlKeyboard& kb)
{
    if (m_keymapFd)
        kb.sendKeymap(proto::WlKeyboardKeymapFormat::XkbV1, m_keymapFd.get(), m_keymapSize);
}

void Seat::addKeyboard(proto::WlKeyboard* kb)
{
    m_keyboards.add(kb);
    sendKeymap(*kb);
    kb->sendRepeatInfo(kRepeatRate, kRepeatDelay);
    if (m_keyboardFocus && m_keyboardFocus->client() == kb->client())
        sendKeyboardEnter(*kb, nextSerial());
}

void Seat::sendKeyboardEnter(proto::WlKeyboard& kb, std::uint32_t serial)
{
    wl_array keys;
    wl_array_init(&keys);
    for (std::uint32_t key : m_pressedKeys) {
        if (auto* p = static_cast<std::uint32_t*>(wl_array_add(&keys, sizeof(std::uint32_t))))
            *p = key;
    }
    kb.sendEnter(serial, m_keyboardFocus->resource(), &keys);
    wl_array_release(&keys);
    kb.sendModifiers(serial, m_modsSent[0], m_modsSent[1], m_modsSent[2], m_modsSent[3]);
}

void Seat::setKeyboardFocus(Surface* surface)
{
    if (surface == m_keyboardFocus)
        return;
    const std::uint32_t serial = nextSerial();
    if (m_keyboardFocus) {
        for (auto* kb : m_keyboards.forClient(m_keyboardFocus->client()))
            kb->sendLeave(serial, m_keyboardFocus->resource());
    }

    m_keyboardFocus = surface;
    m_keyboardFocusGone = surface ? surface->aboutToBeDestroyed.connect([this] { m_keyboardFocus = nullptr; })
                                  : Connection{};
    if (surface) {
        for (auto* kb : m_keyboards.forClient(surface->client()))
            sendKeyboardEnter(*kb, serial);
        sendSelection(surface->client());
        sendPrimarySelection(surface->client());
    }
}

void Seat::notifyKey(std::uint32_t timeMs, std::uint32_t key, bool pressed)
{
    const bool wasPressed = std::ranges::find(m_pressedKeys, key) != m_pressedKeys.end();
    if (pressed == wasPressed)
        return; // duplicate press / stray release
    if (pressed)
        m_pressedKeys.push_back(key);
    else
        std::erase(m_pressedKeys, key);

    if (m_xkbState)
        xkb_state_update_key(m_xkbState, key + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP); // evdev -> XKB keycode

    const std::uint32_t serial = nextSerial();
    rememberInput(serial, m_keyboardFocus); // presses and releases: menus open on either
    if (m_keyboardFocus) {
        const auto state = pressed ? proto::WlKeyboardKeyState::Pressed : proto::WlKeyboardKeyState::Released;
        for (auto* kb : m_keyboards.forClient(m_keyboardFocus->client()))
            kb->sendKey(serial, timeMs, key, state);
    }

    if (!m_xkbState)
        return;
    const std::uint32_t mods[4] = {
        xkb_state_serialize_mods(m_xkbState, XKB_STATE_MODS_DEPRESSED),
        xkb_state_serialize_mods(m_xkbState, XKB_STATE_MODS_LATCHED),
        xkb_state_serialize_mods(m_xkbState, XKB_STATE_MODS_LOCKED),
        xkb_state_serialize_layout(m_xkbState, XKB_STATE_LAYOUT_EFFECTIVE),
    };
    if (std::equal(std::begin(mods), std::end(mods), std::begin(m_modsSent)))
        return;
    std::copy(std::begin(mods), std::end(mods), std::begin(m_modsSent));
    if (m_keyboardFocus)
        sendModifiers(m_keyboardFocus->client(), serial);
}

void Seat::sendModifiers(wl_client* client, std::uint32_t serial)
{
    for (auto* kb : m_keyboards.forClient(client))
        kb->sendModifiers(serial, m_modsSent[0], m_modsSent[1], m_modsSent[2], m_modsSent[3]);
}

bool Seat::modifierActive(const char* name) const
{
    return m_xkbState && xkb_state_mod_name_is_active(m_xkbState, name, XKB_STATE_MODS_EFFECTIVE) > 0;
}

void Seat::rememberInput(std::uint32_t serial, Surface* focus)
{
    if (!focus)
        return;
    m_recentInput.push_back({serial, focus->client()});
    if (m_recentInput.size() > 16)
        m_recentInput.erase(m_recentInput.begin());
}

bool Seat::isRecentInputSerial(wl_client* client, std::uint32_t serial) const
{
    return std::ranges::any_of(m_recentInput,
                               [&](const InputSerial& s) { return s.serial == serial && s.client == client; });
}

// ----------------------------------------------------------------- pointer --

void Seat::addPointer(proto::WlPointer* pointer)
{
    m_pointers.add(pointer);
    if (m_pointerFocus && m_pointerFocus->client() == pointer->client()) {
        pointer->sendEnter(nextSerial(), m_pointerFocus->resource(), m_pointerLocal.x, m_pointerLocal.y);
        pointer->sendFrame();
    }
}

void Seat::notifyPointerFocus(Surface* surface, PointF local)
{
    m_pointerLocal = local;
    if (surface == m_pointerFocus)
        return;
    const std::uint32_t serial = nextSerial();
    if (m_pointerFocus) {
        for (auto* p : m_pointers.forClient(m_pointerFocus->client())) {
            p->sendLeave(serial, m_pointerFocus->resource());
            p->sendFrame();
        }
    }

    m_pointerFocus = surface;
    m_pointerFocusGone = surface ? surface->aboutToBeDestroyed.connect([this] { m_pointerFocus = nullptr; })
                                 : Connection{};
    m_pointerEnterSerial = serial;
    // Until the new client picks a cursor, show the default one.
    setCursorShape(kDefaultCursor);
    if (surface) {
        for (auto* p : m_pointers.forClient(surface->client())) {
            p->sendEnter(serial, surface->resource(), local.x, local.y);
            p->sendFrame();
        }
    }
}

void Seat::requestCursor(wl_client* client, std::uint32_t serial, std::optional<std::uint32_t> shape)
{
    if (!m_pointerFocus || m_pointerFocus->client() != client || serial != m_pointerEnterSerial)
        return; // not under the pointer, or reacting to an older enter
    setCursorShape(shape.value_or(kDefaultCursor));
}

void Seat::setCursorShape(std::uint32_t shape)
{
    if (shape == m_cursorShape)
        return;
    m_cursorShape = shape;
    cursorShapeChanged.emit(shape);
}

void Seat::notifyPointerMotion(std::uint32_t timeMs, PointF local)
{
    m_pointerLocal = local;
    if (!m_pointerFocus)
        return;
    for (auto* p : m_pointers.forClient(m_pointerFocus->client()))
        p->sendMotion(timeMs, local.x, local.y);
}

void Seat::notifyButton(std::uint32_t timeMs, std::uint32_t button, bool pressed)
{
    if (pressed)
        m_pressedButtons.push_back(button);
    else
        std::erase(m_pressedButtons, button);

    const std::uint32_t serial = nextSerial();
    if (pressed)
        m_lastButtonSerial = serial;
    rememberInput(serial, m_pointerFocus);
    if (!m_pointerFocus)
        return;
    const auto state = pressed ? proto::WlPointerButtonState::Pressed : proto::WlPointerButtonState::Released;
    for (auto* p : m_pointers.forClient(m_pointerFocus->client()))
        p->sendButton(serial, timeMs, button, state);
}

void Seat::notifyAxis(const PointerAxisEvent& e)
{
    if (!m_pointerFocus)
        return;
    for (auto* p : m_pointers.forClient(m_pointerFocus->client())) {
        p->sendAxisSource(proto::WlPointerAxisSource(e.source));
        if (e.value120 != 0) {
            if (p->version() >= 8)
                p->sendAxisValue120(axisOf(e), e.value120);
            else
                p->sendAxisDiscrete(axisOf(e), e.value120 / 120);
        }
        if (e.value != 0.0)
            p->sendAxis(e.timeMs, axisOf(e), e.value);
        if (e.stop)
            p->sendAxisStop(e.timeMs, axisOf(e));
    }
}

void Seat::notifyFrame()
{
    if (!m_pointerFocus)
        return;
    for (auto* p : m_pointers.forClient(m_pointerFocus->client()))
        p->sendFrame(); // dropped automatically for v1-4 pointers
}

// --------------------------------------------------------------- selection --

void Seat::addDataDevice(proto::WlDataDevice* device)
{
    m_dataDevices.add(device);
    if (m_keyboardFocus && m_keyboardFocus->client() == device->client())
        sendSelection(device->client());
}

void Seat::setSelection(DataSource* source)
{
    if (source == m_selection)
        return;
    if (m_selection)
        m_selection->sendCancelled(); // its owner lost the clipboard
    m_selection = source;
    m_selectionGone = source ? source->destroyed.connect([this] {
        m_selection = nullptr;
        if (m_keyboardFocus)
            sendSelection(m_keyboardFocus->client());
    })
                             : Connection{};
    if (m_keyboardFocus)
        sendSelection(m_keyboardFocus->client());
}

void Seat::sendSelection(wl_client* client)
{
    for (auto* device : m_dataDevices.forClient(client)) {
        if (!m_selection) {
            device->sendSelection(nullptr);
            continue;
        }
        // A fresh offer per device: the offer is how the client reads the data.
        auto* offer = makeResource<DataOffer>(client, device->version(), 0u, m_selection);
        if (!offer)
            continue;
        device->sendDataOffer(offer->resource());
        for (const auto& mime : m_selection->mimeTypes())
            offer->sendOffer(mime.c_str());
        device->sendSelection(offer->resource());
    }
}

void Seat::addPrimaryDevice(proto::ZwpPrimarySelectionDeviceV1* device)
{
    m_primaryDevices.add(device);
    if (m_keyboardFocus && m_keyboardFocus->client() == device->client())
        sendPrimarySelection(device->client());
}

void Seat::setPrimarySelection(PrimarySource* source)
{
    if (source == m_primary)
        return;
    if (m_primary)
        m_primary->sendCancelled();
    m_primary = source;
    m_primaryGone = source ? source->destroyed.connect([this] {
        m_primary = nullptr;
        if (m_keyboardFocus)
            sendPrimarySelection(m_keyboardFocus->client());
    })
                           : Connection{};
    if (m_keyboardFocus)
        sendPrimarySelection(m_keyboardFocus->client());
}

void Seat::sendPrimarySelection(wl_client* client)
{
    for (auto* device : m_primaryDevices.forClient(client)) {
        if (!m_primary) {
            device->sendSelection(nullptr);
            continue;
        }
        auto* offer = makeResource<PrimaryOffer>(client, device->version(), 0u, m_primary);
        if (!offer)
            continue;
        device->sendDataOffer(offer->resource());
        for (const auto& mime : m_primary->mimeTypes())
            offer->sendOffer(mime.c_str());
        device->sendSelection(offer->resource());
    }
}

} // namespace dragon
