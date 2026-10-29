#include "xwayland/Xwm.h"

#include "Server.h"
#include "core/Log.h"
#include "desktop/Desktop.h"
#include "desktop/Toplevel.h"
#include "protocols/Compositor.h"
#include "xwayland/XwaylandShell.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <xcb/composite.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

namespace dragon {

// ------------------------------------------------------------------- atoms --

enum Xwm::Atom : int {
    WM_PROTOCOLS,
    WM_DELETE_WINDOW,
    WM_TAKE_FOCUS,
    WM_STATE,
    WM_CHANGE_STATE,
    WM_S0,
    UTF8_STRING,
    NET_WM_NAME,
    NET_WM_STATE,
    NET_WM_STATE_FULLSCREEN,
    NET_WM_STATE_MAXIMIZED_VERT,
    NET_WM_STATE_MAXIMIZED_HORZ,
    NET_WM_STATE_FOCUSED,
    NET_ACTIVE_WINDOW,
    NET_SUPPORTED,
    NET_SUPPORTING_WM_CHECK,
    NET_WM_MOVERESIZE,
    NET_WM_CM_S0,
    NET_CLIENT_LIST,
    NET_WM_WINDOW_TYPE,
    WL_SURFACE_SERIAL,
    AtomCount,
};

struct Xwm::Atoms {
    std::array<xcb_atom_t, AtomCount> values{};
    xcb_atom_t operator[](Atom a) const { return values[a]; }
};

namespace {

constexpr std::array<const char*, Xwm::AtomCount> kAtomNames = {
    "WM_PROTOCOLS",
    "WM_DELETE_WINDOW",
    "WM_TAKE_FOCUS",
    "WM_STATE",
    "WM_CHANGE_STATE",
    "WM_S0",
    "UTF8_STRING",
    "_NET_WM_NAME",
    "_NET_WM_STATE",
    "_NET_WM_STATE_FULLSCREEN",
    "_NET_WM_STATE_MAXIMIZED_VERT",
    "_NET_WM_STATE_MAXIMIZED_HORZ",
    "_NET_WM_STATE_FOCUSED",
    "_NET_ACTIVE_WINDOW",
    "_NET_SUPPORTED",
    "_NET_SUPPORTING_WM_CHECK",
    "_NET_WM_MOVERESIZE",
    "_NET_WM_CM_S0",
    "_NET_CLIENT_LIST",
    "_NET_WM_WINDOW_TYPE",
    "WL_SURFACE_SERIAL",
};

// ICCCM WM_STATE values.
constexpr std::uint32_t kWithdrawnState = 0;
constexpr std::uint32_t kNormalState = 1;

// _NET_WM_MOVERESIZE directions -> xdg_toplevel resize edges.
constexpr std::uint32_t kMoveResizeMove = 8;
constexpr std::uint32_t kMoveResizeCancel = 11;
constexpr std::array<std::uint32_t, 8> kResizeEdges = {
    5,  // top-left
    1,  // top
    9,  // top-right
    8,  // right
    10, // bottom-right
    2,  // bottom
    6,  // bottom-left
    4,  // left
};

template <typename T>
struct FreeDeleter {
    void operator()(T* p) const { std::free(p); }
};
template <typename T>
using Reply = std::unique_ptr<T, FreeDeleter<T>>;

} // namespace

// ---------------------------------------------------------- XwaylandWindow --

// One X window. Managed windows are Toplevels on the desktop; override-redirect
// ones (menus, tooltips) are unmanaged surfaces at their own position.
class XwaylandWindow final : public Toplevel {
public:
    XwaylandWindow(Xwm& wm, xcb_window_t window, bool isOverrideRedirect, Rect geometry)
        : m_wm(wm), id(window), overrideRedirect(isOverrideRedirect), x11Geometry(geometry) {}

    ~XwaylandWindow() override
    {
        if (overrideRedirect && m_surface)
            m_wm.m_desktop.removeUnmanaged(m_surface);
    }

    // --- Toplevel ---
    Surface* surface() const override { return m_surface; }
    Rect geometry() const override { return {0, 0, x11Geometry.width, x11Geometry.height}; }
    std::string title() const override { return m_title; }
    std::string appId() const override { return m_appId; }
    Size minSize() const override { return m_minSize; }
    Size maxSize() const override { return m_maxSize; }
    bool maximized() const override { return m_maximized; }
    bool fullscreen() const override { return m_fullscreen; }

