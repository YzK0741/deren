// -*- C++ -*-
// ============================================================================
// file: tests/probe_backend.cpp
//
// The probe backend of tests/test_dynamic_link.cpp: the smallest stand-in for the
// real promise/ contract (promise/api_core.hpp, RHI plan v4 §3.3 - §3.5, §4.1).
// CMake builds this one source file twice - as a DLL (with DEREN_API_SHARED +
// DEREN_API_BUILD) and as a static library (with neither) - and the test links the
// static half while opening the DLL half at run time, so both branches of
// promise/abi_export.hpp are exercised and the two halves must answer the same.
//
// This file includes the contract header instead of declaring the C ABI itself (it
// used to; the declarations live in promise/api_core.hpp now), which is also the
// check that the header can be compiled by the backend side: the three entry points
// below are EXACTLY the ones the header declares, and `derive` from the header's
// abstract classes is what makes the compiler verify that every virtual is
// implemented.
//
// What the probe implements, and why it is this small:
//
//   - `api_core::abilities()` announces device_address | descriptor_heap, and
//     `query_extension()` answers those two and returns nullptr for anything else.
//     That pair is what the plan's "named failure, no silent downgrade" rests on
//     (§1.9, §3.6): an engine that asks for ray_tracing gets a null, not a stub.
//   - The factories return nullptr except `create_buffer()`: a probe has no device,
//     so it has nothing to hand out for the rest.
//   - `create_buffer()` hands out a STATICALLY allocated probe buffer and stores the
//     descriptor's size in it. That is what makes the tier-2 path testable without
//     answering the ownership question: `device_address::buffer_address()` is asked
//     about that very object, so the test sees a POD descriptor cross into the DLL,
//     a virtual call come back out, and both halves agree. Static storage also means
//     the engine has nothing to free - releasing a resource through the backend
//     (plan §4.2) is S1's, and this round does not pretend to answer it.
//   - `frame_begin()` counts frames, so a cross-boundary virtual call with a
//     by-value POD result is observable too.
// ============================================================================
#include "../promise/api_core.hpp"

#include <cstdint>
#include <span>

namespace {

    namespace promise = deren::promise;

    /// The probe's buffer: a real object, statically allocated (see the banner).
    struct probe_buffer final : promise::buffer {
        [[nodiscard]] std::uint64_t size() const noexcept override {
            return this->size_bytes;
        }

        std::uint64_t size_bytes = 0;
    };

    /// device_address, implemented over that buffer: the answer depends on the size
    /// the descriptor carried, so the test can tell "the ability was called about
    /// this object" from "the ability was called about something else".
    struct probe_device_address final : promise::device_address {
        [[nodiscard]] promise::extension_kind kind() const noexcept override {
            return promise::extension_kind::device_address;
        }

        [[nodiscard]] std::uint64_t buffer_address(promise::buffer const& resource, std::uint64_t offset) const noexcept override {
            return address_base + resource.size() + offset;
        }

        [[nodiscard]] std::uint64_t acceleration_structure_address(promise::acceleration_structure const&) const noexcept override {
            return 0ull; // the probe has no acceleration structures
        }

        static constexpr std::uint64_t address_base = 0x1000ull;
    };

    /// descriptor_heap, announced but inert: recording into a command list needs a
    /// device, and the probe's `begin_commands()` returns nullptr.
    struct probe_descriptor_heap final : promise::descriptor_heap {
        [[nodiscard]] promise::extension_kind kind() const noexcept override {
            return promise::extension_kind::descriptor_heap;
        }

        void push_data(promise::command_list&, std::span<std::byte const>) override {
        }
    };

    /// The probe's api_core. `final` so that a missing override is a compile error
    /// rather than an inherited pure virtual in an abstract class nobody notices.
    struct impl final : promise::api_core {
        [[nodiscard]] promise::ability_bits abilities() const noexcept override {
            return promise::to_bits(promise::extension_kind::device_address) |
                   promise::to_bits(promise::extension_kind::descriptor_heap);
        }

        [[nodiscard]] promise::extension* query_extension(promise::extension_kind kind) noexcept override {
            switch (kind) {
            case promise::extension_kind::device_address:
                return &this->address;
            case promise::extension_kind::descriptor_heap:
                return &this->heap;
            default:
                return nullptr; // not announced, so not available (§3.6)
            }
        }

        [[nodiscard]] promise::swapchain* create_swapchain(promise::swapchain_desc const&) override {
            return nullptr;
        }

        [[nodiscard]] promise::buffer* create_buffer(promise::buffer_desc const& desc) override {
            this->buffer.size_bytes = desc.size;
            return &this->buffer;
        }

        [[nodiscard]] promise::image* create_image(promise::image_desc const&) override {
            return nullptr;
        }

        [[nodiscard]] promise::sampler* create_sampler(promise::sampler_desc const&) override {
            return nullptr;
        }

        [[nodiscard]] promise::shader* create_shader(promise::shader_desc const&) override {
            return nullptr;
        }

        [[nodiscard]] promise::pipeline* create_pipeline(promise::pipeline_desc const&) override {
            return nullptr;
        }

        [[nodiscard]] promise::query* create_query(promise::query_desc const&) override {
            return nullptr;
        }

        [[nodiscard]] promise::command_list* begin_commands() override {
            return nullptr; // no device, no pool: the test checks this is an answer, not a crash
        }

        [[nodiscard]] promise::submit_info frame_begin() override {
            promise::submit_info info{};
            info.frame_index = this->frames_handed_out;
            info.image_index = this->image_index;
            ++this->frames_handed_out;
            return info;
        }

        void present() override {
            ++this->presents;
        }

        void wait_idle() override {
            ++this->waits;
        }

        probe_buffer buffer{};
        probe_device_address address{};
        probe_descriptor_heap heap{};
        std::uint32_t frames_handed_out = 7; // deliberately not 0: a default would hide a lost write
        std::uint32_t image_index = 3;
        std::uint32_t presents = 0;
        std::uint32_t waits = 0;
    };

} // namespace

extern "C" DEREN_API_EXPORT std::uint32_t deren_abi_version() {
    return deren::promise::abi_version;
}

extern "C" DEREN_API_EXPORT deren::promise::api_core* deren_make_api_core(std::uint32_t abi_version,
                                                                          deren::promise::error* out_error) {
    if (abi_version != deren::promise::abi_version) {
        if (out_error != nullptr) {
            *out_error = deren::promise::error::abi_mismatch;
        }
        return nullptr;
    }
    if (out_error != nullptr) {
        *out_error = deren::promise::error::ok;
    }
    return new impl();
}

extern "C" DEREN_API_EXPORT void deren_destroy_api_core(deren::promise::api_core* core) {
    delete core;
}
