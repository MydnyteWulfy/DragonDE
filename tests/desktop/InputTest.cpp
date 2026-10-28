// Input end to end: raw backend events -> InputRouter -> Desktop focus policy
// -> Seat -> a real client's wl_pointer / wl_keyboard / wl_data_device.

#include "../server/ServerFixture.h"

#include "desktop/Desktop.h"
#include "desktop/InputRouter.h"
#include "protocols/Seat.h"
#include "protocols/XdgShell.h"

#include "protocols/XdgActivation.h"

#include "cursor-shape-v1-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <string>

using namespace dragon;

namespace {

class TestBackend final : public Backend {
public:
    const char* name() const override { return "test"; }
    bool start(Gpu&) override { return true; }
};

// Everything the client's seat objects received, in order.
struct ClientInput {
    std::vector<std::string> log;
    wl_surface* pointerSurface = nullptr;
    wl_surface* keyboardSurface = nullptr;
    PointF pointer;
    std::uint32_t lastButtonSerial = 0;
    std::uint32_t lastKeySerial = 0;
    std::uint32_t enterSerial = 0;
    std::uint32_t modsDepressed = 0;
    std::string keymap;
    std::int32_t repeatRate = 0, repeatDelay = 0;

    void add(std::string event) { log.push_back(std::move(event)); }
    bool saw(const std::string& event) const { return std::ranges::find(log, event) != log.end(); }
};

class InputTest : public test::ServerFixture {
protected:
    TestBackend backend;
    std::unique_ptr<Desktop> desktop;
    std::unique_ptr<InputRouter> router;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    wl_data_device_manager* ddm = nullptr;
    ClientInput in;
    int quits = 0;
    std::vector<std::string> spawned;

    void SetUp() override
    {
        ServerFixture::SetUp();
        desktop = std::make_unique<Desktop>(*server);
        desktop->setArea({0, 0, 800, 600});
        router = std::make_unique<InputRouter>(
            backend, *desktop, server->seat(),
            WindowManagerConfig{.modifier = "Mod1", .modifierLabel = "Alt", .terminal = "test-terminal"},
            WindowManagerActions{.quit = [this] { ++quits; },
                                 .spawn = [this](const std::string& cmd) { spawned.push_back(cmd); }});

        static const wl_registry_listener listener = {
            .global =
                [](void* d, wl_registry* reg, std::uint32_t name, const char* iface, std::uint32_t) {
                    auto* self = static_cast<InputTest*>(d);
                    const std::string_view i(iface);
                    if (i == "wl_seat")
                        self->seat = self->track(static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, 8)));
                    else if (i == "wl_data_device_manager")
                        self->ddm = self->track(static_cast<wl_data_device_manager*>(
                            wl_registry_bind(reg, name, &wl_data_device_manager_interface, 3)));
                },
            .global_remove = [](void*, wl_registry*, std::uint32_t) {},
        };
        auto* registry = wl_display_get_registry(client);
        wl_registry_add_listener(registry, &listener, this);
        ASSERT_TRUE(roundtrip());
        wl_registry_destroy(registry);
        ASSERT_TRUE(seat && ddm);

        pointer = track(wl_seat_get_pointer(seat));
        wl_pointer_add_listener(pointer, &kPointerListener, &in);
        keyboard = track(wl_seat_get_keyboard(seat));
        wl_keyboard_add_listener(keyboard, &kKeyboardListener, &in);
        ASSERT_TRUE(roundtrip());
    }

    void TearDown() override
    {
        if (client)
            disconnect();
        router.reset();
        desktop.reset();
        ServerFixture::TearDown();
    }

    // Maps a w x h window; the desktop centers it in 800x600.
    std::unique_ptr<test::ClientWindow> window(std::int32_t w, std::int32_t h)
    {
        auto win = makeWindow();
        EXPECT_TRUE(roundtrip());
        mapWindow(*win, w, h);
        EXPECT_TRUE(roundtrip());
        return win;
    }

    void move(double x, double y)
    {
        backend.input.pointerMotionAbsolute.emit(nullptr, x, y, 100);
        backend.input.pointerFrame.emit();
        ASSERT_TRUE(roundtrip());
    }
    void click(std::uint32_t button, bool pressed)
    {
        backend.input.pointerButton.emit(button, pressed, 200);
        backend.input.pointerFrame.emit();
        ASSERT_TRUE(roundtrip());
    }
    void key(std::uint32_t code, bool pressed)
    {
        backend.input.key.emit(code, pressed, 300);
        ASSERT_TRUE(roundtrip());
    }

    static constexpr wl_pointer_listener kPointerListener = {
        .enter =
            [](void* d, wl_pointer*, std::uint32_t serial, wl_surface* s, wl_fixed_t x, wl_fixed_t y) {
                auto* in = static_cast<ClientInput*>(d);
                in->enterSerial = serial;
                in->pointerSurface = s;
                in->pointer = {wl_fixed_to_double(x), wl_fixed_to_double(y)};
                in->add("enter");
            },
        .leave =
            [](void* d, wl_pointer*, std::uint32_t, wl_surface*) {
                auto* in = static_cast<ClientInput*>(d);
                in->pointerSurface = nullptr;
                in->add("leave");
            },
        .motion =
            [](void* d, wl_pointer*, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
                auto* in = static_cast<ClientInput*>(d);
                in->pointer = {wl_fixed_to_double(x), wl_fixed_to_double(y)};
                in->add("motion");
            },
        .button =
            [](void* d, wl_pointer*, std::uint32_t serial, std::uint32_t, std::uint32_t button, std::uint32_t state) {
                auto* in = static_cast<ClientInput*>(d);
                in->lastButtonSerial = serial;
                in->add(std::format("button {} {}", button, state));
            },
        .axis = [](void* d, wl_pointer*, std::uint32_t, std::uint32_t axis,
                   wl_fixed_t v) { static_cast<ClientInput*>(d)->add(std::format("axis {} {}", axis, wl_fixed_to_double(v))); },
        .frame = [](void* d, wl_pointer*) { static_cast<ClientInput*>(d)->add("frame"); },
        .axis_source = [](void* d, wl_pointer*, std::uint32_t s) { static_cast<ClientInput*>(d)->add(std::format("source {}", s)); },
        .axis_stop = [](void* d, wl_pointer*, std::uint32_t, std::uint32_t a) { static_cast<ClientInput*>(d)->add(std::format("stop {}", a)); },
        .axis_discrete = [](void*, wl_pointer*, std::uint32_t, std::int32_t) {},
        .axis_value120 = [](void* d, wl_pointer*, std::uint32_t a, std::int32_t v) {
            static_cast<ClientInput*>(d)->add(std::format("value120 {} {}", a, v));
        },
        .axis_relative_direction = [](void*, wl_pointer*, std::uint32_t, std::uint32_t) {},
    };

    static constexpr wl_keyboard_listener kKeyboardListener = {
        .keymap =
            [](void* d, wl_keyboard*, std::uint32_t format, std::int32_t fd, std::uint32_t size) {
                auto* in = static_cast<ClientInput*>(d);
                if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
                    void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                    if (map != MAP_FAILED) {
                        in->keymap.assign(static_cast<const char*>(map), size);
                        munmap(map, size);
                    }
                }
                close(fd);
            },
        .enter =
            [](void* d, wl_keyboard*, std::uint32_t, wl_surface* s, wl_array*) {
                auto* in = static_cast<ClientInput*>(d);
                in->keyboardSurface = s;
                in->add("kb-enter");
            },
        .leave =
            [](void* d, wl_keyboard*, std::uint32_t, wl_surface*) {
                auto* in = static_cast<ClientInput*>(d);
                in->keyboardSurface = nullptr;
                in->add("kb-leave");
            },
        .key = [](void* d, wl_keyboard*, std::uint32_t serial, std::uint32_t, std::uint32_t key, std::uint32_t state) {
            static_cast<ClientInput*>(d)->lastKeySerial = serial;
            static_cast<ClientInput*>(d)->add(std::format("key {} {}", key, state));
        },
        .modifiers = [](void* d, wl_keyboard*, std::uint32_t, std::uint32_t depressed, std::uint32_t, std::uint32_t,
                        std::uint32_t) { static_cast<ClientInput*>(d)->modsDepressed = depressed; },
        .repeat_info = [](void* d, wl_keyboard*, std::int32_t rate, std::int32_t delay) {
            auto* in = static_cast<ClientInput*>(d);
            in->repeatRate = rate;
            in->repeatDelay = delay;
        },
    };
};

} // namespace

