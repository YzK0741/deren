# RHI resource and synchronization contract expansion

The user approved extending the existing ABI 53 standard around resource creation, mapped updates,
submission completion, binding state, capability errors and evidence. Existing implementation-backed rules
are clarified directly; behavior-changing decisions require user input.

## Approved decision and approaches

Use directly mapped buffers without explicit cache-maintenance calls. The user selected this over merely
recording the gap and designing flush/invalidate later. Vulkan requires HOST_VISIBLE | HOST_COHERENT for
all exposed host-mapped buffer intents; HOST_CACHED is a preference for uniform_cached, not a substitute
for coherence. VMA CPU_ONLY already implies coherence for cached/readback roles; staging_source currently
uses CPU_TO_GPU without requiring coherence. Explicit requirements avoid dependence on VMA policy.

No ABI/layout change, new mapping API or automatic retention is introduced. Coherence is distinct from
completion, execution ordering and resource barriers. Internal upload staging may remain non-coherent
because its upload code explicitly flushes. Failed memory-type selection follows existing allocation failure
handling; this change does not invent a recoverable out-of-memory API.

## Scope and validation

Document creation intent versus extra flags, initialization lifetime, mapped-span validity and CPU/GPU
access ordering, tokens versus presentation and barriers, plus a test coverage matrix. Keep implementation
notes distinct from portable guarantees. No new unknown-enum/flag rejection policy is promised.

Use the existing generated production-body fixture for allocation requirements, then device readback after
host mutation and token completion. Existing pending/timeout/foreign-token tests remain authoritative.