    void setActivated(bool on) override
    {
        if (on)
            m_wm.focusX(this);
    }

    void setSize(Size size) override
    {
        if (size.empty())
            return;
        const std::uint32_t values[] = {std::uint32_t(size.width), std::uint32_t(size.height)};
        xcb_configure_window(m_wm.m_conn, id, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
    }

    void setPosition(Point p) override
    {
        x11Geometry.x = p.x;
        x11Geometry.y = p.y;
        const std::uint32_t values[] = {std::uint32_t(p.x), std::uint32_t(p.y)};
        xcb_configure_window(m_wm.m_conn, id, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
    }

    void setMaximized(bool on) override
    {
        m_maximized = on;
        writeNetWmState();
    }
    void setFullscreen(bool on) override
    {
        m_fullscreen = on;
        writeNetWmState();
    }
    void setResizing(bool) override {}
    void setBounds(Size) override {}

    void close() override
    {
        if (m_deleteWindow)
            sendProtocolMessage(m_wm.m_atoms->values[Xwm::WM_DELETE_WINDOW]);
        else
            xcb_kill_client(m_wm.m_conn, id); // no polite way: disconnect it
    }

    void raised() override
    {
        const std::uint32_t above = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(m_wm.m_conn, id, XCB_CONFIG_WINDOW_STACK_MODE, &above);
    }

    // --- X11 side ---
    void sendProtocolMessage(xcb_atom_t protocol)
    {
        xcb_client_message_event_t msg{};
        msg.response_type = XCB_CLIENT_MESSAGE;
        msg.format = 32;
        msg.window = id;
        msg.type = m_wm.m_atoms->values[Xwm::WM_PROTOCOLS];
        msg.data.data32[0] = protocol;
        msg.data.data32[1] = XCB_CURRENT_TIME;
        xcb_send_event(m_wm.m_conn, false, id, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&msg));
    }

    void writeNetWmState()
    {
        const auto& a = m_wm.m_atoms->values;
        std::vector<xcb_atom_t> state;
        if (m_fullscreen)
            state.push_back(a[Xwm::NET_WM_STATE_FULLSCREEN]);
        if (m_maximized) {
            state.push_back(a[Xwm::NET_WM_STATE_MAXIMIZED_VERT]);
            state.push_back(a[Xwm::NET_WM_STATE_MAXIMIZED_HORZ]);
        }
        if (m_wm.m_focused == this)
            state.push_back(a[Xwm::NET_WM_STATE_FOCUSED]);
        xcb_change_property(m_wm.m_conn, XCB_PROP_MODE_REPLACE, id, a[Xwm::NET_WM_STATE], XCB_ATOM_ATOM, 32,
                            std::uint32_t(state.size()), state.data());
    }

    void setWmState(std::uint32_t state)
    {
        const std::uint32_t data[] = {state, XCB_WINDOW_NONE};
        const auto atom = m_wm.m_atoms->values[Xwm::WM_STATE];
        xcb_change_property(m_wm.m_conn, XCB_PROP_MODE_REPLACE, id, atom, atom, 32, 2, data);
    }

    void attach(Surface* surface)
    {
        if (surface == m_surface)
            return;
        detachSurface();
        m_surface = surface;
        if (!surface)
            return;
        m_surfaceCommitted = surface->committed.connect([this] {
            updateMapped();
            if (overrideRedirect)
                return;
            committed.emit();
        });
        m_surfaceGone = surface->aboutToBeDestroyed.connect([this] {
            const bool wasMapped = m_desktopMapped;
            if (overrideRedirect)
                m_wm.m_desktop.removeUnmanaged(m_surface);
            m_surface = nullptr;
            m_desktopMapped = false;
            if (wasMapped && !overrideRedirect)
                unmapped.emit();
        });
        updateMapped();
    }

    void detachSurface()
    {
        if (overrideRedirect && m_surface)
            m_wm.m_desktop.removeUnmanaged(m_surface);
        if (m_desktopMapped && !overrideRedirect) {
            m_desktopMapped = false;
            unmapped.emit();
        }
        m_surface = nullptr;
        m_surfaceCommitted.disconnect();
        m_surfaceGone.disconnect();
    }

    // On the desktop once X mapped it *and* its surface has content.
    void updateMapped()
    {
        const bool shouldShow = xMapped && m_surface && m_surface->hasContent();
        if (overrideRedirect) {
            if (shouldShow)
                m_wm.m_desktop.setUnmanaged(m_surface, x11Geometry.position());
            else if (m_surface)
                m_wm.m_desktop.removeUnmanaged(m_surface);
            m_desktopMapped = shouldShow;
            return;
        }
        if (shouldShow == m_desktopMapped)
            return;
        m_desktopMapped = shouldShow;
        if (shouldShow)
            mapped.emit();
        else
            unmapped.emit();
    }

    Xwm& m_wm;
    const xcb_window_t id;
    bool overrideRedirect;
    Rect x11Geometry; // root coordinates
    bool xMapped = false;
    std::uint64_t serial = 0;

    std::string m_title;
    std::string m_appId;
    Size m_minSize;
    Size m_maxSize;
    bool m_acceptsInput = true;
    bool m_deleteWindow = false;
    bool m_takeFocus = false;
    bool m_maximized = false;
    bool m_fullscreen = false;
    bool m_desktopMapped = false;

private:
    Surface* m_surface = nullptr;
    Connection m_surfaceCommitted;
    Connection m_surfaceGone;
};

// --------------------------------------------------------------------- Xwm --

Xwm::Xwm(Server& server, EventLoop& loop, Desktop& desktop, XwaylandShell& shell, int wmFd)
    : m_server(server), m_loop(loop), m_desktop(desktop), m_shell(shell), m_atoms(std::make_unique<Atoms>())
{
    m_conn = xcb_connect_to_fd(wmFd, nullptr); // takes ownership of the fd
    if (xcb_connection_has_error(m_conn)) {
        log::error("xwayland", "window manager cannot talk to Xwayland");
        return;
    }
    if (!setupRoot())
        return;

    m_source = loop.addFd(xcb_get_file_descriptor(m_conn), WL_EVENT_READABLE, [this](int, std::uint32_t) {
        dispatch();
    });
    m_flush = loop.beforeSleep.connect([this] { xcb_flush(m_conn); });
    m_associated = shell.associated.connect([this](Surface*, std::uint64_t serial) {
        for (auto& [id, w] : m_windows) {
            if (w->serial == serial)
                associate(*w, serial);
        }
    });
    // Focus moving to a Wayland window takes it away from X windows too.
    m_focusChanged = desktop.focusChanged.connect([this](Desktop::Window* window) {
        if (!window || !dynamic_cast<XwaylandWindow*>(window->toplevel))
            focusX(nullptr);
    });
    m_ok = true;
    xcb_flush(m_conn);
    log::info("xwayland", "window manager running");
}

Xwm::~Xwm()
{
    m_associated.disconnect();
    m_focusChanged.disconnect();
    // Tell the desktop the windows are gone before deleting them.
    for (auto& [id, w] : m_windows) {
        w->detachSurface();
        w->destroyed.emit();
    }
    m_windows.clear();
    m_source = {};
    m_flush.disconnect();
    if (m_conn)
        xcb_disconnect(m_conn);
}

bool Xwm::setupRoot()
{
    // Atoms: ask for all, then collect the replies.
    std::array<xcb_intern_atom_cookie_t, AtomCount> cookies;
    for (int i = 0; i < AtomCount; ++i)
        cookies[i] = xcb_intern_atom(m_conn, 0, std::uint16_t(std::strlen(kAtomNames[i])), kAtomNames[i]);
    for (int i = 0; i < AtomCount; ++i) {
        Reply<xcb_intern_atom_reply_t> r(xcb_intern_atom_reply(m_conn, cookies[i], nullptr));
        if (!r)
            return false;
        m_atoms->values[i] = r->atom;
    }
    const auto& a = m_atoms->values;

    const xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(m_conn)).data;
    m_root = screen->root;

