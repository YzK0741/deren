#pragma once
inline void check_lazy_loading_contract() {
        using gate_type = deren::vulkan::detail::lazy_loading<rhi::lazy_state, rhi::error>;
        gate_type gate(rhi::lazy_state::unloaded);
        std::atomic<unsigned> attempts{0};
        std::latch entered(1), release(1), second_started(1);
        rhi::error first{}, second{};
        std::thread a([&] { first = gate.run([&] { ++attempts; entered.count_down(); release.wait(); return rhi::error::ok; }); });
        entered.wait();
        CHECK(gate.state() == rhi::lazy_state::loading);
        CHECK(gate.unload([] {}) == rhi::error::not_ready);
        std::thread b([&] { second_started.count_down(); second = gate.run([&] { ++attempts; return rhi::error::ok; }); });
        second_started.wait();
        release.count_down();
        a.join();
        b.join();
        CHECK(first == rhi::error::ok && second == rhi::error::ok);
        CHECK(attempts == 1 && gate.state() == rhi::lazy_state::ready);
        gate_type failed(rhi::lazy_state::unloaded);
        CHECK(failed.run([] { return rhi::error::operation_failed; }) == rhi::error::operation_failed);
        CHECK(failed.state() == rhi::lazy_state::failed);
        CHECK(failed.run([&] { ++attempts; return rhi::error::ok; }) == rhi::error::operation_failed);
        CHECK(attempts == 1);
        CHECK(failed.unload([] {}) == rhi::error::operation_failed);
        unsigned releases = 0;
        gate.users.fetch_add(1);
        CHECK(gate.unload([&] { ++releases; }) == rhi::error::not_ready);
        gate.users.fetch_sub(1);
        CHECK(gate.unload([&] { ++releases; }) == rhi::error::ok);
        CHECK(releases == 1 && gate.state() == rhi::lazy_state::unloaded);
        CHECK(gate.unload([&] { ++releases; }) == rhi::error::ok);
        CHECK(releases == 1);
}