TEST_F(InputTest, KeyboardGetsKeymapAndRepeatInfo)
{
    EXPECT_TRUE(in.keymap.starts_with("xkb_keymap")) << in.keymap.substr(0, 40);
    EXPECT_EQ(in.repeatRate, 25);
    EXPECT_EQ(in.repeatDelay, 600);
}

TEST_F(InputTest, NewWindowGetsFocusAndActivatedState)
{
    auto w = window(100, 100);
    EXPECT_EQ(in.keyboardSurface, w->surface);
    ASSERT_TRUE(roundtrip());
    EXPECT_TRUE(w->hasState(XDG_TOPLEVEL_STATE_ACTIVATED));
}

TEST_F(InputTest, KeysAndModifiersReachTheFocusedWindow)
{
    auto w = window(100, 100);
    key(KEY_LEFTSHIFT, true);
    key(KEY_A, true);
    key(KEY_A, false);

    EXPECT_TRUE(in.saw(std::format("key {} 1", KEY_A)));
    EXPECT_TRUE(in.saw(std::format("key {} 0", KEY_A)));
    EXPECT_NE(in.modsDepressed & 1u, 0u) << "Shift is the first modifier";
    EXPECT_TRUE(server->seat().modifierActive("Shift"));

    key(KEY_LEFTSHIFT, false);
    EXPECT_EQ(in.modsDepressed, 0u);
}

TEST_F(InputTest, PointerEntersMovesAndLeaves)
{
    auto w = window(100, 100); // at (350, 250)
    move(360, 270);
    EXPECT_EQ(in.pointerSurface, w->surface);
    EXPECT_EQ(in.pointer, (PointF{10, 20}));

    move(370, 280);
    EXPECT_EQ(in.pointer, (PointF{20, 30}));
    EXPECT_TRUE(in.saw("motion"));

    move(10, 10);
    EXPECT_EQ(in.pointerSurface, nullptr);
    EXPECT_TRUE(in.saw("leave"));
}

TEST_F(InputTest, ClickFocusesAndRaises)
{
    auto a = window(200, 200); // at (300, 200)
    auto b = window(200, 200); // cascades to (332, 232), focused
    ASSERT_EQ(in.keyboardSurface, b->surface);

    move(310, 210); // only a is here
    click(BTN_LEFT, true);
    click(BTN_LEFT, false);

    EXPECT_EQ(in.keyboardSurface, a->surface);
    EXPECT_EQ(desktop->windows().back()->toplevel, toplevels[0]) << "a is on top now";
    ASSERT_TRUE(roundtrip());
    EXPECT_TRUE(a->hasState(XDG_TOPLEVEL_STATE_ACTIVATED));
    EXPECT_FALSE(b->hasState(XDG_TOPLEVEL_STATE_ACTIVATED));
}

TEST_F(InputTest, HeldButtonKeepsPointerOnTheWindow)
{
    auto w = window(100, 100); // at (350, 250)
    move(360, 260);
    click(BTN_LEFT, true);

    move(10, 10); // way outside, button still down
    EXPECT_EQ(in.pointerSurface, w->surface) << "implicit grab: no leave while pressed";
    EXPECT_EQ(in.pointer, (PointF{-340, -240}));

    click(BTN_LEFT, false);
    EXPECT_EQ(in.pointerSurface, nullptr) << "released outside: now it leaves";
}

