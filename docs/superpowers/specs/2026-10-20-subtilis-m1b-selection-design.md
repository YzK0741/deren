# subtilis M1b：主视图 CPU cut 接线

承接上传代际完成记录和用户“继续”。本子阶段只让显式上传的构建结果参与现有主视图 visibility 通路；不自动构建导入资产，不改 RHI、shader ABI 或 reservation。目标是让一份完整 cut 同时控制 raster、resolve 和 indirect dispatch。

## 入口与身份

增加纯 CPU 的运行时适配层，将 primitive 当前状态与其 uploaded_geometry 比较：vertex/index 指针及两份 lease 必须对应同一代际，index_type=uint32，draw_index_count/vertex_count/stride 与 source 一致，GPU buffer 大小覆盖完整 payload，meshlet_count 等于 asset.lod0_count，host LOD0 meshlet 窗口完整且与源前缀一致。叶的公开字段可被调用者改写，因此不能只检查代际指针非空。

有 source 但关联不一致时不读取其 hierarchy，返回完整 shared LOD0 和 source_mismatch。没有 source 时继续原有 prepare_lod0。已知损坏的原始共享表不在本轮新增修复范围，仍由既有 visibility admission 控制；关联失败不能悄悄使用别的代际的粗层。

shader 的固定顶点布局已确认是64字节（normal12/UV24）。build_statistics 保留这两个构建偏移，自动纳入既有元数据预算；非标准属性偏移只能最细层。visibility admission 拒绝非64字节 stride，避免即使回退 LOD0 仍发生错误读取。此检查同样覆盖旧 source 为空的路径。

## 资格与视图

仅静态、非实例、默认 pipeline 的主视图 opaque item 可选择粗层。MASK/BLEND、overlay、变形和其它非支持路径保持完整 LOD0。不能仅用 toon_family 判定 exact-depth：当前 frame_character 包含所有非背景 opaque 叶。character_forward feature 活跃时，所有非 environment 叶保守 force_finest；之后若改变实际消费列表，要同步此事实。阴影、BLAS、普通 geometry lane 完全不调用新的 selector。

model/view/projection 来自当前冻结帧的 leaf.push.model 和 current_ubo，尺寸使用 render_extent()，不使用 presentation 尺寸。投影只接受既有 RH / Vulkan ZO 的透视或正交形状；near 从该投影系数解出并保守舍入，不能把固定 .1 当成所有相机的近裁面。无法识别、非有限、零尺寸或非法 near 时强制完整最细层，沿用 selector 的无效视图行为。初期目标误差使用 reference 默认 1px，不新增配置开关。TAA 使用本帧实际 jittered projection 做剔除，保持与 raster 一致；不增加历史或滞回。

## 发布与消费

复用 prepare_hierarchy 和 meshlet_culled_write，以现有帧槽及叶的完整 reservation 为界一次写入完整 selected records。成功后才发布 visible_run；容量、来源、alias overlap 或写入失败回 shared LOD0。写入前不得改变 table row、push 或 indirect 命令。完全剔除的 cut 发布 visible/count0，不写无意义记录，也不占用 published 区间。

record_visibility_items 只选一份 run，继续用 apply_meshlet_run 同时更新 row 和 push，再按同一 count 写 indirect。不改变现有 graph 对可选 culled-table 的读声明，也不允许其它 host-cull 写者在该帧覆写表。记录代际身份失败、forced finest、coarse 记录/三角形等诊断时保持轻量，不逐帧格式化日志。

## 验收

先用上传代际和 portable probe 测试身份破坏、资格、正交/透视/非法视图、相机 near、非均匀缩放和完整 cut 发布；再验证 runtime 的 per-frame table/row/push/indirect 一致，以及写入失败和 alias 的完整回退。覆盖 MASK、character-forward 的普通非 toon 对象、environment、变形、无代际旧路径和两帧槽独立性。

完成 Release 全套、相关 ASan/UBSan、适配层静态检查和一次独立审查后，才进入真实资产相机距离序列、接缝、resize/TAA 与性能验收。probe 不能代替硬件图像验收，不宣称严格 1px 几何误差或帧时收益。
