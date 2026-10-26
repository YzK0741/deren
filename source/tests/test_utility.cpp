// Headless unit tests: utility module (pure CPU) ===============================
// Covers xxh3 content hashing (drives image dedup), data_block key semantics,
// the thread pool, and the BVH frustum culling used by the main pass.
#include "../utility/shared/log_test_hooks.hpp"
#include "../utility/shared/memory_test_hooks.hpp"
#include "vessel_child.hpp"
#include "vk_test.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iterator>
#include <memory>
#include <semaphore>
#include <span>
#include <thread>
#include <vector>

import deren.utility.shared;
import deren.utility;

namespace {
    // Compile-time self-checks: data_block is fully constexpr (zero-init default + FNV-1a).
    // FNV-1a-64 golden vectors: {1,2,3,4} -> 13725386680924731485, {0,0,0,0} -> 5558979605539197941.
    constexpr uint8_t golden_bytes[] = {1, 2, 3, 4};
    static_assert(deren::utility::data_block<4>(golden_bytes).hash64() == 13725386680924731485ull);
    static_assert(deren::utility::data_block<4>().hash64() == 5558979605539197941ull); // default = zeroed
    static_assert(deren::utility::data_block<4>(golden_bytes) == deren::utility::data_block<4>(golden_bytes));

    // deren::utility::write_png is pure CPU, so the dependency-free PNG encoder is testable headlessly:
    // the test walks the chunk list and re-checks every CRC32 (a wrong encoder would not survive
    // a real decoder, but structure + checksums already catch the usual mistakes).
    void test_write_png() {
        deren::utility::vessel::vector<uint8_t> const pixels = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}; // 2x2
        std::filesystem::path const path = "test_write_png.png";
        auto const written = deren::utility::write_png(path, 2, 2, pixels);
        CHECK(written.has_value());
        if (!written.has_value()) {
            return;
        }

        std::ifstream file(path, std::ios::binary);
        CHECK(file.good());
        deren::utility::vessel::vector<uint8_t> const bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
        CHECK(bytes.size() > 16);

        uint8_t const signature[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        CHECK(std::equal(std::begin(signature), std::end(signature), bytes.begin()));

        auto const read_be32 = [&bytes](std::size_t const at) {
            return (static_cast<uint32_t>(bytes[at]) << 24) | (static_cast<uint32_t>(bytes[at + 1]) << 16) | (static_cast<uint32_t>(bytes[at + 2]) << 8) | static_cast<uint32_t>(bytes[at + 3]);
        };

        bool saw_ihdr = false;
        bool saw_idat = false;
        bool saw_iend = false;
        bool crc_ok = true;
        std::size_t offset = 8;
        while (offset + 12 <= bytes.size()) {
            uint32_t const length = read_be32(offset);
            if (offset + 12 + length > bytes.size()) {
                crc_ok = false;
                break;
            }
            std::string_view const type(reinterpret_cast<char const*>(bytes.data() + offset + 4), 4);

            uint32_t crc = 0xFFFFFFFFu;
            for (std::size_t i = offset + 4; i < offset + 8 + length; ++i) {
                crc ^= bytes[i];
                for (int32_t k = 0; k < 8; ++k) {
                    crc = (crc & 1u) != 0u ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
                }
            }
            crc ^= 0xFFFFFFFFu;
            if (crc != read_be32(offset + 8 + length)) {
                crc_ok = false;
            }

            if (type == "IHDR") {
                saw_ihdr = true;
                CHECK(read_be32(offset + 8) == 2);  // width
                CHECK(read_be32(offset + 12) == 2); // height
            } else if (type == "IDAT") {
                saw_idat = true;
            } else if (type == "IEND") {
                saw_iend = true;
                break;
            }
            offset += 12 + length;
        }
        CHECK(saw_ihdr);
        CHECK(saw_idat);
        CHECK(saw_iend);
        CHECK(crc_ok);
        std::filesystem::remove(path);
    }
    // ---- binary writer (deren::utility::write_binary / write_single) ----
    // The writer is the one primitive every binary format in the project goes through, so these
    // checks pin down its contract: byte order per tagged scalar, exactly size() bytes for a range
    // (no length prefix), native layout for a POD, and a hard stop at the first failure.
    void test_write_binary_scalar_byte_order() {
        deren::utility::vessel::ostringstream out;
        CHECK(deren::utility::write_binary(out,
                                           deren::utility::be(uint16_t{0x0102}),
                                           deren::utility::le(uint16_t{0x0102}),
                                           deren::utility::be(uint32_t{0x01020304}),
                                           deren::utility::le(uint32_t{0x01020304}),
                                           deren::utility::be(uint32_t{0xFFFFFFFE}), // a full-width value, no sign surprises
                                           deren::utility::le(int32_t{-2}))
                  .has_value());
        deren::utility::vessel::string const bytes = out.str();
        deren::utility::vessel::string const expected = deren::utility::vessel::string("\x01\x02", 2) + deren::utility::vessel::string("\x02\x01", 2) +
                                                        deren::utility::vessel::string("\x01\x02\x03\x04", 4) + deren::utility::vessel::string("\x04\x03\x02\x01", 4) +
                                                        deren::utility::vessel::string("\xFF\xFF\xFF\xFE", 4) + deren::utility::vessel::string("\xFE\xFF\xFF\xFF", 4);
        CHECK_MSG(bytes == expected, "big/little endian bytes");
        CHECK(bytes.size() == 20);

        // floats go through the IEEE-754 bit pattern (1.0f == 0x3F800000)
        deren::utility::vessel::ostringstream float_out;
        CHECK(deren::utility::write_binary(float_out, deren::utility::be(1.0f), deren::utility::le(1.0f)).has_value());
        CHECK(deren::utility::vessel::string("\x3F\x80\x00\x00", 4) + deren::utility::vessel::string("\x00\x00\x80\x3F", 4) == float_out.str());

        // a bare (untagged) scalar defaults to little-endian
        deren::utility::vessel::ostringstream plain;
        CHECK(deren::utility::write_binary(plain, uint16_t{0x0102}).has_value());
        CHECK(deren::utility::vessel::string("\x02\x01", 2) == plain.str());
    }

