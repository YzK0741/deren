# clangd IncludeCleaner 清理：实现单元的无用标准头

日期：2026-10-19 · 批次：clangd-include-cleanup · 基线：`910a0113`

## 方法

用 clangd 工具链的 `clang-include-cleaner`（clang 23.1.3，与项目同一 MSYS2 clang64）
对 `build-release-dyn-clang64/compile_commands.json` 里的 245 个源文件做 include 分析：

```
clang-include-cleaner -p build-release-dyn-clang64 --print=changes <file>
```

只把 `- <header>`（建议删除）作为候选，忽略它给出的 `+ "<...>.cppm"` / `+ <header>`
插入建议——本项目的头文件是模块，不是文本头，工具的插入建议不适用。

## 实际清理

**37 个 `.cpp` 实现单元，删除 53 个无用标准头**（`<array>`、`<string>`、`<vector>`、
`<memory>`、`<limits>`、`<bit>`、`<new>`、`<sstream>`、`<algorithm>` 等），例如：

- `source/engine/pass/*.cpp` 的 `<array>`/`<string>`
- `source/backends/vulkan/core/core.frame_resources.cpp` 的 `<memory>`
- `source/engine/runtime/runtime.cpp` 的 `<bit>`
- `source/gaussian_loader/gaussian_loader.cpp` 的 `<array>`/`<vector>`
- `.cpp` 的 global-module-fragment include 列表变短

删除后 **完整构建通过、ctest 51/51、`gaussian_ribbon` 窗口 capture exit 0、无 VUID/panic**。
这些文件是模块实现单元（`module;` + includes + `module x;`），改 include 只重编译对象、
不影响模块 BMI；clang-format 在构建前自动对齐了被改文件（1 行注释对齐）。

## 为什么没有动模块接口（.cppm）

同样对 `.cppm` 应用了 105 处删除（56 个文件）做实验，构建失败：

```
error: declaration of 'inout_ptr_t' in module deren.engine.backend_loader
error: declaration of 'operator new' in module deren.engine.backend_loader
```

原因正是 `source/app/chores.cpp:3-10` 记录的那条规则：本项目的模块 GMF 里文本包含
libc++ 头，是为了让同一个 TU 里的"模块 std 实体"和"文本 libc++ 实体"合并；删掉这些
"clangd 认为未使用"的 include 后，消费者 TU 再通过文本头 `#include <memory>` 拿到同名
声明就与模块里的声明冲突。**因此 `.cppm` 的 GMF include 是承重的，全部保留**（实验已
完整回滚）。

## 未清理的范围

- `.cppm` 模块接口：GMF include 承重（见上）；
- 第三方头（`glm/`、`vulkan/`、`GLFW/`、`stb/`、`toml++/`、`boost/` 等）：工具大量误报
  "未使用"，实际通过宏/类型/模板使用，全部跳过；
- `source/tests/`：测试冻结批次，未动；
- `source/vstd/`、`source/third_party/`：不是项目代码。
