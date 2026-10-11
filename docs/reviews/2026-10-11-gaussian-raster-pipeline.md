# Gaussian raster pipeline checkpoint - 2026-10-11

Scope: callable RHI raster pass, CPU references, immutable frame uploads, Slang pair and isolated HDR GPU probe. Viewer/normal scene frame-chain integration remains the next batch.

Committed work before final fixes: a5ff51b0 plan, a489efc9 CPU reference, 846f556b raster pass/snapshots/shaders, 02137c30 static upload intent correction, 7a673709 GPU probe. None has been pushed yet; origin/master remains126c8e9c.

Independent final review covered126c8e9c..7a673709 and returned three Important findings, no Critical or Minor. A single fix pass addressed all three:

- Delete asset/frame copy, move and assignment operations, closing a mutable-alias path that could release buffers referenced by an in-flight snapshot. Owner API regression RED126 checks/6 failures.
- Include pixel regularization in side culling by checking the actual projected footprint in both CPU and shader. Four-edge regression RED131/10; the independent GPU edge oracle read green0 instead of0.09443781.
- Evaluate active local view-direction SH radiance before frame GPU allocation. Overflow regression RED134/12, including unchanged allocation count.

After these fixes: raster135/0; GPU five cases/5181 pixels, maximum RGB error0.0008074641 versus tolerance0.005 on RTX4060 Laptop, NVIDIA617.42; no validation errors. Updated vertex/fragment spirv-val PASS. Full suite before these fixes was23/23; post-fix full suite, format and strict analysis are being recorded in the companion progress report.

Review rulings: viewer/TAA/interactive real-asset scenarios follow later; the host retains submitted snapshots by completion slot and keeps the backend root alive; GPU sorting and unified mesh transparency follow later; CPU allocation throughput/Cholesky overdraw await viewer measurement; other GPUs/backends have structural RHI coverage only; unrelated Vulkan constructor edits and root probe DLL deletion remain excluded.

Implementation rulings: immutable per-frame initial uploads; explicit GLM matrix columns; Cholesky ellipse basis; coherent bytes prepared before submission; existing upload-capable uniform_gpu_only plus storage/address flags for static data because storage_gpu_only is allocate-only. No RHI ABI change.

Quota checkpoint: primary94% used/6% remaining, weekly31% used. User requires stopping at5% remaining and saving progress. Do not spend reset credits. Next continuation: check this report's final local gate result, commit any remaining verified fixes, push authorized master commits, verify LLVM23/Debug/ASan CI, then complete the raster batch before viewer integration. No re-review is required for the covered RED/GREEN fixes.

Post-review local gate: CTest exit=0, boundary/format exit=0, strict clang-tidy exit=0.
