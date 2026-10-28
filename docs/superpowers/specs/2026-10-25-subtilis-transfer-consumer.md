# subtilis geometry transfer consumer

Migrate upload_geometry's two immutable vertex/index payloads to one buffer_transfer batch when both
command_submission and scoped_synchronization are available. Preserve ready-on-return semantics. Add an
engine-only geometry_upload_mode (automatic, synchronous, batched) for explicit comparison and capability
refusal: automatic falls back only on capability absence; an actual batch failure is returned, never retried
silently through a second path. No RHI ABI change, background job, frame-capture migration or registry API.

On the batch path, the private one-shot resource subchain creates allocation-only vertex/index generations;
source remains a dependency. Alias each generation's ownership into a buffer lease, enqueue copied payloads,
submit once and wait for that receipt before constructing/publishing uploaded_geometry. No uninitialized
snapshot is externally exposed. Do not give allocation-only requests a fake initialization key: this private
subchain is never reconciled by callers after publication. The synchronous path retains existing content
initialization keys and factory upload. Existing AS build pre-barriers already cover transfer writes.

Publish for vertex input and available shader-read stages. A batch-capability refusal happens before creating
resources. Tests keep the CPU fallback, force-batch unsupported, byte-exact GPU readback for both modes,
one batch receipt for two uploads, and timing with warmups/repeats. Record host-ready latency and allocation
tradeoffs, not frame-rate claims. Staging pooling is still deferred.
