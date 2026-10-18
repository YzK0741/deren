# RHI 描述符空间（descriptor_space）与纹理组位置语义

日期：2026-10-18 · 提交批次：descriptor-space · 关联设计：`docs/superpowers/specs/2026-10-18-descriptor-space.md`

## 决策

`texture_group` 保留为"不可变、可共享、带引用计数的作者对象"，它的两个此前隐式的语义被提升为契约词汇：

1. **向描述符空间写入**：`rhi::descriptor_write` + `descriptor_space::write()`。一次写入 = 组的一个
   sampled 槽位，参数是可移植的 `image` + 可选 `image_view_desc`（无任何原生参数）；后端内部与
   `assign_texture_group` 共用同一个 helper（`core::write_group_texture_locked`），所以"组注册"和
   "显式写入"不会走两条实现。
2. **注册后的位置描述**：`rhi::descriptor_position`（冻结 16B POD：space/base/count/generation）。
   `descriptor_space::position(texture_group)` 只对本空间注册的 live 组返回 count==16 的位置，
   外来组/已释放组/其它 core 的组返回空位置。`generation` 在 token 复用时变化，因此过期位置会被
   `write()` 以 `invalid_argument` 拒绝，而不是 mis-cast。

## ABI

**没有 bump**：`descriptor_space` 是新 tier-2 ability（新 bit 12），其词汇是新增 by-value POD 类型，
`structure_type::descriptor_write = 6` 是追加的枚举值；没有既有 vtable 槽位移动、没有既有 POD 改形、
C 入口签名不变。决定记录在 `rhi.contract.cppm` 的 NOT-A-BUMP 批次注释（与 `is_concrete/do_concrete`、
错误枚举追加同一先例）。

## 后端映射（Vulkan）

- 空间 = 现有 `texture_group_state`：组表槽 `table_slot=17664`、sampled 槽池 `texture_base=17665`；
  `position.base = allocation+1`（即既有 shader-facing push token），`count = texture_group_capacity`，
  `generation` 由注册计数器给出。
- `live_groups`（指针集合）改为 `groups_by_token`（token -> 组）：`write()` 从位置定位组并校验
  generation；`load_texture_group` 与析构走同一张表。
- 空组（没有任何纹理）没有位置（count==0）：没有可引用的记录，与旧行为（token 0）一致。
- ability 与 `descriptor_heap` 同条件广播（`descriptor_heaps.ready()`），`ready()` 动态回答。
- 具体名新增 `deren_descriptor_space_vulkan`（`deren::vulkan::concrete`，总数 27 -> 28）。

## 测试与验证（全部在本批最终树上实测）

- `test_dynamic_link`（probe，static + DLL 两半）：ability 双向一致性 walk 覆盖新 bit；空组
  position==0；外来组 position==0；默认/越界 write 被 `invalid_argument` 拒绝。
- `test_runtime_injection`（CPU portable probe，无需设备）：带纹理的组返回稳定 position
  （space=0/base!=0/count=16/generation!=0）；`write()` 命中 ok；越界 index invalid_argument；
  storage_image 类型 unsupported；释放组后旧位置 write 被 `invalid_argument` 拒绝。
- `test_texture_group --with-device`（真实 Vulkan 后端）：**109 checks, 0 failed -> PASS**，含真实
  position 稳定性、`write()` ok、更窄 view 的 unsupported、越界 invalid_argument、释放后 stale 拒绝；
  原有采样输出逐值不变（组加载/共享槽位语义未变）。
- `ctest`：**49/49 passed**；`test_native_boundary_zero` / `test_backend_boundary` 仍 PASS。
- `scripts/check_rhi_validation_audit.py`：具体名常量 **28**；表外 concrete 字面量 0；
  `is_concrete` 21 条语句/27 次调用；`do_concrete` 11 处。
- 像素门禁：完整冻结 OFF gate **15/15 PASS**；`-Visibility -Compare` **PASS**（引擎未消费
  `texture_group`，渲染路径不变）。

## 非目标 / 后续

- 材质记录/shader 从绝对槽位迁移到 `descriptor_position`（D1a-3，需要改 render layout/材质表/shader，
  会重基线 ON 参考）；本批只保证契约/后端/probe 语义可用且可测。
- 多 descriptor space、storage image/buffer/AS 的统一 position（D1b）；`position.space` 为它们预留。
- 删除 `vulkan_heap_image_desc/info`、`native_image_format`（D2，需要 view 描述完备后）。
