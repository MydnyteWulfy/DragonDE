#pragma once

#include "core/Signal.h"
#include "render/Swapchain.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <sys/types.h>
#include <vector>

namespace dragon {

class Gpu;

struct OutputMode {
    std::int32_t width = 0;  // pixels
    std::int32_t height = 0;
    std::int32_t refreshMhz = 60000;

    friend bool operator==(const OutputMode&, const OutputMode&) = default;
};

// Something we display frames on: a monitor (DRM), a window (nested), or
// nothing at all (headless). Owns its swapchain.
class Output {
public:
    explicit Output(std::string name) : m_name(std::move(name)) {}
    virtual ~Output() = default;
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    const std::string& name() const { return m_name; }
    // Human-readable, e.g. "DragonDE nested window".
    virtual std::string description() const { return m_name; }

    const OutputMode& mode() const { return m_mode; }

    // Null until the backend knows the output's size (e.g. first configure).
    Swapchain* swapchain() const { return m_swapchain.get(); }

    // Shows `slot` (just rendered into, so busy). The backend releases it once
    // the display no longer needs it.
    virtual void present(SwapchainSlot& slot) = 0;

    std::uint64_t framesPresented() const { return m_framesPresented; }
    // The slot shown by the latest present() (for screenshots and tests).
    const SwapchainSlot* lastPresented() const { return m_lastPresented; }

    // Now is a good time to render a new frame (vblank, frame callback, timer).
    Signal<> frame;
    Signal<> modeChanged;

protected:
    void setMode(OutputMode mode)
    {
        if (mode == m_mode)
            return;
        m_mode = mode;
        modeChanged.emit();
    }

    // Backends call this from present().
    void markPresented(SwapchainSlot& slot)
    {
        m_lastPresented = &slot;
        ++m_framesPresented;
    }
    // Replacing the swapchain invalidates lastPresented().
    void setSwapchain(std::unique_ptr<Swapchain> swapchain)
    {
        m_lastPresented = nullptr;
        m_swapchain = std::move(swapchain);
    }

    std::unique_ptr<Swapchain> m_swapchain;

private:
    std::string m_name;
    OutputMode m_mode;
    const SwapchainSlot* m_lastPresented = nullptr;
    std::uint64_t m_framesPresented = 0;
};

// Where outputs (and later input devices) come from.
//
// Startup order: create the backend, ask preferredDevice(), create the Gpu on
// that device, then start(gpu). Outputs must not outlive the Gpu.
class Backend {
public:
    virtual ~Backend() = default;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    virtual const char* name() const = 0;

    // The GPU the backend wants us to render on (any DRM node of it), if it cares.
    virtual std::optional<dev_t> preferredDevice() const { return std::nullopt; }

    // Creates the initial outputs (emitting outputAdded for each).
    virtual bool start(Gpu& gpu) = 0;

    std::span<const std::unique_ptr<Output>> outputs() const { return m_outputs; }

    Signal<Output*> outputAdded;
    Signal<Output*> outputRemoved;
    // The backend can't continue (nested window closed, parent compositor gone, ...).
    Signal<> quitRequested;

protected:
    Backend() = default;

    Output* addOutput(std::unique_ptr<Output> output);
    void removeOutput(Output* output);
    // Derived destructors call this when outputs depend on derived state.
    void clearOutputs();

private:
    std::vector<std::unique_ptr<Output>> m_outputs;
};

} // namespace dragon