    // Every top-level window gets its own buffer (and so its own wl_surface).
    Reply<xcb_composite_query_version_reply_t> v(
        xcb_composite_query_version_reply(m_conn, xcb_composite_query_version(m_conn, 0, 4), nullptr));
    xcb_composite_redirect_subwindows(m_conn, m_root, XCB_COMPOSITE_REDIRECT_MANUAL);

    // Be the window manager: get told about (and decide) maps and configures.
    const std::uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                               XCB_EVENT_MASK_PROPERTY_CHANGE;
    auto cookie = xcb_change_window_attributes_checked(m_conn, m_root, XCB_CW_EVENT_MASK, &mask);
    if (Reply<xcb_generic_error_t> err{xcb_request_check(m_conn, cookie)}) {
        log::error("xwayland", "another window manager owns the X display");
        return false;
    }

    // An EWMH-compliant WM announces itself through a check window.
    m_wmWindow = xcb_generate_id(m_conn);
    xcb_create_window(m_conn, XCB_COPY_FROM_PARENT, m_wmWindow, m_root, 0, 0, 10, 10, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_wmWindow, a[NET_SUPPORTING_WM_CHECK], XCB_ATOM_WINDOW, 32,
                        1, &m_wmWindow);
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, a[NET_SUPPORTING_WM_CHECK], XCB_ATOM_WINDOW, 32, 1,
                        &m_wmWindow);
    const char name[] = "DragonDE";
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_wmWindow, a[NET_WM_NAME], a[UTF8_STRING], 8,
                        sizeof(name) - 1, name);
    xcb_set_selection_owner(m_conn, m_wmWindow, a[WM_S0], XCB_CURRENT_TIME);
    xcb_set_selection_owner(m_conn, m_wmWindow, a[NET_WM_CM_S0], XCB_CURRENT_TIME);

    const xcb_atom_t supported[] = {a[NET_WM_STATE],          a[NET_WM_STATE_FULLSCREEN],
                                    a[NET_WM_STATE_MAXIMIZED_VERT], a[NET_WM_STATE_MAXIMIZED_HORZ],
                                    a[NET_WM_STATE_FOCUSED],  a[NET_ACTIVE_WINDOW],
                                    a[NET_WM_MOVERESIZE],     a[NET_SUPPORTING_WM_CHECK]};
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, a[NET_SUPPORTED], XCB_ATOM_ATOM, 32,
                        std::size(supported), supported);
    const xcb_window_t none = XCB_WINDOW_NONE;
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, a[NET_ACTIVE_WINDOW], XCB_ATOM_WINDOW, 32, 1, &none);
    return true;
}

