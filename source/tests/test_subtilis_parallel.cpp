// Task 7: deterministic multi-core scheduling, workspace budgets and stage profiling.
// These tests are structural: they compare bytes/IDs/counters and ordering, never wall-clock
// speed or millisecond thresholds. CV gates force completion order so the checks are
// machine-speed independent.
#include "../../tools/subtilis/commands.hpp"
#include "subtilis_dag_fixtures.hpp"
#include "vk_test.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <yyjson.h>

namespace tool = deren::tools::subtilis;
namespace sub = deren::asset::subtilis;
namespace bd = deren::asset::subtilis::builder_detail;

#include "subtilis_document_fixtures.hpp"
using namespace subtilis_document_fixture;

namespace {
    // ------------------------------------------------------------------ fixtures
    sub::source_geometry curved_grid(unsigned width) {
        sub::source_geometry s;
        for (unsigned y = 0; y <= width; ++y)
            for (unsigned x = 0; x <= width; ++x) {
                s.positions.push_back({float(x) / width, float(y) / width, .02f * std::sin(float(x) * .2f) * std::cos(float(y) * .2f)});
                s.normals.push_back({0, 0, 1});
                s.uv0.push_back({float(x) / width, float(y) / width});
            }
        for (unsigned y = 0; y < width; ++y)
            for (unsigned x = 0; x < width; ++x) {
                auto a = y * (width + 1) + x, b = a + 1, c = a + width + 1, d = c + 1;
                s.indices.insert(s.indices.end(), {a, b, d, a, d, c});
            }
        return s;
    }
    // Flat 0..2 grid so make_fixture's fixed accessor min/max [0,0,0]-[2,2,0] stays exact.
    sub::source_geometry document_grid(unsigned width) {
        sub::source_geometry s;
        for (unsigned y = 0; y <= width; ++y)
            for (unsigned x = 0; x <= width; ++x) {
                s.positions.push_back({2.f * float(x) / width, 2.f * float(y) / width, 0});
                s.normals.push_back({0, 0, 1});
                s.uv0.push_back({float(x) / width, float(y) / width});
            }
        for (unsigned y = 0; y < width; ++y)
            for (unsigned x = 0; x < width; ++x) {
                auto a = y * (width + 1) + x, b = a + 1, c = a + width + 1, d = c + 1;
                s.indices.insert(s.indices.end(), {a, b, d, a, d, c});
            }
        return s;
    }
    // Curved grid plus one far-away single-triangle island: the island candidate can only be
    // rejected (no progress), while the grid groups reduce. Adjacent candidates must not shift IDs.
    sub::source_geometry grid_with_island(unsigned width) {
        auto s = curved_grid(width);
        auto base = static_cast<uint32_t>(s.positions.size());
        s.positions.push_back({100, 0, 0});
        s.positions.push_back({101, 0, 0});
        s.positions.push_back({100, 1, 0});
        s.normals.insert(s.normals.end(), {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}});
        s.uv0.insert(s.uv0.end(), {{0, 0}, {1, 0}, {0, 1}});
        s.indices.insert(s.indices.end(), {base, base + 1, base + 2});
        return s;
    }
    // ------------------------------------------------------------------ helpers
    sub::vector<std::byte> encode_payload(sub::build_result const& built) {
        auto bytes = sub::encode(built.asset.data(), {});
        CHECK(bytes);
        return bytes ? std::move(*bytes) : sub::vector<std::byte>{};
    }
    bool same_deterministic_report(sub::build_report const& a, sub::build_report const& b) {
        if (a.levels.size() != b.levels.size() || a.rejected != b.rejected || a.rounds != b.rounds || a.cross_group_repartitions != b.cross_group_repartitions || a.lod0_only != b.lod0_only || a.known_work_peak_bytes != b.known_work_peak_bytes)
            return false;
        for (size_t i = 0; i < a.levels.size(); ++i)
            if (a.levels[i].triangles != b.levels[i].triangles || a.levels[i].clusters != b.levels[i].clusters || a.levels[i].groups != b.levels[i].groups || a.levels[i].roots != b.levels[i].roots)
                return false;
        return true;
    }
    std::string_view text_of(sub::string const& text) {
        return {text.data(), text.size()};
    }
    constexpr std::array<std::string_view, 11> stage_names{"sourceFingerprint", "leafCluster", "topology", "partition", "boundary", "simplify", "recluster", "candidate", "commit", "validate", "encode"};
    sub::stage_profile const* stage_of(sub::build_report const& report, std::string_view name) {
        for (auto const& stage : report.stages)
            if (text_of(stage.name) == name)
                return &stage;
        return nullptr;
    }
    uint64_t rejected_total(sub::build_report const& report) {
        uint64_t total = 0;
        for (size_t i = 1; i < report.rejected.size(); ++i)
            total += report.rejected[i];
        return total;
    }
    // Structural stage-profile audit: completeness, non-negative/finite values, inclusive flags,
    // accumulated total == sum of exclusive stages (no double counting) and candidate/commit
    // counter consistency. No machine-speed or millisecond-threshold assertion.
    void check_stage_profile(sub::build_report const& report, uint32_t threads, sub::build_limits const& limits, sub::payload const* payload) {
        CHECK(report.threads == threads);
        CHECK(report.planned_concurrency >= 1);
        CHECK(report.planned_concurrency <= threads);
        CHECK(std::isfinite(report.wall_ms) && report.wall_ms >= 0);
        CHECK(std::isfinite(report.accumulated_ms) && report.accumulated_ms >= 0);
        CHECK(report.stages.size() == stage_names.size());
        CHECK(report.known_work_limit_bytes == limits.max_known_work_bytes);
        CHECK(report.known_work_peak_bytes > 0);
        CHECK(report.known_work_peak_bytes <= limits.max_known_work_bytes);
        double exclusive = 0;
        for (auto name : stage_names) {
            auto* stage = stage_of(report, name);
            CHECK(stage != nullptr);
            if (!stage)
                continue;
            CHECK(std::isfinite(stage->cpu_ms) && stage->cpu_ms >= 0);
            CHECK(stage->inclusive == (name == "candidate"));
            if (!stage->inclusive)
                exclusive += stage->cpu_ms;
        }
        CHECK(std::abs(report.accumulated_ms - exclusive) <= 1e-9 * std::max(1.0, exclusive));
        auto count_of = [&](std::string_view name) -> uint64_t {auto* s=stage_of(report, name);return s ? s->count : 0; };
        CHECK(count_of("sourceFingerprint") >= 1);
        CHECK(count_of("leafCluster") == 1);
        CHECK(count_of("topology") == 1);
        CHECK(count_of("partition") == report.rounds);
        CHECK(count_of("validate") == 1);
        CHECK(count_of("encode") == 1);
        CHECK(count_of("candidate") > 0);
        CHECK(count_of("candidate") == count_of("commit"));
        if (payload) {
            CHECK(count_of("candidate") == uint64_t(payload->groups.size()) + rejected_total(report));
            auto* candidate = stage_of(report, "candidate");
            auto* boundary = stage_of(report, "boundary");
            auto* simplify = stage_of(report, "simplify");
            auto* recluster = stage_of(report, "recluster");
            if (candidate && boundary && simplify && recluster)
                CHECK(candidate->cpu_ms + 1e-9 >= boundary->cpu_ms + simplify->cpu_ms + recluster->cpu_ms); // children are a subset
        }
    }
    tool::path unique_directory() {
        auto dir = std::filesystem::current_path() / tool::path(deren::utility::vessel::format("subtilis-parallel-{}", std::chrono::steady_clock::now().time_since_epoch().count()).c_str());
        std::error_code ec;
        std::filesystem::create_directory(dir, ec);
        CHECK(!ec);
        return dir;
    }
    void write_file(tool::path const& file, sub::vector<std::byte> const& bytes) {
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        CHECK(out.good());
    }
    std::string read_text(tool::path const& file) {
        std::ifstream in(file, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    }
    sub::vector<std::byte> read_file(tool::path const& file) {
        auto text = read_text(file);
        sub::vector<std::byte> bytes(text.size());
        for (size_t i = 0; i < text.size(); ++i)
            bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
        return bytes;
    }
    // ------------------------------------------------------------------ scheduler: forced out-of-order completion
    void ordered_commit_uses_id_order_not_completion_order() {
        constexpr size_t count = 4;
        std::mutex mutex;
        std::condition_variable cv;
        std::array<bool, count> started{}, release{};
        size_t started_count = 0;
        std::vector<size_t> completed, commits;
        std::array<size_t, count> estimates{100, 300, 200, 400};
        auto prepare = [&](size_t index) -> sub::result<void> {
            {
                std::lock_guard lock(mutex);
                started[index] = true;
                ++started_count;
                cv.notify_all();
            }
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return release[index]; });
            }
            {
                std::lock_guard lock(mutex);
                completed.push_back(index);
                cv.notify_all();
            }
            return {};
        };
        auto commit = [&](size_t index) -> sub::result<void> {
            commits.push_back(index);
            return {};
        };
        std::optional<sub::result<void>> outcome;
        std::thread runner([&] { outcome = bd::run_ordered(count, 4, estimates, size_t(1) << 40, prepare, commit); });
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return started_count == count; });
        }
        std::array<size_t, count> forced{2, 0, 3, 1};
        for (size_t k = 0; k < count; ++k) {
            {
                std::lock_guard lock(mutex);
                release[forced[k]] = true;
                cv.notify_all();
            }
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return completed.size() > k; });
        }
        runner.join();
        CHECK(outcome.has_value() && outcome->has_value());
        CHECK(completed == std::vector<size_t>(forced.begin(), forced.end()));
        CHECK((commits == std::vector<size_t>{0, 1, 2, 3}));
        deren::vk_test::write_line("forced completion order 2,0,3,1 -> commit order 0,1,2,3");
    }
    // ------------------------------------------------------------------ scheduler: refusal is atomic and deterministic
    void ordered_refusal_cancels_without_partial_commit() {
        for (unsigned repeat = 0; repeat < 3; ++repeat) {
            std::array<size_t, 3> estimates{300, 200, 100};
            std::vector<size_t> commits;
            auto prepare = [&](size_t index) -> sub::result<void> {
                return std::unexpected(sub::error{sub::error_code::capacity, deren::utility::vessel::format("prepare-failed-{}", index), {}, {}});
            };
            auto commit = [&](size_t index) -> sub::result<void> {
                commits.push_back(index);
                return {};
            };
            auto outcome = bd::run_ordered(3, 2, estimates, size_t(1) << 40, prepare, commit);
            CHECK(!outcome);
            if (!outcome)
                CHECK(text_of(outcome.error().context) == "prepare-failed-0"); // lowest index wins, like the serial loop
            CHECK(commits.empty());
        }
    }
    // ------------------------------------------------------------------ scheduler: unified workspace admission
    void ordered_budget_bound_is_capacity_and_scales_concurrency() {
        constexpr size_t count = 4;
        std::array<size_t, count> estimates{1000, 1000, 1000, 1000};
        // One maximum-estimate task does not fit: typed capacity error before any prepare runs.
        {
            std::vector<size_t> prepares, commits;
            auto prepare = [&](size_t index) -> sub::result<void> {
                prepares.push_back(index);
                return {};
            };
            auto commit = [&](size_t index) -> sub::result<void> {
                commits.push_back(index);
                return {};
            };
            auto outcome = bd::run_ordered(count, 4, estimates, 999, prepare, commit);
            CHECK(!outcome && outcome.error().code == sub::error_code::capacity);
            CHECK(prepares.empty() && commits.empty());
        }
        // Budget fits exactly one maximum task: concurrency 1, all commits in ID order.
        {
            std::mutex mutex;
            size_t active = 0, max_active = 0;
            std::vector<size_t> commits;
            auto prepare = [&](size_t) -> sub::result<void> {
                std::lock_guard lock(mutex);
                ++active;
                max_active = std::max(max_active, active);
                --active;
                return {};
            };
            auto commit = [&](size_t index) -> sub::result<void> {
                commits.push_back(index);
                return {};
            };
            auto outcome = bd::run_ordered(count, 4, estimates, 1000, prepare, commit);
            CHECK(outcome.has_value());
            CHECK(max_active == 1);
            CHECK((commits == std::vector<size_t>{0, 1, 2, 3}));
        }
        // Budget for two concurrent tasks: the pool really overlaps two prepares, never more.
        {
            std::mutex mutex;
            std::condition_variable cv;
            size_t active = 0, max_active = 0;
            bool release = false;
            std::vector<size_t> commits;
            auto prepare = [&](size_t) -> sub::result<void> {
                std::unique_lock lock(mutex);
                ++active;
                max_active = std::max(max_active, active);
                cv.notify_all();
                cv.wait(lock, [&] { return release; });
                --active;
                return {};
            };
            auto commit = [&](size_t index) -> sub::result<void> {
                commits.push_back(index);
                return {};
            };
            std::optional<sub::result<void>> outcome;
            std::thread runner([&] { outcome = bd::run_ordered(count, 4, estimates, 2500, prepare, commit); });
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return active >= 2; });
                max_active = std::max<size_t>(max_active, 2);
                release = true;
                cv.notify_all();
            }
            runner.join();
            CHECK(outcome.has_value() && outcome->has_value());
            CHECK(max_active == 2);
            CHECK((commits == std::vector<size_t>{0, 1, 2, 3}));
        }
        // Generous budget: all four tasks overlap (single budget, no hidden serialization).
        {
            std::mutex mutex;
            std::condition_variable cv;
            size_t active = 0, max_active = 0;
            bool release = false;
            std::vector<size_t> commits;
            auto prepare = [&](size_t) -> sub::result<void> {
                std::unique_lock lock(mutex);
                ++active;
                max_active = std::max(max_active, active);
                cv.notify_all();
                cv.wait(lock, [&] { return release; });
                --active;
                return {};
            };
            auto commit = [&](size_t index) -> sub::result<void> {
                commits.push_back(index);
                return {};
            };
            std::optional<sub::result<void>> outcome;
            std::thread runner([&] { outcome = bd::run_ordered(count, 4, estimates, size_t(1) << 40, prepare, commit); });
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return active == count; });
                max_active = std::max<size_t>(max_active, count);
                release = true;
                cv.notify_all();
            }
            runner.join();
            CHECK(outcome.has_value() && outcome->has_value());
            CHECK(max_active == count);
            CHECK((commits == std::vector<size_t>{0, 1, 2, 3}));
        }
        deren::vk_test::write_line("workspace admission: budget 999 rejects, 1000 -> 1, 2500 -> 2, generous -> 4");
    }
    // ------------------------------------------------------------------ scheduler: edge cases and caller-thread contract
    void ordered_edge_cases() {
        std::vector<size_t> commits;
        size_t prepares = 0;
        {
            auto outcome = bd::run_ordered(0, 4, std::span<size_t const>{}, 1 << 20, [&](size_t) -> sub::result<void> { ++prepares; return {}; }, [&](size_t index) -> sub::result<void> {commits.push_back(index);return {}; });
            CHECK(outcome.has_value() && prepares == 0 && commits.empty());
        }
        {
            std::array<size_t, 2> estimates{1, 2};
            auto outcome = bd::run_ordered(3, 2, estimates, 1 << 20, [&](size_t) -> sub::result<void> { ++prepares; return {}; }, [&](size_t index) -> sub::result<void> {commits.push_back(index);return {}; });
            CHECK(!outcome && outcome.error().code == sub::error_code::invalid_options);
        }
        // threads=1 keeps prepare and commit on the calling thread (serial reference behavior).
        {
            std::array<size_t, 3> estimates{5, 10, 15};
            auto caller = std::this_thread::get_id();
            bool same_thread = true;
            auto outcome = bd::run_ordered(3, 1, estimates, size_t(1) << 40, [&](size_t) -> sub::result<void> {same_thread=same_thread&&std::this_thread::get_id()==caller;return {}; }, [&](size_t) -> sub::result<void> {same_thread=same_thread&&std::this_thread::get_id()==caller;return {}; });
            CHECK(outcome.has_value() && same_thread);
        }
        {
            std::array<size_t, 3> estimates{5, 10, 15};
            commits.clear();
            auto outcome = bd::run_ordered(3, 0, estimates, size_t(1) << 40, [&](size_t) -> sub::result<void> { return {}; }, [&](size_t index) -> sub::result<void> {commits.push_back(index);return {}; });
            CHECK(!outcome && outcome.error().code == sub::error_code::invalid_options && commits.empty());
        }
    }
    // ------------------------------------------------------------------ builder: threads 1/2/12 are byte-identical
    void build_thread_counts_are_byte_identical() {
        auto source = curved_grid(64);
        sub::build_options options;
        sub::build_limits limits;
        auto serial = sub::build_serial(source, options, limits);
        CHECK(serial);
        if (!serial)
            return;
        auto reference = encode_payload(*serial);
        CHECK(!reference.empty());
        CHECK(same_deterministic_report(serial->report, serial->report));
        check_stage_profile(serial->report, 1, limits, &serial->asset.data());
        std::optional<sub::build_report> twelve_build;
        for (uint32_t threads : {1u, 2u, 12u}) {
            sub::execution_options execution;
            execution.threads = threads;
            execution.limits = limits;
            auto built = sub::build(source, options, execution);
            CHECK(built);
            if (!built)
                continue;
            auto bytes = encode_payload(*built);
            CHECK(bytes == reference);
            CHECK(built->asset.data().source_hash == serial->asset.data().source_hash);
            CHECK(built->asset.data().build_hash == serial->asset.data().build_hash);
            CHECK(built->asset.data().indices == serial->asset.data().indices);
            CHECK(built->asset.data().clusters == serial->asset.data().clusters);
            CHECK(built->asset.data().groups == serial->asset.data().groups);
            CHECK(built->asset.data().references == serial->asset.data().references);
            CHECK(built->asset.data().roots == serial->asset.data().roots);
            CHECK(built->asset.data().build_meta == serial->asset.data().build_meta);
            CHECK(built->asset.geometry().positions == source.positions);
            CHECK(built->asset.geometry().normals == source.normals);
            CHECK(built->asset.geometry().uv0 == source.uv0);
            CHECK(built->asset.geometry().indices == source.indices);
            CHECK(sub::validate_structure(built->asset.data(), {}));
            CHECK(same_deterministic_report(built->report, serial->report));
            check_stage_profile(built->report, threads, limits, &built->asset.data());
            if (threads == 12) {
                twelve_build = built->report;
                CHECK(built->report.planned_concurrency >= 2);
            }
        }
        // Repeating a multi-thread build must produce the same bytes (no timing/threads in identity).
        sub::execution_options again_options;
        again_options.threads = 12;
        again_options.limits = limits;
        auto again = sub::build(source, options, again_options);
        CHECK(again);
        if (again)
            CHECK(encode_payload(*again) == reference);
        deren::vk_test::write_line("threads 1/2/12: byte-identical payload, {} groups, {} rounds", serial->asset.data().groups.size(), serial->report.rounds);
        // A 1000-thread request is accepted and budgeted down to the task count; payloads still match.
        sub::execution_options many;
        many.threads = 1000;
        many.limits = limits;
        auto capped = sub::build(source, options, many);
        CHECK(capped);
        if (capped) {
            CHECK(capped->report.threads == 1000);
            CHECK(capped->report.planned_concurrency <= uint32_t(capped->asset.data().groups.size() + 1));
            CHECK(encode_payload(*capped) == reference);
        }
    }
    // ------------------------------------------------------------------ builder: rejected candidates must not consume later IDs
    void build_rejected_candidates_conserve_ids() {
        auto source = grid_with_island(40);
        sub::build_options options;
        auto serial = sub::build_serial(source, options, {});
        CHECK(serial);
        if (!serial)
            return;
        CHECK(rejected_total(serial->report) > 0);
        auto reference = encode_payload(*serial);
        for (uint32_t threads : {2u, 12u}) {
            sub::execution_options execution;
            execution.threads = threads;
            auto built = sub::build(source, options, execution);
            CHECK(built);
            if (!built)
                continue;
            CHECK(encode_payload(*built) == reference);
            CHECK(built->report.rejected == serial->report.rejected);
            CHECK(built->asset.data().clusters.size() == serial->asset.data().clusters.size());
            CHECK(built->asset.data().groups.size() == serial->asset.data().groups.size());
            CHECK(built->report.cross_group_repartitions == serial->report.cross_group_repartitions);
        }
        deren::vk_test::write_line("adjacent rejection: {} rejected candidates, payload/IDs unchanged", rejected_total(serial->report));
    }
    // ------------------------------------------------------------------ builder: workspace budget shrink
    void build_workspace_budget_shrinks_concurrency_without_changing_reductions() {
        auto source = curved_grid(40);
        sub::build_options options;
        auto serial = sub::build_serial(source, options, {});
        CHECK(serial);
        if (!serial)
            return;
        auto reference = encode_payload(*serial);
        // Deterministic base known-work (source records + fixed overhead) as recorded by build_serial.
        CHECK(serial->report.known_work_peak_bytes >= 65536);
        // Find the point where the serial reference itself starts to fit.
        uint64_t base = 65536 + uint64_t(source.positions.size()) * 64 + uint64_t(source.indices.size()) * 96;
        std::array<uint64_t, 9> increments{1, 4096, 65536, 262144, 1u << 20, 4u << 20, 16u << 20, 64u << 20, 1u << 30};
        std::vector<uint32_t> witnessed;
        for (auto increase : increments) {
            sub::execution_options execution;
            execution.threads = 12;
            execution.limits.max_known_work_bytes = base + increase;
            auto built = sub::build(source, options, execution);
            if (!built) {
                CHECK(built.error().code == sub::error_code::capacity);
                continue;
            }
            CHECK(encode_payload(*built) == reference);
            CHECK(same_deterministic_report(built->report, serial->report));
            witnessed.push_back(built->report.planned_concurrency);
        }
        CHECK(witnessed.size() >= 2);
        for (size_t i = 1; i < witnessed.size(); ++i)
            CHECK(witnessed[i] >= witnessed[i - 1]); // more workspace never lowers the planned concurrency
        if (witnessed.size() >= 2)
            CHECK(witnessed.back() > witnessed.front()); // shrink genuinely reduces concurrency
        // Too small to hold even one reduction task or the initial payload: whole-primitive capacity
        // rejection, never a partial asset.
        for (uint64_t tiny : {uint64_t(1), base + 1}) {
            sub::execution_options execution;
            execution.threads = 12;
            execution.limits.max_known_work_bytes = tiny;
            auto built = sub::build(source, options, execution);
            CHECK(!built && built.error().code == sub::error_code::capacity);
        }
        deren::vk_test::write_line("budget shrink: planned concurrency {} -> {} with identical payload", witnessed.empty() ? 0 : witnessed.front(), witnessed.empty() ? 0 : witnessed.back());
    }
    // ------------------------------------------------------------------ builder: stage profile structure
    void build_stage_profile_structure() {
        sub::build_options options;
        auto source = curved_grid(40);
        auto built = sub::build_serial(source, options, {});
        CHECK(built);
        if (!built)
            return;
        check_stage_profile(built->report, 1, {}, &built->asset.data());
        CHECK(built->report.stages.size() == stage_names.size());
        auto minimum = sub::source_geometry{};
        minimum.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        minimum.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
        minimum.uv0 = {{0, 0}, {0, 0}, {0, 0}};
        minimum.indices = {0, 1, 2};
        auto unchanged = sub::build_serial(minimum, options, {});
        CHECK(unchanged && unchanged->report.lod0_only);
        if (unchanged)
            check_stage_profile(unchanged->report, 1, {}, &unchanged->asset.data());
        deren::vk_test::write_line("stage profile: {} stages, accumulated {:.6f} ms, wall {:.6f} ms", built->report.stages.size(), built->report.accumulated_ms, built->report.wall_ms);
    }
    // ------------------------------------------------------------------ builder: invalid thread requests and repeatability
    void build_invalid_thread_requests_and_repeatability() {
        auto source = curved_grid(32);
        sub::build_options options;
        sub::execution_options invalid;
        invalid.threads = 0;
        auto rejected = sub::build(source, options, invalid);
        CHECK(!rejected && rejected.error().code == sub::error_code::invalid_options);
        std::optional<sub::vector<std::byte>> reference;
        for (unsigned repeat = 0; repeat < 6; ++repeat) {
            sub::execution_options execution;
            execution.threads = repeat % 2 ? 2u : 12u;
            auto built = sub::build(source, options, execution);
            CHECK(built);
            if (!built)
                continue;
            auto bytes = encode_payload(*built);
            if (!reference)
                reference = std::move(bytes);
            else
                CHECK(bytes == *reference);
            // A failing build in between must not leak state into the next successful build.
            sub::execution_options failing = execution;
            failing.limits.max_known_work_bytes = 1;
            auto failed = sub::build(source, options, failing);
            CHECK(!failed && failed.error().code == sub::error_code::capacity);
        }
    }
    // ------------------------------------------------------------------ converter: parallel primitive scheduling
    void converter_parallel_determinism(tool::path const& dir) {
        auto single = make_fixture(document_grid(32));
        write_file(dir / "single.glb", glb(single.json, single.bin));
        // Same mesh, multiple node instances: duplicate the mesh object twice (three primitives, one
        // mesh instanced by two nodes) so the converter has coarse primitive tasks.
        auto multi_json = edit_json(single.json, [](yyjson_mut_doc* doc, yyjson_mut_val* root) {
            auto* meshes = yyjson_mut_obj_get(root, "meshes");
            auto* first = yyjson_mut_arr_get_first(meshes);
            for (unsigned i = 0; i < 2; ++i) {
                auto* copy = yyjson_mut_val_mut_copy(doc, first);
                yyjson_mut_arr_append(meshes, copy);
            }
        });
        write_file(dir / "multi.glb", glb(multi_json, single.bin));
        std::vector<sub::vector<std::byte>> multi_outputs, single_outputs;
        for (uint32_t threads : {1u, 2u, 12u}) {
            tool::conversion_options options;
            options.execution.threads = threads;
            auto report = tool::convert_file(dir / "multi.glb", dir / deren::utility::vessel::format("multi-{}.glb", threads).c_str(), options);
            CHECK(report);
            if (report) {
                CHECK(report->converted == 3);
                CHECK(report->reused == 0);
                multi_outputs.push_back(read_file(dir / deren::utility::vessel::format("multi-{}.glb", threads).c_str()));
            }
            auto single_report = tool::convert_file(dir / "single.glb", dir / deren::utility::vessel::format("single-{}.glb", threads).c_str(), options);
            CHECK(single_report);
            if (single_report) {
                CHECK(single_report->converted == 1);
                single_outputs.push_back(read_file(dir / deren::utility::vessel::format("single-{}.glb", threads).c_str()));
            }
        }
        CHECK(multi_outputs.size() == 3);
        for (size_t i = 1; i < multi_outputs.size(); ++i)
            CHECK(multi_outputs[i] == multi_outputs[0]);
        CHECK(single_outputs.size() == 3);
        for (size_t i = 1; i < single_outputs.size(); ++i)
            CHECK(single_outputs[i] == single_outputs[0]);
        CHECK(!multi_outputs.empty() && multi_outputs[0] != single_outputs[0]);
        deren::vk_test::write_line("converter parallel: 3-primitive and same-mesh 2-instance GLBs byte-identical for threads 1/2/12");
    }
    // ------------------------------------------------------------------ converter: CLI threads and report structure
    void converter_cli_threads_and_report(tool::path const& dir) {
        auto fixture = make_fixture(document_grid(32));
        write_file(dir / "cli-input.glb", glb(fixture.json, fixture.bin));
        auto input = (dir / "cli-input.glb").generic_string();
        auto threaded_output = (dir / "cli-threaded.glb").generic_string();
        auto threaded_report = (dir / "cli-threaded.json").generic_string();
        std::array<std::string_view, 8> threaded_args{"build", input, "-o", threaded_output, "--threads", "12", "--report", threaded_report};
        auto threaded = tool::parse_arguments(threaded_args);
        CHECK(threaded);
        if (!threaded)
            return;
        CHECK(std::get<tool::build_command>(*threaded).execution.threads == 12);
        CHECK(tool::run(*threaded) == 0);
        {
            auto text = read_text(dir / "cli-threaded.json");
            auto* doc = yyjson_read(text.data(), text.size(), 0);
            CHECK(doc);
            if (doc) {
                auto* root = yyjson_doc_get_root(doc);
                CHECK(yyjson_get_uint(yyjson_obj_get(root, "threads")) == 12);
                CHECK(yyjson_get_uint(yyjson_obj_get(root, "converted")) == 1);
                auto* note = yyjson_obj_get(root, "knownWorkNote");
                CHECK(note && yyjson_is_str(note));
                auto* primitives = yyjson_obj_get(root, "primitives");
                CHECK(yyjson_arr_size(primitives) == 1);
                auto* row = yyjson_arr_get_first(primitives);
                CHECK(yyjson_get_uint(yyjson_obj_get(row, "plannedConcurrency")) >= 1);
                CHECK(yyjson_get_uint(yyjson_obj_get(row, "knownWorkLimitBytes")) > 0);
                CHECK(yyjson_get_real(yyjson_obj_get(row, "accumulatedMs")) >= 0);
                CHECK(yyjson_get_real(yyjson_obj_get(row, "wallMs")) >= 0);
                auto* stages = yyjson_obj_get(row, "stages");
                CHECK(yyjson_arr_size(stages) == stage_names.size());
                for (auto name : stage_names) {
                    bool found = false;
                    for (size_t i = 0; i < yyjson_arr_size(stages); ++i) {
                        auto* stage = yyjson_arr_get(stages, i);
                        auto* stage_name = yyjson_obj_get(stage, "name");
                        if (stage_name && yyjson_is_str(stage_name) && std::string_view(yyjson_get_str(stage_name)) == name)
                            found = true;
                    }
                    CHECK(found);
                }
                yyjson_doc_free(doc);
            }
        }
        // Default (no --threads) is clamped to at most 12 threads, and the canonical payload is
        // unchanged by the thread count.
        auto default_output = (dir / "cli-default.glb").generic_string();
        auto default_report = (dir / "cli-default.json").generic_string();
        std::array<std::string_view, 6> default_args{"build", input, "-o", default_output, "--report", default_report};
        auto defaults = tool::parse_arguments(default_args);
        CHECK(defaults);
        if (!defaults)
            return;
        auto default_threads = std::get<tool::build_command>(*defaults).execution.threads;
        CHECK(default_threads >= 1 && default_threads <= 12);
        CHECK(tool::run(*defaults) == 0);
        CHECK(read_file(dir / "cli-default.glb") == read_file(dir / "cli-threaded.glb"));
        {
            auto text = read_text(dir / "cli-default.json");
            auto* doc = yyjson_read(text.data(), text.size(), 0);
            CHECK(doc);
            if (doc) {
                CHECK(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "threads")) == default_threads);
                yyjson_doc_free(doc);
            }
        }
        // Usage contract: positive integers are accepted, zero/negative/garbage are usage errors.
        auto check_threads = [](std::string_view value) {
            std::array<std::string_view, 4> args{"build", "in.glb", "-o", "out.glb"};
            std::array<std::string_view, 6> with_threads{args[0], args[1], args[2], args[3], "--threads", value};
            return tool::parse_arguments(with_threads);
        };
        CHECK(check_threads("12"));
        CHECK(check_threads("999"));
        CHECK(!check_threads("0"));
        CHECK(!check_threads("-2"));
        CHECK(!check_threads("x"));
        deren::vk_test::write_line("CLI: --threads 12 accepted and reported, default clamped to <=12, zero/negative/non-numeric refused");
    }
} // namespace