    void test_write_binary_ranges_and_pod() {
        // a one-byte range is written as-is: exactly size() bytes, and an empty one writes nothing
        deren::utility::vessel::ostringstream out;
        uint8_t const raw[] = {0xDE, 0xAD, 0xBE, 0xEF};
        deren::utility::vessel::array<uint8_t, 4> const chunk = {'I', 'E', 'N', 'D'};
        deren::utility::vessel::vector<uint32_t> const words = {1u, 2u};
        std::span<uint8_t const> const empty = {};

        CHECK(deren::utility::write_binary(out,
                                           std::string_view{"IHDR"}, // a fixed character sequence, no terminator
                                           raw,
                                           chunk,
                                           deren::utility::le(words[0]),
                                           std::span{words}.subspan(1, 1),
                                           empty)
                  .has_value());
        deren::utility::vessel::string expected = "IHDR";
        expected += deren::utility::vessel::string("\xDE\xAD\xBE\xEF", 4);
        expected += "IEND";
        expected += deren::utility::vessel::string("\x01\x00\x00\x00", 4); // the single little-endian uint32
        expected += deren::utility::vessel::string("\x02\x00\x00\x00", 4);
        CHECK_MSG(out.str() == expected, "byte ranges write exactly their bytes, no length prefix");

        // The POD path writes the object's native representation. Padding bytes are UNSPECIFIED, so
        // this checks the contract rather than the bytes: sizeof(T) goes out and the members sit at
        // their real offsets. That is exactly why a portable file format writes its fields through
        // the writer (be()/le()) instead of memcpy-ing a struct.
        struct padded {
            uint8_t small = 0xAB;
            uint32_t large = 0x01020304u; // forces 3 padding bytes between the members
        };
        padded const pod = {};
        deren::utility::vessel::ostringstream pod_out;
        CHECK(deren::utility::write_binary(pod_out, pod).has_value());
        CHECK(pod_out.str().size() == sizeof(padded));
        CHECK(static_cast<uint8_t>(pod_out.str()[offsetof(padded, small)]) == 0xABu);
        uint32_t restored = 0;
        std::memcpy(&restored, pod_out.str().data() + offsetof(padded, large), sizeof(restored));
        CHECK(restored == 0x01020304u);

        // a whole contiguous range of trivially copyable values is one block write, so N elements
        // cost exactly N * sizeof(element) bytes
        deren::utility::vessel::ostringstream block;
        deren::utility::vessel::array<float, 3> const xyz = {1.0f, 2.0f, 3.0f};
        CHECK(deren::utility::write_binary(block, xyz).has_value());
        CHECK(block.str().size() == sizeof(float) * 3);
    }

    // a sink that can fail on demand (mirrors a full disk / a closed pipe)
    struct failing_sink {
        deren::utility::vessel::string bytes = {};
        bool ok = true;
        int32_t writes = 0;
        void write(char const* data, std::size_t size) {
            ++writes;
            if (!ok) {
                return; // a real stream would set badbit; the writer must notice via operator bool
            }
            bytes.append(data, size);
        }
        explicit operator bool() const noexcept {
            return ok;
        }
    };

    void test_write_binary_failure_stops_the_fold() {
        failing_sink sink;
        sink.ok = false;
        auto const failed = deren::utility::write_binary(sink, deren::utility::be(uint32_t{1}), deren::utility::be(uint32_t{2}));
        CHECK_MSG(!failed.has_value(), "a failed sink must be reported");
        CHECK(!failed.error().empty());
        CHECK_MSG(sink.writes == 1, "the fold stops at the first failure");

        failing_sink good;
        CHECK(deren::utility::write_binary(good, deren::utility::be(uint32_t{0x01020304})).has_value());
        CHECK(good.bytes.size() == 4);
    }

    void test_write_binary_file_round_trip() {
        std::filesystem::path const path = "test_write_binary.bin";
        deren::utility::vessel::vector<uint8_t> const payload = {0, 1, 2, 250, 251, 252};
        auto const written = deren::utility::write_binary_file(path, deren::utility::be(uint32_t{0x01020304}), payload, std::string_view{"END"});
        CHECK(written.has_value());
        auto const read_back = deren::utility::read_binary_to_vector(path, 64);
        CHECK(read_back.has_value());
        if (read_back.has_value()) {
            deren::utility::vessel::string const expected = deren::utility::vessel::string("\x01\x02\x03\x04", 4) + deren::utility::vessel::string(payload.begin(), payload.end()) + "END";
            CHECK_MSG(deren::utility::vessel::string(read_back->begin(), read_back->end()) == expected, "round trip");
        }
        std::filesystem::remove(path);

        // a path that cannot be opened is an error, not a crash
        CHECK(!deren::utility::write_binary_file(std::filesystem::path{"no_such_dir/x.bin"}, uint32_t{1}).has_value());
    }

