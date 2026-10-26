# Lazy resources acceptance (ABI 56)

Implemented shared uint8_t source ownership and deferred GPU allocation/upload through new
resource_factory create_lazy_buffer/create_lazy_image factories. Normal buffer/image APIs remain usable
for the supported profile; metadata queries, logical views/groups and descriptor writes do not upload.
Native bindings and resource barriers demand resources; superseded logical binds can stay unloaded.

Both resource groups and fixed descriptor slots resolve lazy members at demand. Recordings and pending
submissions retain source/resource ownership transitively, including reusable commands and secondary
composition. Bound containers conservatively demand all members. This does not provide scene selection,
automatic eviction, asynchronous loading or an automatic upload-strategy threshold.

## Verification

- Full Release build with 14 workers: success.
- CTest: 71/71, including new deterministic common loading-gate concurrency/failure checks in
  test_rhi_ownership and injected upload sequence errors in test_texture_upload_layout.
- Device lane: 8/8. Ordinary resource-group probe277 checks, lazy-mode probe281 checks; same shader
  readback is `73 255 1 255 41`. Existing frame/secondary/pending-ownership cases run in both modes.
- Dedicated validation-enabled lazy probe: exit0,281/0; no VUID/Validation Error/SYNC-HAZARD diagnostics.
  Logs: tmp/lazy-acceptance-final. Lane summary: tmp/lazy-device-probes.json.
- git diff --check: clean. No commit or push performed; preceding uncommitted work preserved.

Independent review identified five correctness issues, all fixed: serialize texture-group publication;
avoid native reads before acquiring ready; check buffer/image upload sequencing; validate lazy image
barrier ranges before allocation; reject lazy host-copy readback. Regression tests include superseded
binds, source lifetime after caller release, descriptor write remaining unloaded, invalid ranges and
unsupported clear/readback, plus common state-gate concurrency/failure and upload-stage fault injection.

## Limits and implementation decisions

Only initial device-only buffers and sampled color images are supported. Raw addresses, mapped lazy
buffers, storage/attachment lazy images and direct copy/build/indirect use are excluded. Source data
remains shared-owned and immutable for the lazy object's entire lifetime, including after upload.

The existing allocator helpers return no detailed failure code, so physical refusal maps to
operation_failed; validation errors and host-object allocation failure retain their own codes. A failed
wait after successful upload submission, without device loss, cannot prove safe resource destruction;
fast_panic terminates instead of recycling/freeing possibly pending resources. No whole-device idle or
recoverable quarantine was added. Existing upload policy is unchanged.

Thread tests exercise the production common loading gate; no real device-loss injection, TSan run,
multithreaded Vulkan stress or measured VMA allocation/block-count claim is made. No scene residency or
frame-time improvement is claimed until runtime adopts lazy resources and a residency policy exists.