TEST_F(InputTest, EmptyInputRegionLetsClicksThrough)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    auto* region = wl_compositor_create_region(compositor);
    wl_surface_set_input_region(w->surface, region); // empty: no input anywhere
    wl_region_destroy(region);
    mapWindow(*w, 100, 100);
    ASSERT_TRUE(roundtrip());

    move(400, 300);
    EXPECT_EQ(in.pointerSurface, nullptr);
}

TEST_F(InputTest, WheelScrollIsForwardedWithValue120)
{
    auto w = window(100, 100);
    move(400, 300);
    in.log.clear();

    backend.input.pointerAxis.emit(PointerAxisEvent{
        .timeMs = 5, .horizontal = false, .value = 15.0, .value120 = 120, .source = PointerAxisEvent::Source::Wheel});
    backend.input.pointerFrame.emit();
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(in.log, (std::vector<std::string>{"source 0", "value120 0 120", "axis 0 15", "frame"}));
}

TEST_F(InputTest, MoveRequestQuotesTheButtonSerial)
{
    auto w = window(100, 100);
    std::uint32_t requested = 0;
    auto c = toplevels[0]->moveRequested.connect([&](std::uint32_t serial) { requested = serial; });
    move(400, 300);
    click(BTN_LEFT, true);

    xdg_toplevel_move(w->toplevel, seat, in.lastButtonSerial);
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(requested, server->seat().lastButtonSerial());
    click(BTN_LEFT, false);
}

TEST_F(InputTest, ClipboardCopyAndPaste)
{
    auto w = window(100, 100);
    auto* device = track(wl_data_device_manager_get_data_device(ddm, seat));

    struct Clipboard {
        wl_data_offer* offer = nullptr;
        std::vector<std::string> offeredTypes;
        std::string sendRequestType;
        int sendFd = -1;
    } cb;
    static const wl_data_offer_listener offerListener = {
        .offer = [](void* d, wl_data_offer*, const char* type) { static_cast<Clipboard*>(d)->offeredTypes.emplace_back(type); },
        .source_actions = [](void*, wl_data_offer*, std::uint32_t) {},
        .action = [](void*, wl_data_offer*, std::uint32_t) {},
    };
    static const wl_data_device_listener deviceListener = {
        .data_offer = [](void* d, wl_data_device*, wl_data_offer* offer) {
            wl_data_offer_add_listener(offer, &offerListener, d);
        },
        .enter = [](void*, wl_data_device*, std::uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer*) {},
        .leave = [](void*, wl_data_device*) {},
        .motion = [](void*, wl_data_device*, std::uint32_t, wl_fixed_t, wl_fixed_t) {},
        .drop = [](void*, wl_data_device*) {},
        .selection = [](void* d, wl_data_device*, wl_data_offer* offer) {
            auto* c = static_cast<Clipboard*>(d);
            if (c->offer && c->offer != offer)
                wl_data_offer_destroy(c->offer);
            c->offer = offer;
        },
    };
    static const wl_data_source_listener sourceListener = {
        .target = [](void*, wl_data_source*, const char*) {},
        .send = [](void* d, wl_data_source*, const char* type, std::int32_t fd) {
            auto* c = static_cast<Clipboard*>(d);
            c->sendRequestType = type;
            c->sendFd = fd;
        },
        .cancelled = [](void*, wl_data_source*) {},
        .dnd_drop_performed = [](void*, wl_data_source*) {},
        .dnd_finished = [](void*, wl_data_source*) {},
        .action = [](void*, wl_data_source*, std::uint32_t) {},
    };
    wl_data_device_add_listener(device, &deviceListener, &cb);
    ASSERT_TRUE(roundtrip());

    // Copy: offer text and take the selection.
    auto* source = track(wl_data_device_manager_create_data_source(ddm));
    wl_data_source_add_listener(source, &sourceListener, &cb);
    wl_data_source_offer(source, "text/plain;charset=utf-8");
    wl_data_device_set_selection(device, source, in.lastButtonSerial);
    ASSERT_TRUE(roundtrip());
    ASSERT_NE(cb.offer, nullptr);
    track(cb.offer);
    EXPECT_EQ(cb.offeredTypes, (std::vector<std::string>{"text/plain;charset=utf-8"}));

    // Paste: ask for the data through a pipe; the source writes into it.
    int fds[2];
    ASSERT_EQ(pipe2(fds, O_CLOEXEC), 0);
    wl_data_offer_receive(cb.offer, "text/plain;charset=utf-8", fds[1]);
    close(fds[1]);
    ASSERT_TRUE(roundtrip());
    ASSERT_GE(cb.sendFd, 0);
    EXPECT_EQ(cb.sendRequestType, "text/plain;charset=utf-8");
    ASSERT_EQ(write(cb.sendFd, "hello", 5), 5);
    close(cb.sendFd);

    char buf[16] = {};
    EXPECT_EQ(read(fds[0], buf, sizeof(buf)), 5);
    EXPECT_STREQ(buf, "hello");
    close(fds[0]);
}

// ------------------------------------------------------- window management --

TEST_F(InputTest, AltDragMovesWindowAndClientNeverSeesTheClick)
{
    auto w = window(100, 100); // at (350, 250)
    move(360, 260);
    in.log.clear();

    key(KEY_LEFTALT, true);
    click(BTN_LEFT, true);
    move(410, 290);
    click(BTN_LEFT, false);
    key(KEY_LEFTALT, false);

    EXPECT_EQ(desktop->windows()[0]->position, (Point{400, 280}));
    for (const auto& e : in.log)
        EXPECT_FALSE(e.starts_with("button")) << "the grab must not leak the click: " << e;
    EXPECT_EQ(in.pointerSurface, w->surface) << "pointer re-enters after the grab";
}