    void test_bounded_file_reads() {
        namespace utility = deren::utility;
        namespace vessel = utility::vessel;
        using code = utility::file_error_code;
        std::filesystem::path const path = "bounded-read.bin";
        static_assert(std::same_as<decltype(utility::read_binary_to_vector(path, 8)),
                                   std::expected<vessel::vector<uint8_t>, utility::file_error>>);
        static_assert(std::same_as<decltype(utility::read_binary_to_string(path, 8)),
                                   std::expected<vessel::string, utility::file_error>>);
        vessel::vector<uint8_t> const bytes{0, 0x7f, 0x80, 0xff, 0, 13, 10, 42};
        CHECK(utility::write_binary_file(path, bytes).has_value());
        auto const binary = utility::read_binary_to_vector(path, bytes.size());
        auto const text = utility::read_binary_to_string(path, bytes.size());
        CHECK(binary.has_value());
        CHECK(text.has_value());
        if (binary)
            CHECK(*binary == bytes);
        if (text) {
            CHECK(text->size() == bytes.size());
            for (std::size_t i = 0; i < text->size(); ++i)
                CHECK(static_cast<uint8_t>((*text)[i]) == bytes[i]);
        }
        // A rejected oversize read must not allocate any payload through the vessel allocator.
        utility_memory_test_fail_after(0);
        auto const limited = utility::read_binary_to_vector(path, bytes.size() - 1);
        auto const limited_text = utility::read_binary_to_string(path, 0);
        utility_memory_test_fail_after(-1);
        CHECK(!limited);
        CHECK(!limited_text);
        if (!limited)
            CHECK(limited.error().code == code::too_large);
        if (!limited_text)
            CHECK(limited_text.error().code == code::too_large);
        CHECK(utility::file_error_name(code::too_large) == "too_large");
        CHECK(utility::write_binary_file(path, std::string_view{}).has_value());
        auto const empty_binary = utility::read_binary_to_vector(path, 0);
        auto const empty_text = utility::read_binary_to_string(path, 0);
        CHECK(empty_binary && empty_binary->empty());
        CHECK(empty_text && empty_text->empty());
        std::filesystem::remove(path);
        auto const missing = utility::read_binary_to_vector(path, 16);
        CHECK(!missing);
        if (!missing) {
            CHECK(missing.error().code == code::status_failed);
            CHECK(static_cast<bool>(missing.error().cause));
        }
        auto const directory = utility::read_binary_to_string(std::filesystem::current_path(), 16);
        CHECK(!directory);
        if (!directory)
            CHECK(directory.error().code == code::not_regular_file);
        // Cross the chunk boundary; every byte matters, including zeros and signed-char values.
        vessel::vector<uint8_t> large(1024 * 1024 + 37);
        for (std::size_t i = 0; i < large.size(); ++i)
            large[i] = static_cast<uint8_t>(i % 251);
        CHECK(utility::write_binary_file(path, large).has_value());
        // This exceeds string inline storage too, so both readers must reject before allocation.
        utility_memory_test_fail_after(0);
        auto const large_limited = utility::read_binary_to_vector(path, large.size() - 1);
        auto const large_limited_text = utility::read_binary_to_string(path, large.size() - 1);
        utility_memory_test_fail_after(-1);
        CHECK(!large_limited);
        CHECK(!large_limited_text);
        if (!large_limited)
            CHECK(large_limited.error().code == code::too_large);
        if (!large_limited_text)
            CHECK(large_limited_text.error().code == code::too_large);
        auto const large_binary = utility::read_binary_to_vector(path, large.size());
        auto const large_text = utility::read_binary_to_string(path, large.size());
        CHECK(large_binary && *large_binary == large);
        CHECK(large_text && large_text->size() == large.size());
        if (large_text && large_text->size() == large.size()) {
            CHECK(std::equal(large_text->begin(), large_text->end(), large.begin(),
                             [](char actual, uint8_t expected) { return static_cast<uint8_t>(actual) == expected; }));
        }
        std::filesystem::remove(path);
    }

    // Compile-time contract of the accepted types: the set is closed on purpose (see binary_writable).
    struct not_writable {
        deren::utility::vessel::string text = {};
    };
    static_assert(deren::utility::binary_writable<uint32_t>);
    static_assert(deren::utility::binary_writable<deren::utility::ordered<uint32_t, deren::utility::endian::big>>);
    static_assert(deren::utility::binary_writable<std::span<uint8_t const>>);
    static_assert(deren::utility::binary_writable<deren::utility::vessel::array<float, 3>>);
    static_assert(deren::utility::binary_writable<std::string_view>);
    static_assert(!deren::utility::binary_writable<not_writable>);
    static_assert(!deren::utility::binary_writable<char const*>); // no raw pointers: use a span

    void test_xxh3_content_hash() {
        uint8_t const a[] = {1, 2, 3, 4, 5};
        uint8_t const b[] = {1, 2, 3, 4, 5};
        uint8_t const c[] = {1, 2, 3, 4, 6};
        deren::utility::xxh3_digest const da = deren::utility::xxh3_128bits(std::span<uint8_t const>(a));
        deren::utility::xxh3_digest const db = deren::utility::xxh3_128bits(std::span<uint8_t const>(b));
        deren::utility::xxh3_digest const dc = deren::utility::xxh3_128bits(std::span<uint8_t const>(c));
        CHECK(da == db); // deterministic
        CHECK(da != dc); // content-sensitive
        CHECK(deren::utility::xxh3_digest::size_byte == 16);
    }

