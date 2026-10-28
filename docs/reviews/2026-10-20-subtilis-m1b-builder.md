# subtilis M1b CPU builder 验证记录

实现范围是 [builder 设计](../superpowers/specs/2026-10-20-subtilis-m1b-builder-design.md) 的纯 CPU 部分。公开入口为 `deren::engine::subtilis::build`；返回 move-only `built_geometry`，同时拥有原始顶点字节、规范化 LOD0 索引前缀、追加的 coarse 索引和只读 validated asset。上传和运行时选层尚未接线，整个 M1b 尚未完成。

meshoptimizer 固定为 v1.3 / `9e1f07b159d3cb777f1c67ed31fc11fd117986f4`，MIT；23 个上游文件逐个校验 SHA256，只编译 builder 需要的五个源文件。依赖头文件私有，首次调用通过 `call_once` 安装 utility::memory 分配回调，进程 OOM 仍走既有 fast_panic。

## 行为

- 保留原顶点和 LOD0 三角形顺序；16/32-bit 索引按 first_index/base_vertex 规范化为 uint32/base0。
- 以完整子组为单位分区、简化、重新聚类。只接受三角形数和记录数都减少的父组；不成功的组完整保留。
- 从原始 LOD0 区域计算有向位置类边界；锁定完整区域边界及 UV/normal seam 的所有副本。输出必须保留原边界和各源组件。退化、非流形等不支持区域保持细节。
- 反向广度层次打包，直接孩子连续且先于父组；发布前交给 asset 工厂重新校验。索引 payload 和 hierarchy 不可独立修改。
- 包围球按实际存储的 float 中心重算半径并向上舍入；误差为累计的简化器混合指标，不是严格 Hausdorff 距离。

## 本地证据

| 检查 | 结果 |
|---|---|
| Release 完整构建，12 线程 | 通过 |
| Release 完整 CTest | 54/54 |
| ASan + UBSan，subtilis 四项测试 | 4/4，halt_on_error |
| builder 测试 | 70 项检查通过 |
| 新增四个 builder 翻译单元 clang-tidy | 无项目诊断，warnings-as-errors |
| 上游文件 SHA256 | 23/23 |

32×32 单元规则网格在默认参数下：2048 → 510 三角形，25 → 6 条根切面记录，父级深度 2。group_size=2 时深度 3，共 44 个 group。这只是功能验收数据，不代表真实场景帧时收益。

测试覆盖源数据寿命、字节非对齐、输入/选项错误、有限性、预算恰好足够及少一字节、重复构建一致、并发首次初始化、独立闭合岛、接缝副本、退化/非流形回退、不等深森林、深度停止、保守球体，以及 select_cut/prepare_hierarchy 发布。混合切面通过逐个完整细化父组遍历有界序列，核对边界、叶覆盖和祖先/后代互斥；未宣称枚举所有组合或完成图像验收。

## 实施裁定与限制

- 沿用用户授权的当前 checkout 和内联实施；仅作本地提交，未推送。
- 辅助 bash 脚本在沙箱内无法创建 MSYS 命名对象，使用 PowerShell 建立等价的计划专用暂存记录；不影响产品代码。
- 上游 float 算法使用由实际引用顶点确定的 primitive 相对坐标，误差换回源单位；源顶点字节不变。指标仍是估计，不能当成严格几何距离界。
- max_output_bytes 核算输出逻辑数据与元数据，不限制临时分配峰值或容器额外容量。

独立审查覆盖 `db1005fa..5d55cfab`，发现一项 P2：有限的极大属性/权重可使上游中间误差变成 NaN，却返回有限的零误差。曲面网格、max_error=0、normal_weight=FLT_MAX 的回归测试已观察到失败；在私有简化调用边界隔离浮点环境，检查 invalid/divide-by-zero/overflow 并恢复调用线程状态后通过。另测默认权重配极大 UV，同样保留完整细节。修复后重新完成 Release 54/54、sanitizer 4/4 与静态检查；没有 Critical 或待处理 Minor。

审查中明确排除的范围：GPU 上传、代际绑定和图像/性能验收仍按设计留待下一阶段（代价：当前还没有运行时几何减量）；未编译执行的 codec/utility 上游文件只作固定源码快照（代价：本轮未审查这些非 builder 路径）。

下一步把同一 built_geometry 的顶点、完整索引和 hierarchy 绑定到同一资源代际，先验证上传/生命周期，再接主视图合法静态 opaque item 的 LOD。MASK、toon exact-depth、变形、阴影和 BLAS 继续使用 LOD0，最后完成相机距离序列、接缝、resize/TAA 与性能验收。
