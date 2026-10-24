# Unified shader parameter push

User scope: unify push first; lazy recording remains deferred. Public shader parameters are a recorder
operation, independent of descriptor containers. Use command_recorder::push_constants(uint32 offset,
span<byte const> data) noexcept -> error, shared by primary/frame/mergeable recorders. Existing engine
parameter packing remains unchanged. Vulkan records immediately through its private heap push_data.

Remove descriptor_heap::push_data, heap_push_info and public descriptor_heap_properties::max_push_data.
Keep structure_type::heap_push value retired, never reused. Rename range helper to
validate_push_constants_range and place it beside error in the base contract. Query usable recording
capacity through device_capabilities::max_push_constants_size, reporting actual active backend budget
(Vulkan heap maxPushDataSize, or zero when no supported push mechanism is ready). No new manager or
capability interface; default recorder returns unsupported. ABI 52 covers vtable/property changes.

A push consumes the supplied bytes before return. Require recording, four-byte offset/size alignment,
nonempty bytes and overflow-safe containment within capacity. Before pipeline bind ordinary pushes
are allowed; once bound, reject overlap with legacy texture token or any general-group-owned uint32
field. No deferred bytes, merging, pipeline/resource state changes or draw return-type changes. Internal
group token publication remains private and may write its own protected fields. Preserve independent
command/secondary/pending ownership, resource groups, GUI heap workaround and startup behavior.

Move CPU probe capture to recorder operations; backends without implementation refuse explicitly.
Migrate all engine/device tests; no compatibility forwarder. Do not fabricate a push on a null recorder
or pretend another virtual implementation is a Vulkan recorder. Keep heap bind provenance tests.

Validation: compile-time API/range red tests; CPU capture and recording-state/range tests; real GPU
parameter bytes/range boundaries and protected-field checks on primary, secondary and frame; existing
resource/texture group readback, completion, full CTest and Gaussian/PBR/startup regressions.
No commit or publish. Member names have no adornments; build with 14 jobs. Preserve dirty workspace.
