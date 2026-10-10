# 原生 Slang 迁移与现有代码审查

日期：2026-10-09。Shader 迁移提交：`1088891f`；高斯泼溅计划提交：`d7f864c1`。

## 本轮完成

- 高斯泼溅计划已写入 `docs/superpowers/plans/2026-10-09-gaussian-splatting.md`。先做静态训练结果查看器，再做 GPU 排序及混合场景；本轮没有实现高斯功能。
- 全部 39 个运行时 shader 入口和 7 个公共文件已改成原生 Slang，移除编译命令中的 `-allow-glsl`、`VR_SLANG`，旧 GLSL 与官方参考原文归档到 `source/reference/shaders/`。
- 保持描述符堆 64/32 字节步长、CPU/GPU 数据布局、矩阵方向和双面法线行为。原生求逆通过 Slang 的 SPIR-V interop 保留旧 `MatrixInverse` 指令。
- 补充源码门禁，防止公共 GLSL 或兼容编译选项回到运行时 shader 目录。相关源码断言已更新到 Slang 写法。

## 验证证据

Release 构建通过；20/20 CTest 通过，包括 shader 来源、资源布局、卡渲、文档和后端边界测试。
14/14 冻结场景实际哈希一致，涵盖 PBR、SSAO、AA、透明、阴影、变形及新旧卡渲链。
RT 检查确认 GPU hit/miss/mask、两个 TLAS 槽位及 40 帧绘制，验证日志无错误。
GUI 开关各 40 帧，验证日志无错误且截图不同。
PowerShell 和 POSIX 手动编译各产生 35 个生产输出，与 CMake 对应输出逐字节相同；另外 4 个 RT 探针入口由 CMake 编译。

日志位于本机构建目录的 `native-slang-build.log`、`native-slang-ctest.log`、`native-slang-render.log`、
`native-slang-manual.log`、`native-slang-posix.log`、`native-slang-rt.log`、`native-slang-gui.log`。
冻结门禁参数是 `-Full -Compare frozen`；本机旧参考图过期，其独立的 changed 计数不是冻结门禁结果。

本机 Slang 为 2026.13.1，未运行新的远程 CI。独立审查核对了
[CI 固定版本 2026.18.2 的官方解析源码](https://github.com/shader-slang/slang/blob/v2026.18.2/source/slang/slang-parser.cpp)，
确认使用的内联 SPIR-V 语法受支持；这不替代远程构建验证。

## 现有代码发现

以下为独立静态审查、主线程复核的发现，尚未修改这些既有实现，也未为全部问题运行专门复现场景。
正常路径的测试通过不能覆盖这些边界条件。Shader 迁移审查没有发现可确认的 P1/P2。

### 1. [P1] 多 geometry BLAS 的计数和范围数组不足

位置：`source/backends/vulkan/core/core.api_core.cpp:3328`，同类 build/refit 位于 3523、3585。
创建接口接受多个 geometry，循环却反复覆盖同一个 `structure->range`。尺寸查询的 `geometryCount`
是完整 geometry 数量，`pMaxPrimitiveCounts` 却仅指向一个局部 uint；实际构建也只提供一个 range。
请求含两个 geometry 的 BLAS 时，驱动会越界读取计数/范围，可能错误分配、错误构建或崩溃。
建议保存每份 geometry 的计数与 range，并给尺寸查询、build、refit 传入对应数组。

### 2. [P2] 缓冲初始化忽略输入 span 的长度

位置：`source/backends/vulkan/core/core.api_core.cpp:1170`、`source/backends/vulkan/core/vma.cppm:793`。
例如申请 4096 字节 storage_coherent 缓冲，只提供 16 字节 initial_bytes，后端仍以 desc.size
执行 memcpy，越界读取源内存。vertex/index 上传也有同类问题。
建议在后端资源入口校验非空初始化 span 足以覆盖上传长度，并增加短输入回归测试。

### 3. [P2] 删除中间 GUI panel 使其他引用失效

位置：`source/backends/vulkan/graphical_user_interface/gui_dll.cpp:208`。
GUI adapter 存放在 `std::deque<panel_adapter>`，调用方持有创建函数返回的引用。
创建 A/B/C 后删除 B，中间 erase 会使其他元素引用失效；继续使用 A/C 可能访问已移动或销毁对象。
建议 adapter 也使用稳定地址的拥有型指针，与底层 panel 的生命周期保持一致。

### 4. [P2] descriptor heap 接口漏填 commands 时解引用空指针

位置：`source/backends/vulkan/core/core.api_core.cpp:899`。
heap 已就绪时传入默认 heap_bind_info，commands 为 null，且没有 native extension。
命令选择函数进入非 frame-view 分支，强转后调用 native()；push_data 同样可触发。
建议在转换前返回 invalid_argument，并覆盖 bind/push_data 的空命令缓冲输入。

### 5. [P2] 动画用资产局部 node 索引匹配整个场景树

位置：`source/engine/animation/controller.cppm:331`；背景与主模型加载位置：`source/app/main.cpp:2154`、2340。
独立资产的 source_index 都从各自 node 表开始；controller 却遍历整个 runtime scene 并按此索引分组。
背景 GLB 与动画主模型索引重叠时，主模型动画会改写背景的同号节点，skin 世界矩阵缓存也可能取错来源。
建议引入导入实例身份，或只注册主模型实际导入的节点；合成节点也应有明确的非资产身份。

### 6. [P2] morph 的上一帧权重实际来自同槽位的前两帧

位置：`source/engine/animation/controller.cpp:288`。
代码在覆盖 active frame slot 的权重前，将该槽位旧权重复制为 previous。双帧槽各有独立缓冲，
连续绘制时 N 帧读到的是 N-2，而不是 N-1；因此 morph motion vector 会错误，影响 TAA 重投影。
建议像 skin history 一样保存全局上一帧权重，再发布到当前槽位。

### 7. [P2] 多个 mesh 共享 skin 时使用第一个 mesh 的逆世界矩阵

位置：`source/engine/animation/controller.cpp:407`、`source/engine/animation/init.cppm:443`。
所有引用同一 skin 的 mesh 共用一个 skin block，矩阵更新却只用第一个 mesh_world_inv。
若 M1、M2 两个 mesh 的世界变换不同，第二个 mesh 在 shader 中得到 M2 * inverse(M1) * J * IBM，
错误继承 mesh 之间的变换。建议按 mesh 实例分配 skin block，或统一为世界空间蒙皮并同步修改 shader。

## 后续优先级与范围

先修复多 geometry BLAS 的 P1，再处理缓冲长度和 GUI 引用稳定性；随后补齐 heap 参数检查和动画实例/历史状态。
建议完成这些正确性问题后开始高斯泼溅第一阶段。
本轮审查集中在 shader、引擎动画/资源路径、RHI、Vulkan 资源/RT 和 GUI 生命周期，未穷尽所有 pass 或跨帧同步路径。
工作区原有 `deren_probe_backend_detach_probe.dll` 删除改动保留，未纳入本轮提交。
