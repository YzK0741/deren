# clang-tidy `readability-braces-around-statements` 全量修复

日期：2026-10-19 · 批次：clang-tidy-braces · 基线：`8d5bf555`

## 方法

仓库根 `.clang-tidy` 已启用 `readability-*`（只排除了 magic-numbers 等风格项），
所以 `readability-braces-around-statements` 正是 CLion/clangd 里 "Statement should be
inside braces (fix available)" 的来源。用项目自己的 clang-tidy 23.1.3 按
`compile_commands.json` 批量应用自动修复：

```
clang-tidy -p build-release-dyn-clang64 \
  --checks='-*,readability-braces-around-statements' --fix <file>
```

范围：`source/**` 中进入编译数据库的文件，排除 `source/tests/`（冻结）、
`source/vstd/`、`source/third_party/`。7 个并行 worker。

## 结果

- **79 个文件，1077 处单语句 if/for/while/else 补上花括号**；
- 构建前的 clang-format 目标会格式化被改文件（与 `.clang-format` 一致），
  所以 diff 里包含少量注释跟随 `{` 的移动；
- 语义零变化：clang-tidy 的该 fix 只插入括号（和必要的换行），不动表达式/控制流；
- 复扫同一批文件：**剩余 warning = 0**。

## 验证

- 完整构建通过（模块 BMI 因 `.cppm` 改动重新生成）；
- `ctest` **51/51**；
- `gaussian_ribbon` 窗口 capture：**exit 0**，无 `[ERROR]`/VUID/panic。

## 未处理

- `source/tests/`：测试批次冻结，未编辑（CLion 打开测试文件仍会提示同类 warning；
  需要时可用同样命令对测试补一轮）；
- `.clang-tidy` 中其余启用的 `bugprone-*`/`clang-analyzer-*`/`performance-*`/
  `readability-*` 检查还没跑批量 fix，后续可以按同样流程逐类处理
  （建议顺序：`modernize-*` 低风险项 → `readability-*` 剩余项 → `bugprone-*`）。
