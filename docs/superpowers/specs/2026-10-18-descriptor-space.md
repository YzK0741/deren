# 描述符空间（descriptor_space）与纹理组位置语义（设计）

日期：2026-10-18 · 状态：已实现（本批）· 关联：`docs/dynamic_runtime.md`、`source/promise/rhi/rhi.extension.cppm`、`source/backends/vulkan/core/core.texture_groups.cpp`

## 1. 要解决的问题

`texture_group` 现在把"组分配 + 描述符写入 + 组记录发布"三件事藏在一个对象里：调用方拿到
`texture_group_ref` 之后，能引用的只有"这个组"本身；组的**位置**（组表 token、16 个 sampled 纹理槽）
是 backend 的私有知识，只能通过 `command_buffer::load_texture_group` 的 push token 隐式使用。
材质数据如果要引用一组纹理，就没有契约层面的位置值可以存（现在的绝对槽位只存在于 render layout 的
后端知识里）。

本批把 `texture_group` 的语义拆成两条显式的、契约可表达的动作：

1. **向描述符空间写入**：`descriptor_space::write(descriptor_write)` —— 把组的一个 sampled token
   按可移植的描述（`image` + `image_view_desc`，不含任何原生参数）写进空间里的位置；
2. **注册后的位置描述**：`descriptor_space::position(texture_group const&)` 返回冻结的
   `descriptor_position`，注册之后位置不变（不可变组），调用方可以把它放进材质记录 / push 常量，
   而不是把绝对槽位泄漏出去。

`texture_group` 对象本身**保留**：它仍然是"不可变、可共享、带引用计数"的作者对象；位置只是它的
一个可查询属性。

## 2. API（contract，新增 tier-2 ability，不 bump abi）

```cpp
enum class extension_kind : std::uint32_t {
    ...
    frame_resources = 1u << 11,
    descriptor_space = 1u << 12,   // APPENDED（新 bit，旧 bit 不动）
};

struct descriptor_position {              // frozen by value（16B）
    std::uint32_t space = 0;              // 0 = renderer 主空间（v1 唯一空间）
    std::uint32_t base = 0;               // 组在空间中的 shader-facing 身份（组表 token）
    std::uint32_t count = 0;              // 组覆盖的 sampled 槽数（texture_group_capacity=16）
    std::uint32_t generation = 0;         // 位置被复用时变化，用于识别过期位置
};

struct descriptor_space_properties {
    std::uint32_t group_capacity;         // 可容纳的 live 组数
    std::uint32_t texture_capacity;       // 可容纳的 sampled 槽数
    std::uint32_t group_record_bytes;     // 组的 shader-facing 记录大小
    std::uint32_t texture_slot_bytes;     // 一个 sampled 描述符的 stride
};

struct descriptor_write {                 // 一次可移植的 sampled 描述符写入
    structure_header header{structure_type::descriptor_write, ...};
    descriptor_position where{};
    std::uint32_t index = 0;              // 0..count-1
    descriptor_type type = descriptor_type::sampled_image;
    image const* resource = nullptr;
    image_view_desc const* view = nullptr; // 可选；null = 组 profile 的视图（全 mip、单层）
};

struct descriptor_space : extension {
    static constexpr extension_kind extension_id = extension_kind::descriptor_space;
    extension_kind kind() const noexcept final;
    virtual bool ready() const noexcept = 0;
    virtual descriptor_space_properties properties() const noexcept = 0;
    virtual descriptor_position position(texture_group const& group) const noexcept = 0;
    virtual error write(descriptor_write const& info) noexcept = 0;
};
```

### 语义规则（写进契约注释）

1. `position()` 对**本空间的 live 组**返回 `count == 16` 的位置；其它任何对象（外来组、已释放组、
   别的空间）返回 `count == 0` 的空位置。位置是不可变组注册后给出的**稳定值**。
2. `write()` 的 `where` 必须是 live 位置：`space` 匹配、`base` 命中组表、`generation` 与组当前
   generation 相等，否则 `invalid_argument`。`index >= where.count` 也是 `invalid_argument`。
3. v1 只支持 `descriptor_type::sampled_image` / `combined_image_sampler`；其它类型
   `unsupported`（storage/buffer/AS 留给后续 descriptor_space 的扩展批次）。
4. `write()` 的 `view == nullptr` 表示组 profile 的默认视图（全 mip、array layer 1、role sampled）；
   显式 view 只接受与组 profile 兼容的形状，否则 `unsupported`。
