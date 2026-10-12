#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <utility>

namespace dragon {

namespace detail {
struct SignalCore {
    virtual ~SignalCore() = default;
    virtual void disconnect(std::uint64_t id) = 0;
    virtual bool contains(std::uint64_t id) const = 0;
};
} // namespace detail

// RAII handle for a signal connection. Destroying it disconnects the slot.
// Safe to outlive the signal it came from.
class Connection {
public:
    Connection() = default;
    Connection(std::weak_ptr<detail::SignalCore> core, std::uint64_t id)
        : m_core(std::move(core)), m_id(id) {}
    ~Connection() { disconnect(); }

    Connection(Connection&& other) noexcept
        : m_core(std::move(other.m_core)), m_id(std::exchange(other.m_id, 0)) {}
    Connection& operator=(Connection&& other) noexcept
    {
        if (this != &other) {
            disconnect();
            m_core = std::move(other.m_core);
            m_id = std::exchange(other.m_id, 0);
        }
        return *this;
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void disconnect()
    {
        if (auto core = m_core.lock(); core && m_id)
            core->disconnect(m_id);
        m_core.reset();
        m_id = 0;
    }

    bool connected() const
    {
        auto core = m_core.lock();
        return core && core->contains(m_id);
    }

private:
    std::weak_ptr<detail::SignalCore> m_core;
    std::uint64_t m_id = 0;
};

// Typed signal, single-threaded. Re-entrancy rules:
//  - slots connected during emit() are not called until the next emit()
//  - slots disconnected during emit() (including themselves) are not called again
//  - the Signal may be destroyed from inside one of its own slots
template <typename... Args>
class Signal {
public:
    Signal() = default;
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    template <typename F>
    [[nodiscard]] Connection connect(F&& fn)
    {
        const auto id = m_core->nextId++;
        m_core->slots.push_back({id, std::function<void(Args...)>(std::forward<F>(fn)), true});
        return Connection(m_core, id);
    }

    void emit(Args... args)
    {
        // Keep the core alive even if a slot destroys this Signal.
        auto core = m_core;
        ++core->emitDepth;
        // deque::push_back never invalidates references, so indices stay valid
        // while slots connect new slots; snapshot the count to skip newcomers.
        const auto count = core->slots.size();
        for (std::size_t i = 0; i < count; ++i) {
            auto& slot = core->slots[i];
            if (slot.alive)
                slot.fn(args...);
        }
        if (--core->emitDepth == 0)
            core->compact();
    }

    std::size_t connectionCount() const
    {
        std::size_t n = 0;
        for (const auto& s : m_core->slots)
            n += s.alive ? 1 : 0;
        return n;
    }

private:
    struct Core final : detail::SignalCore {
        struct Slot {
            std::uint64_t id;
            std::function<void(Args...)> fn;
            bool alive;
        };

        std::deque<Slot> slots;
        std::uint64_t nextId = 1;
        int emitDepth = 0;

        void disconnect(std::uint64_t id) override
        {
            // Only mark dead: the slot may be executing right now.
            for (auto& s : slots) {
                if (s.id == id)
                    s.alive = false;
            }
            if (emitDepth == 0)
                compact();
        }

        bool contains(std::uint64_t id) const override
        {
            for (const auto& s : slots) {
                if (s.id == id && s.alive)
                    return true;
            }
            return false;
        }

        void compact() { std::erase_if(slots, [](const Slot& s) { return !s.alive; }); }
    };

    std::shared_ptr<Core> m_core = std::make_shared<Core>();
};

} // namespace dragon
