# UE 5.8.2 源码参考与 deren 方案取舍

本次仅阅读 `D:/UnrealEngine-5.8.2-release` 并修订 deren 规划文档，没有修改 UE 或 deren 生产代码，没有编译 UE，也没有据此声称 deren 新路径已验证。`Engine/Build/Build.version` 的 Major/Minor/Patch 为 5/8/2，Changelist 为 0；下列定位对应本地文件，不假设与其他公开分支内容相同。

主计划：[渲染扩展与轻量层级几何](2026-10-14-render-extensibility-cluster-geometry.md)。这里区分“源码实际机制”与“deren 的设计选择”。只参考模块职责与机制，不复制 UE 的源文件、编码布局或完整运行时。

## 核对过的源码入口

| 主题 | UE 本地源码与定位 | 实际观察 | deren 的选择 |
| --- | --- | --- | --- |
| 渲染身份 | [PrimitiveComponentId.h](D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/Engine/Public/PrimitiveComponentId.h:7) | 渲染侧用 component ID 标识，避免直接传游戏线程对象指针。此类型是整数 ID，不是 index/generation 句柄。 | 采用 deren 自己的 scene 槽位句柄；不能声称 UE 此处已经提供 generation 防复活机制。 |
| 身份与存储位置 | [PrimitiveSceneInfo.h](D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/Renderer/Public/PrimitiveSceneInfo.h:274)、[GPUScene.h](D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/Renderer/Private/GPUScene.h:244) | primitive 有 persistent identity，另有 packed index；GPUScene 提供 instance slot 分配与更新入口。 | CPU/node/instance 身份与 GPU dense slot 分离；history 和 animation 不跟随 packed slot。 |
| 分类更新 | [ScenePrimitiveUpdates.h](D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/Renderer/Private/ScenePrimitiveUpdates.h:14)、[GPUScene.h](D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/Renderer/Private/GPUScene.h:273) | dirty flags 区分 transform、bounds、GPU state、instance data；有 previous transform override。 | 合并 deren 单写者的分类脏记录；无需立即引入 UE 的线程命令队列。提交历史协议是 deren 拟定规则。 |
| RDG 访问枚举 | [RenderGraphBuilder.cpp](D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/RenderCore/Private/RenderGraphBuilder.cpp:119) | SRV/UAV、attachment 等归到 parent texture，并枚举子资源范围。 | 用已有 registry lease 解析对象身份；bindless/address 显式 use，不能只看 descriptor。 |
| 屏障编译 | [RenderGraphBuilder.cpp](D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/RenderCore/Private/RenderGraphBuilder.cpp:3783) | 编译逐子资源状态，兼容状态合并与是否真正需要 transition 是不同步骤。 | 先纯数据范围/依赖编译，后 RHI lowering；GENERAL 布局不免除写同步；首轮固定顺序、单队列。 |
| 外部状态 | [RenderGraphBuilder.h](D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/RenderCore/Public/RenderGraphBuilder.h:79)、[同文件 extraction](D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/RenderCore/Public/RenderGraphBuilder.h:345) | external registration、extraction 与 final access 是显式接口。 | manual/compiled 子链通过 imported/final state 交接，history 与 WSI 不藏在隐式全局状态里。 |
| 叶 cluster 划分 | [ClusterDAG.cpp](D:/UnrealEngine-5.8.2-release/Engine/Source/Developer/NaniteBuilder/Private/ClusterDAG.cpp:135) | triangle adjacency 和空间 locality 参与 partition。 | 固定索引窗口继续服务阶段三；层级资产阶段才做紧凑拓扑划分，不把窗口当最终 cluster。 |
| group 构建 | [ClusterDAG.cpp](D:/UnrealEngine-5.8.2-release/Engine/Source/Developer/NaniteBuilder/Private/ClusterDAG.cpp:1378)、[父级 LOD 数据](D:/UnrealEngine-5.8.2-release/Engine/Source/Developer/NaniteBuilder/Private/ClusterDAG.cpp:1587) | 合并孩子进行 reduction/repartition；父 clusters 共享 group 的 LOD bounds/error，带 generating group 身份。 | 粗表示可含多个 cluster；group 整体选择。首版固定边界的唯一父树较易验证；完整 DAG 留后续。 |
| 简化边界 | [Cluster.cpp](D:/UnrealEngine-5.8.2-release/Engine/Source/Developer/NaniteBuilder/Private/Cluster.cpp:1021) | 简化器接收 material/attributes；ExternalEdges 的两端位置锁定，保留 LockedEdges 信息。 | 组内合并简化，外边界和属性接缝需保留；混合 LOD 不可只验父子覆盖，须验边连接/T-junction。 |
| 历史遮挡恢复 | [NaniteCullRaster.cpp](D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/Renderer/Private/Nanite/NaniteCullRaster.cpp:7003)、[恢复阶段](D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/Renderer/Private/Nanite/NaniteCullRaster.cpp:7021)、[NaniteClusterCulling.usf](D:/UnrealEngine-5.8.2-release/Engine/Shaders/Private/Nanite/NaniteClusterCulling.usf:501) | main/post 两阶段；历史拒绝项保留，主阶段光栅后构建 HZB，再复测和光栅。 | 先 current-frame depth prepass，后可选历史两阶段恢复；历史拒绝不能直接等同本帧不可见。 |
| 安全间接参数 | [NaniteClusterCulling.usf](D:/UnrealEngine-5.8.2-release/Engine/Shaders/Private/Nanite/NaniteClusterCulling.usf:989) | CalculateSafeRasterizerArgs 在 SW/HW visible 范围冲突时裁剪数量。可防越界，不能据此保证完整几何覆盖。 | 不照抄裁剪策略。finalize 前溢出用全量备用 runs；首次光栅后恢复阶段溢出需要重新 clear 并回退整视图。 |
| 流送依赖与发布 | [NaniteStreamingManager.cpp](D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/Engine/Private/Rendering/NaniteStreamingManager.cpp:950)、[依赖检查](D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/Engine/Private/Rendering/NaniteStreamingManager.cpp:1031)、[安装](D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/Engine/Private/Rendering/NaniteStreamingManager.cpp:1447) | registered 页依赖引用计数；检查依赖 residency；install/uninstall 有 fixup，不能将磁盘到货视为 representation 就绪。 | 第一版独立页解码、完整 group 发布、常驻粗覆盖；使用 deren 帧版本页表/lease 回收，不照搬 fixup 格式。 |
| 异步反馈 | [NaniteStreamingManager.cpp](D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/Engine/Private/Rendering/NaniteStreamingManager.cpp:2315) | 队列 readback 与读取最新请求分开，流送还有 root allocation、pending 和每帧 installs 限制。 | GPU 渲染不等待磁盘；预算同时计算 root、resident、retiring、staging 与 upload；过期反馈版本校验。 |

