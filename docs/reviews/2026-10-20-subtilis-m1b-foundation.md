# Subtilis M1b CPU 基础与 CI 修复

基线：`feb66065`。保留用户的计时与源码整理提交；用户要求完成 CI 后暂停，本轮将在本地验证后提交并推送，记录远程结果。

## 实现边界

- 新增 `deren.engine.subtilis.asset`：自有不可变 CPU 数据、底向上唯一父森林、完整 roots/children/cluster 归属、规范化最细索引前缀、单调误差、保守 double 包围球和最大 cut 容量。
- 新增 `deren.engine.subtilis.lod`：确定性、无历史的 CPU cut，父/子表示互斥，保留多根顺序；透视、正交、非均匀缩放/shear、近平面与无效视图策略；复用 M1a publication/fallback。
- 未接入 runtime，默认关闭策略与 M1a 渲染行为保持现状。没有简化构建器、磁盘格式或真实多级几何上传；不能宣称已有运行时降面收益。
- 工厂只证明结构/窗口/元数据约束，实际顶点包围、几何误差、替代区域与接缝由后续 builder 证明。CPU 记录不是 GPU/disk ABI。

## 测试驱动发现

先新增测试并观察缺少模块的构建失败，再实现模块。之后两项回归先失败再修复：移动后资产的缓存容量未清零；近平面强制细化状态未传到安全深度的后代。后者现在同时覆盖透视与正交。

资产检查包含输入独立所有权、非法边/根/窗口、重复覆盖与预算；cut 检查包含完整替代、深度/变换、容量清空、发布失败与 alias、合法零结果。两个模块均用两万层链验证迭代处理，不依赖递归栈。

## ASan CI 原因与取舍

旧运行 [39240772119](https://github.com/YzK0741/deren/actions/runs/39240772119) 的 ASan job 在 `probe_backend` DLL 链接失败：`__sanitizer_annotate_contiguous_container` 未定义。`-fno-sanitize=all` 只能禁用 DLL 自身的编译器插桩，不能消除在 ASan BMI 中预处理后保留的 libc++ 显式模板调用。

MinGW/Clang 现在统一给 BMI 生产者和消费者定义 libc++ 支持的 `__SANITIZER_DISABLE_CONTAINER_OVERFLOW__`。只给 DLL 消费者定义不能修复问题，独立最小复现已验证这一点。保留原有 DLL 的 compile/link `-fno-sanitize=all`，不引入空运行时符号或第二份 ASan。

覆盖取舍：统一关闭 libc++ 的可选逻辑容器越界标注，编译器的堆/栈/全局内存 ASan 和 UBSan 插桩保留。独立复现中的动态库加载调用成功，故意堆越界仍被 ASan 检出、除零仍被 UBSan 检出。实际项目的新 ASan 构建也已成功链接 `probe_backend`，并完成动态加载测试（305 checks）。DLL 导入表没有 ASan runtime，符号表未发现 ASan/UBSan/容器标注引用。

本地原始证据在忽略的 `build-release-dyn-clang64/ci-annotation-repro/`（`summary.md`、`host-asan.log`、`host-ubsan.log`）、`subtilis-asan-build.log` 与 `build-asan/Testing/Temporary/LastTest.log`。复现不替代实际项目验证。

## 静态检查与审查

按 CI 原清单重跑发现 19 条剩余告警及已删除的 `better_pmr.cpp` 过期条目：移除过期条目，消除无效声明顶层 const、可移动所有权/快照的多余拷贝、冗余 lambda 参数和缺少尾逗号。Clang 23 对模块空默认参数逗号的误定位仅在对应声明上定点豁免；上传源按值持有的策略保留并说明，不全局关闭检查。新 Subtilis 三个模块加入 CI 静态检查。

独立只读审查未发现资产/cut 的可操作问题；后续 CI 整理的所有权移动也另行复核。完整最终门禁结果见下方验收。

## 验收

- 完整 Release 构建成功，CTest **53/53** 通过（含边界、文档/CI 清单、既有 M1a 与新 CPU 模块）。
- 最终 ASan/UBSan 构建成功，相关测试 **7/7** 通过：Subtilis 三项、dynamic_link、高斯 splatting/raster/gpu_frame。新资产与 cut 分别 30、33 项检查，原 M1a 29 项检查；动态加载 305 项检查。
- CI 同等静态检查（31 个文件、warnings-as-errors）通过，退出码 0；未全局禁用检查。

独立审查和所有权整理复核均未发现可操作问题。不能把本地结果视为新的远程 CI 结果；远程流水线结束后补记并暂停。测试没有验证实际简化几何的图像和性能，因为该部分尚未接入。

### 首轮远程结果及收尾修复

提交 `508769d5` 的 [39311962445](https://github.com/YzK0741/deren/actions/runs/39311962445)：Release 构建/测试、Debug 完整构建、格式、clang-tidy、POSIX 全部通过。ASan 构建/链接通过，测试暴露两处既有遗漏：

1. `test_runtime_injection --child-post-resources` 只填 `resolved_io.list`，未填 push endpoint 使用的 `cmd`。严格 UBSan 在本地复现 `taa.cpp:267` 的空引用绑定；fixture 现在借用同一个有效的帧命令对象，失败时输出子进程诊断。不改 TAA 生产逻辑。
2. `test_acceleration_structures` 被单独注册到 CTest，却不在 ASan 构建清单；现在统一纳入 `VR_TEST_TARGETS` 和 CI。其默认模式在设备开关之前执行多项 CPU 资源检查，因此保留 sanitizer 覆盖。去掉默认 CPU 测试对 Vulkan DLL 的强制构建依赖：该 DLL 不插桩，强拉它会使已插桩静态库的 ASan/UBSan 符号链接失败。完整 Release/Debug 仍构建后端；设备模式需显式构建 `test_acceleration_structures deren_vulkan`，脚本提示同步更新。

收尾本地验收：完整 Release 构建与 **53/53** 测试通过；按 CI 原目标清单构建并启用相同的 halt-on-error 设置，ASan/UBSan **52/52** 通过（与 CI 一样仅排除需要完整应用导入表的 `test_native_boundary_zero`）。独立审查未发现可操作问题。等待修复提交的远程结果后暂停。

## 下一入口

固定 meshoptimizer 版本与许可证，先实现按 primitive/材质划分、锁区域外边界的索引简化构建器，将生成结果交给 `asset::make`。随后再做全驻留几何上传、MASK/toon/阴影强制 LOD0 接线及距离序列/混合层级接缝的视觉与性能验收。不能仅凭本轮结构校验启用真实简化。
