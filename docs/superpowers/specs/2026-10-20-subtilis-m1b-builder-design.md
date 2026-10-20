# Subtilis M1b：索引层级构建器

状态：待审阅；基线 `d9285550`。承接 [既有设计 §11、§13](2026-10-18-subtilis-design.md) 和已通过 CI 的 CPU asset/cut。

## 1. 目标与交付边界

目标是生成有真实减量、可供现有 CPU cut 消费的层级几何，让下一步上传有可靠输入。采用唯一父 group 森林、原顶点复用、全驻留路线；不引入 DAG、流送、软件光栅或 GPU select。

本子阶段交付可独立测试的纯 CPU builder，包括固定的 meshoptimizer 依赖、完整索引 payload、validated asset、来源/构建统计。随后接入上传与 runtime；只有真实几何上传、距离序列及图像/性能验收完成，才算整个 M1b 完成。本阶段不把 builder 塞进每帧流程，也不发布磁盘格式。

已有约束：utility::vessel、自有分配服务和 OOM fast_panic；每个 item 的既有 reservation、14-bit 可见序号、48B meshlet 不变。保留用户改动和未跟踪文件。继续沿用 5h 剩余 <=5% 时记录并暂停的要求。

## 2. 路线选择

选 meshoptimizer 的基础聚类、分区与简化函数，加自有的小型唯一父树。它直接适配已有 `asset::make` 和 `select_cut`。

直接采用上游 clusterlod 的 DAG 会改动已验证的覆盖/预算模型；全 primitive 的离散 LOD 虽更简单，却绕过了现有 group cut。两者均不作为本阶段的正式实现。

## 3. 依赖与分配

- 固定 meshoptimizer **v1.3 / `9e1f07b159d3cb777f1c67ed31fc11fd117986f4`**，MIT。实施时再 vendor 必需源文件、许可证、来源清单和校验值；更新 `source/third_party/VERSIONS.md`。构建不联网。
- 项目私有静态 target，不导出 meshoptimizer 头或上游数据结构；不引入 gltfpack、JS、编解码资产格式和上游 demo 容器。
- 项目适配层在任何算法调用前通过一次性初始化安装 utility::memory 分配/释放回调。所有项目入口都经过此适配层，初始化之后不再切换全局回调；不与算法并发修改 allocator。
- 项目数据使用 vessel。依赖的临时分配同样经过现有 mimalloc 服务；OOM 沿用 fast_panic，不返回 nullptr 给预期成功的第三方分配调用。非法输入和预算超限则返回 typed error。

