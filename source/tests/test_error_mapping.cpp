// Headless unit tests: the error mechanism's translation tables (pure CPU, no GPU) ==================
// Every VkResult this backend can plausibly receive goes through the three per-call-site
// translators (source/backends/vulkan/core/error_tables.cpp, declared in source/backends/vulkan/core/error_tables.hpp) and the
// unified contract value is asserted per site. The load-bearing row is VK_SUBOPTIMAL_KHR: the SAME code
// is "carry on" (ok) at acquire and "rebuild" (out_of_date) at present - the one behavior a
// single global map could never hold, and the reason the translation is per call site at all.
//
// Unnamed codes are asserted too: the tables name only what can plausibly arrive (measured: 41
// VkResults exist, almost none reachable), everything else falls to operation_failed with the raw
// value carried beside it (error_info::native_code) - NOT to a guessed class.
//
// This TU is also a second compilation of the contract's pinned error_info layout: the
// static_asserts inside rhi.contract.cppm were checked when this test's BMI graph was built, so a
// toolchain that changes the layout fails here before any call site can misread a diagnostic.
#include "vk_test.h"

#include <cstdint>
#include <source_location>
#include <string_view>
#include <vulkan/vulkan.h>

import deren.promise.rhi;
// NO BACKEND MODULE IS IMPORTED ANY MORE (the SHARED flip): the translators and `failed` live in
// `deren.vulkan.error_tables`, a NEUTRAL module owned by the static target `vulkan_error_tables`,
// which this test links. So the test drives the REAL tables without needing the DLL to export a module
// interface - the thing the flip's one-export rule forbids - and without a second copy of them.
import deren.vulkan.error_tables;

namespace rhi = deren::promise::rhi;

namespace {

    // One row per plausible native code: what acquire / present / generic each make of it.
    struct mapping_row {
        VkResult native;
        rhi::error at_acquire;
        rhi::error at_present;
        rhi::error at_generic;
    };

    constexpr mapping_row mappings[] = {
        // the success row: all three sites agree
        {VK_SUCCESS, rhi::error::ok, rhi::error::ok, rhi::error::ok},
        // THE call-site row: one native code, two different decisions
        {VK_SUBOPTIMAL_KHR, rhi::error::ok, rhi::error::out_of_date, rhi::error::operation_failed},
        // OUT_OF_DATE is a swapchain-shaped code: acquire and present know what to do with it
        // (skip-and-rebuild), a generic call site does not - there it stays unclassified rather
        // than sending the caller to rebuild a swapchain a creation/query/submit never touched
        {VK_ERROR_OUT_OF_DATE_KHR, rhi::error::out_of_date, rhi::error::out_of_date, rhi::error::operation_failed},
        {VK_TIMEOUT, rhi::error::timeout, rhi::error::timeout, rhi::error::timeout},
        {VK_ERROR_SURFACE_LOST_KHR, rhi::error::surface_lost, rhi::error::surface_lost, rhi::error::surface_lost},
        {VK_ERROR_NATIVE_WINDOW_IN_USE_KHR, rhi::error::surface_lost, rhi::error::surface_lost, rhi::error::surface_lost},
        {VK_ERROR_DEVICE_LOST, rhi::error::device_lost, rhi::error::device_lost, rhi::error::device_lost},
        {VK_ERROR_OUT_OF_DEVICE_MEMORY, rhi::error::out_of_device_memory, rhi::error::out_of_device_memory, rhi::error::out_of_device_memory},
        {VK_ERROR_OUT_OF_POOL_MEMORY, rhi::error::out_of_device_memory, rhi::error::out_of_device_memory, rhi::error::out_of_device_memory},
        {VK_ERROR_FRAGMENTED_POOL, rhi::error::out_of_device_memory, rhi::error::out_of_device_memory, rhi::error::out_of_device_memory},
        {VK_ERROR_OUT_OF_HOST_MEMORY, rhi::error::out_of_host_memory, rhi::error::out_of_host_memory, rhi::error::out_of_host_memory},
        {VK_ERROR_MEMORY_MAP_FAILED, rhi::error::out_of_host_memory, rhi::error::out_of_host_memory, rhi::error::out_of_host_memory},
        // creation-shaped failures: only the generic sites (create/query/submit) translate them
        {VK_ERROR_INITIALIZATION_FAILED, rhi::error::operation_failed, rhi::error::operation_failed, rhi::error::initialization_failed},
        {VK_ERROR_INCOMPATIBLE_DRIVER, rhi::error::operation_failed, rhi::error::operation_failed, rhi::error::initialization_failed},
        // the "no such mechanism" family: unsupported, and only in generic
        {VK_ERROR_EXTENSION_NOT_PRESENT, rhi::error::operation_failed, rhi::error::operation_failed, rhi::error::unsupported},
        {VK_ERROR_FEATURE_NOT_PRESENT, rhi::error::operation_failed, rhi::error::operation_failed, rhi::error::unsupported},
        {VK_ERROR_LAYER_NOT_PRESENT, rhi::error::operation_failed, rhi::error::operation_failed, rhi::error::unsupported},
        {VK_ERROR_FORMAT_NOT_SUPPORTED, rhi::error::operation_failed, rhi::error::operation_failed, rhi::error::unsupported},
        // the deliberate fall-through: named nowhere, guessed nowhere - operation_failed everywhere,
        // the raw code rides along as error_info::native_code
        {VK_ERROR_UNKNOWN, rhi::error::operation_failed, rhi::error::operation_failed, rhi::error::operation_failed},
        {VK_ERROR_TOO_MANY_OBJECTS, rhi::error::operation_failed, rhi::error::operation_failed, rhi::error::operation_failed},
        {VK_ERROR_NOT_PERMITTED_EXT, rhi::error::operation_failed, rhi::error::operation_failed, rhi::error::operation_failed},
        {VK_ERROR_VALIDATION_FAILED_EXT, rhi::error::operation_failed, rhi::error::operation_failed, rhi::error::operation_failed},
    };