void Xwm::dispatch()
{
    while (xcb_generic_event_t* event = xcb_poll_for_event(m_conn)) {
        handleEvent(event, event->response_type & 0x7f);
        std::free(event);
    }
    if (xcb_connection_has_error(m_conn) && m_ok) {
        log::warn("xwayland", "lost the connection to Xwayland");
        m_ok = false;
        m_source.setMask(0);
    }
    xcb_flush(m_conn);
}

void Xwm::handleEvent(const void* e, std::uint8_t type)
{
    switch (type) {
    case XCB_CREATE_NOTIFY: onCreate(e); break;
    case XCB_DESTROY_NOTIFY: onDestroy(e); break;
    case XCB_MAP_REQUEST: onMapRequest(e); break;
    case XCB_MAP_NOTIFY: onMap(e); break;
    case XCB_UNMAP_NOTIFY: onUnmap(e); break;
    case XCB_CONFIGURE_REQUEST: onConfigureRequest(e); break;
    case XCB_CONFIGURE_NOTIFY: onConfigure(e); break;
    case XCB_PROPERTY_NOTIFY: onProperty(e); break;
    case XCB_CLIENT_MESSAGE: onClientMessage(e); break;
    case 0: {
        const auto* err = static_cast<const xcb_generic_error_t*>(e);
        log::debug("xwayland", "X error {} (request {}, resource {:#x})", err->error_code, err->major_code,
                   err->resource_id);
        break;
    }
    default: break;
    }
}

XwaylandWindow* Xwm::find(std::uint32_t window) const
{
    auto it = m_windows.find(window);
    return it == m_windows.end() ? nullptr : it->second.get();
}

