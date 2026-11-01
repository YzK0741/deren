// Headless unit test: deren.engine.profiling - the CPU phase window and the RAII scope timer.
//
// PURE CPU, deliberately: the module's GPU half (per-pass timestamps) is a separate step, and nothing
// here needs a device, a window or an RHI call. The window's contract is what this pins: a frame only
// becomes a published measurement at the folding phase (submit), the report appears once per
// window_length frames, and a phase name marked with '*' is a SUB-phase (adding it to a total would
// count that time twice).
#include "vk_test.h"

import deren.engine.profiling;

#include <chrono>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <type_traits>

using namespace std::chrono_literals;

namespace {
    using deren::engine::profiling::cpu_phase;
    using deren::engine::profiling::cpu_phase_names;
    using deren::engine::profiling::cpu_phase_timer;
    using deren::engine::profiling::cpu_phases;

    /// a view of the module's string without copying it (vessel::string is a basic_string, not a std one)
    std::string_view text_of(deren::utility::vessel::string const& value) {
        return std::string_view{value.c_str(), value.size()};
    }

    constexpr std::size_t index_of(cpu_phase const phase) noexcept {
        return static_cast<std::size_t>(phase);
    }

    void test_phase_table() {
        CHECK(cpu_phase_names.size() == index_of(cpu_phase::count));
        CHECK(cpu_phase_names[index_of(cpu_phase::pace)] == std::string_view{"pace"});
        CHECK(cpu_phase_names[index_of(cpu_phase::submit)] == std::string_view{"submit"});
        // the five SUB-phases carry the marker the total's exclusion rule reads
        CHECK(cpu_phase_names[index_of(cpu_phase::cull)].ends_with("*"));
        CHECK(cpu_phase_names[index_of(cpu_phase::cluster)].ends_with("*"));
        CHECK(cpu_phase_names[index_of(cpu_phase::shadow)].ends_with("*"));
        CHECK(cpu_phase_names[index_of(cpu_phase::submit_queue)].ends_with("*"));
        CHECK(cpu_phase_names[index_of(cpu_phase::present)].ends_with("*"));
        std::size_t marked = 0;
        for (auto const name : cpu_phase_names) {
            if (name.ends_with("*")) {
                ++marked;
            }
        }
        CHECK(marked == 5u);
        // ... and a full phase must NOT carry it, or the total would silently drop it
        CHECK(!cpu_phase_names[index_of(cpu_phase::scene)].ends_with("*"));
        CHECK(!cpu_phase_names[index_of(cpu_phase::post)].ends_with("*"));
    }

    void test_disabled_window() {
        cpu_phases phases;
        phases.set_enabled(false);
        CHECK(text_of(phases.summary()) == std::string_view{"cpu timings: off ([render] gpu_timings = false)"});
        // end() is gated: a disabled measurement accumulates nothing and publishes nothing
        phases.end(cpu_phase::submit, std::chrono::steady_clock::now() - 1ms);
        CHECK(text_of(phases.summary()) == std::string_view{"cpu timings: off ([render] gpu_timings = false)"});
        CHECK(phases.means()[index_of(cpu_phase::submit)] == 0.0);
    }

    void test_collecting_and_fold() {
        cpu_phases phases;
        CHECK(text_of(phases.summary()) == std::string_view{"cpu: collecting..."});
        CHECK(phases.means()[index_of(cpu_phase::pace)] == 0.0);

        // add() accumulates the frame being measured, but only end(submit) folds it into the window:
        // a frame that never reaches submit keeps no partial numbers.
        phases.add(cpu_phase::pace, 1500us);
        phases.add(cpu_phase::scene, 2500us);
        CHECK(phases.means()[index_of(cpu_phase::pace)] == 0.0);
        phases.end(cpu_phase::submit, std::chrono::steady_clock::now());
        auto const means = phases.means();
        CHECK(std::abs(means[index_of(cpu_phase::pace)] - 1.5) < 1e-9);
        CHECK(std::abs(means[index_of(cpu_phase::scene)] - 2.5) < 1e-9);
        CHECK(means[index_of(cpu_phase::shadow)] == 0.0);
    }

    void test_window_rollover() {
        cpu_phases phases;
        for (int i = 0; i < static_cast<int>(cpu_phases::window_length); ++i) {
            phases.add(cpu_phase::pace, 1000us); // 1.0 ms per frame
            phases.end(cpu_phase::submit, std::chrono::steady_clock::now());
        }
        // window_length frames close a window: the label is published and the accumulation resets
        auto const label = phases.summary();
        auto const label_view = text_of(label);
        CHECK(label_view.find("cpu (60f):") != std::string_view::npos);
        CHECK(label_view.find("total") != std::string_view::npos);
        CHECK(phases.means()[index_of(cpu_phase::pace)] == 0.0);
    }

    void test_scope_timer() {
        static_assert(!std::is_copy_constructible_v<cpu_phase_timer>, "the timer owns one measurement");
        static_assert(!std::is_copy_assignable_v<cpu_phase_timer>);
        static_assert(!std::is_move_constructible_v<cpu_phase_timer>);
        static_assert(!std::is_move_assignable_v<cpu_phase_timer>);

        cpu_phases phases;
        {
            cpu_phase_timer const scope{phases, cpu_phase::scene};
            CHECK(text_of(phases.summary()) == std::string_view{"cpu: collecting..."});
        }
        // scene is not the folding phase, so a whole scope still leaves the window at zero completed frames
        CHECK(text_of(phases.summary()) == std::string_view{"cpu: collecting..."});

        for (int i = 0; i < static_cast<int>(cpu_phases::window_length); ++i) {
            cpu_phase_timer const scope{phases, cpu_phase::submit};
        }
        auto const label = phases.summary();
        CHECK(text_of(label).find("cpu (60f):") != std::string_view::npos);
    }
} // namespace

int32_t main() {
    test_phase_table();
    test_disabled_window();
    test_collecting_and_fold();
    test_window_rollover();
    test_scope_timer();
    return deren::vk_test::finish("test_profiling");
}
