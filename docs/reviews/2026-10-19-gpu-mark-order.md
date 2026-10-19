# GPU timing mark 顺序修复：Gaussian 标记被重复写入

日期：2026-10-19 · 批次：gpu-mark-order · 基线：`c5df0913`

## 症状

运行时每帧输出（Gaussian 场景）：

```
runtime: GPU timing mark 8 was refused (9) - the pass report is mislabeled
runtime: GPU timing mark 9 was refused (9) - the pass report is mislabeled
```

`9` 是契约的 `rhi::error::invalid_argument`。

## 根因

GPU 标记是**位置**语义的：后端只接受"下一个期望的下标"，否则拒绝（
`source/backends/vulkan/core/core.cpp` 的 `frame_commands::mark_gpu_timing`：
`owner.gpu_timing_marks[slot] != mark_index -> invalid_argument`）。

但同帧有两个发射者写了同一批下标：

1. `source/engine/runtime/runtime.post_graph.cpp` 的 deferred lighting 回调在 lighting 记录完后，
   一次性把 6..13（`lighting_end`、`gaussian_prepare/sort/finalize/raster_end`、
   `transparent_end`、`taa_end`、`main_end`）作为"后面 pass 的 0 ms 占位"全部写入；
2. Gaussian GPU 渲染在 lighting **之后**记录（`graph::gaussian` pass），其
   `gpu_frame_timing` 回调会真的写 `gaussian_prepare_end(7)`、`gaussian_sort_end(8)`、
   `gaussian_finalize_end(9)`（`source/engine/runtime/runtime.frames.cppm`）。

于是 lighting 回调先把计数推到 14，Gaussian 真正记录时再写 7/8/9 → 被位置检查拒绝
（每帧 3 行；用户看到的 8、9 是其中两行）。`9` 之后的下标（10..）期间没有再发，所以
`bloom_end(14)` 反而通过了 —— 这就是"只有 Gaussian 的标记被拒"的原因。

该重复随 `f57c54b2`（Gaussian GPU 渲染优化）引入：Gaussian 开始自己发标记后，
旧的占位列表没有同步。

## 修复

标记改为**单一发射者 + 补齐**：

- `runtime::gpu_marks_written` 记录本帧已被后端接受的标记数（在打开 timing range 时清零）；
- `runtime::gpu_mark()` 成功时递增；
- 新增 `runtime::gpu_mark_through(last)`：从下一个期望下标补齐到 `last`（区间 0 ms），
  拒绝时不空转；
- lighting 回调只写 `lighting_end(6)`；
- Gaussian 回调在记录后 `gpu_mark_through(gaussian_raster_end)`：Gaussian timing 已写的 7/8/9
  保留，缺的补上；未跑 Gaussian 时一次补 7..10；
- transparent / TAA copy 回调在记录后分别补齐 `transparent_end(11)` / `taa_end(12)`；
- composite 回调在 `bloom_end(14)` 之前 `gpu_mark_through(main_end(13))`：debug-view 帧、
  无 lighting 帧、被禁用的可选 stage 都在这里补齐，保证位置链不断。

这样每个真实阶段仍然拿到自己的标记，缺的阶段以 0 ms 占位补齐，不再有重复写入。

## 验证

- Gaussian 场景（`source/tests/fixtures/gaussian_ribbon.ply`，`gpu_timings = true`，
  validation layers on，`--capture-frames 2 --capture-windowed`）：exit 0，
  **无任何 `GPU timing mark ... was refused`**，无 VUID/panic；
- Gaussian + TAA 场景（`taa = true`，3 帧）：exit 0，无拒绝行；
- `test_runtime_dyn --with-device` 1167 checks PASS；`test_texture_group` 88 PASS；
  `test_acceleration_structures` 495 PASS；
- `ctest` 50/51：`test_stacktrace` 的 symbolizer 断言失败与本改动无链接关系
  （`test_stacktrace.exe` 不链接 engine/runtime，且直接运行复现），属环境/符号化侧问题，
  单独跟踪。
