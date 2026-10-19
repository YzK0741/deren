# 文档 review 轮次：ABI 43–45 / GUI-owned recording（3 位 reviewer）

日期：2026-10-18 · 批次：docs-review-round · 基线：`11e1c418`（RHI ABI 45 / GUI ABI 5）

## 方法

把文档 review 拆给 3 位独立 reviewer，各自独占写范围、互不重叠，Lead 只做收尾合并与验证：

| reviewer | 写范围 | 任务 |
| --- | --- | --- |
| reviewer-reviews | `docs/reviews/` | 核对近批 review 记录的 ABI 号、类型名、测试计数、门禁声明；修复 follow-up 链 |
| reviewer-contract-docs | `source/promise/rhi/`, `source/promise/gui/`, `source/backends/vulkan/` | 契约 / GUI / 后端注释与 Doxygen 的当前事实（ABI 43/44/45、容器、GUI 录制目标） |
| reviewer-manuals | `docs/dynamic_runtime.md`, `mainpage.md`, `resource_clusters.md`, `managed_resources.md`, `runtime_split.md` | 面向用户的文档：所有权、escape、GUI 录制的过期陈述 |

约束：只改文档/注释，不改语义/签名/测试；不 build、不 commit（由 Lead 统一验证提交）。

## 主要修正

**review 记录**（15 个文件）
- 补全 follow-up 链：`unique-ptr-factories` → `shared-object-migration`（容器被 ABI 44 取代）；`escape-final-cleanup` → `native-escape-retirement`（最后一槽被 ABI 43 删除）；`native-escape-retirement` → `gui-owned-recording`（GUI 录制目标改由 overlay 持有，旧 "main context 转换" 描述作废）。
- 修正事实：不存在的 `2026-10-18-rhi-containers` 前置引用、escape slot 数 9→8、工厂数（契约 11 / 后端 13）、旧 `object_ptr`/`release_deleter`/`std::shared_ptr` 所有权叙述标记 superseded、批量测试计数按各自 commit 标注。
- 修复一处损坏的 markdown（加粗未闭合）。

**契约 / 后端注释**
- `rhi.contract.cppm` ABI 历史：45/44/43 注记齐全且 newest-first；修正 ABI 41 注记（`object_ptr` 在 ABI 43 批次改名为 `unique_object`）；恢复 ABI 18/21 的历史 `std::shared_ptr` 拼写；`denen`→`deren` 残留。
- 所有权 prose 统一：入口/根/工厂都描述 `rhi::shared_object` / `unique_object` 的控制块与 `release()`，不再说 "shared_ptr + backend deleter"；后端内部 `std::shared_ptr` 明确标注为内部保留、非契约句柄。
- GUI 流程 prose 统一为 gui ABI 5：overlay 持有两个 mergeable 段 → `record(begin)` 开始、cast 到共享 Vulkan 类、`get()`、录、结束并返回段 → host 在 pass 实例结束后的独立 secondary-contents 实例执行；`inherit_descriptor_heaps=false` 的 ImGui 理由写在字段旁。
- `@file` 审计：范围内唯一错误路径（`core.declarations.cppm` 写成 `@file core.cppm`）已修正；`@param/@return` 名称核对一致；4 处历史控制字符损坏的注释已修复。

**面向用户文档**
- `dynamic_runtime.md`：ABI 规则包含 by-value 字段追加（ABI 45），当前 45/5；C 入口返回 `rhi::shared_object<api_core>`；escape/extension 段改写为 ABI 45/GUI ABI 5 的录制交接。
- `mainpage.md`：契约所有权（unique/shared containers、无 `std::shared_ptr` 管理 RHI 句柄）、ABI 45、能力清单（ray tracing 不再是 extension）、GUI plugin 段、heap 的 abi-45 opt-out。
- `resource_clusters.md` / `managed_resources.md`：`object_ptr` 词汇清除，`from_shared` 与别名 owner 语义改为 `rhi::shared_object`；generation 容器所有权注明。
- `runtime_split.md`：无相关过期陈述，仅按文档自身方法刷新 §0 行数统计。

**Lead 收尾的残留**
- 三个 overlay host（post/fxaa/upscale）里 "overlay records RAW / inside this instance" 的旧注释改为 record+finish 两实例事实；
- `runtime.cpp` enable_debug_gui、`runtime.cppm` 模块头、`runtime.declarations.cppm` 的 `shared_ptr<core>`/`shared_ptr<api_core>` 等当前事实叙述；
- `docs/vessel.md` "ABI33 / GUI ABI3" → "ABI45 / GUI ABI5"。

## 验证

- 行为零变化：`git diff` 对 `source/` 的新增行仅注释（含 3 处 `#include` 行尾注释与 1 处 `return {};` 行尾注释）；
- `cmake --build build-release-dyn-clang64` 成功；`ctest` **49/49 passed**（含 `test_docs`）；
- `doxygen Doxyfile` **0 warnings**。

## 已知残留在范围外（未修改，记录备查）

- `docs/superpowers/**` 的 2026-10-09/10 计划与 spec 仍写 "ABI 33 / contract:300"（历史规划文档，按 docs audit 的既定策略保留）；
- `docs/reviews/2026-10-15-vessel-complete.md` 的 ABI33/GUI3 是其批次历史值；
- 冻结测试注释里的 escape/native_device 描述（测试只读，属历史叙述）；
- reviewer 报出的纯代码遗留（死 log flag、写后不读的 extension 名、重复的 mergeable forward declaration/alias、未使用的 `<memory>` include）不属于本 review 范围，留给下一次代码清理批次。
