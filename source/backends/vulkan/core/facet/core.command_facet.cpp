// -*- C++ -*-
// ============================================================================
// file: source/backends/vulkan/core/facet/core.command_facet.cpp
//
// The command face holds the submission/completion state and the provenance sets.
// Its virtual entry points implement the command factories and submission here;
// the root keeps thin forwarders for its callers.
// ============================================================================
module;
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <vulkan/vulkan.h>
module deren.vulkan.core;
import deren.vstd;
import deren.utility.shared;
import deren.promise.rhi;
import :declarations;

namespace deren::vulkan {

    namespace rhi = deren::promise::rhi;

    rhi::unique_object<rhi::command_buffer> core::command_facet::create_command_buffer(rhi::command_buffer_desc const& declared_desc) {
        // THE ABI GUARD, the same rule every factory descriptor follows: only the prefix the caller
        // declares is read, so an older caller's structure keeps this build's default.
        rhi::command_buffer_kind kind = rhi::command_buffer_kind::primary;
        if (covered_by(declared_desc.struct_size, offsetof(rhi::command_buffer_desc, kind), sizeof(rhi::command_buffer_desc::kind))) {
            kind = declared_desc.kind;
        }
        // abi 37: PRIMARY IS THE ONLY CONTRACT KIND LEFT. A secondary `command_buffer` is retired (the
        // multithreaded passes record into `mergeable_command_buffer` segments); the value is still REFUSED
        // by name rather than silently promoted, so a caller that asks for the retired shape hears about it.
        if (kind != rhi::command_buffer_kind::primary) {
            deren::utility::log("rhi: create_command_buffer refused: kind {} is retired (primary only; mergeable segments are a separate factory)",
                                static_cast<std::uint32_t>(kind));
            return nullptr;
        }
        auto* const answer = this->full_core.allocate_command_buffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        if (answer == nullptr) {
            return nullptr;
        }
        deren::utility::log("rhi: create_command_buffer primary -> native {:#x} (its command pool)",
                            reinterpret_cast<std::uintptr_t>(*answer->buffer));
        return rhi::unique_object<rhi::command_buffer>{answer};
    }

    rhi::shared_object<rhi::command_buffer> core::command_facet::make_command_buffer(rhi::command_buffer_desc const& desc) {
        rhi::command_buffer* const raw = this->create_command_buffer(desc).release();
        if (raw == nullptr) {
            return {}; // the refusal was named where it happened; an empty shared_object is the same answer
        }
        // THE CONTROL BLOCK HOLDS THE DROP, and the drop IS `release()`: the one factory reference is
        // the one the last shared copy drops - no second lifetime rule.
        return rhi::shared_object<rhi::command_buffer>(raw);
    }

    rhi::mergeable_command_buffer_ref core::command_facet::make_mergeable_command_buffer(rhi::command_buffer_desc const& declared_desc) {
        // THE KIND IS THE FACTORY'S, not the descriptor's: a mergeable is recorded as a SECONDARY in Vulkan
        // terms (the backend holds the primary that executes the batch), so a caller's `primary` value is
        // replaced rather than refused - the TYPE already says which submission rule applies.
        (void)declared_desc; // a mergeable's descriptor carries nothing the allocation needs today
        rhi::command_buffer* const raw = this->full_core.allocate_command_buffer(VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        if (raw == nullptr) {
            return {};
        }
        auto* const raw_mergeable = new (std::nothrow) core::mergeable_command_buffer();
        if (raw_mergeable == nullptr) {
            static_cast<core::command_buffer*>(raw)->release();
            return {};
        }
        raw_mergeable->parent = &this->full_core;
        raw_mergeable->set_target(*static_cast<core::command_buffer*>(raw)->buffer);
        raw_mergeable->inner = std::shared_ptr<core::command_buffer>(static_cast<core::command_buffer*>(raw), [](core::command_buffer* const p) noexcept {
            if (p != nullptr) {
                p->release();
            }
        });
        {
            std::lock_guard const lock(this->contract_mergeable_buffers_mutex);
            this->contract_mergeable_buffers.insert(raw_mergeable);
        }
        return rhi::mergeable_command_buffer_ref(raw_mergeable);
    }

    rhi::error core::command_facet::submit(rhi::command_buffer& commands) {
        if (&commands == &this->full_core.frame.commands_view) {
            return rhi::error::invalid_argument;
        }
        return this->full_core.submit_commands(commands, nullptr);
    }

    deren::promise::rhi::error core::command_facet::submit(std::span<deren::promise::rhi::mergeable_command_buffer_ref const> batch) {
        return this->full_core.submit_batch(batch, nullptr);
    }

} // namespace deren::vulkan
