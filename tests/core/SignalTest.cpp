#include "core/Signal.h"

#include <gtest/gtest.h>
#include <optional>
#include <vector>

using dragon::Connection;
using dragon::Signal;

TEST(Signal, EmitsToAllSlotsInOrder)
{
    Signal<int> sig;
    std::vector<int> calls;
    auto a = sig.connect([&](int v) { calls.push_back(v); });
    auto b = sig.connect([&](int v) { calls.push_back(v * 10); });

    sig.emit(3);

    EXPECT_EQ(calls, (std::vector<int>{3, 30}));
}

TEST(Signal, ConnectionDestructorDisconnects)
{
    Signal<> sig;
    int count = 0;
    {
        auto c = sig.connect([&] { ++count; });
        sig.emit();
    }
    sig.emit();

    EXPECT_EQ(count, 1);
    EXPECT_EQ(sig.connectionCount(), 0u);
}

TEST(Signal, SlotCanDisconnectItselfDuringEmit)
{
    Signal<> sig;
    int count = 0;
    Connection c;
    c = sig.connect([&] {
        ++count;
        c.disconnect();
    });

    sig.emit();
    sig.emit();

    EXPECT_EQ(count, 1);
}

TEST(Signal, SlotDisconnectingLaterSlotSkipsIt)
{
    Signal<> sig;
    bool secondCalled = false;
    Connection second;
    auto first = sig.connect([&] { second.disconnect(); });
    second = sig.connect([&] { secondCalled = true; });

    sig.emit();

    EXPECT_FALSE(secondCalled);
}

TEST(Signal, SlotsConnectedDuringEmitWaitForNextEmit)
{
    Signal<> sig;
    int lateCalls = 0;
    std::vector<Connection> added;
    auto c = sig.connect([&] { added.push_back(sig.connect([&] { ++lateCalls; })); });

    sig.emit();
    EXPECT_EQ(lateCalls, 0);

    sig.emit();
    EXPECT_EQ(lateCalls, 1);
}

TEST(Signal, ConnectionMayOutliveSignal)
{
    Connection c;
    {
        Signal<> sig;
        c = sig.connect([] {});
        EXPECT_TRUE(c.connected());
    }
    EXPECT_FALSE(c.connected());
    c.disconnect(); // must not crash
}

TEST(Signal, SignalMayBeDestroyedFromItsOwnSlot)
{
    auto sig = std::make_optional<Signal<>>();
    bool afterCalled = false;
    auto a = sig->connect([&] { sig.reset(); });
    auto b = sig->connect([&] { afterCalled = true; });

    sig->emit();

    EXPECT_FALSE(sig.has_value());
    // Remaining slots of the in-flight emit still run; the core stays alive until it ends.
    EXPECT_TRUE(afterCalled);
}
