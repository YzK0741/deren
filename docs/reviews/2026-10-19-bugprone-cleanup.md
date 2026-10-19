# clang-tidy bugprone 族扫描 + 可行动项修复

日期：2026-10-19 · 批次：bugprone-cleanup · 基线：`aff2998b`

## 扫描

沿用 `.clang-tidy` 的 bugprone 配置（排除 std-namespace-modification / invalid-enum-default-init /
easily-swappable-parameters / signed-bitwise），对编译库内 **189 个 TU**（排除 tests/vstd/third_party）扫描：

**55 条、9 类**，原始结果在 `build-release-dyn-clang64/bugprone-audit.raw.txt`。

## 已修复（22 条，全部是机械/明确项）

- `bugprone-argument-comment` **6**：`/*depth_test_enabled=*/` → `/*depth_test=*/`
  （goo_rim / toon_screen_rim / visibility_raster / runtime.cpp 三处）；
- `bugprone-implicit-widening-of-multiplication-result` **13**：把乘法的一个操作数显式转成 64 位
  （`size_t`/`VkDeviceSize`/`std::uint64_t`），涉及 core.api_core、descriptor_heap、
  core.texture_groups、init_utils、meshlet、ray_traced_shadow、shared_utility、stacktrace、
  stacktrace_native；语义不变；
- `bugprone-multi-level-implicit-pointer-conversion` **1**：`UpdateProcThreadAttribute` 的
  `HANDLE*` 参数加 `static_cast<void*>`（stacktrace_native.cpp:540）；
- `bugprone-narrowing-conversions` **2**：`acceleration_structure.cpp:57` 的
  `matrix[column][row]` 索引显式 `static_cast<int>`（glm 的 `length_type`）。

验证：完整构建通过、`ctest` 51/51、`gaussian_ribbon` 窗口 capture exit 0。

## 剩余 33 条（不改代码，属于误报或需产品决策）

| 检查 | 数量 | 原因 |
|---|---:|---|
| `bugprone-sizeof-container` | 19 | ABI 字段宽度检查 `sizeof(rhi_xxx_desc::member)` 与 `sizeof(std::array<...>)` 的合法用法，检查把成员误判成容器 |
| `bugprone-sizeof-expression` | 2 | 同上，`sizeof(S::next)` 是指针成员但这里问的是 ABI 字段宽度 |
| `bugprone-unchecked-optional-access` | 8 | 需要逐点确认不变量（`modules[index]`、`*packed_mip_size`、`*in.ldr`、`*this->bottom` 等）；确认/加断言后才能改，未动 |
| `bugprone-branch-clone` | 2 | 两个相邻分支有意相同（枚举值共享处理） |
| `bugprone-incorrect-roundings` | 2 | `(float + 0.5f)` 对**非负**尺寸是正确四舍五入，检查为通用告警 |

后续处理建议：false positive 类在 `.clang-tidy` 里加排除并写明理由（与现有排除项风格一致）；
unchecked-optional 逐点 review 后决定加断言/早退或 NOLINT。
