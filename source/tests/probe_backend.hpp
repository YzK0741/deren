// -*- C++ -*-
// ============================================================================
// file: source/tests/probe_backend.hpp
//
// THE PROBE BACKEND AS AN INJECTION SOURCE (batch ⑥).
//
// WHY THIS HEADER EXISTS: the runtime's constructor takes a device root
// (`deren::vulkan::runtime{std::shared_ptr<rhi::api_core>, create_info const&}`), so the engine can be
// tested WITHOUT a loader, a DLL or a GPU - by handing it a probe `api_core`. This is that probe's
// test-only door: it makes one with a CHOSEN `api_version()` answer, which is what makes the
// consumption-side handshake testable (a probe reporting `rhi::abi_version + 1` must be refused).
//
// The declaration is C++ (not `extern "C"`): the test links the same translation unit the C entry lives
// in (`probe_backend_static` / `probe_backend`), so there is no reason to decorate a test hook as ABI
// surface - and the C entries stay the only symbols the DLL exports.
//
// A translation unit that includes this must `import deren.promise.rhi;` FIRST: the parameters name the
// contract's types, and an import declaration cannot live in a header (the same rule
// source/promise/rhi/backend_entry.hpp states).
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <future>
#include <span>

namespace deren::vk_test {
    /// A probe `api_core` built from @p desc, answering @p reported_api_version from `api_version()`.
    ///
    /// `reported_api_version` is the knob: pass `deren::promise::rhi::abi_version` for the normal path
    /// (what the C entry always does) or anything else to drive the engine's refusal. The rest of the
    /// object behaves exactly like the one the entry hands out - it is the same `impl`.
    [[nodiscard]] std::shared_ptr<deren::promise::rhi::api_core>
    probe_make_core(deren::promise::rhi::create_info const& desc, std::uint32_t reported_api_version, bool portable_mode = false);

    /// Borrow a portable probe's heap buffer while the runtime owning it is alive.
    [[nodiscard]] std::span<std::byte> probe_heap_buffer(deren::promise::rhi::api_core& core, std::uint64_t offset);
    // Fail one image/buffer creation after N successful creations (portable probe only).
    [[nodiscard]] deren::promise::rhi::image const* probe_heap_image(deren::promise::rhi::api_core& core, std::uint64_t offset);
    void probe_buffer_allocation_fill(deren::promise::rhi::api_core& core, std::uint8_t value);
    void probe_resource_failure(deren::promise::rhi::api_core& core, std::int32_t successful_creations);
    void probe_heap_image_failure(deren::promise::rhi::api_core& core, std::int32_t successful_writes, bool persistent = false);
    void probe_present_completion(deren::promise::rhi::api_core& core, std::shared_future<deren::promise::rhi::error> completion);
} // namespace deren::vk_test
