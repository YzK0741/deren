# 摄像机导入身份与原生 AS 诊断修复

## 摄像机世界变换

authored camera 使用的 `source_index` 是资产局部索引。背景和主模型出现同号节点时，原来的 DFS 会先选中背景节点。
现在应用调用 `scene_tree::find_world`，同时匹配 `import_instance` 和 `source_index`，并累积全部父节点变换。
没有找到匹配节点时，输出矩阵保持原值。

回归测试使用两个导入实例的同号摄像机，并覆盖父级平移、旋转、非均匀缩放与不存在的身份。
旧逻辑在 248 checks 中失败 5 项；修复后全部通过。
证据：`build-release-dyn-clang64/camera-red.log`、`camera-green.log`。

## 原生 AS：真实 heap 地址兼容处理

原诊断的 `--native-heap-traversal` 稳定在 fence wait 返回 `VK_ERROR_DEVICE_LOST`。
这项问题在原审查基线和旧 shader 上同样存在。本轮没有跳过遍历，也没有为原生路径添加 AS binding mapping。

逐项控制实验将故障定位到当前环境中的 `ResourceHeapEXT` AS 读取路径：

- GPU：NVIDIA GeForce RTX 4060 Laptop GPU，驱动 617.42；本地 Slang 2026.13.1。
- 驱动写入两个 heap 槽位的首个 64 位值，与各自的 TLAS device address 完全一致。
  观测偏移为 1112576、1112640，地址为 `0xe972500`、`0xe977800`。
- 使用真实第一个 TLAS 地址作为控制值时，`OpConvertUToAccelerationStructureKHR` 与追踪正常完成；
  第二槽射线正确地错过第一个 TLAS。转换指令本身不是该失败的充分原因。
- 通过 heap built-in 读取整数地址、直接读取不透明 AS，以及改用 8 字节 array stride，仍然设备丢失。
- 改为从绑定 heap 的真实 GPU 地址读取两个槽位，随后进行同样的 AS 转换和追踪，两槽都返回预期结果。

正式兼容处理只用于这项诊断：host 将 `descriptor_heap::bindings().resource.address` 放进 24 字节 push block；
native shader 通过 BDA 读取 `(17384 + frame_slot) * 64` 的 AS 地址，并执行原来的射线。
它仍读取驱动写入的实际 heap 数据，验证两个不同 TLAS、命中、几何未命中和 instance mask。
host 检查 heap 地址/读取范围，静态断言保证 push block 中地址位于 offset 16。

**范围限制**：通过的是原生 heap 数据的 BDA 访问；这不代表 `ResourceHeapEXT` built-in 的驱动问题已修复。
shader 保留 `builtin_main`，可单独编译以复现原始路径。生产映射路径、其他 shader 的访问方式均未修改。
未验证其他 GPU/驱动；没有修改系统驱动或替换编译器。

Slang 明确记录了 [AS 的 64 位 heap load 与转换规则](https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/03-convenience-features.html)，
[Vulkan heap 接口](https://docs.vulkan.org/spec/latest/chapters/interfaces.html)允许通过 heap 指针读取数据类型。
上述控制实验支持当前驱动对该 built-in 访问的兼容性故障；尚未在上游定位具体实现缺陷。

## 验证

- 完整 Release 构建通过。
- 20/20 CTest 通过，摄像机回归为 248 checks、0 failures。
- 映射 AS 单/双 geometry 各 84 checks、0 failures。
- 原生 AS 单/双 geometry 各 81 checks、0 failures。
  四种组合的两槽结果均为 `1,0,0 / 1,0,0`，Vulkan 验证日志无错误或警告。
- 独立代码复审没有发现可确认的新 P1/P2；明确记录了 BDA 与 built-in 验证范围的区别。
- 冻结渲染基准 14/14 实际哈希一致，未修改基准。

证据位于 `build-release-dyn-clang64/followups-build.log`、`followups-ctest.log`、`followups-as-*/`。
渲染证据为 `followups-render.log`。没有运行远程 CI。
可选真实 MMD motion 检查因未设置 `VR_MMD_MOTION` 而跳过。

工作区原有的 `deren_probe_backend_detach_probe.dll` 删除未纳入提交。