void Xwm::onCreate(const void* event)
{
    const auto* e = static_cast<const xcb_create_notify_event_t*>(event);
    if (e->window == m_wmWindow || e->parent != m_root || find(e->window))
        return;
    auto w = std::make_unique<XwaylandWindow>(*this, e->window, e->override_redirect,
                                              Rect{e->x, e->y, e->width, e->height});
    // Property changes on it (title, hints, ...) come to us.
    const std::uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_FOCUS_CHANGE;
    xcb_change_window_attributes(m_conn, e->window, XCB_CW_EVENT_MASK, &mask);
    XwaylandWindow* raw = w.get();
    m_windows.emplace(e->window, std::move(w));
    if (!raw->overrideRedirect)
        m_desktop.addToplevel(*raw);
}

void Xwm::onDestroy(const void* event)
{
    const auto* e = static_cast<const xcb_destroy_notify_event_t*>(event);
    auto it = m_windows.find(e->window);
    if (it == m_windows.end())
        return;
    XwaylandWindow* w = it->second.get();
    if (m_focused == w)
        m_focused = nullptr;
    w->detachSurface();
    if (!w->overrideRedirect)
        w->destroyed.emit(); // the desktop forgets it
    m_windows.erase(it);
}

void Xwm::onMapRequest(const void* event)
{
    const auto* e = static_cast<const xcb_map_request_event_t*>(event);
    XwaylandWindow* w = find(e->window);
    if (!w)
        return;
    readProperties(*w);
    w->setWmState(kNormalState);
    w->writeNetWmState();
    xcb_map_window(m_conn, e->window);
}

void Xwm::onMap(const void* event)
{
    const auto* e = static_cast<const xcb_map_notify_event_t*>(event);
    XwaylandWindow* w = find(e->window);
    if (!w)
        return;
    if (w->overrideRedirect != bool(e->override_redirect)) {
        // Rare: it changed kind while unmapped. Re-register as the right kind.
        w->detachSurface();
        if (!w->overrideRedirect)
            w->destroyed.emit();
        w->overrideRedirect = e->override_redirect;
        if (!w->overrideRedirect)
            m_desktop.addToplevel(*w);
        if (w->serial)
            associate(*w, w->serial);
    }
    if (w->overrideRedirect)
        readProperties(*w);
    w->xMapped = true;
    w->updateMapped();
}

void Xwm::onUnmap(const void* event)
{
    const auto* e = static_cast<const xcb_unmap_notify_event_t*>(event);
    XwaylandWindow* w = find(e->window);
    if (!w)
        return;
    w->xMapped = false;
    w->updateMapped();
    if (!w->overrideRedirect)
        w->setWmState(kWithdrawnState);
    if (m_focused == w)
        focusX(nullptr);
}

void Xwm::onConfigureRequest(const void* event)
{
    const auto* e = static_cast<const xcb_configure_request_event_t*>(event);
    XwaylandWindow* w = find(e->window);

    // Before it's on the desktop, and for windows that place themselves, it
    // gets what it asks for. Once managed, the desktop owns the position: only
    // size changes go through.
    std::uint16_t mask = e->value_mask;
    if (w && !w->overrideRedirect && w->m_desktopMapped)
        mask &= std::uint16_t(~(XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y));

    std::vector<std::uint32_t> values;
    if (mask & XCB_CONFIG_WINDOW_X)
        values.push_back(std::uint32_t(e->x));
    if (mask & XCB_CONFIG_WINDOW_Y)
        values.push_back(std::uint32_t(e->y));
    if (mask & XCB_CONFIG_WINDOW_WIDTH)
        values.push_back(e->width);
    if (mask & XCB_CONFIG_WINDOW_HEIGHT)
        values.push_back(e->height);
    if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
        values.push_back(0); // no borders: they'd be outside the surface
    if (mask & XCB_CONFIG_WINDOW_SIBLING)
        values.push_back(e->sibling);
    if (mask & XCB_CONFIG_WINDOW_STACK_MODE)
        values.push_back(e->stack_mode);
    xcb_configure_window(m_conn, e->window, mask, values.data());
}

void Xwm::onConfigure(const void* event)
{
    const auto* e = static_cast<const xcb_configure_notify_event_t*>(event);
    XwaylandWindow* w = find(e->window);
    if (!w)
        return;
    w->x11Geometry = {e->x, e->y, e->width, e->height};
    if (w->overrideRedirect)
        w->updateMapped(); // follows its own position
}

