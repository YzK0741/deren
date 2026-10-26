#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

namespace deren::vulkan::detail {
    // Shared by buffers, images and deferred texture groups. The work publishes physical state
    // before ready's release; failed work is terminal. Readers must acquire ready before handles.
    template <class State, class Error>
    struct lazy_loading {
        explicit lazy_loading(State initial = State::ready) noexcept : value(initial) {}
        std::atomic<State> value;
        std::atomic<std::uint32_t> users{0};
        std::mutex mutex;
        Error failure = Error::ok;
        [[nodiscard]] State state() const noexcept { return value.load(std::memory_order_acquire); }
        template <class Release>
        [[nodiscard]] Error unload(Release&& release) noexcept {
            std::unique_lock lock(mutex, std::try_to_lock);
            if (!lock.owns_lock()) return Error::not_ready;
            if (state() == State::unloaded) return Error::ok;
            if (state() == State::failed) return failure;
            if (users.load(std::memory_order_acquire) != 0) return Error::not_ready;
            release();
            value.store(State::unloaded, std::memory_order_release);
            return Error::ok;
        }
        template <class Work>
        [[nodiscard]] Error run(Work&& work) noexcept {
            std::lock_guard lock(mutex);
            if (state() == State::ready) return Error::ok;
            if (state() == State::failed) return failure;
            value.store(State::loading, std::memory_order_release);
            failure = work();
            value.store(failure == Error::ok ? State::ready : State::failed, std::memory_order_release);
            return failure;
        }
    };
} // namespace deren::vulkan::detail