TEST_F(InputTest, TitleBarDragThroughMoveRequest)
{
    auto w = window(100, 100);
    move(360, 260);
    click(BTN_LEFT, true); // the client sees this press on its "title bar"...
    xdg_toplevel_move(w->toplevel, seat, in.lastButtonSerial); // ...and asks us to move it
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(in.pointerSurface, nullptr) << "pointer leaves the client during the move";

    move(460, 360);
    click(BTN_LEFT, false);
    EXPECT_EQ(desktop->windows()[0]->position, (Point{450, 350}));
}

TEST_F(InputTest, MoveRequestWithStaleSerialIsIgnored)
{
    auto w = window(100, 100);
    move(360, 260);
    click(BTN_LEFT, true);
    const std::uint32_t serial = in.lastButtonSerial;
    click(BTN_LEFT, false);

    xdg_toplevel_move(w->toplevel, seat, serial); // button already released
    ASSERT_TRUE(roundtrip());
    move(460, 360);

    EXPECT_EQ(desktop->windows()[0]->position, (Point{350, 250}));
}

TEST_F(InputTest, ResizeFromBottomRightSendsSizesAndResizingState)
{
    auto w = window(100, 100);
    move(445, 345);
    click(BTN_LEFT, true);
    xdg_toplevel_resize(w->toplevel, seat, in.lastButtonSerial, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
    ASSERT_TRUE(roundtrip());

    move(485, 365);
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(w->width, 140);
    EXPECT_EQ(w->height, 120);
    EXPECT_TRUE(w->hasState(XDG_TOPLEVEL_STATE_RESIZING));

    click(BTN_LEFT, false);
    ASSERT_TRUE(roundtrip());
    EXPECT_FALSE(w->hasState(XDG_TOPLEVEL_STATE_RESIZING));
}

TEST_F(InputTest, ResizingFromTheLeftKeepsTheRightEdge)
{
    auto w = window(100, 100); // x 350..450
    move(355, 300);
    key(KEY_LEFTALT, true);
    click(BTN_RIGHT, true); // Alt+right-drag: nearest corner is the left one
    move(325, 300);
    ASSERT_TRUE(roundtrip());
    ASSERT_EQ(w->width, 130);

    // The client follows the configure with a wider buffer.
    xdg_surface_ack_configure(w->xdg, w->lastSerial);
    wl_surface_attach(w->surface, makeBuffer(130, 100), 0, 0);
    wl_surface_commit(w->surface);
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(desktop->windows()[0]->position.x, 320) << "right edge stays at 450";
    click(BTN_RIGHT, false);
    key(KEY_LEFTALT, false);
}

TEST_F(InputTest, ShortcutsRunAndAreHiddenFromClients)
{
    auto w = window(100, 100);
    in.log.clear();

    key(KEY_LEFTALT, true);
    key(KEY_ENTER, true);
    key(KEY_ENTER, false);
    key(KEY_Q, true);
    key(KEY_Q, false);
    key(KEY_LEFTSHIFT, true);
    key(KEY_ESC, true);
    key(KEY_ESC, false);
    key(KEY_LEFTSHIFT, false);
    key(KEY_LEFTALT, false);
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(spawned, (std::vector<std::string>{"test-terminal"}));
    EXPECT_TRUE(w->closed);
    EXPECT_EQ(quits, 1);
    for (std::uint32_t k : {KEY_ENTER, KEY_Q, KEY_ESC}) {
        EXPECT_FALSE(in.saw(std::format("key {} 1", k))) << k;
        EXPECT_FALSE(in.saw(std::format("key {} 0", k))) << k;
    }
    EXPECT_TRUE(in.saw(std::format("key {} 1", KEY_LEFTALT))) << "plain modifiers still reach the client";
}

TEST_F(InputTest, MaximizeFillsTheAreaAndRestores)
{
    auto w = window(100, 100);
    xdg_toplevel_set_maximized(w->toplevel);
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(w->width, 800);
    EXPECT_EQ(w->height, 600);
    EXPECT_TRUE(w->hasState(XDG_TOPLEVEL_STATE_MAXIMIZED));
    EXPECT_EQ(desktop->windows()[0]->position, (Point{0, 0}));

    xdg_toplevel_unset_maximized(w->toplevel);
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(w->width, 100);
    EXPECT_FALSE(w->hasState(XDG_TOPLEVEL_STATE_MAXIMIZED));
    EXPECT_EQ(desktop->windows()[0]->position, (Point{350, 250}));
}

TEST_F(InputTest, WindowsAreToldWhatFits)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(w->bounds, (Size{800, 600})) << "bounds come with the very first configure";

    desktop->setArea({0, 0, 1024, 768});
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(w->bounds, (Size{1024, 768}));
}

TEST_F(InputTest, OversizedWindowIsAskedToFit)
{
    auto w = window(1000, 900); // bigger than 800x600 (a client ignoring configure_bounds)
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(w->width, 800);
    EXPECT_EQ(w->height, 600);
    EXPECT_EQ(desktop->windows()[0]->position, (Point{0, 0}));
}

TEST_F(InputTest, CapabilitiesAdvertiseMaximizeAndFullscreen)
{
    auto w = window(100, 100);
    EXPECT_TRUE(w->sawCapabilities);
}

// ------------------------------------------------------------------ popups --

namespace {
struct ClientPopup {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_popup* popup = nullptr;
    std::int32_t x = 0, y = 0, w = 0, h = 0;
    std::uint32_t serial = 0;
    bool done = false;
};

struct PopupSpec {
    Size size{100, 50};
    Rect anchorRect{0, 0, 10, 10};
    std::uint32_t anchor = XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
    std::uint32_t gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
    std::uint32_t constraints = 0;
};
} // namespace

