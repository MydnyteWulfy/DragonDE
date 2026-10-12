// Input end to end: raw backend events -> InputRouter -> Desktop focus policy
// -> Seat -> a real client's wl_pointer / wl_keyboard / wl_data_device.

#include "../server/ServerFixture.h"

#include "desktop/Desktop.h"
#include "desktop/InputRouter.h"
#include "protocols/Seat.h"

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

    void SetUp() override
    {
        ServerFixture::SetUp();
        desktop = std::make_unique<Desktop>(*server);
        desktop->setArea({0, 0, 800, 600});
        router = std::make_unique<InputRouter>(backend, *desktop, server->seat());

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
            [](void* d, wl_pointer*, std::uint32_t, wl_surface* s, wl_fixed_t x, wl_fixed_t y) {
                auto* in = static_cast<ClientInput*>(d);
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
        .key = [](void* d, wl_keyboard*, std::uint32_t, std::uint32_t, std::uint32_t key, std::uint32_t state) {
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
