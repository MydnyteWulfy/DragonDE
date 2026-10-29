#include "core/FileWatcher.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <unistd.h>
#include <wayland-server-core.h>

using dragon::EventLoop;
using dragon::FileWatcher;

namespace {
struct WatcherFixture : ::testing::Test {
    std::unique_ptr<wl_event_loop, decltype(&wl_event_loop_destroy)> raw{wl_event_loop_create(),
                                                                         wl_event_loop_destroy};
    EventLoop loop{raw.get()};
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("dragon-watch-test-" + std::to_string(getpid()));

    void SetUp() override { std::filesystem::create_directories(dir); }
    void TearDown() override { std::filesystem::remove_all(dir); }

    // Dispatches until `count` changes or ~1s passes.
    int waitFor(const int& count, int seen)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (count == seen && std::chrono::steady_clock::now() < end)
            loop.dispatch(50);
        return count;
    }
};
} // namespace

TEST_F(WatcherFixture, NoticesWritesAndReplacements)
{
    const auto file = dir / "config.lua";
    FileWatcher watcher(loop, file);
    ASSERT_TRUE(watcher.watching());
    int changes = 0;
    auto c = watcher.changed.connect([&] { ++changes; });

    std::ofstream(file) << "a";
    EXPECT_EQ(waitFor(changes, 0), 1);

    // Like an editor: write elsewhere, rename over it.
    std::ofstream(dir / "config.lua.tmp") << "b";
    std::filesystem::rename(dir / "config.lua.tmp", file);
    EXPECT_EQ(waitFor(changes, 1), 2);

    // Other files in the directory don't count.
    std::ofstream(dir / "other.txt") << "c";
    loop.dispatch(200);
    loop.dispatch(0);
    EXPECT_EQ(changes, 2);
}

TEST_F(WatcherFixture, MissingDirectoryIsNotAnError)
{
    FileWatcher watcher(loop, dir / "nope" / "config.lua");
    EXPECT_FALSE(watcher.watching());
}