class PopupTest : public InputTest {
protected:
    // Creates a popup on `parent` and does the initial commit; `grab` asks for a grab with that serial.
    std::unique_ptr<ClientPopup> popup(test::ClientWindow& parent, const PopupSpec& spec,
                                       std::optional<std::uint32_t> grab = std::nullopt)
    {
        auto p = std::make_unique<ClientPopup>();
        auto* positioner = xdg_wm_base_create_positioner(wmBase);
        xdg_positioner_set_size(positioner, spec.size.width, spec.size.height);
        xdg_positioner_set_anchor_rect(positioner, spec.anchorRect.x, spec.anchorRect.y, spec.anchorRect.width,
                                       spec.anchorRect.height);
        xdg_positioner_set_anchor(positioner, spec.anchor);
        xdg_positioner_set_gravity(positioner, spec.gravity);
        xdg_positioner_set_constraint_adjustment(positioner, spec.constraints);
        p->surface = makeSurface();
        p->xdg = track(xdg_wm_base_get_xdg_surface(wmBase, p->surface));
        p->popup = track(xdg_surface_get_popup(p->xdg, parent.xdg, positioner));
        xdg_positioner_destroy(positioner);
        xdg_popup_add_listener(p->popup, &kPopupListener, p.get());
        xdg_surface_add_listener(p->xdg, &kPopupSurfaceListener, p.get());
        if (grab)
            xdg_popup_grab(p->popup, seat, *grab);
        wl_surface_commit(p->surface);
        EXPECT_TRUE(roundtrip());
        return p;
    }

    void mapPopup(ClientPopup& p)
    {
        xdg_surface_ack_configure(p.xdg, p.serial);
        wl_surface_attach(p.surface, makeBuffer(p.w, p.h), 0, 0);
        wl_surface_commit(p.surface);
        ASSERT_TRUE(roundtrip());
    }

    static constexpr xdg_popup_listener kPopupListener = {
        .configure = [](void* d, xdg_popup*, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) {
            auto* p = static_cast<ClientPopup*>(d);
            p->x = x, p->y = y, p->w = w, p->h = h;
        },
        .popup_done = [](void* d, xdg_popup*) { static_cast<ClientPopup*>(d)->done = true; },
        .repositioned = [](void*, xdg_popup*, std::uint32_t) {},
    };
    static constexpr xdg_surface_listener kPopupSurfaceListener = {
        .configure = [](void* d, xdg_surface*, std::uint32_t serial) { static_cast<ClientPopup*>(d)->serial = serial; },
    };
};

TEST_F(PopupTest, FlipsUpWhenItWouldRunOffTheBottom)
{
    auto w = window(100, 100); // y 250..350 in a 600px area
    auto p = popup(*w, {.size = {100, 300}, .anchorRect = {0, 90, 10, 10}, .constraints = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y});

    // Below: 350..650 doesn't fit. Flipped above the anchor: 90-300 = -210 -> 40..340 fits.
    EXPECT_EQ(p->y, -210);
    EXPECT_EQ(p->h, 300);
}

TEST_F(PopupTest, SlidesLeftAtTheRightEdge)
{
    auto w = window(100, 100); // x 350..450 in an 800px area
    auto p = popup(*w, {.size = {500, 50}, .anchorRect = {90, 0, 10, 10}, .anchor = XDG_POSITIONER_ANCHOR_TOP_RIGHT,
                        .constraints = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X});

    EXPECT_EQ(350 + p->x + p->w, 800) << "slid flush against the right edge";
}

TEST_F(PopupTest, UnconstrainedPopupIsLeftAlone)
{
    auto w = window(100, 100);
    auto p = popup(*w, {.size = {100, 300}, .anchorRect = {0, 90, 10, 10}}); // no adjustments allowed
    EXPECT_EQ(p->y, 100);
}

TEST_F(PopupTest, GrabbedMenuTakesKeyboardAndOutsideClickDismissesIt)
{
    auto w = window(100, 100); // at (350, 250)
    move(360, 260);
    click(BTN_RIGHT, true); // right-click opens a context menu
    click(BTN_RIGHT, false);
    auto p = popup(*w, {}, in.lastButtonSerial);
    mapPopup(*p);
    EXPECT_EQ(in.keyboardSurface, p->surface) << "the menu gets the keyboard";

    in.log.clear();
    move(50, 50); // background
    click(BTN_LEFT, true);
    click(BTN_LEFT, false);

    EXPECT_TRUE(p->done) << "clicking outside closes the menu";
    for (const auto& e : in.log)
        EXPECT_FALSE(e.starts_with("button")) << "the dismissing click goes nowhere: " << e;
    EXPECT_EQ(in.keyboardSurface, w->surface) << "keyboard back to the window";
}

TEST_F(PopupTest, ClickInsideTheSameClientKeepsTheMenu)
{
    auto w = window(100, 100);
    move(360, 260);
    click(BTN_RIGHT, true);
    click(BTN_RIGHT, false);
    auto p = popup(*w, {}, in.lastButtonSerial); // anchored 10px into the window: (350, 260) 100x50
    mapPopup(*p);

    move(370, 280); // on the menu
    click(BTN_LEFT, true);
    click(BTN_LEFT, false);

    EXPECT_FALSE(p->done);
    EXPECT_TRUE(in.saw(std::format("button {} 1", BTN_LEFT))) << "the menu gets the click";
}

TEST_F(PopupTest, GrabWithoutRealInputIsRefused)
{
    auto w = window(100, 100);
    auto p = popup(*w, {}, 12345u); // made-up serial
    mapPopup(*p);

    EXPECT_TRUE(p->done);
    EXPECT_EQ(in.keyboardSurface, w->surface);
}

TEST_F(PopupTest, GrabAfterMapIsAProtocolError)
{
    auto w = window(100, 100);
    auto p = popup(*w, {});
    mapPopup(*p);
    xdg_popup_grab(p->popup, seat, 1);

    EXPECT_FALSE(roundtrip());
    EXPECT_EQ(protocolError(), (std::pair<std::string, std::uint32_t>{"xdg_popup", XDG_POPUP_ERROR_INVALID_GRAB}));
}

