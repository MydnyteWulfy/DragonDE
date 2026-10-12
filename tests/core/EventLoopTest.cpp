#include "core/EventLoop.h"

#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <unistd.h>
#include <wayland-server-core.h>

using namespace std::chrono_literals;
using dragon::EventLoop;

namespace {
struct LoopFixture : ::testing::Test {
    std::unique_ptr<wl_event_loop, decltype(&wl_event_loop_destroy)> raw{wl_event_loop_create(),
                                                                         wl_event_loop_destroy};
    EventLoop loop{raw.get()};
};
} // namespace

TEST_F(LoopFixture, IdleRunsOnceOnNextDispatch)
{
    int count = 0;
    loop.addIdle([&] { ++count; });

    loop.dispatch(0);
    loop.dispatch(0);

    EXPECT_EQ(count, 1);
}

TEST_F(LoopFixture, TimerFiresAfterArm)
{
    bool fired = false;
    auto timer = loop.addTimer([&] { fired = true; });
    ASSERT_TRUE(timer);

    timer.arm(1ms);
    loop.dispatch(100);

    EXPECT_TRUE(fired);
}

TEST_F(LoopFixture, FdCallbackSeesReadableData)
{
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    char got = 0;
    auto src = loop.addFd(fds[0], WL_EVENT_READABLE, [&](int fd, uint32_t mask) {
        EXPECT_TRUE(mask & WL_EVENT_READABLE);
        ASSERT_EQ(read(fd, &got, 1), 1);
    });
    ASSERT_EQ(write(fds[1], "x", 1), 1);

    loop.dispatch(100);
    EXPECT_EQ(got, 'x');

    src = {};
    close(fds[0]);
    close(fds[1]);
}

TEST_F(LoopFixture, DestroyedSourceNeverFires)
{
    bool fired = false;
    {
        auto timer = loop.addTimer([&] { fired = true; });
        timer.arm(1ms);
    }
    loop.dispatch(20);

    EXPECT_FALSE(fired);
}
