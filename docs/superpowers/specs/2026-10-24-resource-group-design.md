# Resource group design

User-approved direction: implement resource_group first, defer lazy recording. texture_group becomes a
member of the general resource group. Do not expose descriptor containers to engine callers. Ordinary
member names, no adornments. Existing rendering migration remains incremental.

## Approach

Extend the immutable shared-group model (chosen by the user), rather than expose descriptor-space
allocations or add individual bind_image/bind_buffer recorder verbs. resource_group contains a flat list
of numbered logical bindings. A member can be a texture_group, sampled/storage image with a view,
uniform/storage buffer with byte range, sampler, or top-level acceleration structure. No recursive
resource groups. Each member carries a contract shared owner, not an unowned raw resource/address;
convert a factory unique_object into shared_object, or use an existing shared/alias owner.

Pipeline layout declares binding number, kind, shader stages, and shader parameter byte offset.
These offsets describe the shader interface, not descriptor memory. Vulkan v1 represents each binding
by one uint32 shader handle: image/buffer/AS/sampler handle or existing texture-group token. The shader
interface is validated against SPIR-V uint32 push members. Other backends may translate the same logical
layout to their own binding mechanism. No physical heap address/stride/slot allocation in group API.

Factory returns expected<resource_group_ref,error>. Descriptor spans are copied before return. Empty
or duplicate bindings, null/foreign resources, invalid ranges/usages, invalid layouts are rejected.
Group composition is immutable; buffer/image contents can change under ordinary synchronization.
Pipeline/group binding numbers and kinds must match exactly (order-independent). Creation is atomic:
failed products publish no group; temporary descriptor slots are reclaimed. Unsupported shader stages
or resource shapes report unsupported explicitly. No multi-group slots in this first version.

load_resource_group requires a recording command and a compatible bound pipeline, validates before
recording anything and retains the whole group. Recorded and pending primary, secondary, batch and
frame work keep members/descriptors alive. Rerecord/retire releases them using existing machinery.
No implicit barriers or core ownership; core/backend must outlive all group references. Existing
texture_group entry points remain during migration; legacy and general layouts cannot be combined
on one pipeline. Ordinary heap pushes cannot overwrite group-owned parameter fields.

Backend descriptor slots use a reusable private allocator, independent of scene fixed slots, returned
only after the last group reference dies. Sampler descriptors use private free slots after shared
samplers. Existing texture-group fixed reservation currently overlaps the scene/dynamic allocations;
move its private table/pool to a disjoint fixed range and update its shader helper as prerequisite.
ABI 51: recorder/factory slots and pipeline descriptor append. Coordinate full rebuild.

## Verification

Compile/API contract checks, pure layout validation, real Vulkan mixed buffer/image/texture/sampler
shader readback; invalid/foreign/range/layout rejection, atomic failure, primary/secondary/batch/frame
retention, reserved-push protection, descriptor reuse, completion tokens, existing texture-group device
test, full CTest, Gaussian/PBR windowed smoke. AS can be tested through existing built TLAS fixtures;
report any unexercised capability honestly. No lazy state, draw return-type change, commit or push.

## Builder-check rulings

Reserve texture-group block during construction before dynamic descriptors; resource working budget
increases from 1280 KiB to 4 MiB. Shared members must own lifetime: reject borrow() using a non-virtual
shared_object::owns_lifetime() predicate. Lifetime aliases promise their owner retains the target.
Group composition does not freeze nested texture-group writes; existing mutation/synchronization
rules still apply. Uniform binding lowers to a storage descriptor under current Slang heap ABI;
backend uniform allocations carry the necessary native storage usage. No extra frontend flags required.