// -------------------------------------------- primary selection, cursors, activation --

namespace {
// Binds one global by interface name.
template <typename T>
T* bindGlobal(test::ServerFixture& f, const wl_interface* iface, std::uint32_t version)
{
    struct Ctx {
        const wl_interface* iface;
        std::uint32_t version;
        void* bound = nullptr;
    } ctx{iface, version};
    static const wl_registry_listener listener = {
        .global =
            [](void* d, wl_registry* reg, std::uint32_t name, const char* i, std::uint32_t) {
                auto* c = static_cast<Ctx*>(d);
                if (std::string_view(i) == c->iface->name)
                    c->bound = wl_registry_bind(reg, name, c->iface, c->version);
            },
        .global_remove = [](void*, wl_registry*, std::uint32_t) {},
    };
    auto* registry = wl_display_get_registry(f.client);
    wl_registry_add_listener(registry, &listener, &ctx);
    EXPECT_TRUE(f.roundtrip());
    wl_registry_destroy(registry);
    EXPECT_NE(ctx.bound, nullptr) << iface->name;
    return f.track(static_cast<T*>(ctx.bound));
}
} // namespace

TEST_F(InputTest, MiddleClickPasteThroughPrimarySelection)
{
    auto w = window(100, 100);
    auto* mgr = bindGlobal<zwp_primary_selection_device_manager_v1>(*this, &zwp_primary_selection_device_manager_v1_interface, 1);
    auto* device = track(zwp_primary_selection_device_manager_v1_get_device(mgr, seat));

    struct Primary {
        zwp_primary_selection_offer_v1* offer = nullptr;
        int sendFd = -1;
    } pr;
    static const zwp_primary_selection_device_v1_listener deviceListener = {
        .data_offer = [](void*, zwp_primary_selection_device_v1*, zwp_primary_selection_offer_v1*) {},
        .selection = [](void* d, zwp_primary_selection_device_v1*, zwp_primary_selection_offer_v1* offer) {
            auto* p = static_cast<Primary*>(d);
            if (p->offer && p->offer != offer)
                zwp_primary_selection_offer_v1_destroy(p->offer);
            p->offer = offer;
        },
    };
    static const zwp_primary_selection_source_v1_listener sourceListener = {
        .send = [](void* d, zwp_primary_selection_source_v1*, const char*, std::int32_t fd) {
            static_cast<Primary*>(d)->sendFd = fd;
        },
        .cancelled = [](void*, zwp_primary_selection_source_v1*) {},
    };
    zwp_primary_selection_device_v1_add_listener(device, &deviceListener, &pr);

    // Select text with the mouse: the selection is set with that click's serial.
    move(400, 300);
    click(BTN_LEFT, true);
    click(BTN_LEFT, false);
    auto* source = track(zwp_primary_selection_device_manager_v1_create_source(mgr));
    zwp_primary_selection_source_v1_add_listener(source, &sourceListener, &pr);
    zwp_primary_selection_source_v1_offer(source, "text/plain");
    zwp_primary_selection_device_v1_set_selection(device, source, in.lastButtonSerial);
    ASSERT_TRUE(roundtrip());
    ASSERT_NE(pr.offer, nullptr);
    track(pr.offer);

    int fds[2];
    ASSERT_EQ(pipe2(fds, O_CLOEXEC), 0);
    zwp_primary_selection_offer_v1_receive(pr.offer, "text/plain", fds[1]);
    close(fds[1]);
    ASSERT_TRUE(roundtrip());
    ASSERT_GE(pr.sendFd, 0);
    ASSERT_EQ(write(pr.sendFd, "selected", 8), 8);
    close(pr.sendFd);
    char buf[16] = {};
    EXPECT_EQ(read(fds[0], buf, sizeof(buf)), 8);
    EXPECT_STREQ(buf, "selected");
    close(fds[0]);
}

TEST_F(InputTest, PrimarySelectionNeedsRealInput)
{
    auto w = window(100, 100);
    auto* mgr = bindGlobal<zwp_primary_selection_device_manager_v1>(*this, &zwp_primary_selection_device_manager_v1_interface, 1);
    auto* device = track(zwp_primary_selection_device_manager_v1_get_device(mgr, seat));
    auto* source = track(zwp_primary_selection_device_manager_v1_create_source(mgr));
    zwp_primary_selection_device_v1_set_selection(device, source, 4242); // never sent to us
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(server->seat().primarySelection(), nullptr);
}

