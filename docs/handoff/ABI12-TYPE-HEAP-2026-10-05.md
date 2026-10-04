# 阶段交接：ABI12 接口身份与通用 heap

## 结论与起点

接续分支 `codex/abi8-followup-2026-10-04`，本轮起点为 ABI11 `e205122f9f601fa54f05b3cd0f9b7858897c8ee0`。完成本轮 RHI 类型标识、带标签的 heap 参数、真实 Vulkan 实现及消费者迁移，并修正独立审查发现的三项参数校验问题。

产品仍为静态后端，GLFW 仍静态链接，应用仍有后端符号依赖。不得把本轮的编译/边界结果写成正式动态后端或 GPU 验收通过。

## 与上一份报告的关系

- `ABI9-HEAP-QUERY-2026-10-04.md` 对应 `2e6efff`：查询由直接后端依赖转成 Vulkan escape，35 →32 个符号。
- `ABI10-HEAP-WRITE-2026-10-04.md` 对应 `ed5018f`：原生 image/buffer 描述符写入转成 escape 参数，32 →30 个符号。
- ABI11 `e205122`：迁移 heap bind、push 和 binding snapshot。原交接缺新鲜边界证据，本轮在 ABI12 修改前实测为 27 个符号、44 个引用点、3 个消费者、0 个 owning-STL，未当时更新基线。
- ABI12：上述七项服务统一归入通用 `descriptor_heap`，增加对象身份及参数标签。最终边界仍为 27/44/3/0，没有新增或重新引入后端依赖，基线从30收紧到27。**这3个符号的减少属于ABI11，不能计成ABI12新收益。**

详细设计及调用示例见 [RHI-TYPE-EXTENSIONS-2026-10-05.md](../rhi/RHI-TYPE-EXTENSIONS-2026-10-05.md)。

## 云端核对

本轮开始时 fetch 得到 `upstream/master=f18de9427f3ff3c3ece4f49a31d18a04776c4dab`。相较此前 `5eb8eed`，新增提交清理24张根目录截图并修改 `.gitignore`，没有 RHI 改动，主仓库仍为 ABI8。本轮未合并该清理提交。

开始时 origin 接续分支已经包含 `e205122`，旧报告的“ABI11尚未push”已过时。origin 为 `machines-6657/deren`，upstream 为 `YzK0741/deren`；本轮按既有授权推送 origin 接续分支，不强推，不修改 upstream/master。

## 文件与行为

- `promise/rhi/rhi.contract.cppm`：ABI12、共同对象根、固定接口编号、请求标签/大小/next校验、`operation_failed`。
- `promise/rhi/rhi.api_core.cppm`：十个 tier-1 接口自动初始化身份，typed extension query。
- `promise/rhi/rhi.extension.cppm`：六个扩展固定 kind/type，七项通用 heap 服务和四个请求结构；原生过渡参数显式命名为 Vulkan；推送常量范围验证。
- `vulkan/core/core.*`：真实 frame_heap，实现参数映射、活跃 image 归属检查、普通命令域校验和按真实就绪广播能力。
- `vulkan/core/descriptor_heap/descriptor_heap.cppm`：底层 push 同样检查4字节对齐及范围。
- `vulkan/runtime/runtime.*`：heap helper 切换新接口，保持原生主/次命令缓冲录制点及字段；IBL三处使用普通RHI image请求。
- `tests/probe_backend.cpp`、`tests/test_dynamic_link.cpp`：移除无法服务录制的假 heap，更新ABI及身份检查，增加编译期参数断言。运行期检查只编译，未执行。
- `scripts/backend_boundary_baseline.mingw.json`：以新鲜应用、chores与kit消费者实测收紧到27；没有放宽条件或更换消费者。

## 独立审查与修正

只读审查发现三个 P2：native image 放行 combined sampler、buffer 放行两种 dynamic 类型、push 缺4字节对齐。依据 Vulkan-Headers registry 的 `type-11210`、`offset-11418`、`data-11419` 修正。

修正后的三类非法映射返回 `unsupported`，错位推送返回 `invalid_argument`；审查者复核未发现新的必须修项。审查为静态核对，不替代运行验收。

## 验证命令与实际结果

日志目录：`D:\deren-workspace\work\rhi-type-extension`。工具：clang64 22.1.7、CMake4.3.3、Vulkan-Headers1.4.357，Release。

```powershell
$env:PATH='D:/deren-workspace/work/toolchain/clang64/bin;'+$env:PATH
& 'D:/deren-workspace/work/toolchain/clang64/bin/cmake.exe' --build D:/deren-workspace/work/build-upstream-abi8 --parallel 2 -- -k 0
python scripts/check_backend_boundary.py --build-dir D:/deren-workspace/work/build-upstream-abi8 --update --report D:/deren-workspace/work/rhi-type-extension/boundary-abi12.json
git diff --check
```

| 日志/证据 | 结果 | 含义 |
| --- | --- | --- |
| `type-red.log/.exit` | exit1 | 新契约缺失时测试目标编译失败 |
| `type-green.log/.exit` | exit1 | 首轮发现命名空间别名和未使用lambda，均已修正 |
| `type-green-final.log/.exit` | exit0 | 初次完整ABI12编译/链接通过 |
| `review-red.log/.exit` | exit1 | 三个非法类型映射静态断言失败；推送验证接口尚未实现 |
| `review-green.log/.exit` | exit0 | 修正后完整编译/链接通过，日志未发现error/warning；deren.exe、probe DLL及测试可执行文件均生成 |
| `boundary-abi12.log/.exit` | exit1 | 首次忘记设置clang64 PATH，误用UCRT GNU nm，无法解析LLVM LTO object；未更新基线 |
| `boundary-abi12-final.log/.exit` | exit0 | 使用同一工具链的llvm-nm后，完整消费者测量与ratchet通过 |
| `boundary-abi12.json`、仓库baseline | 27/44/3/0 | 符号/引用点/消费者/owning-STL；无反向依赖、无过期消费者 |
| `git diff --check` | exit0 | 无空白格式错误 |

编译时验证了错标签、短header、短请求、未知next、允许较长尾部；新增推送正常边界、错位、空数据、越界和超大长度断言，以及三个非法描述符映射断言。没有运行任何测试程序、CTest、窗口或GPU。

## 异机交叉验证与后续优先级

1. 使用ABI12契约同时重编译应用、probe和后端；验证loader及ABI拒绝路径。当前构建带LTO和 `-march=x86-64-v3`，异机CPU/工具链需匹配。`VR_NATIVE_ARCH=OFF` 并未取消x86-64-v3。
2. 有兼容GPU时验证真实heap写入/绑定/push、主次命令缓冲继承、IBL三处图像与原版本一致；检查Vulkan validation输出和截图。
3. 修补runtime错误传播。旧void bind caller及部分push caller仍忽略返回结果，不能宣传所有失败已向上层传播。
4. 继续消除剩余27个后端符号，再推进共享GLFW、窗口所有权和正式DLL导入/导出，零边界只是其中一个门槛。

寿命前提仍有效：资源不能在调用时并发release，core必须比资源活得更久。活跃image集合只检查归属，不自动持有引用；类型身份不认证具体实现、设备地址或原生句柄。普通组合采样器请求暂不支持，未知扩展链明确拒绝。
