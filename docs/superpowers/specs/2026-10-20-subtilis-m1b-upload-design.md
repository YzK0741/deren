# subtilis M1b：上传代际绑定

状态：显式上传与 runtime 创建已实现，完成本地验证和独立审查修复；证据见 [验证记录](../../reviews/2026-10-20-subtilis-m1b-upload.md)。实际选层及硬件验收仍待后续。

承接已批准 builder 设计 §11 及用户的“继续”。本轮实现已有几何上传通路的下一步，不启用每帧层级选择或导入时自动简化。

`subtilis::uploaded_geometry` 是只读共享对象，统一持有 `shared_ptr<built_geometry const>`、顶点/index GPU buffer 和现有 pass-chain。索引上传完整 uint32 payload，普通绘制、阴影和 BLAS 的 index_count 始终只覆盖 LOD0 前缀。CPU hierarchy 不单独替换，记录 first_index 与该代际的完整 payload 一致。

`upload_geometry(core, source, flags)` 通过现有 scene resource requests 同步创建两个 buffer。上传字节借用 immutable source，initialization_key 和资源依赖保留其身份；返回前检查完整快照。任一 buffer 失败不发布半个代际，不修改已有 primitive。资源创建不进入渲染图，不改变 RHI ABI。

primitive 增加显式的只读代际绑定：两个 geometry_buffer 视图持有同一代际租约，同时写入 uint32、vertex_count/stride、LOD0 draw_index_count。`primitive_create_info::subtilis_geometry` 可选提供 CPU 构建结果；存在时它是全部几何字段的唯一来源，原始 geometry 字段不参与上传，material/model 仍来自 info。null 保持既有行为。销毁先清视图，再清代际/资源 owner；外部租约或 BLAS 可延长整组几何寿命。创建入口仍要求 runtime idle，与已有接口一致，不增加运行中替换 API。

实现位于 geometry/subtilis_upload.cppm 和 .cpp；接口仅依赖 builder/RHI/资源描述，pass-chain/provider 只在实现文件中使用，避免 primitive 与 scene_tables 的模块环。

测试验证完整上传字节（包括 coarse 尾部）、来源寿命、同代际视图、旧租约在另一个代际创建后的有效性、任一分配失败、空 source、primitive destroy 与最终 lease 释放、已有 builder/asset/cut 不回归。完整 Release、相关 sanitizer、静态检查及一次独立审查作为完成门槛。此轮不能宣称已经减少运行时三角形。
