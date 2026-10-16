# Render Graph A1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking. Execute inline; use an independent final code review. Do not commit or push without user authorization.

**Goal:** 实现可独立测试的纯 CPU 图核心，从规范访问生成内容依赖、裁剪结果及稳定执行计划。

**Architecture:** 图外资源以值类型身份登记；builder 拥有名称、pass 访问和输出根。Boost.Graph 管理通用依赖存储、反向可达性和过滤视图；deren 实现内容版本及稳定 Kahn 策略。编译结果拥有诊断与计划数据，不保存 builder 借用指针，不涉及 GPU。

**Tech Stack:** C++23 modules、Boost.Graph 1.92.0、utility/vessel、expected、CTest、Clang64、ASan/UBSan。

**Spec:** [Render Graph 设计](../specs/2026-10-15-render-graph-design.md)，本计划仅覆盖 A1。

## Global Constraints

- 资源创建位于图外；不增加 graph.create、资源池、lease 适配或 GPU 执行。
- 纯编译模块不 import runtime、RHI 或 Vulkan，不暴露 Boost 类型。
- A1 只支持完整资源范围；mip/layer/aspect、字节区间、visitor 和屏障属于 A2。
- 全部项目存储使用 vessel/utility allocator，CPU OOM 仍走 fast_panic。
- 环和非法输入返回 expected，不调用 BGL throwing topological_sort。
- 资源簇与旧 registry/passchain/runner 保持原样，不迁移生产 pass。
- 保留所有已有工作区修改；构建用 -j12，内存不足才降并发。

## Review Focus

1. 同一物理对象重复登记的 alias 必须合并，不能绕过值依赖或 hazard（Task 1/2）。
2. 无用 discard 写不能因 WAW 被误保留；仍活跃的读取不能被后续覆盖抢先执行（Task 2/3）。
3. dead pass 的未初始化读可被裁剪，但 dead pass 的错图 ref 仍必须报错（Task 1/2）。
4. builder 销毁、名称源修改和编译失败后，结果/诊断必须独立有效（Task 1/4）。
5. 多根、平行边、深链及环不能导致重复执行、入度失配或递归栈溢出（Task 3/4）。

## File Structure

- `source/engine/render_graph/types.cppm`：导出的纯值类型、句柄、错误与计划。
- `source/engine/render_graph/render_graph.cppm`：主模块、builder 声明及拥有的输入记录。
- `source/engine/render_graph/render_graph.cpp`：构图检查和编译过程。
- `source/engine/render_graph/graph_backend.hpp`：仅实现单元使用的 BGL selector、图属性分配、遍历工作区与稳定排序。
- `source/tests/test_render_graph.cpp`：通过公共模块测试图语义。
- 修改 `test_boost_graph.cpp` 复用上述私有 backend，避免长期维护两份分配器/调度适配；保留其 new 观测与 OOM 子进程测试。
- `CMakeLists.txt`：新增独立 `render_graph_core` 静态目标，只链接 shared_utility/vstd；Boost 私有链接，避免为了 CPU 测试构建整个 engine。
- 将 render_graph_core 加入现有第一方编译选项和 sanitizer 配置范围，不因新目标遗漏无异常、无 RTTI、警告或调试参数。
- `.github/workflows/ci.yml`：加入 test_render_graph 的 sanitizer 构建列表。
- `docs/render_graph.md` / `Doxyfile`：记录实际接口与 A1 限制；英文 ASCII，遵守现有 docs 测试。

## Frozen A1 Interface

命名空间 `deren::engine::render_graph`，主模块 `deren.engine.render_graph`，类型分区 `deren.engine.render_graph:types`（同一命名模块，见执行 ledger 的接口裁定）。