TEST_F(InputTest, CursorShapeFromTheClientUnderThePointer)
{
    auto w = window(100, 100);
    auto* mgr = bindGlobal<wp_cursor_shape_manager_v1>(*this, &wp_cursor_shape_manager_v1_interface, 2);
    auto* device = track(wp_cursor_shape_manager_v1_get_pointer(mgr, pointer));
    std::vector<std::uint32_t> shapes;
    auto c = server->seat().cursorShapeChanged.connect([&](std::uint32_t s) { shapes.push_back(s); });

    move(400, 300);
    wp_cursor_shape_device_v1_set_shape(device, in.enterSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
    ASSERT_TRUE(roundtrip());
    wp_cursor_shape_device_v1_set_shape(device, in.enterSerial + 999, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT); // stale
    ASSERT_TRUE(roundtrip());
    wl_pointer_set_cursor(pointer, in.enterSerial, nullptr, 0, 0); // hide it (games do this)
    ASSERT_TRUE(roundtrip());
    move(10, 10); // leave: back to the default arrow
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(shapes, (std::vector<std::uint32_t>{WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT, 0, 1}));
}

namespace {
std::string requestToken(test::ServerFixture& f, xdg_activation_v1* activation, std::optional<std::uint32_t> serial,
                         wl_seat* seat)
{
    std::string token;
    static const xdg_activation_token_v1_listener listener = {
        .done = [](void* d, xdg_activation_token_v1*, const char* t) { *static_cast<std::string*>(d) = t; },
    };
    auto* t = xdg_activation_v1_get_activation_token(activation);
    xdg_activation_token_v1_add_listener(t, &listener, &token);
    if (serial)
        xdg_activation_token_v1_set_serial(t, *serial, seat);
    xdg_activation_token_v1_commit(t);
    EXPECT_TRUE(f.roundtrip());
    xdg_activation_token_v1_destroy(t);
    return token;
}
} // namespace

TEST_F(InputTest, ActivationWithAUserActionFocusesTheTarget)
{
    auto a = window(100, 100);
    auto b = window(100, 100); // focused
    auto* activation = bindGlobal<xdg_activation_v1>(*this, &xdg_activation_v1_interface, 1);

    key(KEY_ENTER, true); // e.g. a key press in b that opens a link in a
    key(KEY_ENTER, false);
    const auto token = requestToken(*this, activation, in.lastKeySerial, seat);
    ASSERT_FALSE(token.empty());
    xdg_activation_v1_activate(activation, token.c_str(), a->surface);
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(in.keyboardSurface, a->surface);
}

TEST_F(InputTest, ActivationWithoutUserActionDoesNotStealFocus)
{
    auto a = window(100, 100);
    auto b = window(100, 100);
    auto* activation = bindGlobal<xdg_activation_v1>(*this, &xdg_activation_v1_interface, 1);

    const auto token = requestToken(*this, activation, std::nullopt, seat);
    xdg_activation_v1_activate(activation, token.c_str(), a->surface);
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(in.keyboardSurface, b->surface);
}

TEST_F(InputTest, LaunchTokenFocusesTheLaunchedApp)
{
    auto a = window(100, 100);
    auto b = window(100, 100);
    auto* activation = bindGlobal<xdg_activation_v1>(*this, &xdg_activation_v1_interface, 1);

    // What an app launched with Mod+Enter finds in XDG_ACTIVATION_TOKEN.
    const auto token = server->activation().createCompositorToken();
    xdg_activation_v1_activate(activation, token.c_str(), a->surface);
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(in.keyboardSurface, a->surface);

    xdg_activation_v1_activate(activation, token.c_str(), b->surface); // single use
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(in.keyboardSurface, a->surface);
}

TEST_F(InputTest, ClientCursorImageIsTrackedWithItsHotspot)
{
    auto w = window(100, 100);
    std::vector<std::uint32_t> shapes;
    auto c = server->seat().cursorShapeChanged.connect([&](std::uint32_t s) { shapes.push_back(s); });
    move(400, 300);

    auto* cursor = makeSurface();
    wl_surface_attach(cursor, makeBuffer(16, 16), 0, 0);
    wl_surface_commit(cursor);
    wl_pointer_set_cursor(pointer, in.enterSerial, cursor, 3, 4);
    ASSERT_TRUE(roundtrip());

    Surface* serverCursor = server->seat().cursorSurface();
    ASSERT_NE(serverCursor, nullptr);
    EXPECT_EQ(serverCursor->resource(), surfaces.back()->resource());
    EXPECT_EQ(server->seat().cursorHotspot(), (Point{3, 4}));
    EXPECT_EQ(shapes, (std::vector<std::uint32_t>{0})) << "the backend's own cursor is hidden";

    // New frame of an animated cursor, with the hotspot moved by an offset.
    wl_surface_attach(cursor, makeBuffer(16, 16), 0, 0);
    wl_surface_offset(cursor, 1, 2);
    wl_surface_commit(cursor);
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(server->seat().cursorHotspot(), (Point{2, 2}));

    move(10, 10); // leaving the window: back to the default shape
    EXPECT_EQ(server->seat().cursorSurface(), nullptr);
    EXPECT_EQ(shapes.back(), 1u);
}

// ------------------------------------------------------------ drag and drop --

namespace {
// Everything both ends of a drag observed (the test client plays both).
struct DragLog {
    std::vector<std::string> source; // target / action / drop_performed / finished / cancelled
    std::vector<std::string> device; // enter / leave / motion / drop
    wl_data_offer* offer = nullptr;
    std::vector<std::string> offeredTypes;
    std::uint32_t sourceActions = 0;
    std::uint32_t offerAction = 0;
    std::uint32_t enterSerial = 0;
    int sendFd = -1;
};

const wl_data_offer_listener kDragOfferListener = {
    .offer = [](void* d, wl_data_offer*, const char* t) { static_cast<DragLog*>(d)->offeredTypes.emplace_back(t); },
    .source_actions = [](void* d, wl_data_offer*, std::uint32_t a) { static_cast<DragLog*>(d)->sourceActions = a; },
    .action = [](void* d, wl_data_offer*, std::uint32_t a) { static_cast<DragLog*>(d)->offerAction = a; },
};
const wl_data_device_listener kDragDeviceListener = {
    .data_offer = [](void* d, wl_data_device*, wl_data_offer* o) { wl_data_offer_add_listener(o, &kDragOfferListener, d); },
    .enter = [](void* d, wl_data_device*, std::uint32_t serial, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer* o) {
        auto* l = static_cast<DragLog*>(d);
        l->offer = o;
        l->enterSerial = serial;
        l->device.push_back("enter");
    },
    .leave = [](void* d, wl_data_device*) { static_cast<DragLog*>(d)->device.push_back("leave"); },
    .motion = [](void* d, wl_data_device*, std::uint32_t, wl_fixed_t, wl_fixed_t) {
        auto* l = static_cast<DragLog*>(d);
        if (l->device.empty() || l->device.back() != "motion")
            l->device.push_back("motion");
    },
    .drop = [](void* d, wl_data_device*) { static_cast<DragLog*>(d)->device.push_back("drop"); },
    .selection = [](void*, wl_data_device*, wl_data_offer*) {},
};
const wl_data_source_listener kDragSourceListener = {
    .target = [](void* d, wl_data_source*, const char* t) {
        static_cast<DragLog*>(d)->source.push_back(std::string("target ") + (t ? t : "-"));
    },
    .send = [](void* d, wl_data_source*, const char*, std::int32_t fd) { static_cast<DragLog*>(d)->sendFd = fd; },
    .cancelled = [](void* d, wl_data_source*) { static_cast<DragLog*>(d)->source.push_back("cancelled"); },
    .dnd_drop_performed = [](void* d, wl_data_source*) { static_cast<DragLog*>(d)->source.push_back("drop_performed"); },
    .dnd_finished = [](void* d, wl_data_source*) { static_cast<DragLog*>(d)->source.push_back("finished"); },
    .action = [](void* d, wl_data_source*, std::uint32_t a) {
        static_cast<DragLog*>(d)->source.push_back(std::format("action {}", a));
    },
};
} // namespace

class DragTest : public InputTest {
protected:
    wl_data_device* device = nullptr;
    wl_data_source* source = nullptr;
    DragLog log;

    // Presses on the window and starts dragging "text/plain" (copy or move).
    void startDrag(std::unique_ptr<test::ClientWindow>& w, std::optional<std::uint32_t> serial = std::nullopt,
                   wl_surface* icon = nullptr)
    {
        device = track(wl_data_device_manager_get_data_device(ddm, seat));
        wl_data_device_add_listener(device, &kDragDeviceListener, &log);
        source = track(wl_data_device_manager_create_data_source(ddm));
        wl_data_source_add_listener(source, &kDragSourceListener, &log);
        wl_data_source_offer(source, "text/plain");
        wl_data_source_set_actions(source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
        move(360, 260);
        click(BTN_LEFT, true);
        wl_data_device_start_drag(device, source, w->surface, icon, serial.value_or(in.lastButtonSerial));
        ASSERT_TRUE(roundtrip());
    }
};

TEST_F(DragTest, CopyDragAndDropRoundTrip)
{
    auto w = window(100, 100); // at (350, 250): both source and target here
    startDrag(w);
    ASSERT_TRUE(server->seat().dragActive());
    EXPECT_EQ(log.device, (std::vector<std::string>{"enter"})) << "entered where the drag started";
    EXPECT_EQ(log.offeredTypes, (std::vector<std::string>{"text/plain"}));
    EXPECT_EQ(log.sourceActions, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    EXPECT_TRUE(in.saw("leave")) << "the pointer leaves the origin while dragging";

    // The target accepts text and prefers copy.
    wl_data_offer_accept(log.offer, log.enterSerial, "text/plain");
    wl_data_offer_set_actions(log.offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(log.offerAction, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    EXPECT_EQ(log.source, (std::vector<std::string>{"target text/plain", "action 1"}));

    move(380, 280);
    click(BTN_LEFT, false); // drop
    EXPECT_EQ(log.device, (std::vector<std::string>{"enter", "motion", "drop"}));
    EXPECT_EQ(log.source.back(), "drop_performed");
    EXPECT_FALSE(server->seat().dragActive());

    // The target reads the data, then finishes.
    int fds[2];
    ASSERT_EQ(pipe2(fds, O_CLOEXEC), 0);
    wl_data_offer_receive(log.offer, "text/plain", fds[1]);
    close(fds[1]);
    ASSERT_TRUE(roundtrip());
    ASSERT_GE(log.sendFd, 0);
    ASSERT_EQ(write(log.sendFd, "dragged", 7), 7);
    close(log.sendFd);
    char buf[16] = {};
    EXPECT_EQ(read(fds[0], buf, sizeof(buf)), 7);
    close(fds[0]);
    EXPECT_STREQ(buf, "dragged");

    wl_data_offer_finish(log.offer);
    wl_data_offer_destroy(log.offer);
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(log.source.back(), "finished");
}

TEST_F(DragTest, DroppingOnNothingCancels)
{
    auto w = window(100, 100);
    startDrag(w);
    wl_data_offer_accept(log.offer, log.enterSerial, "text/plain");
    ASSERT_TRUE(roundtrip());

    move(10, 10); // off the window: leave
    click(BTN_LEFT, false);

    EXPECT_EQ(log.device.back(), "leave");
    EXPECT_EQ(log.source.back(), "cancelled");
    EXPECT_FALSE(server->seat().dragActive());
    EXPECT_EQ(in.pointerSurface, nullptr);
    if (log.offer)
        wl_data_offer_destroy(log.offer);
}

TEST_F(DragTest, EscapeCancels)
{
    auto w = window(100, 100);
    startDrag(w);
    key(KEY_ESC, true);
    key(KEY_ESC, false);

    EXPECT_EQ(log.source.back(), "cancelled");
    EXPECT_FALSE(in.saw(std::format("key {} 1", KEY_ESC))) << "Esc was the compositor's";
    click(BTN_LEFT, false);
    if (log.offer)
        wl_data_offer_destroy(log.offer);
}

TEST_F(DragTest, DragWithoutAPressIsRefused)
{
    auto w = window(100, 100);
    startDrag(w, /*serial=*/7777);

    EXPECT_FALSE(server->seat().dragActive());
    EXPECT_EQ(log.source, (std::vector<std::string>{"cancelled"}));
    click(BTN_LEFT, false);
}

TEST_F(DragTest, DragIconFollowsThePointer)
{
    auto w = window(100, 100);
    auto* icon = makeSurface();
    wl_surface_attach(icon, makeBuffer(8, 8), 0, 0);
    wl_surface_commit(icon);
    startDrag(w, std::nullopt, icon);

    ASSERT_NE(server->seat().dragIcon(), nullptr);
    EXPECT_EQ(server->seat().dragIcon()->resource(), surfaces.back()->resource());
    click(BTN_LEFT, false);
    EXPECT_EQ(server->seat().dragIcon(), nullptr);
    if (log.offer)
        wl_data_offer_destroy(log.offer);
}