## deren 当前约束的核对入口

- [scene_tree.cppm](../../../source/engine/scene_tree/scene_tree.cppm)：roots/children 为内嵌 vector，迭代器持有节点地址；primitive leaf 唯一所有。稳定句柄必须迁移调用点，不能仅替换容器名称。
- [controller.cppm](../../../source/engine/animation/controller.cppm:356) 和 [controller.cpp](../../../source/engine/animation/controller.cpp:218)：动画存节点指针并直接写 local，skin/cache 也涉及节点身份。此处是阶段一必须覆盖的使用者。
- [primitive.cppm](../../../source/engine/primitive/primitive.cppm:53) 和 [instances.cppm](../../../source/engine/render_resource/instances.cppm:88)：已有 geometry lease 与 generation/resource snapshot，应复用，避免另一套所有权体系。
- [几何所有权记录](../../reviews/2026-10-14-geometry-resources.md)：GPU geometry/BLAS lease 已迁移，但 CPU caster 仍借用 scene tree，不能把此前完成的 GPU 所有权迁移误认为动态删除已安全。
- [共享资源记录](../../reviews/2026-10-14-shared-assets.md)：heap 替换依赖 frame boundary、等待、发布和回滚；不可变 CPU 快照不自动冻结同一 GPU heap 地址的内容。
- [meshlet.cppm](../../../source/engine/meshlet/meshlet.cppm:44)：当前 85 triangles、48-byte records、65536 静态表预算；这些是当前路径限制，不是未来通用资产规格。
- [rhi.api_core.cppm](../../../source/promise/rhi/rhi.api_core.cppm:347)：use-pair buffer barriers、stage hint 和 barrier_group；[间接 mesh 命令](../../../source/promise/rhi/rhi.api_core.cppm:1688) 是固定 count 形式。不能假设任意 stage/range 或 indirect-count 已存在。
- [pass_chain](../../../source/engine/pass/chain.cppm:100) 保持资源 reconcile、lease 和 pass 顺序，尚非通用图调度器。新编译层不应承担第二次资源创建。
- [post.cppm](../../../source/engine/pass/post.cppm:7)：四级 bloom 与 composite 在同一模块；exposure 为 push 参数，UI 可嵌入 composite 的 rendering instance。第一段自动同步选 bloom 四级，并在 composite 前交回人工边界。

## 三种推进方式及选择

1. **保留 deren 帧链，逐步增加协议与 GPU 几何 provider（采用）。** 与现有 lease、GBuffer/PBR 和 RHI 边界一致；每阶段有局部回退，测试能分辨存储、同步、绘制和简化问题。
2. **先整体改成 UE 风格 RDG/GPUScene。** 能取得更大的统一调度空间，但会同时改变所有权、顺序、屏障和场景接口，使已有图像回归难以定位；当前四阶段目标不需要这项成本。
3. **直接接入完整 Nanite 式 DAG、visibility buffer 和流送。** 能为更多渲染能力铺路，但依赖新的资产编码、材质路径、队列容量和页修补机制；与“静态不透明、硬件光栅、沿用 GBuffer”首版目标不匹配。

当前采用方式 1。后续实测发现固定边界 group 的简化率不足时，再独立设计 DAG/group 边界协调；不以“参考 Nanite”为理由扩大首版范围。

## 本次文档检查

已核对上述本地文件和关键实现片段，主计划保留四阶段及所有原验收项，新增项明确为拟定协议；文档相对链接与源码定位另由本次静态检查确认。未运行 deren 构建、CTest 或 Vulkan 验证，因为此次未改生产代码。原有迁移报告的通过记录不计为新几何方案验证证据。