- `texture_ref/buffer_ref/pass_ref`：默认无效，内部 incarnation + uint32 index；构造仅由 builder 完成，支持相等比较。
- `resource_identity { uint64_t device; uint64_t object; }`：非零的逻辑设备域及实际对象标识。A1 不解引用或拥有 GPU 对象；后续绑定层提供生命周期证明。
- `initial_contents { undefined, defined }`。
- `content_access { read, discard_write, preserve_write, read_write, invalidate }`。invalidate 表示无输出内容的写访问，不可作为有效 export；附件组合在 A2 映射到规范操作。
- `resource_access`：由 texture_ref 或 buffer_ref 加 content_access 构造的规范输入；A1 没有 shader 用途或 stage。
- `pass_options { bool side_effect = false; }`。
- `builder` 不可复制/移动，无 reset，内部取得不复用的 incarnation。
- `import_texture(resource_identity, initial_contents, string_view name) -> expected<texture_ref, graph_error>`；buffer 对应同签名。
- `add_pass(string_view name, span<resource_access const>, pass_options = {}) -> expected<pass_ref, graph_error>`。
- `export_resource(texture_ref/buffer_ref) -> expected<void, graph_error>`：以编译时最后内容版本作为输出根；不代表任何 RHI 最终状态。
- `order_after(pass_ref pass, pass_ref predecessor) -> expected<void, graph_error>`。
- `compile() -> expected<compiled_graph, graph_error>`：首次调用冻结 builder，包括失败结果；后续 mutation/compile 返回 frozen_builder。

上述 add_pass 是 A1 的低层规范输入；A2 参数 visitor 生成它，不要求最终调用者维护两份访问列表。ref 构造方法、边和 BGL 图不成为公共 API。

`graph_error` 拥有 error_code、pass/资源名称及相关索引（无对应项用无效索引），不借用输入字符串。错误码覆盖 invalid_identity、foreign_ref、invalid_ref、conflicting_import、conflicting_access、undefined_read、undefined_export、cycle、frozen_builder、capacity_exceeded。

`compiled_graph` 拥有登记顺序的 pass/资源记录、live 标记、执行顺序、内容生产者关系及原因化的依赖边。边原因区分 value、RAW、WAR、WAW、explicit_order；相同端点可保留多个原因，排程不能重复计数失配。所有索引属于结果本身，结果能在 builder 析构后读取；后续 GPU 执行需要另外提供绑定表。

## Task 1: 资源身份、构图和所有权

**Interfaces:** 产生 Frozen A1 Interface 的类型、登记操作和空图编译；后续任务消费 builder 拥有的规范记录。

- [x] 写 `test_render_graph`：空图得到空计划；两个 builder 的 ref 不可互用；默认 ref 拒绝；名称源修改后名称保持；compile 后拒绝修改/再次编译。
- [x] 写身份断言：同 device/object/kind 重复 import 返回相同 ref；内容初态矛盾拒绝；不同对象同名、不同设备同 object 不合并；同 device/object 作为不同资源种类登记拒绝。
- [x] 在 CMake 注册测试并构建，确认因缺少模块/接口失败，而非配置错误。
- [x] 实现 types、builder 登记及上限检查。任何拒绝不得追加部分 pass/import/根/顺序边。长度转 uint32 前检查，incarnation 耗尽不得回绕复用。
- [x] 同一 pass 对 alias 的相同操作合并；不同操作拒绝并要求显式 read_write/preserve_write，避免访问枚举顺序改变语义。
- [x] 构建并运行 focused 测试，保存 RED/GREEN 证据。

## Task 2: 内容版本和值依赖裁剪

**Interfaces:** compile 在登记顺序扫描规范访问，生成值依赖图、live 标记和按资源索引的最后版本；Task 3 仅消费存活访问。

- [x] 写断言：discard A -> discard B -> export，仅 B 活跃；preserve/read_write B 则保留 A。
- [x] 写断言：活跃的 undefined read/preserve/read_write 报错；相同无效内容读取在 dead pass 中不报内容错误，但错图/default ref 即使 dead 也拒绝。
- [x] 写断言：import defined 的只读输出可没有生产者；无根全部裁剪；side_effect 保留其读取所需生产链；export undefined/invalidate 后内容拒绝。
- [x] 写 alias 断言：通过另一个 import 读取仍连接同一生产者；多个根的公共前驱只执行一次。
- [x] 运行新增断言确认失败，再实现值依赖与内容有效性。discard 不消费旧值；preserve/read_write 消费旧版本；invalidate 产生无效版本。记录潜在内容错误，在根裁剪后仅报告活跃路径上的错误。
- [x] 使用 BGL reverse_graph + 显式 vessel BFS 工作区求可达性，不用物理 WAW 边参与值裁剪。
- [x] 运行 focused 测试确认通过。