void Xwm::onProperty(const void* event)
{
    const auto* e = static_cast<const xcb_property_notify_event_t*>(event);
    if (XwaylandWindow* w = find(e->window))
        readProperty(*w, e->atom);
}

void Xwm::onClientMessage(const void* event)
{
    const auto* e = static_cast<const xcb_client_message_event_t*>(event);
    const auto& a = m_atoms->values;
    XwaylandWindow* w = find(e->window);
    if (!w)
        return;

    if (e->type == a[WL_SURFACE_SERIAL]) {
        associate(*w, std::uint64_t(e->data.data32[0]) | (std::uint64_t(e->data.data32[1]) << 32));
    } else if (e->type == a[NET_WM_STATE]) {
        // data32: action (0 remove, 1 add, 2 toggle), property 1, property 2
        const std::uint32_t action = e->data.data32[0];
        auto apply = [action](bool current) { return action == 2 ? !current : action == 1; };
        bool maximizeTouched = false, fullscreenTouched = false;
        for (int i = 1; i <= 2; ++i) {
            const xcb_atom_t prop = e->data.data32[i];
            if (prop == a[NET_WM_STATE_FULLSCREEN])
                fullscreenTouched = true;
            else if (prop == a[NET_WM_STATE_MAXIMIZED_VERT] || prop == a[NET_WM_STATE_MAXIMIZED_HORZ])
                maximizeTouched = true;
        }
        if (fullscreenTouched)
            w->fullscreenRequested.emit(apply(w->fullscreen()));
        if (maximizeTouched)
            w->maximizeRequested.emit(apply(w->maximized()));
    } else if (e->type == a[NET_ACTIVE_WINDOW]) {
        // Honored when an X window already has focus (the app moving it between its
        // own windows) or a pager asks (source 2); otherwise it's focus stealing.
        const std::uint32_t source = e->data.data32[0];
        if (Desktop::Window* dw = m_desktop.windowFor(w); dw && (source == 2 || m_focused))
            m_desktop.focus(dw);
    } else if (e->type == a[NET_WM_MOVERESIZE]) {
        const std::uint32_t direction = e->data.data32[2];
        if (direction == kMoveResizeMove)
            w->moveRequested.emit(std::nullopt);
        else if (direction < kResizeEdges.size())
            w->resizeRequested.emit(std::nullopt, kResizeEdges[direction]);
        else if (direction != kMoveResizeCancel)
            log::debug("xwayland", "unsupported _NET_WM_MOVERESIZE direction {}", direction);
    }
}

void Xwm::associate(XwaylandWindow& window, std::uint64_t serial)
{
    window.serial = serial;
    if (Surface* surface = m_shell.surfaceForSerial(serial))
        window.attach(surface);
}

void Xwm::focusX(XwaylandWindow* window)
{
    const auto& a = m_atoms->values;
    XwaylandWindow* previous = std::exchange(m_focused, window);
    if (window) {
        if (window->m_acceptsInput)
            xcb_set_input_focus(m_conn, XCB_INPUT_FOCUS_POINTER_ROOT, window->id, XCB_CURRENT_TIME);
        if (window->m_takeFocus)
            window->sendProtocolMessage(a[WM_TAKE_FOCUS]);
        window->writeNetWmState();
    } else {
        xcb_set_input_focus(m_conn, XCB_INPUT_FOCUS_POINTER_ROOT, XCB_WINDOW_NONE, XCB_CURRENT_TIME);
    }
    if (previous && previous != window)
        previous->writeNetWmState(); // still alive: onDestroy clears m_focused first
    const xcb_window_t active = window ? window->id : xcb_window_t(XCB_WINDOW_NONE);
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, a[NET_ACTIVE_WINDOW], XCB_ATOM_WINDOW, 32, 1, &active);
}

void Xwm::readProperties(XwaylandWindow& w)
{
    for (xcb_atom_t atom : {xcb_atom_t(XCB_ATOM_WM_NAME), m_atoms->values[NET_WM_NAME], xcb_atom_t(XCB_ATOM_WM_CLASS),
                            xcb_atom_t(XCB_ATOM_WM_HINTS), xcb_atom_t(XCB_ATOM_WM_NORMAL_HINTS),
                            m_atoms->values[WM_PROTOCOLS], m_atoms->values[NET_WM_STATE]})
        readProperty(w, atom);
}

