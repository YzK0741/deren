// CPU tests for the engine-owned RHI outcome policy.
#include "vk_test.h"

#include <expected>
#include <memory>
#include <variant>
import deren.engine.outcome;
import deren.promise.rhi;

namespace {
    // The verdict family (hopper's shape, deren's error type): propagate carries, enforce decides.
    // The two lying-classifier guards and the fatal execution inside enforce terminate BY DESIGN
    // (panic -> std::terminate), so this test exercises exactly the verdicts that keep the process
    // alive; the terminating paths are witnessed by the code, not by a test that would have to
    // survive them.
    void test_verdict_enforce_and_propagate() {
        using deren::engine::outcome::enforce;
        using deren::engine::outcome::fatal;
        using deren::engine::outcome::pass_failure;
        using deren::engine::outcome::pass_success;
        using deren::engine::outcome::propagate;
        using deren::engine::outcome::result;
        using deren::engine::outcome::verdict;
        namespace rhi = deren::promise::rhi;

        rhi::error_info const raw{
            .code = rhi::error::out_of_date, .api = rhi::graphics_api::vulkan, .native_code = -1000001004, .message = "swapchain expired"};
        rhi::error_info const dead_device{.code = rhi::error::device_lost, .api = rhi::graphics_api::vulkan, .message = "device gone"};
        result<int> const good = 42;
        result<int> const bad = std::unexpected(raw);

        // propagate: success stays success; failure carries the error_info it was handed, untouched.
        verdict const carried_good = propagate(good);
        verdict const carried_bad = propagate(bad);
        CHECK(std::holds_alternative<pass_success>(carried_good));
        auto const* const carried = std::get_if<pass_failure>(&carried_bad);
        CHECK_MSG(carried != nullptr, "propagate answers pass_failure for a failed outcome");
        if (carried != nullptr) {
            CHECK(carried->failure.code == rhi::error::out_of_date);
            CHECK(carried->failure.native_code == -1000001004);
            CHECK(carried->failure.message == "swapchain expired");
        }

        // propagate as an honest classifier: the outcome passes through unchanged.
        result<int> const untouched = enforce(good, propagate);
        CHECK(untouched.has_value());
        CHECK(untouched.value() == 42);
        result<int> const kept = enforce(bad, propagate);
        CHECK(!kept.has_value());
        CHECK(kept.error().code == rhi::error::out_of_date);

        // A classifier that KNOWS what the failure means rewrites the error: the caller sees the
        // story (rebuild), the raw native code rides along.
        auto const classify_acquire = [](result<int> const& outcome) -> verdict {
            if (outcome) {
                return pass_success{};
            }
            if (outcome.error().code == rhi::error::out_of_date) {
                return pass_failure{rhi::error_info{
                    .code = rhi::error::out_of_date, .api = rhi::graphics_api::vulkan, .native_code = outcome.error().native_code, .message = "rebuild the swapchain and skip this frame"}};
            }
            return fatal{outcome.error()};
        };
        result<int> const rewritten = enforce(bad, classify_acquire);
        CHECK(!rewritten.has_value());
        CHECK(rewritten.error().code == rhi::error::out_of_date);
        CHECK(rewritten.error().message == "rebuild the swapchain and skip this frame");
        CHECK(rewritten.error().native_code == -1000001004);

        // A failure outside the classifier's rewrite table classifies as fatal: the verdict carries
        // the reason (executing it is the engine's panic, which a live test cannot survive).
        verdict const decided_fatal = classify_acquire(std::unexpected(dead_device));
        auto const* const fatal_case = std::get_if<fatal>(&decided_fatal);
        CHECK_MSG(fatal_case != nullptr, "a failure the classifier cannot pass answers fatal");
        if (fatal_case != nullptr) {
            CHECK(fatal_case->reason.code == rhi::error::device_lost);
        }
    }
    void test_void_and_move_only_results() {
        using namespace deren::engine::outcome;
        auto empty = enforce(result<void>{}, propagate);
        CHECK(empty.has_value());
        auto owned = enforce(result<std::unique_ptr<int>>{std::make_unique<int>(7)}, propagate);
        CHECK(owned.has_value());
        CHECK(owned.value() != nullptr);
        CHECK(*owned.value() == 7);
    }
} // namespace

int32_t main() {
    test_verdict_enforce_and_propagate();
    test_void_and_move_only_results();
    return deren::vk_test::finish("test_engine_outcome");
}
