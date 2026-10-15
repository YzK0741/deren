# 栈回溯方案移植评估

评估源码：C:/Users/23530/Desktop/yzk/stacktrace/st_stacktrace.hpp、st_stacktrace.cpp、README.md。
结论：适合借鉴分层设计和 llvm-symbolizer 符号化思路；原版需要改造后才能进入 deren 的 panic/OOM/崩溃路径。
用户接受为调试信息放宽编译参数，保持无异常风格。

## 实测

独立探针使用本机 clang64 23.1.3、C++23、fno-exceptions/fno-rtti。
operator new 计数只统计探针进程的 C++ new，不证明 OS/库内部所有分配均为零。
这不是完整 deren EXE/DLL 崩溃验证。

| 构建 | st::capture 帧数 | capture 期间 C++ new 次数 | Windows 原生取帧帧数 |
|---|---:|---:|---:|
| 当前 Release 参数：O3/LTO/no unwind tables/no asynchronous unwind tables | 0 | 1 | 3 |
| O0 + g | 6 | 1 | 5 |
| O3 + LTO + g，保留展开表 | 4 | 1 | 5 |
| O3 + LTO + g，关闭两种展开表 | 0 | 1 | 3 |
| O2 + g，保留展开表/帧指针，禁用尾调用，关闭 LTO | 5 | 1 | 5 |

当前参数下 libunwind 输出 pc not in table；单独增加 g 没有修复它。
README 逐个开关的矩阵不能证明 deren 实际组合有效。PE 中仍有 pdata 也不能证明所有业务函数均有展开信息。
诊断参数下取帧恢复，但 trace_to_string 固定 skip=3 会跳过 measured_capture，输出从 main 开始。
O3/LTO 控制组甚至只输出 ntdll。depth=2 在 O0 控制组输出5帧、诊断组输出4帧。
证据与探针源码在同目录；原方案文件未改动，deren 生产代码未改动。

## 必须改造的部分

1. st_stacktrace.cpp:65：Clang/GCC 的 native_capture 创建 vector，违反 header 中无堆分配的描述。
   Windows 按平台使用 CaptureStackBackTrace；调用方提供固定容量地址数组。POSIX 回调直接写调用方数组并计数跳帧。
   API 返回错误/截断状态，校验容量和 skip 的算术边界，不用动态字符串保存底层错误。
   崩溃/OOM 中只能称 best effort，不能保证展开器或 OS 内部绝不分配。
2. :303-320：去掉随优化改变的固定内部帧数假设，封装边界明确 noinline，分别验证所有入口。
   最终帧数必须限于 depth；上限固定，不允许配置请求任意大缓冲。
3. :416：Windows 异常过滤器丢弃 EXCEPTION_POINTERS，取到的是过滤器当前栈。
   崩溃路径保存异常码、ExceptionAddress 和 ContextRecord；从故障上下文展开或交给外部 dump 工具。
   SetUnhandledExceptionFilter 也不会自动覆盖显式 abort、所有 terminate 和栈耗尽场景。
4. :184-186：ReadFile 和无限等待没有截止时间，符号化器卡住会拖住诊断/退出。
   正常诊断符号化需限制时间、输出大小、句柄继承，固定工具路径，禁用环境变量改变输出协议。
   使用结构化输出或严格固定文本协议。失败立即保留原始地址。
5. :32：g_error 是可写全局 string，多线程符号化存在数据竞争，last_error 指针寿命不稳定。
   使用调用结果自己的错误状态，正常上下文容器采用 vessel。OOM 不调用此分配路径。
6. :353：inlines=false 没有追加 --no-inlines；LLVM 默认仍展开内联帧。
   :431：POSIX handler 的 backtrace、module 查询、string/vector 和 posix_spawn 不构成安全的信号处理路径。
   当前批先 Windows，POSIX 实测后再声称跨平台。

## 建议接入方式

shared_utility 持有唯一进程级服务；Windows 头、平台取帧和原始输出放普通 TU，utility 导出小型无异常 API。
复用现有 source_location 捕获点，避免另建 Origin。区分当前栈捕获与故障上下文捕获。

- 普通诊断：固定缓冲捕获，正常上下文允许 vessel 文本生成、限时符号化。
- panic/OOM：先写原始诊断和地址，符号化放到进程外/事后，不走 logger、分配器或无限等待。
- 崩溃：保存故障上下文，外部 dump/符号化为优先路线。DbgHelp 并发约束不能靠崩溃线程等待普通锁解决。

优先采用独立 RelWithDebInfo 诊断构建（O2/g），并明确保留 unwind tables、frame pointer、禁用 sibling-call 优化。
诊断构建关闭 LTO；fno-exceptions/fno-rtti 保留。Debug 用 O0 或低优化便于单步。
这些选项需要覆盖 EXE、shared_utility、renderer/GUI DLL，不只是栈回溯源文件。
发行构建也可保留展开表与匹配的独立调试符号；对发布文件大小和性能的变化另做实测。
EXE/DLL 与符号文件按同一次构建归档；当前无调试信息的 Release 不能凭移植符号化器凭空得到行号。
不为修复栈回溯而开启 C++ 异常，也不全局恢复 -O0。

正式接入验证需覆盖 EXE -> DLL -> EXE 调用链、LTO/非 LTO、missing symbolizer/debug symbols、子进程卡住、
并发 panic、分配失败注入、真实访问违规的 ExceptionAddress、栈溢出降级，以及原始记录事后解码。

参考：
- https://llvm.org/docs/CommandGuide/llvm-symbolizer.html
- https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturestackbacktrace
- https://learn.microsoft.com/en-us/windows/win32/api/dbghelp/nf-dbghelp-stackwalk64
- https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump
