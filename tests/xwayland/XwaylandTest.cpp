// X11 end to end: a real Xwayland started by XwaylandServer, our Xwm managing
// it, and the test as an X11 client (on its own thread, so blocking xcb calls
// can't stall the compositor loop running on the test thread).

#include "../server/ServerFixture.h"

#include "desktop/Desktop.h"
#include "desktop/Toplevel.h"
#include "xwayland/XwaylandServer.h"
#include "xwayland/XwaylandShell.h"
#include "xwayland/Xwm.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>
#include <xcb/xcb.h>

using namespace dragon;
using namespace std::chrono_literals;

namespace {

class XwaylandTest : public test::ServerFixture {
protected:
    std::unique_ptr<Desktop> desktop;
    std::unique_ptr<XwaylandShell> shell;
    std::unique_ptr<XwaylandServer> xwayland;
    std::unique_ptr<Xwm> xwm;
    std::vector<Connection> xconnections;
    std::thread xclient;
    std::atomic<bool> stopClient = false;

    void SetUp() override
    {
        ServerFixture::SetUp();
        desktop = std::make_unique<Desktop>(*server);
        desktop->setArea({0, 0, 800, 600});
        shell = std::make_unique<XwaylandShell>(*server);
        xwayland = std::make_unique<XwaylandServer>(*server, *loop);
        xconnections.push_back(xwayland->started.connect([this](wl_client* c) { shell->setXwaylandClient(c); }));
        xconnections.push_back(xwayland->ready.connect(
            [this](int fd) { xwm = std::make_unique<Xwm>(*server, *loop, *desktop, *shell, fd); }));
        xconnections.push_back(xwayland->stopped.connect([this] { xwm.reset(); }));
        ASSERT_TRUE(xwayland->listen());
        xwayland->start();
        ASSERT_TRUE(pumpUntil([this] { return xwm && xwm->ok(); }, 10s)) << "Xwayland didn't come up";
    }

    void TearDown() override
    {
        stopClient = true;
        if (xclient.joinable()) {
            while (xclient.joinable()) { // keep serving Xwayland until the client thread is done
                loop->dispatch(10);
                if (clientDone)
                    xclient.join();
            }
        }
        xwm.reset();
        xwayland.reset();
        xconnections.clear();
        if (client)
            disconnect();
        desktop.reset();
        shell.reset();
        ServerFixture::TearDown();
    }

    bool pumpUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout = 5s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done()) {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            loop->dispatch(10);
        }
        return true;
    }

    // Runs `body` as an X11 client on its own thread.
    void runXClient(std::function<void(xcb_connection_t*)> body)
    {
        xclient = std::thread([this, body = std::move(body)] {
            xcb_connection_t* c = xcb_connect(xwayland->displayName().c_str(), nullptr);
            if (!xcb_connection_has_error(c))
                body(c);
            xcb_disconnect(c);
            clientDone = true;
        });
    }

    Desktop::Window* windowTitled(const std::string& title) const
    {
        for (const auto& w : desktop->windows()) {
            if (w->toplevel->title() == title)
                return w.get();
        }
        return nullptr;
    }

    std::atomic<bool> clientDone = false;
};

xcb_atom_t atom(xcb_connection_t* c, const char* name)
{
    auto* r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, std::uint16_t(std::strlen(name)), name), nullptr);
    const xcb_atom_t a = r ? r->atom : xcb_atom_t(XCB_ATOM_NONE);
    std::free(r);
    return a;
}

// A plain top-level window with a title, class and WM_DELETE_WINDOW support.
xcb_window_t createXWindow(xcb_connection_t* c, const char* title, std::int16_t x, std::int16_t y, std::uint16_t w,
                        std::uint16_t h, bool overrideRedirect = false)
{
    const xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const xcb_window_t win = xcb_generate_id(c);
    const std::uint32_t values[] = {screen->white_pixel, overrideRedirect ? 1u : 0u,
                                    XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, win, screen->root, x, y, w, h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                        std::uint32_t(std::strlen(title)), title);
    const char cls[] = "xtest\0XTest";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(cls), cls);
    const xcb_atom_t del = atom(c, "WM_DELETE_WINDOW");
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, atom(c, "WM_PROTOCOLS"), XCB_ATOM_ATOM, 32, 1, &del);
    xcb_map_window(c, win);
    xcb_flush(c);
    return win;
}

// Waits (on the client thread) for an event of `type`, or until stopped.
bool waitFor(xcb_connection_t* c, std::uint8_t type, const std::atomic<bool>& stop,
             const std::function<bool(xcb_generic_event_t*)>& match = {})
{
    while (!stop) {
        if (xcb_generic_event_t* e = xcb_poll_for_event(c)) {
            const bool hit = (e->response_type & 0x7f) == type && (!match || match(e));
            std::free(e);
            if (hit)
                return true;
        } else {
            if (xcb_connection_has_error(c))
                return false;
            std::this_thread::sleep_for(2ms);
        }
    }
    return false;
}

} // namespace

