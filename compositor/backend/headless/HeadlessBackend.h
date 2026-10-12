#pragma once

#include "backend/Backend.h"

#include <cstdint>

namespace dragon {

class EventLoop;

// No display: outputs are offscreen swapchains paced by a timer. Used for
// tests and for running without any screen.
class HeadlessBackend final : public Backend {
public:
    struct Config {
        int outputs = 1;
        std::uint32_t width = 1920;
        std::uint32_t height = 1080;
        std::uint32_t refreshHz = 60;
    };

    HeadlessBackend(EventLoop& loop, Config config) : m_loop(loop), m_config(config) {}
    ~HeadlessBackend() override { clearOutputs(); }

    const char* name() const override { return "headless"; }
    bool start(Gpu& gpu) override;

private:
    EventLoop& m_loop;
    Config m_config;
};

} // namespace dragon
