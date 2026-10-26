// Pure-CPU tests for the prepared/display candidate scheduler (Phase 1 of the pipelined residency
// plan). No device, RHI or engine runtime types are involved.
#include "vk_test.h"

import deren.engine.pipelined_residency;

namespace pipeline = deren::engine::pipelined_residency;

namespace {
    void run_candidate(pipeline::scheduler& scheduler, std::size_t slot) {
        CHECK(scheduler.feedback_ready(slot));
        CHECK(scheduler.start_upload() == slot);
        CHECK(scheduler.finish_upload(slot, true));
        CHECK(scheduler.start_shading() == slot);
        CHECK(scheduler.finish_shading(slot));
    }

    void test_latest_request_and_presentation() {
        pipeline::scheduler scheduler;
        CHECK(scheduler.slot_count() == 3);
        CHECK(!scheduler.has_pending());
        CHECK(scheduler.request_now({1, 7}));
        CHECK(scheduler.has_pending());
        // A newer request replaces the unscheduled mailbox; the old ID is never started.
        CHECK(scheduler.request_now({2, 7}));
        auto slot = scheduler.start_visibility();
        CHECK(slot.has_value());
        CHECK(!scheduler.has_pending());
        CHECK(scheduler.state_of(*slot) == pipeline::phase::visibility);
        run_candidate(scheduler, *slot);
        CHECK(scheduler.latest_complete() == slot);

        auto first = scheduler.present(*slot);
        CHECK(first.displayed);
        CHECK(first.reset_history);
        CHECK(scheduler.presented() == 2);
        auto repeated = scheduler.present(*slot);
        CHECK(!repeated.displayed);
        CHECK(!repeated.reset_history);
        CHECK(scheduler.retire(*slot));
        CHECK(scheduler.state_of(*slot) == pipeline::phase::empty);
    }

    void test_no_free_slot_keeps_mailbox() {
        pipeline::scheduler scheduler;
        CHECK(scheduler.request_now({10, 1}));
        auto first = scheduler.start_visibility();
        CHECK(first.has_value());
        CHECK(scheduler.request_now({11, 1}));
        auto second = scheduler.start_visibility();
        CHECK(second.has_value());
        CHECK(scheduler.request_now({12, 1}));
        auto third = scheduler.start_visibility();
        CHECK(third.has_value());
        // All three slots are busy; a fourth request stays in the mailbox...
        CHECK(scheduler.request_now({13, 1}));
        CHECK(!scheduler.start_visibility());
        CHECK(scheduler.has_pending());
        // Finishing and retiring one slot lets the pending request in.
        run_candidate(scheduler, *first);
        CHECK(scheduler.retire(*first));
        auto fourth = scheduler.start_visibility();
        CHECK(fourth == first);
        CHECK(!scheduler.has_pending());
    }

    void test_monotonic_presentation_and_stale_generation() {
        pipeline::scheduler scheduler;
        CHECK(scheduler.request_now({1, 1}));
        auto old_slot = scheduler.start_visibility();
        CHECK(old_slot.has_value());
        run_candidate(scheduler, *old_slot);
        CHECK(scheduler.latest_complete() == old_slot);

        // A scene-generation change invalidates the already-complete candidate for display, but
        // its slot is not cancelled; it can be retired after the new generation is produced.
        CHECK(scheduler.request_now({2, 2}));
        CHECK(scheduler.stale(*old_slot));
        CHECK(!scheduler.latest_complete().has_value());
        auto new_slot = scheduler.start_visibility();
        CHECK(new_slot.has_value());
        run_candidate(scheduler, *new_slot);
        CHECK(scheduler.latest_complete() == new_slot);
        auto result = scheduler.present(*new_slot);
        CHECK(result.displayed);
        CHECK(result.reset_history);
        CHECK(!scheduler.present(*old_slot).displayed);
        CHECK(scheduler.retire(*old_slot));
    }

    void test_upload_is_single_flight_and_failure_retires() {
        pipeline::scheduler scheduler;
        CHECK(scheduler.request_now({1, 1}));
        auto first = scheduler.start_visibility();
        CHECK(first.has_value());
        CHECK(scheduler.request_now({2, 1}));
        auto second = scheduler.start_visibility();
        CHECK(second.has_value());
        CHECK(scheduler.feedback_ready(*first));
        CHECK(scheduler.feedback_ready(*second));
        CHECK(scheduler.start_upload() == first);
        CHECK(!scheduler.start_upload());
        CHECK(scheduler.finish_upload(*first, false));
        CHECK(scheduler.state_of(*first) == pipeline::phase::retiring);
        CHECK(scheduler.retire(*first));
        CHECK(scheduler.start_upload() == second);
        CHECK(scheduler.finish_upload(*second, true));
    }

    void test_shading_is_serialized() {
        pipeline::scheduler scheduler;
        CHECK(scheduler.request_now({1, 1}));
        auto first = scheduler.start_visibility();
        CHECK(first.has_value());
        run_candidate(scheduler, *first);
        CHECK(scheduler.request_now({2, 1}));
        auto second = scheduler.start_visibility();
        CHECK(second.has_value());
        CHECK(scheduler.feedback_ready(*second));
        CHECK(scheduler.start_upload() == second);
        CHECK(scheduler.finish_upload(*second, true));
        // The second can shade while the first is presentable; only one shading is active.
        CHECK(scheduler.start_shading() == second);
        CHECK(!scheduler.start_shading().has_value());
        CHECK(scheduler.finish_shading(*second));
        CHECK(scheduler.latest_complete() == second);
    }
} // namespace

int main() {
    test_latest_request_and_presentation();
    test_no_free_slot_keeps_mailbox();
    test_monotonic_presentation_and_stale_generation();
    test_upload_is_single_flight_and_failure_retires();
    test_shading_is_serialized();
    return deren::vk_test::finish("test_pipelined_residency");
}
