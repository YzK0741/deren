# subtilis M1b 主视图选层验证

承接上传代际，按 [设计](../superpowers/specs/2026-10-20-subtilis-m1b-selection-design.md) 和 [实施计划](../superpowers/plans/2026-10-20-subtilis-m1b-selection.md) 完成显式来源的 CPU 层级选层接线。实现提交：`34d0f1b0`、`c5d1dae3`。

## 行为

`subtilis::prepare_item` 核对上传代际与叶的两个 buffer、租约控制块、索引类型/窗口、顶点布局和 LOD0 记录。关联失效返回 shared LOD0；无代际沿用原 prepare_lod0。布局元数据保留 normal/UV 偏移；visibility admission 拒绝非64字节 stride，避免固定布局 shader 非法读取。

静态普通 opaque item 使用当前 model、冻结 view/jittered projection 和 render_extent 选择完整 cut。near 从受支持的 RH/ZO 投影解出并保守舍入；非法或不识别视图使用完整最细层。MASK、BLEND、非标准属性偏移及 exact-depth 等策略强制最细层。character_forward 活跃时，普通非 toon 的非背景 opaque 同样保持最细层。

runtime 只准备一次 run：完整写入当前帧槽的可见表后，使用同一 run 设置 resolve row、raster push 和 indirect count。原 reservation、图资源读声明和 host writer 排他规则沿用；无 RHI/shader ABI 变化。普通 geometry、shadow、BLAS 的 draw_index_count 仍为 LOD0。日志仅首次汇总记录数、三角形数和回退项数。

## 证据

| 检查 | 结果 |
|---|---|
| 完整 Release 构建，12线程 | 通过 |
| 完整 Release CTest | 57/57 |
| ASan/UBSan：subtilis 七项、visibility、RHI ownership、resource_cluster | 10/10 |
| 新适配层 clang-tidy，warnings-as-errors | 无项目诊断 |
| 适配层测试 | 51项检查 |
| 实际 runtime/渲染图帧测试 | 102项检查 |

实际帧中的可简化网格从7条记录/512个三角形降为4条记录/256个三角形，未触发回退；MASK 场景仍为7条/512个。这是 portable probe 的数据通路证据，不代表 GPU 耗时收益。

失败优先证据：缺失模块/API、stride admission、实际帧保持7个 LOD0 记录而未选择粗层、斜近裁面被误识别均先观察到 RED，再 GREEN。实际帧测试检查新增 coarse 索引被消费、row/push/indirect 一致、MASK 最细层、普通 PBR 的等深度重绘、环境物体粗层、关闭/无来源旧路径、关联失效、区间冲突回退和两个帧槽数据独立。纯适配器补充租约伪造、变形/实例/overlay/custom pipeline、无效视图、完全剔除及发布拒绝。

## 独立审查

按 executing-plans 要求进行一次独立、只读的全改动审查，范围 `f961ec15..c5d1dae3`。审查覆盖 selector、primitive、shader、character frame 和 graph 周边契约，未发现 Critical、Important 或待处理 Minor；判断可合入本次 CPU 选层/运行时接线里程碑。审查没有重新执行测试，验证数字来自作者实际运行。

审查确认 force_finest 仍可保守视锥剔除，保留全部可见 LOD0 几何，不将其判为不完整 cut。后续两项裁定：硬件图像/性能证据继续后置，代价是尚不能排除 GPU 图像问题或宣称收益；既有共享 meshlet 表损坏修复不纳入本轮，代价是非法手改共享表仍按既有契约处理，不保证能自动修复。没有第二轮审查或额外修复。

## 裁定与限制

- 沿用已授权的当前 checkout、内联实施和本地提交，不推送。接口细节由实现与验证收敛；代价是正式启用前仍可调整。
- 固定 shader 布局不兼容时整帧拒绝 visibility lane；代价是该帧使用原 geometry lane。
- 写入拒绝在纯适配器覆盖，实际帧使用区间冲突验证回退；未新增生产故障注入接口。代价是运行时写者独有故障的测试覆盖有限。
- 环境粗层与普通 PBR 等深度重绘分开测试；组合帧中后续 character push 与 visibility push 同为112字节，会覆盖探针的最后记录。代价是组合硬件验收仍待完成。

本轮没有自动构建导入资产，也未验证实际 GPU 图像、相机距离序列、接缝、resize/TAA 或帧时收益。下一步应将真实资产显式构建/上传到该入口，并进行这些验收，不能据 probe 结果宣称严格1px几何误差或性能提升。
