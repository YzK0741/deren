# Logical descriptor writes (ABI 55)

## Intent and selected approach

Remove physical descriptor byte arithmetic and descriptor-address construction from runtime. Preserve
the existing shader slot ABI, resource_group and texture_group behavior, rendering output and update timing.
The user selected extending the logical RHI binding interface and upgrading ABI, rather than adding an
engine-only adapter at ABI 54. A separate generic descriptor container is unnecessary: extend descriptor_space.

This is a fixed shader-binding profile, not a dynamic descriptor allocator or a promise of arbitrary
backend-independent shader layouts. Shader-facing indices remain logical inputs; only the backend may
convert them into physical storage offsets. Existing shader constants need not change.

## Public contract

- Advance the RHI ABI from 54 to 55. Update backend loader/fixtures, probes, standard and compatibility
  documentation together; old/new binaries must be rejected rather than mixed.
- Extend descriptor_space with explicit fixed-slot image, buffer and acceleration-structure write verbs,
  and a query describing the supported fixed shader-slot interval (base/count). The existing sampled
  texture-group write and its generation rules remain intact.
- Add descriptor_space::bind with a tagged command_recorder request. The backend binds its own storage;
  callers provide no native heap addresses or ranges. Validate recorder provenance/recording state as
  with the existing heap bind path. Runtime uses this verb and checks both ready() and a nonempty fixed
  interval before fixed-profile writes, removing its dependency on descriptor_heap.
- New requests carry structure_header, space, shader slot and portable resource objects. Image writes
  carry image/view/type. Buffer writes carry buffer, resource-relative byte offset, size and descriptor
  type. AS writes carry the acceleration_structure object. No physical descriptor offset, stride or
  buffer/AS device address is accepted by this logical surface.
- Space 0 denotes the existing main space. The fixed interval is reserved for the core lifetime;
  dynamic texture-group positions occupy a different interval. Writes cannot cross into reserved native
  windows or live texture-group storage. Fixed slots do not allocate/recycle identities and need no
  generation field; they are valid only with their originating core and its supported profile.
  Vulkan exposes its existing interval [16384, 18432); dynamic group storage begins at 32768.
- A backend without this profile returns unsupported from the new verbs and reports an empty interval.
  Existing descriptor-space group support is not itself a promise of fixed-slot support.
- Validate header coverage/tag/next, slot interval, type, null/foreign resources, usage and byte ranges
  before writing. Invalid requests must leave the previous descriptor unchanged. Use existing error-code
  conventions: malformed tags/size or resource/range requests invalid_argument, unsupported next chains
  or unavailable capability/type unsupported. Preserve current error distinctions when reusing validators.
- Fixed-slot writes are host updates, borrowed resources. Callers keep resources alive through all GPU
  uses and synchronize replacement against prior uses. No automatic versioning, retention, barriers,
  descriptor copying or thread-safety guarantee is introduced. The caller serializes updates.
- Supported initial types are sampled/storage images, uniform/storage buffers and AS. Combined and
  dynamic types are refused. Image writes require an explicit valid view. Buffer validation includes
  declared usage/flags, nonzero overflow-safe ranges, alignment and native range limits. AS writes publish
  descriptors; build ordering/completion remains the caller's responsibility. Validate AS registry
  membership before casting a supplied object, just as existing buffer/image provenance is checked.
- Invalid-request preservation means validation completes before encoding; it does not promise
  transactional recovery from a driver failure while encoding.

## Backend and runtime migration

Vulkan resolves shader slots to its reserved resource-grid offsets internally, then reuses validated
descriptor encoding. It obtains buffer addresses and AS ranges internally and validates same-core origin.
It retains grid-fit/alignment/descriptor-size checks from initialization.

Runtime constructor/frame/RT writes pass logical slots and resource objects. Remove runtime calls to
heap_slot_offset, multiplications by heap_slot_stride, physical-offset diagnostics and heap-property
logging; log logical slots instead. Runtime readiness/binding uses descriptor_space or the existing
command contract, without manufacturing heap binding data. Resource byte ranges and unrelated indirect,
mapped-memory or shader-table offsets remain legitimate and are not rewritten.

Migrate the software-visibility pass's two direct fixed-grid writes too, so engine producers do not retain
an alternate physical-offset path. Keep render_layout's shader slot constants and backend grid layout;
do not reinterpret every slot arithmetic operation as a physical offset.

## Verification and limits

Headless tests cover capability refusal, request/header validation where possible, ABI fixtures, and a
source boundary check forbidding physical descriptor arithmetic/writes in runtime and migrated pass code.
Device tests cover buffer subranges, sampled/storage image writes, AS writes when supported, foreign
resource/range/slot refusals, and unchanged texture-group generation behavior. Re-run full build (-j14),
CTest and the device lane, including Gaussian/PBR frame paths, with validation. Document unsupported
paths rather than claiming exhaustive shader-type or fault coverage.

No performance improvement is promised: this change makes the binding boundary explicit. Dynamic layout
registration, typed/structured views, descriptor lifetime automation and new shader encodings remain separate work.
