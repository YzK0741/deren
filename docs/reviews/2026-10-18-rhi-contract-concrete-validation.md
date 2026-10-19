# RHI 具体身份校验与转型操作移入契约（2026-10-18）

## 结论

ABI 30 让每个 RHI 对象带一个静态字符串 `type`（**具体实现名**，如 `deren_image_vulkan`）。后端拿到句柄后先校验名字、再 `static_cast` 到自己的具体适配类型。本次把“校验 + 转型”这个**操作**从 Vulkan 后端（`deren.vulkan.core:declarations`）搬到契约层 `deren.promise.rhi:contract`；**名字仍归后端所有**。

## 契约层（source/promise/rhi/rhi.contract.cppm）

| 名字 | 语义 |
| --- | --- |
| `rhi::is_concrete(object const&, std::string_view name)` | 面向调用方的拒绝路径。false = 不是本后端发出的句柄，调用方按契约返回 `rhi::error::invalid_argument`（工厂返回 `nullptr`）。 |
| `template <typename concrete_t> rhi::do_concrete(object const& / object&, name, std::source_location = current())` | 只在 provenance（注册表成员、对象身份比较）**已经证明**之后调用；名称不匹配 = 后端自身簿记不一致 = `deren::utility::panic` 终止，报告的是调用点位置。可变/const 两个重载，供记录类操作（命令缓冲区、build 目标）写入。 |

- 新的唯一项目依赖是 `import deren.utility.shared`：两侧镜像本来就链接它（进程级唯一的 panic sink / 日志），所以失败报告不会分叉出第二个例程。
- “不匹配就 panic”与契约的 `invalid_argument` 承诺不冲突：**外来句柄在函数入口由 `is_concrete` 回答错误码**，`do_concrete` 只处理“来源已证明”之后的内部不变式；这与 `deren::utility::ensure` 的既定规则一致（“代码预期内的失败走值，被破坏的不变式才终止”）。

## 后端（source/backends/vulkan/core）

- `deren::vulkan::concrete` 命名空间保存 27 个具体实现名常量；所有适配类构造函数与所有校验点共用同一真源，名字不会在生产者和校验之间漂移。
- `core.api_core.cpp` 的 19 处比较语句（含 owned/borrowed 双实现的判断）与 `radix_sort.cpp` 的 2 处改为 `rhi::is_concrete`：回答的 `invalid_argument` 不变，行为等价。
- 9 处 `static_cast` 改为 `rhi::do_concrete<具体类型>`，它们全部位于**注册表成员校验之后**（`heap write_image`、`bind_pipeline`、`copy_image_to_buffer`、`barrier` 的 image resolve、`copy_image`、`execute`×2、`submit`）。若名字与注册表不一致，以前是错误布局的未定义行为，现在是命名 panic。

## 为什么不使用 `structure_type`

`structure_type` 是**参数结构** pNext 链的 tag，`validate_structure` 用它配合 `struct_size` / `next` 校验；对象身份是另一件事，见 `2026-10-10-rhi-concrete-type-names.md` 第 42 行（“structure_type 继续描述参数结构的 next 链，与对象具体实现名称是不同的用途”）。本操作是具体实现身份的机制，不是结构 tag。

## 为什么不 bump `abi_version`（仍为 33）

新增的是 `constexpr` / `inline` / `template` 自由函数：没有 vtable 槽、没有 by-value POD、没有入口点签名变化；名字仍由后端拥有；契约新增的依赖 `deren.utility.shared` 是两侧已有的进程级模块。这些正是 ABI 规则里“新类型/新 helper 不编号”的一类（规则在 `rhi.contract.cppm` 的 `abi_version` 注释块中，本次同处记录了一条 NOT A BUMP 说明）。

## 验证

- Release 全量构建通过（`deren_vulkan.dll`、`deren.exe`、49 个测试可执行文件）。
- `ctest`：**49/49 passed**。
- `check-backend-boundary`：0 symbols / 0 reference sites / 0 owned-STL / 0 module imports，baseline 0 不变。
- `test_dynamic_link` 新增契约层用例：`is_concrete` 的两条答案在编译期 `static_assert` 钉住，`do_concrete` 的匹配分支在运行期断言返回同一对象。
- `test_runtime_injection` 新增 `--child-do-concrete-panic` 子进程用例（与两条 abi 拒绝用例同一套 `run_child` / `last_panic` 机制）：断言进程被终止，且诊断里同时出现 `do_concrete`、句柄的实际具体名、要求的具体名，以及**调用点的源文件**——`do_concrete` 的不匹配分支是 panic，只能在子进程里验证。
- `scripts/check_rhi_validation_audit.py`：从当前树重新生成下面的清理审计表（具体名常量、泄漏字面量、`is_concrete`/`do_concrete` 计数、剩余 cast 的分类），避免手写数字漂移。

## 迁移后的清理审计（最终状态，由 `scripts/check_rhi_validation_audit.py` 生成）

| 检查项 | 结果 |
| --- | --- |
| 旧式 `.type ==/!= "deren_…_vulkan"` 校验 | 后端全目录 **0 处**（迁移前 `core.api_core.cpp` 19 条语句 + `radix_sort.cpp` 2 条；迁移后 **21 条语句 / 27 处 `rhi::is_concrete` 调用**，数字由 `scripts/check_rhi_validation_audit.py` 生成） |
| 第一版加在后端的 `is_concrete` / `do_concrete` 定义 | **0 处**；定义只在契约层（`is_concrete` + 两个 `do_concrete` 重载），后端只调用 |
| 适配器构造函数里的名字字面量 | **0 处**；27 个 `deren::vulkan::concrete` 常量是唯一真源 |
| 字符串中间的对象实现名 | **0 处**（`frame_snapshot::release()` 的日志已改用 `concrete::frame_snapshot`）；只剩 `gui_dll.cpp` 的 DLL 文件名 `deren_gui_vulkan`，不是对象身份 |
| 剩余 `static_cast<owned_*>` | **39 处**（脚本实测）：**26** 处有附近显式守卫（`is_concrete` / `do_concrete` / 注册表 / 对象身份）；**8** 处集中在 `buffer_address_view` 与 `frame_escape::native_*`，是 ABI 里“文档化前置条件”的路径（无 RTTI 无法校验堆对象归属，函数注释写明）；**5** 处是创建路径/工厂返回（对象刚建立，来源自证）。从来不是未校验的验证点 |

额外收尾：`create_micromap` / `create_acceleration_structure` 里两处对后端自己创建的 `storage` 缓冲区的 cast 已统一为 `rhi::do_concrete`（来源本来就有保证）；`frame_snapshot::release()` 的拒绝日志改用名字常量。

## 后续（本次未做）

- 19 处里仍有仅靠具体名、没有注册表/owner 兜底的记录类 verb（`bind_vertex_buffer` 等每 draw 调用是热路径）；给它们加锁校验需要先测量，另开一批处理。
- `buffer_address_view` 与 `frame_escape::native_*` 的 7 处前置条件 cast 若也要“误用即命名 panic”，可统一改走 `rhi::do_concrete`（行为从 UB 变为定义良好的终止，需先确认 escape 的调用方前提不变）。（`frame_escape` 已在 ABI 42 随最后一个 escape slot 删除、ABI 43 删除整个 escape 接口，所以这一半作废；`buffer_address_view` 的部分仍适用，见 `2026-10-18-native-escape-retirement.md`。）
- 后端 `concrete` 名字常量目前仍在 `core.declarations.cppm`；若第二个后端出现，可以考虑把“名字常量 + 适配类型”的摆放规则写进 conventions。