    void test_per_call_site_translation_tables() {
        for (mapping_row const& row : mappings) {
            CHECK_MSG(deren::vulkan::acquire_error(row.native) == row.at_acquire, "acquire row diverged");
            CHECK_MSG(deren::vulkan::present_error(row.native) == row.at_present, "present row diverged");
            CHECK_MSG(deren::vulkan::generic_error(row.native) == row.at_generic, "generic row diverged");
        }
        // spell the load-bearing row out once more, by name, so a table edit that drops it cannot
        // pass silently: SUBOPTIMAL is the one code whose meaning the call site owns.
        CHECK(deren::vulkan::acquire_error(VK_SUBOPTIMAL_KHR) == rhi::error::ok);
        CHECK(deren::vulkan::present_error(VK_SUBOPTIMAL_KHR) == rhi::error::out_of_date);
    }

    // zone_of is total over the error enum and a pure function of the code - the mapping is the
    // contract's own comment made executable (rhi.contract.cppm).
    void test_zone_of_total_mapping() {
        CHECK(rhi::zone_of(rhi::error::unsupported) == rhi::error_zone::resource);
        CHECK(rhi::zone_of(rhi::error::invalid_argument) == rhi::error_zone::argument);
        for (rhi::error const code : {rhi::error::not_ready, rhi::error::device_lost, rhi::error::out_of_date, rhi::error::surface_lost,
                                      rhi::error::timeout, rhi::error::out_of_device_memory, rhi::error::out_of_host_memory}) {
            CHECK_MSG(rhi::zone_of(code) == rhi::error_zone::api, "the retry/rebuild/degrade family is the api zone");
        }
        for (rhi::error const code : {rhi::error::ok, rhi::error::abi_mismatch, rhi::error::operation_failed, rhi::error::initialization_failed}) {
            CHECK_MSG(rhi::zone_of(code) == rhi::error_zone::internal, "the machinery family is the internal zone");
        }
    }

    // The producer: the location is captured at the FAILED call site (this file, not a contract
    // default argument at some engine caller), and the negative Vulkan code survives verbatim.
    constexpr auto pinned_producer = [] {
        return deren::vulkan::failed(rhi::error::out_of_date, -1000001004, "swapchain expired");
    }();
    static_assert(pinned_producer.code == rhi::error::out_of_date);
    static_assert(pinned_producer.api == rhi::graphics_api::vulkan);
    static_assert(pinned_producer.native_code == -1000001004); // signed: the Vulkan code is itself
    static_assert(pinned_producer.message == "swapchain expired");

    void test_failed_producer() {
        // constexpr-producible (the layout asserts in the contract rest on trivial copyability), and
        // the default-argument location names the producer's call site - HERE, by file and by line.
        rhi::error_info const produced = deren::vulkan::failed(rhi::error::device_lost, VK_ERROR_DEVICE_LOST, "device refused");
        CHECK(produced.code == rhi::error::device_lost);
        CHECK(produced.native_code == VK_ERROR_DEVICE_LOST);
        CHECK(produced.message == "device refused");
        CHECK(std::string_view(produced.where.file_name()).ends_with("test_error_mapping.cpp"));
        CHECK(produced.where.line() == __LINE__ - 5);

        // the zero-value default: an error_info nobody filled says ok/unknown/0 - the same readable
        // zero a zeroed submit_info meant, now with a "why it is zero" channel next to it.
        rhi::error_info const silence = {};
        CHECK(silence.code == rhi::error::ok);
        CHECK(silence.api == rhi::graphics_api::unknown);
        CHECK(silence.native_code == 0);
        CHECK(silence.message.empty());
    }
} // namespace

int32_t main() {
    test_per_call_site_translation_tables();
    test_zone_of_total_mapping();
    test_failed_producer();
    return deren::vk_test::finish("test_error_mapping");
}