    void test_data_block_key_semantics() {
        deren::utility::data_block<4> zeros{};
        deren::utility::data_block<4> x{};
        deren::utility::data_block<4> y{};
        x.data = {1, 2, 3, 4};
        y.data = {1, 2, 3, 4};
        // the C-array constructor copies element-for-element (also exercised at compile time above)
        deren::utility::data_block<4> const from_c_array(golden_bytes);
        CHECK(from_c_array == x);
        CHECK(x == y);
        CHECK(x != zeros);
        CHECK(zeros < x); // lexicographic ordering for ordered containers
        CHECK(x.hash64() == y.hash64());
        CHECK(x.hash64() != zeros.hash64());
    }

    /** @brief CHECK a computed double against an expected value (the harness has no CHECK_NEAR) */
    void check_near(double const actual, double const expected, double const tolerance, char const* const what) {
        CHECK_MSG(std::abs(actual - expected) <= tolerance, what);
    }

    void test_gpu_timestamp_delta() {
        // 64-bit counters (the common case): a plain difference scaled by the tick period
        check_near(deren::utility::timestamp_delta_milliseconds(1000, 1000 + 2'000'000, 64, 1.0f), 2.0, 1e-9, "2 ms at 1 ns/tick");
        // a sub-nanosecond tick stays exact (timestampPeriod is a float: 1/16 ns here)
        check_near(deren::utility::timestamp_delta_milliseconds(0, 160, 64, 0.0625f), 1.0e-5, 1e-12, "160 ticks at 0.0625 ns/tick = 10 ns");
        // zero delta (two marks with no work between them) and a device that cannot timestamp
        check_near(deren::utility::timestamp_delta_milliseconds(500, 500, 64, 1.0f), 0.0, 1e-12, "empty interval");
        check_near(deren::utility::timestamp_delta_milliseconds(0, 1000, 0, 1.0f), 0.0, 1e-12, "no valid bits = no measurement");
        check_near(deren::utility::timestamp_delta_milliseconds(0, 1000, 64, 0.0f), 0.0, 1e-12, "no tick period = no measurement");
        // 32-bit counter that WRAPPED inside the measured span: the reading after the wrap is
        // smaller than the one before it, and the masked difference must still be the true elapsed
        // ticks (0xFFFFFC00 -> 0x00000200 is 0x400 + 0x200 = 1536 ticks, not 4.29 s and not an
        // underflow). The high bits of both readings are noise - a driver leaves everything above
        // timestampValidBits undefined, so they must be masked away and not leak into the result.
        check_near(deren::utility::timestamp_delta_milliseconds(0xDEADBEEF'FFFFFC00ull, 0x12345678'00000200ull, 32, 1.0f), 1536.0e-6, 1e-12, "32-bit wrap with undefined high bits");
        // the same on a 36-bit counter, whose width is its own: from its maximum to 10 wraps to 11
        // ticks, and the junk hex digits sit above bit 35 where the mask has to drop them
        check_near(deren::utility::timestamp_delta_milliseconds(0x1234567F'FFFFFFFFull, 0x98765430'0000000Aull, 36, 1.0f), 11.0e-6, 1e-12, "36-bit wrap");
    }

