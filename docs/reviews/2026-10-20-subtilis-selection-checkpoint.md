# Subtilis selection checkpoint — 2026-10-20

暂停原因：5 小时额度 usedPercent=95，即剩余5%；按用户要求停止继续实施。基线 ebc87303，上传/绑定阶段已完成。没有在跑的构建、测试或本轮审查代理。

本轮仅新增：
- docs/superpowers/specs/2026-10-20-subtilis-m1b-selection-design.md
- docs/superpowers/plans/2026-10-20-subtilis-m1b-selection.md

设计与计划处于待实施/续接前核对状态；没有新增 C++ 或改变渲染行为。文档测试1/1通过，diff --check通过。此前已验证的代码证据仍为 Release55/55、相关 sanitizer7/7、上传44项检查。

已核对的重要事实：
- record_visibility_items 当前只调用 prepare_lod0；已有 prepare_hierarchy/帧槽写入/apply_meshlet_run 可复用。
- current_ubo 持有本帧 view 和 jittered proj；render_extent() 是渲染尺寸；不能猜 near=.1。
- frame_character 包含所有非背景 opaque 叶，而非仅 toon_family；character_forward 活跃时必须保守保持这些叶的 LOD0。
- primitive 当前字段可被手动改写；关联检查必须比较 buffer、lease、index width/count、stride 和完整源窗口，不可只检查 uploaded_geometry 指针。

续接顺序：
1. 先核对实际 visibility shader 的固定顶点布局和 builder 的 normal_offset/uv_offset 元数据可见性。gltf_loader.cpp 的 vertex 注释为64字节；此前 CPU/probe 测试用32字节顶点，不能直接当成真实 visibility shader 兼容性证据。是否需要在 built_geometry 保留属性布局元数据，尚未定案，不要未经核对便启用 coarse。
2. 同步补全 selection design/plan 中布局兼容性约束，再执行 Task1 纯适配器的失败优先测试。
3. Task2 接实际 runtime 和帧槽一致性；完成全套/相关 sanitizer/tidy/一次独立审查。
4. 不自动推送，不自动使用额度重置。真实图像/性能验收仍未完成。

保留 .vscode/、两个历史 CI checkpoint；均未更改。
