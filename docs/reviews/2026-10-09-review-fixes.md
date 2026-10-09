# 代码审查修复结果

日期：2026-10-09。对应原报告：`2026-10-09-native-slang-and-code-review.md`。
报告中的 1 项 P1、6 项 P2 均已修复，RHI/GUI 插件 ABI 与 shader 不变。

## 分批提交

| 提交 | 修复范围 |
| --- | --- |
| `be5da49b` | 每 geometry 保存 BLAS counts/ranges；拒绝非空短缓冲初始化 span；拒绝 heap 的空 commands |
| `20063372` | panel adapter 使用稳定地址的 list 存储，中间删除不再使其他 panel 引用失效 |
| `29de5492` | 动画按导入实例隔离；morph 保存全局上一帧权重；每个 runtime mesh 独立 skin block |

动画同时保留资产原始 skin 编号读取，兼容旧单资产三参数 init，并让合成 primitive 节点使用无效资产索引。
scene clone 会保留 source_index 和导入身份。主应用显式传入主模型的导入身份。

## 针对性红绿证据

- **BLAS**：旧实现无法创建含两份 geometry 的结构。修复后 GPU 构建与 refit 通过；命中三角形位于第二份 geometry，
  与第一份有不同 primitive count。`test_acceleration_structures --with-device --multi-geometry` 为 83 checks、0 failures，
  两个 TLAS 槽位均返回 hit/miss/mask 的 `1,0,0`。独立 debug 日志没有 Vulkan 验证错误。
- **缓冲/heap**：旧实现有 3 条短 span 拒绝断言失败，随后空 commands 调用崩溃（访问违例）。
  修复后的 `test_runtime_dyn --with-device` 为 18 checks、0 failures，也验证了正确初始化和空 span 分配仍有效。
- **GUI**：旧 deque 删除中间对象后有 4 条保留地址断言失败。修复后 `test_gui_plugin` 为 35 checks、0 failures。
  回归不解引用悬空地址；它比较实际生产存储类型中存活对象的身份与地址。
- **动画**：旧实现为 220 checks、11 failures，覆盖背景/主模型同号节点、合成节点、双槽 morph 历史与 clip 切换、共享 skin。
  最新修复为 238 checks、0 failures，额外覆盖单资产兼容调用、重复 mesh 实例、旋转/非均匀缩放、背景 joint 污染和前置未使用 skin。

原报告中的 VMA 文件位置应为 `source/backends/vulkan/core/vma/vma.cppm`；skin 初始化当前实现位于 controller.cppm。

## 全项目验证

- 完整 Release 构建通过，20/20 CTest 通过，包含 native/backend 边界检查。
- 14/14 冻结渲染场景实际哈希一致，没有修改基准。
- GUI 开关各 40 帧，验证日志干净，截图不同。
- 生产 RT 路径 40 帧通过，GPU 遍历验证 hit/miss/mask 与两个 TLAS 槽位，验证日志干净。
- 独立静态复审没有发现本轮修复引入的可确认 P1/P2。

本地证据位于 `build-release-dyn-clang64/review-*.log` 以及 `review-multi-geometry/`。
没有运行新的远程 CI；真实 MMD motion 的可选测试因未设置 VR_MMD_MOTION 而未执行。

## 单独记录的后续事项

1. **原生 heap AS 诊断仍会设备丢失，原因未定位。**
   `test_acceleration_structures --with-device --native-heap-traversal` 返回 `VK_ERROR_DEVICE_LOST`。
   单/双 geometry 都可触发；用 Slang 迁移前保存的诊断 shader 仍失败。
   为区分回归，临时构建了未修改的 `a8c287f8` Vulkan 后端，诊断同样失败；随后自动恢复当前源码并成功完整重建，
   再次验证当前双 geometry 和资源测试通过。因此不能归因于本轮修复，也没有据此断定驱动有缺陷。
   生产 RT 管线使用的映射装载路径通过。该诊断失败没有被隐藏或改成成功。
   证据：`review-native-baseline-backend.log`、`review-native-current-restored-build.log`。
2. **[P2] authored camera world 查找仍仅比较资产局部 source_index。**
   `source/app/main.cpp:2391`，背景节点与主模型 camera 同号时可能选错摄像机世界变换。
   这是独立复审发现的既有问题，未列入原七项，不属于本轮回归；后续应按同一导入身份筛选并补摄像机路径回归。

工作区原有的 `deren_probe_backend_detach_probe.dll` 删除保持原状，未纳入修复提交。
