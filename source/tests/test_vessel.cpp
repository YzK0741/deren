#include "../utility/shared/memory_test_hooks.hpp"
#include "vessel_child.hpp"
#include "vk_test.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
import deren.utility.shared;
import deren.utility.dynamic_link;
namespace mem = deren::utility::memory;
namespace vessel = deren::utility::vessel;

namespace {
    struct alignas(256) aligned_value {
        int value = 0;
    };
    void test_allocation() {
        static_assert(noexcept(mem::try_allocate(1)));
        static_assert(noexcept(mem::deallocate(nullptr)));
        static_assert(std::allocator_traits<mem::allocator<int>>::is_always_equal::value);
        static_assert(std::is_same_v<std::allocator_traits<mem::allocator<int>>::rebind_alloc<double>, mem::allocator<double>>);
        for (std::size_t alignment : {std::size_t{1}, std::size_t{8}, std::size_t{64}, std::size_t{4096}}) {
            auto block = mem::try_allocate(513, alignment);
            CHECK(block.has_value());
            if (!block)
                continue;
            CHECK(reinterpret_cast<std::uintptr_t>(*block) % alignment == 0);
            std::fill_n(static_cast<unsigned char*>(*block), 513, static_cast<unsigned char>(42));
            std::thread release([pointer = *block] { mem::deallocate(pointer); });
            release.join();
        }
        auto empty = mem::try_allocate(0);
        CHECK(empty && *empty != nullptr);
        if (empty)
            mem::deallocate(*empty);
        mem::deallocate(nullptr);
        for (std::size_t alignment : {std::size_t{0}, std::size_t{3}}) {
            auto invalid = mem::try_allocate(8, alignment);
            CHECK(!invalid && invalid.error() == mem::allocation_error::invalid_alignment);
        }
        auto overflow = mem::try_allocate(std::numeric_limits<std::size_t>::max());
        CHECK(!overflow && overflow.error() == mem::allocation_error::size_overflow);
        utility_memory_test_fail_after(0);
        auto refused = mem::try_allocate(64, 8);
        CHECK(!refused && refused.error() == mem::allocation_error::out_of_memory);
        auto recovered = mem::try_allocate(64, 8);
        CHECK(recovered.has_value());
        if (recovered)
            mem::deallocate(*recovered);
        auto oom = mem::try_allocate(static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()), 64);
        CHECK(!oom && oom.error() == mem::allocation_error::out_of_memory);
    }
    void test_containers() {
        vessel::vector<aligned_value> aligned(3);
        aligned.resize(80);
        CHECK(reinterpret_cast<std::uintptr_t>(aligned.data()) % alignof(aligned_value) == 0);
        vessel::vector<int> a{1, 2, 3};
        vessel::vector<int> b = a;
        b.reserve(256);
        b.push_back(4);
        a.swap(b);
        CHECK(a.size() == 4 && b.size() == 3);
        vessel::vector<int> moved;
        moved = std::move(a);
        CHECK(moved.size() == 4 && moved.back() == 4);
        vessel::vector<bool> bits(137, true);
        bits.push_back(false);
        CHECK(bits.front() && !bits.back());
        vessel::string text(512, 'x');
        text.append("suffix");
        CHECK(text.ends_with("suffix"));
        CHECK(vessel::format("{}:{}", text, 7).ends_with("suffix:7"));
        CHECK(vessel::to_string(std::numeric_limits<std::int64_t>::min()) == "-9223372036854775808");
        CHECK(vessel::to_string(std::numeric_limits<std::uint64_t>::max()) == "18446744073709551615");
        vessel::map<vessel::string, int> ordered;
        ordered.emplace(vessel::string(80, 'k'), 7);
        CHECK(ordered.begin()->second == 7);
        vessel::map<int, aligned_value> aligned_nodes;
        aligned_nodes.emplace(1, aligned_value{});
        CHECK(reinterpret_cast<std::uintptr_t>(&aligned_nodes.begin()->second) % alignof(aligned_value) == 0);
        vessel::unordered_map<vessel::string, int> hashed;
        hashed.emplace(text, 8);
        hashed.reserve(64);
        CHECK(hashed.at(text) == 8);
        vessel::multimap<int, int> duplicates{{1, 2}, {1, 3}};
        vessel::set<int> unique{1, 1, 2};
        vessel::multiset<int> repeated{1, 1};
        vessel::unordered_set<int> hash_set{1, 2};
        vessel::unordered_multiset<int> hash_multi{2, 2};
        vessel::unordered_multimap<int, int> hash_pairs{{1, 2}, {1, 3}};
        CHECK(duplicates.count(1) == 2 && unique.size() == 2 && repeated.count(1) == 2);
        CHECK(hash_set.contains(2) && hash_multi.count(2) == 2 && hash_pairs.count(1) == 2);
        vessel::deque<int> deque{1, 2};
        deque.push_front(0);
        vessel::list<int> list{1, 2};
        list.push_back(3);
        CHECK(deque.front() == 0 && list.back() == 3);
        vessel::queue<int> queue;
        vessel::stack<int> stack;
        vessel::priority_queue<int> priority;
        queue.push(3);
        stack.push(4);
        priority.push(2);
        priority.push(5);
        CHECK(queue.front() == 3 && stack.top() == 4 && priority.top() == 5);
    }
    void test_cross_dll(char const* executable) {
        auto path = std::filesystem::absolute(executable).parent_path() / VESSEL_PROBE_FILENAME;
        auto library = deren::utility::dynamic_link::load(path.string());
        CHECK(library.has_value());
        if (!library)
            return;
        auto alloc_symbol = library->symbol("vessel_probe_allocate");
        auto free_symbol = library->symbol("vessel_probe_free");
        CHECK(alloc_symbol && free_symbol);
        if (!alloc_symbol || !free_symbol)
            return;
        auto alloc = reinterpret_cast<void* (*)(std::size_t, std::size_t) noexcept>(const_cast<void*>(*alloc_symbol));
        auto free = reinterpret_cast<void (*)(void*) noexcept>(const_cast<void*>(*free_symbol));
        void* foreign = alloc(128, 256);
        CHECK(foreign != nullptr);
        CHECK(reinterpret_cast<std::uintptr_t>(foreign) % 256 == 0);
        mem::deallocate(foreign);
        auto local = mem::try_allocate(321, 64);
        CHECK(local.has_value());
        if (local)
            free(*local);
    }
    void test_fatal(char const* executable, char const* mode, char const* expected) {
        deren::utility::vessel::string file = deren::utility::vessel::string(mode + 2) + ".log";
        std::error_code error;
        std::filesystem::remove("deren-crash.log", error);
        auto outcome = vessel_run_child(executable, mode, file.c_str());
        CHECK(outcome.started && outcome.aborted);
        std::ifstream log(file.c_str());
        deren::utility::vessel::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
        CHECK(text.find("allocation child started") != deren::utility::vessel::string::npos);
        CHECK(text.find("deren allocation failure:") == deren::utility::vessel::string::npos);
        std::ifstream report("deren-crash.log");
        text.assign(std::istreambuf_iterator<char>(report), std::istreambuf_iterator<char>());
        CHECK(text.find("deren allocation failure:") != deren::utility::vessel::string::npos);
        CHECK(text.find(expected) != deren::utility::vessel::string::npos);
        CHECK(text.find(" alignment=") != deren::utility::vessel::string::npos);
        CHECK(outcome.aborted);
        CHECK(text.find("frame pc=0x") == deren::utility::vessel::string::npos);
        CHECK(text.find("origin ") == deren::utility::vessel::string::npos);
    }
} // namespace
int main(int argc, char** argv) {
    if (argc == 2) {
        vessel_prepare_child();
        std::fputs("allocation child started\n", stderr);
        std::fflush(stderr);
        if (std::string_view(argv[1]) == "--oom")
            static_cast<void>(mem::allocate(static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()), 64));
        else if (std::string_view(argv[1]) == "--overflow")
            static_cast<void>(mem::allocator<std::uint64_t>{}.allocate(std::numeric_limits<std::size_t>::max()));
        else if (std::string_view(argv[1]) == "--alignment")
            static_cast<void>(mem::allocate(1, 3));
        else if (std::string_view(argv[1]) == "--max-size") {
            utility_memory_test_fail_after(0);
            static_cast<void>(mem::allocator<std::uint64_t>{}.allocate(mem::allocator<std::uint64_t>::max_size()));
        } else if (std::string_view(argv[1]) == "--max-size-plus-one")
            static_cast<void>(mem::allocator<std::uint64_t>{}.allocate(mem::allocator<std::uint64_t>::max_size() + 1));
        else if (std::string_view(argv[1]) == "--injected-oom") {
            utility_memory_test_fail_after(0);
            vessel::vector<int> values;
            values.push_back(7);
        } else if (std::string_view(argv[1]) == "--concurrent-oom") {
            utility_memory_test_pause_first_failure();
            std::thread writer([] { mem::allocation_failure(mem::allocation_error::out_of_memory, 64, 8); });
            while (!utility_memory_test_failure_ready())
                std::this_thread::yield();
            mem::allocation_failure(mem::allocation_error::out_of_memory, 128, 8);
            writer.join();
        }
        return 0;
    }
    test_allocation();
    test_containers();
    test_cross_dll(argv[0]);
    test_fatal(argv[0], "--oom", "out_of_memory");
    test_fatal(argv[0], "--overflow", "size_overflow");
    test_fatal(argv[0], "--alignment", "invalid_alignment");
    test_fatal(argv[0], "--injected-oom", "out_of_memory");
    test_fatal(argv[0], "--concurrent-oom", "out_of_memory");
    test_fatal(argv[0], "--max-size", "out_of_memory");
    test_fatal(argv[0], "--max-size-plus-one", "size_overflow");
    return deren::vk_test::finish("test_vessel");
}