TEST_F(XwaylandTest, X11WindowBecomesADesktopWindow)
{
    runXClient([this](xcb_connection_t* c) {
        createXWindow(c, "x11-test", 10, 10, 200, 150);
        waitFor(c, XCB_DESTROY_NOTIFY, stopClient);
    });

    ASSERT_TRUE(pumpUntil([this] { return windowTitled("x11-test") != nullptr; })) << "never mapped";
    Desktop::Window* w = windowTitled("x11-test");
    EXPECT_EQ(w->toplevel->appId(), "XTest");
    EXPECT_EQ(desktop->geometryOf(*w), (Rect{300, 225, 200, 150})) << "placed by us, centered";
    EXPECT_EQ(desktop->focusedWindow(), w) << "new windows get focus, X11 ones too";
}

TEST_F(XwaylandTest, CloseUsesWmDeleteWindow)
{
    std::atomic<bool> deleteReceived = false;
    runXClient([this, &deleteReceived](xcb_connection_t* c) {
        const xcb_window_t win = createXWindow(c, "closable", 10, 10, 100, 100);
        const xcb_atom_t protocols = atom(c, "WM_PROTOCOLS");
        if (waitFor(c, XCB_CLIENT_MESSAGE, stopClient, [protocols](xcb_generic_event_t* e) {
                return reinterpret_cast<xcb_client_message_event_t*>(e)->type == protocols;
            })) {
            deleteReceived = true;
            xcb_destroy_window(c, win); // a polite client closes when asked
            xcb_flush(c);
        }
        waitFor(c, XCB_DESTROY_NOTIFY, stopClient);
    });

    ASSERT_TRUE(pumpUntil([this] { return windowTitled("closable") != nullptr; }));
    windowTitled("closable")->toplevel->close();

    ASSERT_TRUE(pumpUntil([&] { return deleteReceived.load(); }));
    EXPECT_TRUE(pumpUntil([this] { return windowTitled("closable") == nullptr; })) << "gone from the desktop";
}

TEST_F(XwaylandTest, OverrideRedirectWindowsAreUnmanaged)
{
    runXClient([this](xcb_connection_t* c) {
        createXWindow(c, "tooltip", 50, 60, 80, 20, /*overrideRedirect=*/true);
        waitFor(c, XCB_DESTROY_NOTIFY, stopClient);
    });

    // Not a window (no focus, no placement), but drawn where it asked to be.
    ASSERT_TRUE(pumpUntil([this] {
        for (const auto& item : desktop->renderList()) {
            if (item.origin == Point{50, 60})
                return true;
        }
        return false;
    })) << "never showed up at (50, 60)";
    EXPECT_TRUE(desktop->windows().empty());
}

TEST_F(XwaylandTest, NetWmStateRequestsFullscreen)
{
    runXClient([this](xcb_connection_t* c) {
        const xcb_window_t win = createXWindow(c, "game", 0, 0, 320, 240);
        // Wait until it's mapped, then ask the WM for fullscreen (EWMH).
        waitFor(c, XCB_MAP_NOTIFY, stopClient);
        const xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
        xcb_client_message_event_t msg{};
        msg.response_type = XCB_CLIENT_MESSAGE;
        msg.format = 32;
        msg.window = win;
        msg.type = atom(c, "_NET_WM_STATE");
        msg.data.data32[0] = 1; // add
        msg.data.data32[1] = atom(c, "_NET_WM_STATE_FULLSCREEN");
        xcb_send_event(c, false, screen->root,
                       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char*>(&msg));
        xcb_flush(c);
        waitFor(c, XCB_DESTROY_NOTIFY, stopClient);
    });

    ASSERT_TRUE(pumpUntil([this] { return windowTitled("game") != nullptr; }));
    ASSERT_TRUE(pumpUntil([this] {
        auto* w = windowTitled("game");
        return w && w->toplevel->fullscreen() && desktop->geometryOf(*w) == Rect{0, 0, 800, 600};
    })) << "should fill the 800x600 area";
}

TEST_F(XwaylandTest, X11WindowsGetOurTitleBarUnlessTheyOptOut)
{
    WindowStyle style;
    style.titlebar.height = 30;
    style.border.width = 0;
    desktop->setStyleResolver([style](const Toplevel&) { return style; });

    runXClient([this](xcb_connection_t* c) {
        createXWindow(c, "framed", 10, 10, 200, 150);
        const xcb_window_t bare = createXWindow(c, "frameless", 10, 10, 200, 150);
        // _MOTIF_WM_HINTS: flags = decorations, decorations = none (like Chrome or Steam).
        const std::uint32_t hints[5] = {2, 0, 0, 0, 0};
        const xcb_atom_t motif = atom(c, "_MOTIF_WM_HINTS");
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, bare, motif, motif, 32, 5, hints);
        xcb_flush(c);
        waitFor(c, XCB_DESTROY_NOTIFY, stopClient);
    });

    ASSERT_TRUE(pumpUntil([this] {
        auto* framed = windowTitled("framed");
        auto* bare = windowTitled("frameless");
        return framed && bare && framed->titlebar && !bare->titlebar;
    })) << "the window with Motif 'no decorations' keeps none";
    // The title bar sits above the X window, which stays where X thinks it is.
    Desktop::Window* framed = windowTitled("framed");
    EXPECT_EQ(desktop->frameOf(*framed).titlebar.y + 30, desktop->geometryOf(*framed).y);
}
