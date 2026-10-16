# Render graph typed content parameters

Bounded extension of the existing CPU graph. Typed texture/buffer content fields
and a const noexcept member visitor generate the existing canonical access list.
No GPU workload, shader usage/stage, barrier lowering or production migration is
included. Existing unrelated working-tree changes were preserved.

## Behavior

- texture_parameter/buffer_parameter encode one of the five content operations
  in their type and carry the typed graph ref and subresource range. Named aliases
  cover read, discard_write, preserve_write, read_write and invalidate.
- Parameters implement visit_resources(parameter_visitor&) const noexcept. Explicit
  enumeration supports nested structs, arrays, optional fields and spans over
  owned data. Scalars are excluded from enumeration, without generic reflection.
- The typed add_pass overload allocates the final const value with the utility
  mimalloc allocator, visits that value once, and delegates to the existing
  range/ref/conflict validation and normalization. Rejection releases the candidate
  snapshot and does not append a pass. Frozen builders reject before visiting.
- pass_record.parameters owns a type-erased shared snapshot. get<T>() returns a
  const pointer only for the stored type, without RTTI. Compiled plans can outlive
  their builder; copied plans share the snapshot, even for move-only parameters.
- This is value ownership and shallow constness. Borrowed storage and indirect
  mutable objects are not deep-copied; callers must use owning values or guarantee
  backing storage stays alive and immutable. Resource enumeration must be complete,
  deterministic and free of side effects. This is not GPU lease retention.
- The untyped span overload remains supported. Existing graph algorithms and
  production renderer paths are unchanged.

## Verification

Windows Clang64, C++23, exceptions and RTTI disabled:

- RED: test_render_graph_parameters failed compilation on missing parameter types,
  visitor and pass_parameters concept before implementation.
- GREEN: 84 parameter checks pass, covering nested/array/optional/span enumeration,
  all five content modes, read-write overlap normalization, const snapshots,
  plan lifetime/copying, move-only values, invalid refs/ranges, conflict rollback,
  rejected snapshot release, frozen builder, legacy overload and OOM fast_panic.
- Existing A1 58 checks and range 489 checks remain passing.
- Boost allocation probe: 281 checks, zero ordinary-new calls in the measured path,
  now including a typed snapshot with vessel-owned variable data.
- Full Release build -j12 passed; complete CTest 34/34 passed.
- ASan/UBSan focused build -j12 passed; CTest 5/5 passed (vendor, Boost, graph,
  ranges, parameters).
- Artifact backend check explicitly includes librender_graph_core.a: zero symbols,
  reference sites and owning STL crossings. Full native boundary check passed.

Evidence in ignored build trees: build-release-dyn-clang64/render-graph-parameters-
{red,build,full-build,full-ctest,boundary}.log and boundary.json;
build-vessel-asan-clang64/render-graph-parameters-{build,test}.log.

Independent read-only review found no actionable correctness, lifetime or type-safety
defects; ownership documentation matches the shallow-const contract. No deferred
minor findings. Formatting, documentation test and git diff --check passed.
No Linux/MSVC or GPU validation claimed. Public usage
and ownership contract: docs/render_graph.md. Next A2 increment is shader usage,
stage and abstract synchronization planning. No commit/push or old-path removal.