    void test_thread_pool_runs_every_posted_task() {
        deren::utility::thread_pool pool(2);
        std::atomic<int32_t> counter = 0;
        for (int32_t i = 0; i < 20; ++i) {
            bool const queued = pool.post([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
            CHECK(queued);
        }
        pool.wait_until_free(); // deterministic: every queued task has finished
        CHECK(counter.load(std::memory_order_relaxed) == 20);
    }

    void test_thread_pool_priority_group_wait() {
        deren::utility::thread_pool pool(2);
        std::atomic<int32_t> counter = 0;
        std::function<void()> const tick = [&counter] { counter.fetch_add(1, std::memory_order_relaxed); };
        // runtime pattern: post_batch() then wait_until_priority_done(priority)
        deren::utility::vessel::vector<std::function<void()>> batch(10, tick);
        CHECK(pool.post_batch(batch, 5));
        pool.wait_until_priority_done(5);
        CHECK(counter.load(std::memory_order_relaxed) == 10);
    }

    // Regression: two threads waiting on the SAME pool + a wait_until_free waiter. With
    // notify_one on the idle/completion paths one waiter could consume a notification while its
    // predicate was already true and leave the other waiter sleeping forever (predicate true,
    // no further wake). Would hang this test under the old implementation.
    void test_thread_pool_two_concurrent_waiters() {
        deren::utility::thread_pool pool(2);
        std::counting_semaphore<2> started(0), finish(0);
        std::function<void()> const slow_tick = [&] { started.release(); finish.acquire(); };
        deren::utility::vessel::vector<std::function<void()>> batch(2, slow_tick);
        CHECK(pool.post_batch(batch, 11));
        started.acquire();
        started.acquire();

        std::atomic<bool> waiter_a_done = false;
        std::atomic<bool> waiter_b_done = false;
        std::atomic<bool> free_waiter_done = false;
        std::jthread waiter_a([&] {
            pool.wait_until_priority_done(11);
            waiter_a_done.store(true);
        });
        std::jthread waiter_b([&] {
            pool.wait_until_priority_done(11);
            waiter_b_done.store(true);
        });
        std::jthread free_waiter([&] {
            pool.wait_until_free();
            free_waiter_done.store(true);
        });
        finish.release(2);
        waiter_a.join();
        waiter_b.join();
        free_waiter.join();
        CHECK(waiter_a_done.load());
        CHECK(waiter_b_done.load());
        CHECK(free_waiter_done.load());
    }

    // Completion includes releasing the pool's last capture. The deleter queries the pool,
    // proving that capture release is not performed while holding access_mutex.
    void test_thread_pool_releases_completed_capture() {
        std::binary_semaphore started(0), finish(0);
        std::atomic<bool> released = false;
        deren::utility::thread_pool pool(1);
        auto owner = std::shared_ptr<int>(new int(1), [&pool, &released](int* value) {
            static_cast<void>(pool.is_free());
            delete value;
            released.store(true);
        });
        std::weak_ptr<int> weak = owner;
        CHECK(pool.post([owned = std::move(owner), &started, &finish] {
            started.release();
            finish.acquire();
        },
                        7));
        started.acquire();
        finish.release();
        pool.wait_until_priority_done(7);
        CHECK(released.load());
        CHECK(weak.expired());
        pool.wait_until_free();
        CHECK(weak.expired());
    }

    void test_thread_pool_callable_destructor_can_query() {
        deren::utility::thread_pool pool(1);
        struct query_on_destruction {
            deren::utility::thread_pool* target;
            explicit query_on_destruction(deren::utility::thread_pool* pool)
                : target(pool) {
            }
            query_on_destruction(query_on_destruction const& other) noexcept
                : target(other.target) {
                static_cast<void>(target->is_free());
            }
            ~query_on_destruction() {
                static_cast<void>(target->is_free());
            }
        };
        // Unlike shared_ptr's pointee deleter, this runs on EVERY callable copy's destruction.
        CHECK(pool.post([probe = query_on_destruction{&pool}] {}));
        pool.wait_until_free();
        deren::utility::vessel::vector<std::function<void()>> batch;
        batch.emplace_back([probe = query_on_destruction{&pool}] {});
        CHECK(pool.post_batch(batch)); // user copy constructors must also run outside the lock
        pool.wait_until_free();
        CHECK(pool.is_free());
    }

    void test_thread_pool_rejects_empty_tasks() {
        using pool_type = deren::utility::thread_pool;
        pool_type pool(1, pool_type::shutdown_policy::discard);
        std::binary_semaphore started(0), finish(0);
        CHECK(pool.post([&] { started.release(); finish.acquire(); }));
        started.acquire(); // keep invalid work queued in the old implementation, never invoke it
        CHECK(!pool.post({}));
        std::atomic<int> ran = 0;
        deren::utility::vessel::vector<std::function<void()>> batch;
        batch.emplace_back([&ran] { ++ran; });
        batch.emplace_back();
        bool const accepted = pool.post_batch(batch, 3);
        CHECK(!accepted);
        if (accepted)
            pool.shutdown(); // old invalid batch must be discarded, never invoke its empty entry
        finish.release();
        pool.wait_until_free(); // if rejected, no valid prefix may have been queued either
        pool.shutdown();
        CHECK(ran.load() == 0);
        CHECK(!pool.post([] {}));
        CHECK(!pool.post_batch(std::span<std::function<void()>>(batch.data(), 1)));
        CHECK(pool.post_batch({})); // preserve successful empty-batch no-op
    }

    void test_thread_pool_priority_drain_and_isolation() {
        deren::utility::thread_pool pool(2);
        std::binary_semaphore first_started(0), first_finish(0), later_started(0), later_finish(0);
        CHECK(pool.post([&] { first_started.release(); first_finish.acquire(); }, 5));
        first_started.acquire();
        std::promise<void> waiting;
        auto waiter = std::async(std::launch::async, [&] {
            waiting.set_value();
            pool.wait_until_priority_done(5);
        });
        waiting.get_future().wait();
        CHECK(pool.post([&] { later_started.release(); later_finish.acquire(); }, 5));
        later_started.acquire();
        first_finish.release();
        // This wait only checks non-completion while later same-priority work is held by a gate.
        CHECK(waiter.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
        std::binary_semaphore unrelated_started(0), unrelated_finish(0);
        CHECK(pool.post([&] { unrelated_started.release(); unrelated_finish.acquire(); }, 9));
        unrelated_started.acquire();
        later_finish.release();
        bool const drained = waiter.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        CHECK(drained);             // priority 9 is still running and must not prevent priority 5 from draining
        unrelated_finish.release(); // always unblock before joining, including on regression
        waiter.get();
        pool.wait_until_priority_done(123); // never posted: returns immediately
        pool.wait_until_free();
    }

    void test_thread_pool_shutdown_policies() {
        using pool_type = deren::utility::thread_pool;
        for (auto policy : {pool_type::shutdown_policy::wait, pool_type::shutdown_policy::discard}) {
            std::atomic<int> ran = 0;
            std::atomic<bool> capture_released = false;
            std::binary_semaphore started(0), finish(0);
            {
                pool_type pool(1, policy);
                CHECK(pool.post([&] { started.release(); finish.acquire(); ++ran; }, 4));
                started.acquire();
                auto owner = std::shared_ptr<int>(new int(1), [&pool, &capture_released](int* value) {
                    static_cast<void>(pool.is_free());
                    delete value;
                    capture_released.store(true);
                });
                CHECK(pool.post([owned = std::move(owner), &ran] { ++ran; }, 6));
                pool.shutdown();
                pool.shutdown(); // idempotent, neither call joins the blocked worker
                CHECK(!pool.post([] {}));
                finish.release();
                pool.wait_until_priority_done(6);
                CHECK(capture_released.load());
                pool.wait_until_free();
                CHECK(pool.is_free());
            }
            CHECK(ran.load() == (policy == pool_type::shutdown_policy::wait ? 2 : 1));
        }
        std::atomic<int> drained_at_destruction = 0;
        {
            pool_type pool(2);
            for (int i = 0; i < 12; ++i)
                CHECK(pool.post([&] { ++drained_at_destruction; }));
        }
        CHECK(drained_at_destruction.load() == 12);
    }

    void test_thread_pool_invalid_counts(char const* executable) {
        for (auto mode : {"--zero-workers", "--negative-workers"}) {
            auto child = vessel_run_child(executable, mode, "invalid-workers.log");
            CHECK(child.started && child.aborted);
            std::ifstream log("invalid-workers.log");
            deren::utility::vessel::string console((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
            CHECK(console.find("deren ") == decltype(console)::npos); // no library diagnostics on console
            // Fatal diagnostics always use the emergency report, independent of build type.
            std::ifstream diagnostic(std::filesystem::path(mode) / "deren-crash.log");
            deren::utility::vessel::string text((std::istreambuf_iterator<char>(diagnostic)), std::istreambuf_iterator<char>());
            CHECK(text.find("thread_pool: worker count must be positive") != decltype(text)::npos);
        }
    }

    bool await_log_counter(std::uint64_t (*counter)() noexcept, std::uint64_t target) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (counter() < target && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        return counter() >= target;
    }
    void test_log_wait_is_a_flushed_prefix() {
        deren::utility::wait_log_all();
        auto const pause = utility_log_test_pause_count();
        auto const snapshot = utility_log_test_snapshot_count();
        utility_log_test_pause_after(0);
        deren::utility::log_text("prefix before wait");
        bool const paused = await_log_counter(utility_log_test_pause_count, pause + 1);
        CHECK(paused);
        if (!paused) {
            utility_log_test_pause_after(-1);
            return;
        }
        auto waiter = std::async(std::launch::async, [] { deren::utility::wait_log_all(); });
        CHECK(await_log_counter(utility_log_test_snapshot_count, snapshot + 1));
        deren::utility::log_text("prefix after wait");
        utility_log_test_pause_after(1); // permit first write/flush, hold the later message
        CHECK(await_log_counter(utility_log_test_pause_count, pause + 2));
        CHECK(waiter.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        std::ifstream log("deren-debug.log");
        deren::utility::vessel::string text{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
        CHECK(text.find("prefix before wait") != decltype(text)::npos);
        CHECK(text.find("prefix after wait") == decltype(text)::npos);
        utility_log_test_pause_after(-1); // always release before joining, even if the assertion fails
        waiter.get();
        deren::utility::wait_log_all();
    }

    // The async log sink + wait_log_all() (what panic() flushes with before std::terminate). Two
    // properties matter and neither is visible from a single-threaded smoke test:
    //  - wait_all() must return once the queue is drained even while other threads keep producing,
    //    which is the predicate the `accepting` shutdown gate was added to, and
    //  - it must not be missed by a lost wakeup, so the readers race the worker deliberately.
    // The wait runs on its own thread with a watchdog: a regression here is a hang, and an
    // unbounded unittest hang would be far worse to diagnose than a failed check.
    void test_log_sink_wait_all_under_concurrent_writers() {
        // Deliberately small: every message is a real line in the log this test writes to, and a
        // four-figure flood would drown the very output someone reads when a test fails. Four writers
        // racing the worker is what exercises the queue, not the volume.
        constexpr int32_t writer_count = 4;
        constexpr int32_t per_writer = 25;
        std::atomic<bool> producers_done = false;
        deren::utility::vessel::vector<std::jthread> writers;
        writers.reserve(writer_count);
        for (int32_t w = 0; w < writer_count; ++w) {
            writers.emplace_back([w, &producers_done] {
                for (int32_t i = 0; i < per_writer; ++i) {
                    deren::utility::log("log sink test: writer {} message {}", w, i);
                }
                producers_done.store(true, std::memory_order_relaxed);
            });
        }

        std::atomic<bool> drained = false;
        std::jthread waiter([&drained] {
            deren::utility::wait_log_all();
            drained.store(true, std::memory_order_release);
        });

        // give the waiter a bounded window; the drain itself is microseconds of work
        for (int32_t spin = 0; spin < 2000 && !drained.load(std::memory_order_acquire); ++spin) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK_MSG(drained.load(std::memory_order_acquire), "wait_log_all() did not observe the drained queue within 2 s");
        for (std::jthread& writer : writers) {
            writer.join();
        }
        waiter.join();
        CHECK(producers_done.load(std::memory_order_relaxed));
        // and it stays usable afterwards: the sink is a singleton shared by the whole process
        deren::utility::log("log sink test: still writable after a concurrent drain");
        deren::utility::wait_log_all();
        CHECK(true); // reaching here means the second drain returned too
    }

    // The morton quantizer feeds static_cast<uint32_t>, and a NaN operand makes every "<"/">" FALSE -
    // so a range check written the obvious way lets NaN reach that conversion, which is UB. It is
    // reachable, because these AABBs come from file-loaded meshes.
    //
    // The build's answer is to SANITIZE, not to fail: bvh<T>::normalize() maps a non-finite
    // normalized midpoint onto the origin (those leaves then share one morton code and stay in
    // insertion order, which is a sound order to build from), while
    // generate_morton_from_midpoint()'s own finite check is the backstop for a direct caller that
    // bypasses normalize(). So a build containing a non-finite AABB must SUCCEED and stay usable -
    // asserting a rejection here would be asserting the wrong contract.
    void test_bvh_non_finite_aabb_is_sanitized() {
        float const nan = std::numeric_limits<float>::quiet_NaN();
        float const inf = std::numeric_limits<float>::infinity();
        int32_t ids[3] = {0, 1, 2};

        auto const build_with = [&](glm::vec3 const& bad_min, glm::vec3 const& bad_max) {
            deren::utility::vessel::vector<deren::utility::aabb_box<int32_t>> boxes;
            boxes.push_back(deren::utility::aabb_box<int32_t>{.min = glm::vec3(-1.0f), .max = glm::vec3(1.0f), .extra_data = &ids[0]});
            boxes.push_back(deren::utility::aabb_box<int32_t>{.min = bad_min, .max = bad_max, .extra_data = &ids[1]});
            boxes.push_back(deren::utility::aabb_box<int32_t>{.min = glm::vec3(3.0f), .max = glm::vec3(5.0f), .extra_data = &ids[2]});
            return deren::utility::bvh<int32_t>::make(boxes);
        };

        // Build only: this is the property under test. Whether a particular non-finite box survives
        // a particular frustum depends on the geometry (an infinite AABB may legitimately lie
        // outside it), so cull counts are not asserted here - the point is that nothing crashes,
        // hangs, or converts NaN to a morton index on the way.
        auto const nan_boxes = build_with(glm::vec3(nan, 0.0f, 0.0f), glm::vec3(nan, 1.0f, 1.0f));
        CHECK(nan_boxes.has_value());

        auto const inf_boxes = build_with(glm::vec3(0.0f), glm::vec3(inf, 1.0f, 1.0f));
        CHECK(inf_boxes.has_value());

        // and the tree stays usable afterwards (a degenerate build must not leave it half-built)
        for (deren::utility::bvh<int32_t> const* tree : {&*nan_boxes, &*inf_boxes}) {
            glm::mat4 const proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 200.0f);
            glm::mat4 const view = glm::lookAt(glm::vec3(0.0f, 0.0f, 20.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            (void)tree->frustum_cull(deren::utility::make_frustum(proj * view)); // must not crash
        }
    }

    // Degenerate leaf sets the build has to survive rather than crash on: an extent that collapses
    // (every box at one point, which makes the normalization divide by its 1e-6 floor) and the
    // documented empty-input failure. Neither may corrupt memory on the way.
    void test_bvh_degenerate_inputs_do_not_crash() {
        int32_t id = 0;

        // every box at the same point: extent collapses
        deren::utility::vessel::vector<deren::utility::aabb_box<int32_t>> coincident;
        for (int32_t i = 0; i < 5; ++i) {
            coincident.push_back(deren::utility::aabb_box<int32_t>{.min = glm::vec3(2.0f), .max = glm::vec3(2.0f), .extra_data = &id});
        }
        auto const collapsed = deren::utility::bvh<int32_t>::make(coincident);
        CHECK(collapsed.has_value()); // a valid tree, just a degenerate one
        if (collapsed.has_value()) {
            // and it still culls every leaf into a frustum that contains the point
            glm::mat4 const proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
            glm::mat4 const view = glm::lookAt(glm::vec3(2.0f, 2.0f, 12.0f), glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            CHECK(collapsed->frustum_cull(deren::utility::make_frustum(proj * view)).size() == 5);
        }

        // a single leaf: exercises build_from_leaves' "sole leaf" path, which must hand a
        // childless heap node to the tree (a copy of the node would drag its raw links along)
        deren::utility::vessel::vector<deren::utility::aabb_box<int32_t>> single;
        single.push_back(deren::utility::aabb_box<int32_t>{.min = glm::vec3(-1.0f), .max = glm::vec3(1.0f), .extra_data = &id});
        auto const one = deren::utility::bvh<int32_t>::make(single);
        CHECK(one.has_value());

        // an empty input is the documented make() failure path
        auto const empty = deren::utility::bvh<int32_t>::make(deren::utility::vessel::vector<deren::utility::aabb_box<int32_t>>{});
        CHECK(!empty.has_value());
    }

    void test_bvh_frustum_cull_keeps_visible_boxes() {
        int32_t ids[3] = {0, 1, 2};
        // camera at the origin looking down -z: boxes A and B are in front, C behind
        deren::utility::vessel::vector<deren::utility::aabb_box<int32_t>> boxes;
        boxes.push_back(deren::utility::aabb_box<int32_t>{.min = glm::vec3(-1.0f, -1.0f, -6.0f), .max = glm::vec3(1.0f, 1.0f, -4.0f), .extra_data = &ids[0]});
        boxes.push_back(deren::utility::aabb_box<int32_t>{.min = glm::vec3(-0.5f, -0.5f, -3.0f), .max = glm::vec3(0.5f, 0.5f, -2.0f), .extra_data = &ids[1]});
        boxes.push_back(deren::utility::aabb_box<int32_t>{.min = glm::vec3(-1.0f, -1.0f, 4.0f), .max = glm::vec3(1.0f, 1.0f, 6.0f), .extra_data = &ids[2]});

        auto const tree = deren::utility::bvh<int32_t>::make(boxes);
        CHECK_MSG(tree.has_value(), tree.error().c_str());
        if (!tree.has_value()) {
            return;
        }

        glm::mat4 const proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 20.0f);
        glm::mat4 const view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        deren::utility::frustum const frustum = deren::utility::make_frustum(proj * view);

        auto const inside = tree->frustum_cull(frustum);
        bool saw_a = false;
        bool saw_b = false;
        bool saw_c = false;
        for (deren::utility::bvh_node<int32_t>* node : inside) {
            saw_a |= node->extra_data == &ids[0];
            saw_b |= node->extra_data == &ids[1];
            saw_c |= node->extra_data == &ids[2];
        }
        CHECK(saw_a);
        CHECK(saw_b);
        CHECK(!saw_c); // behind the camera is culled
    }

    // Documented contract: add() new leaves, then rebuild(). Leaf storage must keep element
    // addresses stable across the growth (a vector would reallocate and dangle the internal
    // nodes until rebuild). Regression: repeated add()+rebuild() with a wide frustum must keep
    // returning every leaf.
    void test_bvh_add_rebuild_contract() {
        constexpr int32_t initial = 12;
        constexpr int32_t grown = 18;
        int32_t ids[grown];
        deren::utility::vessel::vector<deren::utility::aabb_box<int32_t>> boxes;
        boxes.reserve(initial);
        for (int32_t i = 0; i < initial; ++i) {
            ids[i] = i;
            float const x = static_cast<float>(i % 4) * 1.5f - 2.25f;
            boxes.push_back(deren::utility::aabb_box<int32_t>{.min = glm::vec3(x - 0.2f, -0.2f, -3.0f), .max = glm::vec3(x + 0.2f, 0.2f, -2.6f), .extra_data = &ids[i]});
        }
        auto tree = deren::utility::bvh<int32_t>::make(boxes);
        CHECK_MSG(tree.has_value(), tree.error().c_str());
        if (!tree.has_value()) {
            return;
        }
        // wide frustum: everything we place is inside
        auto const visible_count = [&tree]() -> std::size_t {
            glm::mat4 const proj = glm::perspective(glm::radians(150.0f), 1.0f, 0.1f, 60.0f);
            glm::mat4 const view = glm::lookAt(glm::vec3(0.0f, 0.0f, 12.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            return tree->frustum_cull(deren::utility::make_frustum(proj * view)).size();
        };
        CHECK(visible_count() == initial);
        for (int32_t i = initial; i < grown; ++i) {
            ids[i] = i;
            float const x = static_cast<float>(i) * 1.5f;
            CHECK(tree->add(deren::utility::aabb_box<int32_t>{.min = glm::vec3(x - 0.2f, -0.2f, -3.0f), .max = glm::vec3(x + 0.2f, 0.2f, -2.6f), .extra_data = &ids[i]}).has_value());
        }
        tree->rebuild();
        CHECK(visible_count() == grown);
    }
    void test_ensure() {
        deren::utility::ensure(true);
        deren::utility::ensure(true, "trivially true");
    }
    void test_log_rotation(char const* executable) {
        std::error_code error;
        std::filesystem::remove_all("rotate-log", error);
        std::filesystem::create_directories("rotate-log", error);
        CHECK(!error);
        if (error)
            return;
        {
            std::ofstream previous("rotate-log/deren-debug.log", std::ios::out | std::ios::trunc);
            previous << "previous session marker\n";
        }
        auto child = vessel_run_child(executable, "rotate-log", "rotate-log/console.log");
        CHECK(child.started && child.code == 0);
        auto read_text = [](char const* path) {
            std::ifstream stream(path);
            return deren::utility::vessel::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        };
        auto const current = read_text("rotate-log/deren-debug.log");
        auto const old = read_text("rotate-log/deren-debug.log.old");
        auto const console = read_text("rotate-log/console.log");
        CHECK(current.find("rotate child marker") != decltype(current)::npos);
        CHECK(current.find("previous session marker") == decltype(current)::npos);
        CHECK(old.find("previous session marker") != decltype(old)::npos);
        CHECK(old.find("===== session ended at") != decltype(old)::npos);
        CHECK(console.find("deren ") == decltype(console)::npos); // no library diagnostics on console
    }

} // namespace

int32_t main(int argc, char** argv) {
    if (argc == 2) {
        vessel_prepare_child();
        std::error_code error;
        std::filesystem::create_directories(argv[1], error);
        if (error)
            return 2;
        std::filesystem::current_path(argv[1], error);
        if (error)
            return 2;
        if (std::string_view(argv[1]) == "--zero-workers") {
            deren::utility::thread_pool pool(0);
        } else if (std::string_view(argv[1]) == "--negative-workers") {
            deren::utility::thread_pool pool(-1);
        } else if (std::string_view(argv[1]) == "rotate-log") {
            deren::utility::log_text("rotate child marker");
            deren::utility::wait_log_all();
        }
        return 0;
    }
    test_xxh3_content_hash();
    test_write_png();
    test_write_binary_scalar_byte_order();
    test_write_binary_ranges_and_pod();
    test_write_binary_failure_stops_the_fold();
    test_write_binary_file_round_trip();
    test_bounded_file_reads();
    test_data_block_key_semantics();
    test_gpu_timestamp_delta();
    test_thread_pool_runs_every_posted_task();
    test_thread_pool_priority_group_wait();
    test_thread_pool_two_concurrent_waiters();
    test_thread_pool_releases_completed_capture();
    test_thread_pool_callable_destructor_can_query();
    test_thread_pool_priority_drain_and_isolation();
    test_thread_pool_invalid_counts(argv[0]);
    test_thread_pool_rejects_empty_tasks();
    test_thread_pool_shutdown_policies();
    test_log_wait_is_a_flushed_prefix();
    test_log_rotation(argv[0]);
    test_log_sink_wait_all_under_concurrent_writers();
    test_bvh_non_finite_aabb_is_sanitized();
    test_bvh_degenerate_inputs_do_not_crash();
    test_bvh_frustum_cull_keeps_visible_boxes();
    test_bvh_add_rebuild_contract();
    test_ensure();
    return deren::vk_test::finish("test_utility");
}
