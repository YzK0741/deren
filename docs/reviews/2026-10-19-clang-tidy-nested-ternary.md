# clang-tidy `readability-avoid-nested-conditional-operator` 修复

日期：2026-10-19 · 批次：clang-tidy-nested-ternary · 基线：`34bedb95`

## 这个 warning 是什么

`readability-avoid-nested-conditional-operator` 检查的是：**一个 `?:` 出现在另一个 `?:` 的
子表达式里**（三目运算符嵌套）。它和编译正确性无关——嵌套三目是合法 C++——报的是可读性：
条件链从左到右的结合性靠缩进表达，读者要自己重建优先级，条件/分支一多就容易读错。

CLion 里点开两行提示的关系是：

```
main.cpp(308,18): warning: conditional operator is used as sub-expression of parent ...
main.cpp(307,16): note: parent conditional operator here
```

即 308 行的内层 `?:` 嵌在 307 行外层 `?:` 的分支里。项目 `.clang-tidy` 启用了
`readability-*`，所以这个检查在 CLion/clangd 里可见。

## 修复方式

对 `source/**`（排除 `source/tests/`、`source/vstd/`、`source/third_party/`）扫描，共
**15 处嵌套表达式、14 个文件**，统一改成显式分支/提前返回，语义不变：

- 纯查表（`sh_degree`、`VkImageViewType`、`VkCullModeFlags`、`load_op`、`cull_mode`、
  UTF-8 宽度等）→ 先给变量默认值，再用 `if / else if` 覆盖；
- 返回值链（`toon_base_ramp_name`、`clamp_render_scale`）→ 提前 `return`；
- 初始化器里的嵌套（`.type =`、`.loadOp =`、`.encode_gamma =`、`.resolution =`）→ 在结构体
  之前计算变量，或就地 `[&]{...}()`（`loadOp` 两处）；
- 条件表达式里的嵌套（`instances.cppm` 的尺寸兼容判断）→ 抽成局部 lambda。

## 验证

- 完整构建通过；
- 对同一批文件复扫 `--checks='-*,readability-avoid-nested-conditional-operator'`：
  **剩余 warning = 0**；
- `ctest` **51/51**；
- `gaussian_ribbon` 窗口 capture：**exit 0**，无 `[ERROR]`/VUID/panic。

## 未处理

`source/tests/`（测试冻结）与第三方/`vstd` 不在范围内；`.clang-tidy` 中其余检查类别
（`bugprone-*`、`modernize-*`、其它 `readability-*`）尚未批量处理。
