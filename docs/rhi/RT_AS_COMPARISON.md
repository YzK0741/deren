# RT 加速结构迁移前后对照（2026-10-08）

> 最新状态：当前 TLAS 描述符映射修正已通过严格 RT 验收。下文“完整 RT 仍失败”等内容保留为修正前的诊断记录。

## TLAS 映射修正与实际遍历验收

RHI 新增 `acceleration_structure_heap_binding` 描述，管线描述尾部字段通过 `struct_size` 覆盖检查读取。Vulkan 后端将 shader 的普通 AS binding 映射到现有 heap TLAS 槽；不分配 descriptor set，不改变图像和缓冲的 heap 访问。后端拒绝越界、未对齐、空数组、重复 binding 和不存在的 shader stage。

新增确定性 GPU 遍历探针：两帧槽分别绑定不同位置的 TLAS，每个槽验证已知命中、几何未命中和实例掩码拒绝，结果均须为 `1,0,0`。本机探针 **75 checks / 0 failed**，提交和等待均为 0。

本轮复跑 `scripts/windows/check_rt.ps1` **通过**：Sponza、1080×960、40 帧，14 个 MASK caster 烘焙，验证层干净，成功生成截图。严格脚本先验证 GPU 遍历结果，再验完整渲染，避免提前返回假通过。该结果证明本机映射路径可用；不把驱动问题的普遍根因视为已证实。

用户要求将实现分批本地提交；构建、格式、GUI 和冻结渲染的最新结果另见 `progress.md`。

## 已确认的修正

| 项目 | 迁移遗漏 | 当前修正 |
|---|---|---|
| 无索引三角形 | 地址为 0 时保留 UINT32，触发 VUID 03806 | 恢复 VK_INDEX_TYPE_NONE_KHR |
| 几何透明性 | 无条件设置 OPAQUE | 恢复非 opaque，允许 any-hit |
| TLAS 策略 | FAST_TRACE | 查询、构建、更新统一 FAST_BUILD；BLAS 保持 FAST_TRACE |
| scratch | 未对实际设备地址对齐 | 分配对齐余量并对地址对齐；可更新结构覆盖 build/update 两种大小 |

这些遗漏已修复，但完整 RT 仍失败。实例明确打包为连续 64 字节 Vulkan 记录；72 字节契约记录的容量分配没有改变 GPU 步长。独立审查未发现单几何场景的对象提前释放。

## 历史基线的假通过：撤回之前的回归范围

设备为 RTX 4060 Laptop GPU、驱动 617.42；同机、同 SDK、验证层开启。**完成 40 帧和 CPU 记录 tracing 日志，不等于 GPU 执行 TraceRay。**

| 历史版本或诊断 | 已验证结果 |
|---|---|
| 无 RHI 的 c245102（26a3190^）原版 | 40 帧，但 RT visibility 描述符在 heap 创建前发布被跳过；不能作为遍历通过基线 |
| f188b2c、7af1018、9e7eb3b | 同类 40 帧假通过；不能用来二分实际遍历故障 |
| c245102，仅在 heap 就绪后重新发布原有 visibility、G-buffer depth/normal 描述符 | 原生 vkQueueSubmit 先返回 0，随后返回 VK_ERROR_DEVICE_LOST (-4)，未产出截图 |
| 874675d、ae66fbd、1357c0e、d3e319f、41d6b71、当前版本 | 描述符有效后，实际遍历导致 device_lost；RHI 提交结果为 11 |
| d3e319f 原版 | AS 属性查询 sType 错误，TLAS 上限为 0；不能作为正常遍历基线 |
| 中间版本只补属性查询 sType | 能构建 103 个 BLAS/TLAS 实例；此补正不能解决实际遍历故障 |

源码显示旧 runtime 在 heap 建立前写入图像描述符，heap 未就绪便直接返回。heap 内存为零，raygen 的 visibility.GetDimensions 得到零尺寸后提前返回。874675d 将 visibility 图像移至引擎，并在 heap 初始化后写描述符，暴露了此前没有执行的遍历。**因此撤回 f188b2c..41d6b71、7af1018..874675d 等“故障引入范围”；874675d 是暴露点，不是已证明的引入点。**

无 RHI 的补描述符实验只重新发布已有图像，不改图像分配、着色器或加速结构。隔离源码位于 build-release-dyn-clang64/rt-no-rhi/src；原始可执行文件和日志在 original-check，源文件备份为 runtime.original.cpp、core.original.cpp，原始 before.tar 保留。诊断日志位于 bin/rt-check/debug.log。旧截图被验收脚本清理，不再作为可用证据。

无 RHI 与当前 rt_shadow.slang 的 SHA256 均为 E803D01B82063434F521B44272015FEFD7A1DFDE4A7A41793586D141990162DA，四个 RT SPIR-V 在比较版本之间也完全相同。AS 构建数量与 CPU dispatch 尺寸只能证明录制意图。

## 已排除或没有形成有效证据的方向

当前 Sponza 和 Box 均失败；恢复旧 opaque/TLAS/scratch 行为仍失败。保留构建与管线但跳过 TraceRay、或实例掩码置零能完成 40 帧，仅为诊断。强制 opaque 并跳过 closest-hit、单独换旧 GLSL raygen/closest-hit、追加 TLAS build-write 到 RT AS-read 屏障，均未解决故障；临时修改已撤回。TLAS 描述符首个 64 位值与对象设备地址一致。GPU 辅助验证在管线创建附近自身退出，不能据此断言应用或驱动根因。

## 下一项单变量实验

验证同一 heap TLAS 槽通过普通 shader AS binding 映射访问，保留 heap、RHI 和实际射线遍历。NVIDIA 开发者论坛的第一手最小复现报告：heap 原生 AS 访问导致 device_lost，而 VkDescriptorSetAndBindingMappingEXT 映射相同描述符能恢复命中。此报告与本机症状相符，但在本机验证前只是根因假设。

参考：https://forums.developer.nvidia.com/t/vk-ext-descriptor-heap-ray-query-causes-vk-error-device-lost/376527

映射由后端翻译，不能把 Vulkan 类型重新带回引擎。验收必须增加 GPU 命中/未命中证据，避免 GetDimensions 提前返回再次造成假通过。

## 验证与交接

AS 探针已修正 vector 字节数、提交前释放对象、micromap 构建依赖，增加无索引几何与真实双实例容量拒绝，真设备 45 checks / 0 failed，日志无 VUID。它只验证构建/更新，不能替代遍历验证。

此前完整构建、CTest 19/19、冻结渲染 14/14、格式检查和动态边界门禁通过。完整 RT 未修复；不关闭 RT、不更新冻结基线、不把全可见诊断图像当修复。未提交或推送。

历史隔离树 main.cpp 应保存为 main.cpp.reference，防止主工作区递归格式扫描收集；重建前恢复，完成后改回。当前 no-RHI 诊断树需同样改回。

用户最新停止规则：额度剩余 25% 时停止，写入阶段结果与下一步；不自动消耗重置额度。