void Xwm::readProperty(XwaylandWindow& w, std::uint32_t atom)
{
    const auto& a = m_atoms->values;
    if (atom == XCB_ATOM_WM_NAME || atom == a[NET_WM_NAME]) {
        // Prefer the UTF-8 _NET_WM_NAME; fall back to the legacy WM_NAME.
        for (xcb_atom_t name : {a[NET_WM_NAME], xcb_atom_t(XCB_ATOM_WM_NAME)}) {
            Reply<xcb_get_property_reply_t> r(xcb_get_property_reply(
                m_conn, xcb_get_property(m_conn, 0, w.id, name, XCB_GET_PROPERTY_TYPE_ANY, 0, 2048), nullptr));
            if (r && r->value_len > 0) {
                w.m_title.assign(static_cast<const char*>(xcb_get_property_value(r.get())),
                                 std::size_t(xcb_get_property_value_length(r.get())));
                return;
            }
        }
    } else if (atom == XCB_ATOM_WM_CLASS) {
        xcb_icccm_get_wm_class_reply_t cls;
        if (xcb_icccm_get_wm_class_reply(m_conn, xcb_icccm_get_wm_class(m_conn, w.id), &cls, nullptr)) {
            w.m_appId = cls.class_name ? cls.class_name : "";
            xcb_icccm_get_wm_class_reply_wipe(&cls);
        }
    } else if (atom == XCB_ATOM_WM_HINTS) {
        xcb_icccm_wm_hints_t hints;
        if (xcb_icccm_get_wm_hints_reply(m_conn, xcb_icccm_get_wm_hints(m_conn, w.id), &hints, nullptr))
            w.m_acceptsInput = !(hints.flags & XCB_ICCCM_WM_HINT_INPUT) || hints.input;
    } else if (atom == XCB_ATOM_WM_NORMAL_HINTS) {
        xcb_size_hints_t hints;
        if (xcb_icccm_get_wm_normal_hints_reply(m_conn, xcb_icccm_get_wm_normal_hints(m_conn, w.id), &hints,
                                                nullptr)) {
            if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE)
                w.m_minSize = {hints.min_width, hints.min_height};
            if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE)
                w.m_maxSize = {hints.max_width, hints.max_height};
        }
    } else if (atom == a[WM_PROTOCOLS]) {
        xcb_icccm_get_wm_protocols_reply_t protocols;
        if (xcb_icccm_get_wm_protocols_reply(m_conn, xcb_icccm_get_wm_protocols(m_conn, w.id, a[WM_PROTOCOLS]),
                                             &protocols, nullptr)) {
            const auto* begin = protocols.atoms;
            const auto* end = protocols.atoms + protocols.atoms_len;
            w.m_deleteWindow = std::find(begin, end, a[WM_DELETE_WINDOW]) != end;
            w.m_takeFocus = std::find(begin, end, a[WM_TAKE_FOCUS]) != end;
            xcb_icccm_get_wm_protocols_reply_wipe(&protocols);
        }
    } else if (atom == a[NET_WM_STATE]) {
        // Initial state set before mapping (e.g. games starting fullscreen).
        Reply<xcb_get_property_reply_t> r(xcb_get_property_reply(
            m_conn, xcb_get_property(m_conn, 0, w.id, a[NET_WM_STATE], XCB_ATOM_ATOM, 0, 32), nullptr));
        if (!r || w.m_desktopMapped)
            return;
        const auto* atoms = static_cast<const xcb_atom_t*>(xcb_get_property_value(r.get()));
        const int count = xcb_get_property_value_length(r.get()) / int(sizeof(xcb_atom_t));
        for (int i = 0; i < count; ++i) {
            if (atoms[i] == a[NET_WM_STATE_FULLSCREEN] && !w.m_fullscreen)
                w.fullscreenRequested.emit(true);
            else if ((atoms[i] == a[NET_WM_STATE_MAXIMIZED_VERT] || atoms[i] == a[NET_WM_STATE_MAXIMIZED_HORZ]) &&
                     !w.m_maximized)
                w.maximizeRequested.emit(true);
        }
    }
}

} // namespace dragon
