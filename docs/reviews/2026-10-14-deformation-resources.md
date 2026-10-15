# 共享变形缓冲迁移 — 2026-10-14

motion transforms、skin/previous-skin matrices及morph data已交给passchain管理；保留之前尚未提交的相机迁移。

## 实现与约束

- 新scene_deformation_pass声明四类frame-slot storage_coherent buffers，保留device_address usage、容量与heap槽位。创建时清零，逐帧不清零。
- previous-skin新增独立资源id并发布到frame resource table；id追加在枚举末尾，已有id和RHI ABI32不变。
- 构造阶段准备资源，保证animation setup与instanced motion写入之前有完整mapping。runtime只保留borrowed handles/mappings与每类snapshot；应用/兼容链分别提供缺失类型。
- 对scope、lifetime、容量、coherent mapping及reset策略做admission，拒绝错误描述后再acquire之前保留原heap与mapping。
- generation变化时先沿用idle drain，再逐槽复制旧映射内容，旧snapshot始终保留至复制完成。这保留setup morph deltas、局部skin写入、instanced motion和previous-skin。CPU chronological history继续保留。
- 正常复用不复制、不清零。morph在provider handover时复制最多48MiB/slot，是一次性迁移成本。raw morph scratch只用于即时写入，reconciliation后需重新获取；现有animation controller已逐操作获取。
- 继续保留原set_skin_matrices、motion/deformation history与morph写入算法及完成槽顺序。

## 发现并修复的原有同步问题

补充animated_skin_plane的G-buffer debug capture后，冻结基线和迁移初版均报告HDR WRITE_AFTER_WRITE。
debug模式使TAA inactive，G-buffer的scene-color输出已写HDR；debug分支错误地从undefined转换，丢掉了先前attachment store的source access。
修复使用color_attachment→color_attachment，排序此前写入与debug CLEAR；40帧原场景由验证失败变为干净。
独立只读审查确认该修复与迁移生命周期无具体缺陷。这是具体barrier错误修复，没有引入自动barrier系统。

## 验证进度

- 新迁移回归RED11/56，随后扩展为GREEN99/99。覆盖setup/尾部数据、四类heap身份、重复producer合并、槽隔离、reuse、handover/retirement、非法容量/清零/scope及创建失败恢复。
- 原skin_history35/35通过。旧managed_frames使用真实motion描述后46/46通过，仍保留增长、失败恢复与退役覆盖；schema更新为40类资源、605/605通过。
- Release构建成功，最终CTest24/24，21.30秒；真实Vulkan runtime1164/1164。
- 最终15场景各40帧同步验证干净，包含动画debug与animation+RT skin bake/TAA；后者日志确认1个skinned caster每帧重新蒙皮并refit，0个失败。
- 原13静态场景与冻结包像素一致。两个动画场景初次使用wall-clock时有差异，不能用作像素gate；按固定0.025秒/帧重做后，debug和RT skin bake/TAA两条路径均逐像素一致。debug旧基线仍有原有同步错误，新版验证干净。
- ML+RT+0.67比例+TAA最大化恢复两次resize验证干净，1280×720与client尺寸一致。

## 证据

归档：[2026-10-14-deformation-resources](2026-10-14-deformation-resources/)，工作/冻结包在build-release-dyn-clang64/deformation-resources-check/。
PNG仅无损重压缩IDAT，解压扫描行字节相同；归档配置、原始日志、像素统计、脚本、CTest和SHA256。
baseline-deformation及before-barrier-fix-deformation保留故障证据，不能计作同步干净的基线。

尚未提交。本轮无未完成的运行路径；剩余所有权工作主要是Gaussian缓冲、共享场景表/材质与IBL/furnace纹理、AS及其构建数据和samplers。
