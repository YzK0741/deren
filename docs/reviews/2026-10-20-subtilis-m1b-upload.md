# Subtilis M1b 上传代际验证

承接 [CPU builder](2026-10-20-subtilis-m1b-builder.md)，本轮实现 [上传设计](../superpowers/specs/2026-10-20-subtilis-m1b-upload-design.md)：构建结果、GPU 顶点和完整索引 payload 统一为只读共享代际，通过已有 pass-chain 创建和持有资源。没有新增 RHI ABI、渲染图资源创建或自动导入时简化。

## 接线

`subtilis::upload_geometry` 接收 core 和 `shared_ptr<built_geometry const>`；两个 scene buffer 都创建成功且通过大小检查后，才返回 `uploaded_geometry`。资源依赖和初始化身份保留同一 source；没有单独替换 hierarchy 的入口。生产对象使用 utility::memory allocator。

`primitive_create_info::subtilis_geometry` 非空时，runtime 从构建结果取得全部几何字节、stride、顶点数和索引元数据。描述中的旧几何字段不参与这条上传，材质及 model 继续来自原描述。为空时保留既有路径。`normal_draw_primitive::bind_subtilis_geometry` 只接受空叶；两个 geometry_buffer 都租用整个代际，外部 BLAS 风格的租约可以让资源在叶销毁后继续存活。创建依然要求 runtime idle，不提供帧中替换。

GPU 索引 buffer 包含 coarse 尾部，index_type 为 uint32；draw_index_count 和共享 meshlet 表仍只覆盖 LOD0 前缀，因此普通绘制、阴影和 BLAS 没有改为 coarse。运行时 selector 尚未消费这份 hierarchy。

## 本地证据

| 验证 | 结果 |
|---|---|
| 完整 Release 构建，12 线程 | 通过 |
| 完整 Release CTest | 55/55 |
| ASan/UBSan：Subtilis、RHI ownership、resource_cluster | 7/7，halt_on_error |
| 上传/绑定测试 | 44 项检查 |
| 新增两个翻译单元 clang-tidy | 无项目诊断，warnings-as-errors |

测试直接经过 portable probe 的 runtime::make_pipeline/make_primitive，验证来源字段优先、完整索引大小与 LOD0 绘制数的区分、包围范围、meshlet 数和分配失败不改变旧 primitive。普通 uint16 创建路径也通过。首次/第二次分配失败、空 source、源数据寿命、外部租约、销毁和禁止已有叶重新绑定均有覆盖。模块/API 缺失和已有叶重新绑定各观察到 RED，完成后 GREEN。

probe 单独记录 initial_bytes，逐字节比较传入 core 的顶点和完整索引（包括 coarse 尾部）；没有改变 device-only buffer 的 mapped() 语义。这证明生产上传描述的内容一致，不等同于真实 GPU DMA、绘制图像或性能验收。

独立审查覆盖 `813de5a7..34623340`，发现一项 P2：空 aggregate pass-key 可用 `{}` 绕过工厂，构造空或混合代际。新增 forgeable_generation concept 的 static_assert 已观察到 RED；改为 explicit 默认构造 key 后 GREEN。修复后完整 Release 55/55、sanitizer 7/7 和两个新增翻译单元静态检查再次通过。没有 Critical 或待处理 Minor。

## 裁定和下一步

沿用用户授权的当前 checkout 和内联实施，在已批准的代际绑定原则下补充具体设计/计划，不再重复询问执行许可；仅作本地提交。代价是详细接口本轮由实现与测试收敛，仍可在正式启用前调整。

审查裁定：自动导入简化与运行时选层继续后置（代价：当前绘制保持 LOD0）；硬件图像/性能验收后置（代价：probe 不能排除实际 GPU 问题）；source 遵守 immutable-owner 合同，禁止从其它可变别名修改或 move-assign（违反时 CPU/GPU 数据可能分歧，API 已明确前提）；保留 primitive 既有公开字段与旧路径，不做整体封装重构（代价：手动改字段可破坏关联，后续 selector admission 必须检查 buffer/代际元数据）。

下一步是主视图静态 opaque item 的 CPU cut 与帧槽发布。需要验证 primitive 的 buffer/代际身份一致，明确 MASK、toon exact-depth、变形、阴影和 BLAS 的 LOD0 策略，再做实际资产距离序列、接缝、resize/TAA 和性能验收。当前没有启用真实层级选层，不能宣称已降低运行时绘制量。