## Task 3: Hazard、显式顺序与稳定计划

**Interfaces:** 消费 Task 2 的 live mask，产出完整 compiled_graph 的存活边和执行顺序；不生成屏障或声称 GPU 已同步。

- [x] 写 RAW/WAR/WAW 原因断言：保留活跃读者至后续覆盖前；dead 写不产生存活依赖；read/read 不增加物理依赖；不同物理对象互不串行。
- [x] 写显式顺序断言：两个活跃节点可按约束重排；order_after 不单独保留 dead 前驱；显式顺序图自身成环即使无输出也拒绝；存活值/hazard 加显式顺序形成环同样拒绝。
- [x] 写稳定性断言：每一步从所有 ready pass 选择最小登记索引；相同内容反复构建结果一致；多原因/重复顺序请求不会重复执行或错误留环。
- [x] 确认新增断言失败，再迁移已验证 BGL backend 适配，类型和定义只出现在实现单元。让 test_boost_graph 复用同一私有适配并保留观测测试。
- [x] 对存活节点按原访问顺序生成 RAW/WAR/WAW，再加入两端存活的 explicit_order；使用最小堆 Kahn 生成顺序和环错误。全体显式边另作结构环检查，不能只因节点 dead 就忽略输入本身的显式环。
- [x] 运行 test_render_graph 和 test_boost_graph，确认语义及分配约定均通过。

## Task 4: 结果诊断、回归和文档

**Interfaces:** 完成独立拥有的 compiled_graph/graph_error；文档只描述实际可用的 A1 API。

- [x] 写结果存活断言：builder 析构后仍能读取顺序、名称、资源与边原因；错误诊断在输入字符串销毁后仍完整。
- [x] 写 20,000 pass 深链、空图、self-loop、duplicate root 测试，确保迭代遍历和确定性；验证深链结果个数与首尾生产者。
- [x] 给新核心接入现有 CPU OOM 测试约定；不引入新 panic 或同步诊断框架。
- [x] 完成 docs/render_graph.md、Doxyfile、CI 列表，更新设计中的 A1 实际状态，A2/B/C 保持未完成。
- [x] Release 全量构建 `cmake --build build-release-dyn-clang64 -j12`，完整 CTest；Debug ASan/UBSan 仅构建并运行新图和 Boost focused 测试，核对非空测试匹配。
- [x] 跑 vendor manifest、backend/native boundary，检查新核心没有 RHI/runtime/native 依赖；保存构建/测试/边界证据并做独立代码审查。
- [x] 审查修正后按影响范围补测；只勾实际完成的任务，不提交/推送，不删除旧机制。

## Verification Commands

```powershell
cmake --build build-release-dyn-clang64 --target test_render_graph test_boost_graph -j12
ctest --test-dir build-release-dyn-clang64 -R '^(test_render_graph|test_boost_graph|test_boost_graph_vendor)$' --output-on-failure
cmake --build build-release-dyn-clang64 -j12
ctest --test-dir build-release-dyn-clang64 --output-on-failure -j8
cmake --build build-vessel-asan-clang64 --target test_render_graph test_boost_graph -j12
ctest --test-dir build-vessel-asan-clang64 -R '^(test_render_graph|test_boost_graph|test_boost_graph_vendor)$' --output-on-failure
```

使用现有 Clang64 PATH 和构建缓存。Windows full CTest 的临时目录/子进程需要既有审批环境时沿用该方式，不能将权限失败误判为产品缺陷。A1 不改变生产 GPU 路径，不以 CPU 测试宣称已验证屏障、画质或 GPU 生命周期。
