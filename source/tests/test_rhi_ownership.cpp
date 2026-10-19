#include "vk_test.h"

#include <atomic>
#include <barrier>
#include <thread>
import deren.promise.rhi;
namespace rhi = deren::promise::rhi;
namespace {
    struct counted final : rhi::object {
        std::atomic<unsigned>& destroyed;
        rhi::shared_object<counted>::weak self;
        explicit counted(std::atomic<unsigned>& count)
            : object("counted")
            , destroyed(count) {
        }
        void release() noexcept {
            ++destroyed;
            delete this;
        }
    };
} // namespace
int main() {
    std::atomic<unsigned> destroyed{0};
    {
        rhi::shared_object<counted> owner{new counted(destroyed)};
        owner->self = owner;
        auto observer = owner->self;
        auto locked = observer.lock();
        CHECK(locked && owner.use_count() == 2);
        owner.reset();
        CHECK(!observer.expired() && destroyed == 0);
        locked.reset();
        CHECK(observer.expired() && !observer.lock() && destroyed == 1);
    }
    // Distinct handles on distinct threads: neither accesses the other's handle.
    // The last external weak may disappear concurrently with the last strong.
    constexpr unsigned iterations = 30000;
    std::barrier gate(2);
    rhi::shared_object<counted> owner;
    rhi::shared_object<counted>::weak observer;
    std::thread strong_thread([&] {
        for (unsigned i = 0; i < iterations; ++i) {
            owner = rhi::shared_object<counted>{new counted(destroyed)};
            observer = owner;
            gate.arrive_and_wait();
            owner.reset();
            gate.arrive_and_wait();
        }
    });
    std::thread weak_thread([&] {
        for (unsigned i = 0; i < iterations; ++i) {
            gate.arrive_and_wait();
            observer.reset();
            gate.arrive_and_wait();
        }
    });
    strong_thread.join();
    weak_thread.join();
    CHECK(destroyed == iterations + 1);
    return deren::vk_test::finish("test_rhi_ownership");
}