5. 写入是 CPU 侧（descriptor heap 的宿主可见内存）动作，发生在录制/提交之前；位置/写入不改变
   `load_texture_group` 的发布顺序（先写、后 load、再提交使用）。
6. 引擎可以保存 `descriptor_position`，但**组的所有权必须仍然存活**；组销毁后旧位置只能被
   `write()`/未来 verb 以 `invalid_argument` 拒绝，无法从裸指针恢复（与 borrowed 规则一致）。

## 3. ABI 决策

- **不是 bump**：新 ability = 新 bit；`descriptor_position` / `descriptor_space_properties` /
  `descriptor_write` 是新 POD 类型；`structure_type::descriptor_write = 6` 是追加的枚举值。
  没有已有 vtable 的槽位移动、没有已有 by-value POD 改形、C 入口签名不变。
- 该决定记录在 `rhi.contract.cppm` 的 NOT-A-BUMP 批次注释里，与 `is_concrete/do_concrete`、错误枚举
  追加的既有先例同一节。

## 4. 后端映射（Vulkan）

- `descriptor_space` = 现有 `texture_group_state` 的公开面：`table_slot = 17664` 是组表描述符，
  `texture_base = 17665` 起是 sampled 槽池；每个组分配一个组表槽（`allocation`），
  `position.base = allocation + 1`（= 现有 push token），`position.count = 16`，
  `position.generation` 在分配时从全局计数器取得。
- `written` 路径统一：`assign_texture_group` 的 16 个 sampled 描述符写入、`write()`、以及
  未来 material 组的写入走同一个 helper（`write_group_texture`），它负责
  image view 校验/租约/`descriptor_heaps.write_image`/组记录更新与发布。
- `texture_group_state::live_groups` 改为按 token 索引的 map，`write()` 才能从位置唯一定位组并检查
  generation（也顺带消掉了 `static_cast<owned_texture_group>` 找不到组时的路径）。
- ability 只在 `descriptor_heaps.ready()` 时广播（与 `descriptor_heap` 位同条件）；`ready()` 动态回答。
- `core::frame_descriptor_space` 是 core 的成员视图（同 `frame_heap` 模式），concrete 名字加入
  `deren::vulkan::concrete` 表。

## 5. Probe backend 与测试

- probe 实现一个 CPU 版 `descriptor_space`：fake 组注册表 + 位置/generation + 写入计数，**两种模式**
  （portable / injected）都广播该 bit，保证一致性门与 `test_dynamic_link` 的 G1 walk 覆盖到它。
- `test_dynamic_link` 的契约检查新增：
  - ability 公告 ↔ `query_extension` 双向一致（G1 walk 自动覆盖）；
  - 空组注册 → `position()` 有效（count=16、generation≠0、space=0）；
  - 外来组 → `position()` count==0；
  - `write()` 的非法位置/索引/类型 → `invalid_argument`；
  - 组释放后旧位置 `write()` → `invalid_argument`（过期位置检测）。
- `test_texture_group`（真设备）追加：真实 Vulkan 后端上 position 有效、load 之后写入位置稳定、
  释放后 stale 写入被拒。
- 因引擎尚未消费 `texture_group`，本批**不改渲染路径**：OFF 15/15 冻结哈希与 ON 参考都不应变化。

## 6. 非目标 / 后续

- 材质记录/shader 从绝对槽位迁移到 `descriptor_position`（属 D1a-3，需要改 render layout/材质表/shader，
  会重基线 ON 参考）；本批只保证契约/后端/probe 语义可用。
- 多 descriptor space、storage image / buffer / AS 的统一 position（D1b）；届时把 `position.space`
  从"只有 0"变成真正的空间标识，并增加对应写动词。
- 删除 `vulkan_heap_image_desc/info`、`native_image_format`（D2，需要 view 描述完备后）。

## 7. 验收

- `cmake --build build-release-dyn-clang64`、`ctest` 全绿（含新增契约用例）；
- `test_native_boundary_zero` / `test_backend_boundary` 仍 PASS；
- `test_docs` / doxygen 0 warnings（如改 INPUT 文档）；
- 完整冻结 OFF gate 15/15 + `-Visibility` 参考 PASS（预期无像素变化）；
- `scripts/check_rhi_validation_audit.py` 输出更新（具体名 27→28）。