依据：[固定版本接口](https://github.com/zeux/meshoptimizer/blob/9e1f07b159d3cb777f1c67ed31fc11fd117986f4/src/meshoptimizer.h)、[固定版本 clusterizer](https://github.com/zeux/meshoptimizer/blob/9e1f07b159d3cb777f1c67ed31fc11fd117986f4/src/clusterizer.cpp)。

## 4. CPU 接口与所有权

新增 `deren.engine.subtilis.builder`，输入一段单 primitive、单材质的三角形 draw window。复用 `meshlet_build_input` 的顶点/索引字节、stride/count、first_index/base_vertex；附带 normal 和 UV 的 float 属性偏移。位置沿用既有 position-first 约定。

`build_options` 包含目标分组大小、简化目标比例、相对 primitive 尺度的误差阈值、normal/UV 权重、最大深度和输出字节预算。初始值：目标 8 个 frontier group、目标索引比例 0.5、误差阈值 0.01、normal 权重 0.1、UV 权重 0.01、深度 16、输出预算 256 MiB。这些是可调构建参数，必须进入来源标识与统计，不是渲染正确性保证。

`build(input, options) -> expected<built_geometry, build_error>`。`built_geometry` 只移动，拥有原顶点有效字节窗口、uint32 索引 payload、validated `asset` 与构建 metadata；对外仅提供 const views。输入销毁后结果仍有效。顶点内容不改写、不重排，避免把顶点/索引代际匹配责任留给调用者。

索引 payload 的 `[0, lod0_index_count)` 是原 draw 的完整三角形顺序，16-bit 输入转换为 uint32，并以宽整数应用 base_vertex 后验证范围；后续仅追加 coarse 索引。所有 meshlet 使用 base_vertex=0。`lod0_index_count` 和 payload 总长度是两个不同量，不能拿总长度当作原几何的绘制或 BLAS 输入数量。

metadata 包含 source hash、builder schema/version、依赖 commit、完整参数、节点/根/记录数、层数、各层三角形数、无进展原因和 payload 字节数。hash 用于来源定位，不承担不可信输入安全验证；定义为有效源字节窗口、布局、draw window 和参数的规范化编码，不能散列带未定义 padding 的 C++ descriptor。

## 5. 输入检查与退化

算法调用前检查字节乘法和窗口边界、2/4 字节索引、三角形计数、base_vertex 的有符号加法、有效顶点索引、属性窗口、有限值及合法参数。用 memcpy 解码字节，不把可能未对齐的输入强转为 float 指针；适配到内部对齐数组。

空 draw 返回合法空结果。非法窗口/索引/属性返回错误；不把损坏输入交给上游 assert。超输出预算或叶记录数超过 16384 时返回 capacity。输出预算不冒充整个第三方算法临时内存的严格上界。

拓扑不适合安全简化时，可成功返回 LOD0-only 资产并报告原因；不得过滤原始三角形、删除独立组件，或凭空制造一张代表三角形。退化、重复面、非流形边等先判定并固定相关区域；无法证明安全就保留最细表示。runtime 之后仍可使用原 M1a 路径。

## 6. 构建流程

1. 复制有效源顶点窗口，规范化索引，建立原始三角形身份、位置等价类和区域邻接信息。相同位置按精确有限坐标识别（统一正负零），不做 epsilon 焊接，也不合并实际顶点属性。
2. 最细层保留现有顺序和每记录最多 85 三角形的切分，作为完整 LOD0 前缀。首个闭环不做 LOD0 再聚类，便于既有回退、阴影和 BLAS 对照；以后有证据再优化。
3. 用上游分区函数组织当前 frontier。一个已有 group 是不可拆分的分区单位，它的完整 coarse run 一起参与；目标大小是 group 数，分区实际大小和 cluster 数另外记录。对分区及成员按稳定源身份排序，不用 unordered 容器迭代顺序定义输出。
4. 合并候选 children 的当前 coarse 三角形，锁定第 7 节的边界/接缝，执行仅改索引的属性感知简化。不使用 permissive、sloppy、prune、位置/属性更新或删除组件的兜底。
5. 把候选结果重新聚类，展开 local indices 为原顶点索引窗口；每记录仍最多 85 三角形。meshoptimizer 的 bounds/cone 布局不直接拷到项目 48B 记录中。
6. 只接受非空、有限误差、边界/组件检查通过，且三角形数和 coarse 记录数都严格少于 children 当前完整表示的父组；其最坏 cut 还必须不超过 children 的预算之和。否则保持 children 为根，记录无进展原因，不生成假父节点。
7. 持续合并，直到无进展或达到深度上限。达到限制时保留全部 frontier 根；不会丢掉未能简化的分支。
8. 最后统一排列与验证，成功后才公开 immutable 结果。

候选追加在临时存储中完成；拒绝候选不留下无人拥有的 cluster 或悬空 child。分区、边界检测和打包均不得用深度不受控的项目递归。

## 7. 混合 LOD 接缝约束

每个 group 的源区域是其全部 LOD0 三角形身份的集合。其边界从该原始区域求得，不能只从已经简化的当前层推断。锁定原始区域完整边界上的位置等价类，以及同位置但 normal/UV 不连续的顶点；所有等价副本一致锁定。单材质 primitive 外边界自然包含材质分区边缘。

简化后重新计算按位置等价类编码的有向边界边集（保留重数），必须和原始区域边界相同；不得用一条长边替代若干原始短边。不能保持边集就拒绝该父组。这比只锁位置更强，防止细侧仍有中间顶点、粗侧已跨过去的 T-junction。

检查源区域中需要保留的连接组件均有输出表示，且未引入跨组件三角形。边界检查用于证明邻组表示能拼接，不等价于严格 Hausdorff 误差、所有内部折叠都合理或最终图像已验收。

## 8. 误差与几何包围

builder 的误差使用 primitive 尺度转换后的绝对选择指标；属性权重也明确按同一尺度换算，避免把 UV/normal 的裸数值当成米。上游返回的混合指标用于质量排序，不宣称严格位置距离界。

父误差取 `max(child.error) + step_error`，用向上取整的有限 float 表示；叶为 0。保持既有 selector 的单调误差约束，记录度量版本。非有限结果不接受。

每个输出 meshlet 的球体从其实际引用的源顶点计算。先用 double 建界，再以实际 float 中心重算包含半径并向上舍入；极值溢出时拒绝候选或返回 typed error。初版 coarse 记录不启用 normal-cone 剔除（axis=0），避免未经验证的 cone 语义转换。asset 工厂再包住 coarse 和全部 descendants。

## 9. 与现有 asset 表达的打包

构建时使用内部逻辑节点；最终按 forest 的广度层次收集，每层保留稳定的根/孩子顺序，再从深层到根层输出。这样每个孩子都先于父节点，同一父的直接孩子连续，兼容 `group_record.children` 的区间表达，且允许分支深度不同。

coarse records 各组独占，重新分配连续区间；其 first_index 始终指向同一 payload。叶的窗口精确覆盖 LOD0 前缀。最后调用现有 `asset::make`，再次验证所有权、覆盖、单调误差和最大 cut；失败不得发布部分 built_geometry。

## 10. 验收与文件边界

文件按责任拆分：`geometry/subtilis_builder.cppm` 提供项目 API；私有实现文件处理输入/边界、分组简化和最终打包；私有 meshoptimizer 适配只处理依赖调用与分配。沿用现有 geometry 目录，不让 RHI 进入 builder。实现计划再列出具体翻译单元和逐任务测试。

必须证明：

- 可简化网格真正产生父层，coarse 三角形与记录数减少；小网格或无进展网格返回完整 LOD0，不以“始终只有叶”通过功能验收。
- 原索引顺序和顶点字节保持、所有输出索引有效、每记录 <=255 索引；16/32-bit、first_index/base_vertex、非对齐字节窗口均有检查。
- UV/normal seam、原始开边界、分组边界和独立组件保留；在小树上枚举合法混合 cut，合并边界与 LOD0 一致，排查裂缝和重复覆盖。
- 三层以上与不平衡森林可打包；每组 child 连续，父子互斥，完整 cut 不超原 reservation；达到深度/预算边界不截断几何。
- 相同输入和参数重复构建的拓扑、索引、记录、metadata 一致；并发初次构建不切换 allocator；结果独立于输入寿命。
- 新增 headless 测试同步 CMake/CI 清单，Release、ASan/UBSan 与新增模块的静态检查通过，现有 M1a/asset/cut 不回归。

## 11. 后续上传接线原则

下一子阶段把同一 built_geometry 的顶点、完整索引 payload 与 CPU hierarchy 绑定到同一个资源代际，不能单独替换其中之一。保留现有 resource/pass-chain ownership，不新增 RHI ABI 或图内资源创建。

仅主视图的合法静态 opaque item 参与真正的 LOD cut；MASK、toon exact-depth 重着色、变形、阴影和 BLAS 保持 LOD0。CPU cut 发布完整可见记录后，raster/resolve/indirect 使用同一 run；失败恢复完整 shared LOD0。先做上传/代际测试，再进行相机距离序列、接缝、resize/TAA 的视觉与性能验收。

这些原则沿用既有设计；本子阶段不提前冻结 runtime attach API、缓存文件格式或公开更多配置开关。
