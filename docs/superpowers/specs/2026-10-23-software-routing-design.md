# Software raster routing redesign

User approved the order: remove CPU tile CSR, improve triangle-scale routing, then GPU routing after HZB. Preserve the existing opt-in triangle producer and validated coverage/paired-depth-ID contract. No commit/push requested. Continue inline and pause with a durable checkpoint if the five-hour remaining quota reaches 5%.

## Architecture and alternatives

Retaining CSR is inexpensive to implement but leaves redundant CPU work. Rewriting all routing on the GPU immediately combines classification, ID mapping, recovery and indirect dispatch changes. The approved staged approach first produces a direct cluster list from the parallel stable split, then changes the routing metric, then moves routing to the GPU. Each stage is measured before changing the next variable.

Stage 1: append one entry for each on-screen software suffix record using the already computed split/order/projections. No normal-frame CSR counts, offsets, duplicate tile entries or sorting. Keep the existing mapped lane stride/entry offset to isolate CPU behavior; only write header/used entries. Legacy tiled path still builds CSR. CPU oracle builds CSR only for explicit diagnostic screenshots. Malformed input, capacity or publication failure must restore complete hardware runs, never silently truncate. Packed IDs continue to name reordered suffix positions including offscreen suffix records.

Stage 2: derive conservative per-cluster triangle-edge scale from immutable asset geometry without changing the 48-byte meshlet record. Compare projected triangle scale rather than entire sphere diameter. Near-plane/unsupported geometry remains hardware. Benchmark thresholds and pathological long/thin triangles; do not enlarge thresholds blindly or promote without correctness/performance evidence.

Stage 3: GPU HZB/recovery survivors feed bounded hardware/software routing lists and indirect arguments, with RHI compute-indirect dispatch support. Preserve full candidate coverage, ID mapping and complete fallback; route recovery only after its final decision. Exact buffer/graph design is refined using stage-2 evidence before implementation. No CPU readback stall in production.

Further unpack removal requires separate timing and explicit graph-visible candidate-buffer ownership. It is not bundled into the CPU-only experiment. Retain software default-off until end-to-end benefit is established.

## GPU routing refinement

Routing must occur after the history HZB decision and again after recovery, because hardware primary depth is needed to build current-frame recovery HZB. Integrate optional routing into existing HZB compaction rather than adding a separate routing dispatch and barriers. CPU keeps DAG selection but publishes original candidate order, per-record object edge metadata (infinity for unsupported item policy) and a zero work-list counter. GPU classifies only visible/recovered candidates, emits hardware survivors and software `(item, original_candidate)` entries, and updates compute-indirect arguments. Hardware IDs name survivor positions; software IDs name original candidates, decoded through existing software_selected table distinction. No CPU split/reorder/CSR on this mode.

Use bounded compare-exchange reservations per item for the software global list; failure leaves that item's visible candidates fully hardware, never truncated. Work-list capacity remains65536. GPU pass mode is optional and requires HZB, triangle pipeline, enabled buffer-int64 atomics, compute indirect support and prepared buffers; otherwise retain working CPU routing. Compute indirect is appended to RHI with default unsupported and an enabled-capability query; Vulkan implementations validate ownership/usage/range/alignment.

Metadata and compute-argument buffers need explicit graph imports/usages. Existing software work list gets HZB write/read-write declarations and software read declarations. Argument writes transition to indirect reads. Shader camera reads are also explicit graph inputs. Existing private atomic scratch remains internal to the software pass. Frame-slot-fenced host publication seeds counter/arguments; primary/recovery routing share and append to the same lane.

Temporary software-eligibility bits may be used inside HZB compaction, but normalize final flags to the existing0/1/3 contract before consumers/probes. Keep existing primary/recovery HW counts and indirect command slots. Capture-only readback can validate hardware/software partition against flags and reconstruct CPU raster from actual GPU list plus immutable source geometry; it must publish GPU writes to host before reading. Production must not read counts back to dispatch.
