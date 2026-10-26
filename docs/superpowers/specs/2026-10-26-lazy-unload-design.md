# Explicit lazy-resource unload

User requests an initial unload method, without automatic eviction. Add `error unload() noexcept`
to lazy_buffer/lazy_image, advance RHI ABI to 57, and preserve the shared uint8_t data and immutable
description so the next native use reloads the original source.

The initial method is deliberately conservative: a ready resource with live views, resource/texture
groups, fixed logical descriptor slots, recorded commands or pending submissions returns not_ready.
Those users cache physical addresses/descriptors, so success requires their references to be reset,
destroyed or replaced. No descriptor-generation invalidation or transparent group rebuild is added.
Ordinary shared ownership alone is not a GPU dependency and is not sufficient to detect one; track
explicit backend dependency leases. Each lease also retains the logical object.

An already unloaded resource returns ok (including metadata-only references); loading returns
not_ready without waiting. A failed resource returns its stored failure and remains failed; unload
is not a retry API. Normal eager objects internally implementing the lazy interface return unsupported.
On success, drop the allocation owner and cached native handle under the materialization mutex, clear
mapping state where applicable, then publish unloaded. Never wait for GPU work or call wait_idle.
Register dependency leases under the same mutex; release them after their dependent use ends. Pending
and reusable command ownership remains unchanged, so a completed reusable recording blocks until reset.

All cached native-image descriptor writes on lazy resources must be through tracked logical bindings.
The raw legacy descriptor_heap write face rejects lazy images; tracked backend lowering uses its
private native writer. Group creation registers leases before reading/encoding physical addresses.
Fixed-slot replacement still follows caller synchronization requirements.

VMA may retain backing blocks or deduplicate image allocations; successful unload releases this object's
allocation reference, not a promise that the process's VRAM usage immediately falls by its payload size.
The application can keep using the logical object, its metadata and source owner after successful unload.

Tests: unloaded idempotence, ready unload/reload with source lifetime, rejection while a command or
logical binding/view retains the resource, success after those dependencies are released, loading and
failed states through the common gate, and normal eager compatibility. Full CPU regression plus existing
device lane and validation-enabled lazy probe. No scene migration, automatic budget or eviction policy.