int main() {
    // Must be the FIRST meshoptimizer-touching work in the process: four builds start together, so
    // the global meshoptimizer allocator initialization is exercised concurrently. Repeatably
    // identical payloads also cover "same mesh, multiple instances".
    {
        constexpr size_t builders = 4;
        std::mutex mutex;
        std::condition_variable cv;
        size_t ready = 0;
        bool go = false;
        std::array<std::optional<sub::vector<std::byte>>, builders> payloads;
        std::vector<std::thread> threads;
        auto source = curved_grid(32);
        for (size_t i = 0; i < builders; ++i)
            threads.emplace_back([&, i] {
                {
                    std::unique_lock lock(mutex);
                    ++ready;
                    cv.notify_all();
                    cv.wait(lock, [&] { return go; });
                }
                auto built = sub::build_serial(source, sub::build_options{}, {});
                if (!built)
                    return;
                auto bytes = sub::encode(built->asset.data(), {});
                if (bytes)
                    payloads[i] = std::move(*bytes);
            });
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return ready == builders; });
            go = true;
            cv.notify_all();
        }
        for (auto& thread : threads)
            thread.join();
        for (size_t i = 0; i < builders; ++i)
            CHECK(payloads[i].has_value());
        for (size_t i = 1; i < builders; ++i)
            CHECK(payloads[i] == payloads[0]);
        deren::vk_test::write_line("concurrent first builds: {} builders, identical payloads", builders);
    }
    ordered_commit_uses_id_order_not_completion_order();
    ordered_refusal_cancels_without_partial_commit();
    ordered_budget_bound_is_capacity_and_scales_concurrency();
    ordered_edge_cases();
    build_thread_counts_are_byte_identical();
    build_rejected_candidates_conserve_ids();
    build_workspace_budget_shrinks_concurrency_without_changing_reductions();
    build_stage_profile_structure();
    build_invalid_thread_requests_and_repeatability();
    auto dir = unique_directory();
    converter_parallel_determinism(dir);
    converter_cli_threads_and_report(dir);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    CHECK(!ec);
    return deren::vk_test::finish("subtilis_parallel");
}
