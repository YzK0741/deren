# utility::vessel 与 mimalloc 分配策略实施记录

用户已在对话中确定命名、OOM 策略并明确授权实施；此前场景/快照阶段一暂停，不修改其生产实现。当前在原工作区实施，以保留已有未提交资源所有权迁移；不创建空白 worktree，不自动提交或推送。

**目标：** `deren::utility::vessel` 提供 STL 容器别名，allocator 使用 shared_utility DLL 中唯一的 mimalloc 分配/释放入口；底层 `try_allocate` 可返回错误，标准容器 allocator 在失败时直接诊断并 abort；移除当前 PMR 路由并迁移项目自有容器。

**边界：** 不改第三方库、vstd 的标准库导出、RHI ABI32 或 GPU 内存分配。（2026-10-18 核对：RHI ABI 现为 33，证据 source/promise/rhi/rhi.contract.cppm:300；ABI32 是当时的基线，本文其它处出现的 ABI32 同理。）跨第三方/标准库/RHI 的具体容器类型保留并显式适配。`std::span/string_view` 等借用视图、array/optional/expected 等无动态容器分配的类型、无法指定 allocator 的 function 以及库内部隐藏分配不宣称已全由 mimalloc 管理。容器元素自己的构造/复制及越界、长度错误也不宣称 universally noexcept。
> 历史规划文档：本文写作时的 RHI ABI 为 33 / GUI ABI 为 3；当前值为 RHI ABI 59 / GUI ABI 5（见 `rhi.contract.cppm` 与 `gui.cppm`）。

## 任务与证据

- [x] 保存原有 dirty patch/status：`docs/reviews/2026-10-14-vessel/workspace-before.*`。
- [x] 基线完整 CTest：24/25；backend boundary 在修改前已有 Python/文件系统测试失败，不能计为本任务新增。
- [x] 注册 test_vessel，先验证旧分配路径缺少统一 fatal diagnostic：2 checks 中 1 failed。
- [x] 实现 `memory::try_allocate(bytes, alignment) noexcept -> expected<void*,allocation_error>`、`allocate`、`deallocate`、allocator<T>；校验对齐、字节数和乘法边界。
- [x] 无分配诊断：固定栈缓冲及直接 OS stderr write，不使用 panic/logger/format、GPU 等待、恢复回调或循环重试。
- [x] 提供 vector/deque/list/string/map/set/unordered 容器及 queue/stack/priority_queue；测试 over-alignment、rebind、vector<bool>、节点容器、复制/移动/交换及跨线程释放。
- [x] DLL fixture 验证外部分配、本模块释放及反方向配对。
- [x] 移除 better_pmr、pmr_manager、init_pmr 与 runtime PMR 容器。
- [x] vessel 字符串构建器 format/to_string 测试先因缺少接口失败，再实现直接生成 vessel 字符串。
- [x] 完成引擎/utility/app/loader/config 中项目自有动态容器和字符串迁移，接好外部具体类型边界并构建。
- [x] 完整 CTest、边界检查及无窗口 GPU 行为验证，审查后记录证据：`docs/reviews/2026-10-14-vessel.md`。

本批迁移范围已完成；后端私有容器与 RHI/GUI 契约具体容器保留，不能表述为整个仓库的所有 STL 分配已接管。原始 new、shared_ptr 控制块、第三方及标准库内部隐藏分配也不在本批范围内。

## 实施裁决

- 用户要求提高并行数：后续构建采用 `cmake --build build-release-dyn-clang64 -j14`；实测可见 16 个逻辑处理器。
- 既有 source/core 修改必须保留。批量迁移仅替换项目容器类型及必要 import/adapter，不重写算法。
- allocator 与 exported vessel 模板放在 shared_utility 的 primary interface，复用已有模块/DLL组织，避免此前 partition re-export 的编译器问题；mimalloc 头和 Windows 头分别留在实现 TU/普通 TU。
- C++ 导出接口中的 allocator 类型变化要求重新构建消费者并升级相应 module banner；RHI 数据/虚函数 ABI32 不改。
- BVH 输入改为借用 span，以便原标准容器和 vessel 都能提供数据；RHI readback 视图同样优先用 span，确需拥有 diagnostic 输出时显式复制到 vessel。

## 验证命令

构建：`cmake --build build-release-dyn-clang64 -j14`。

定位测试：`ctest --test-dir build-release-dyn-clang64 -R "^(test_vessel|test_utility|test_math|test_gltf_loader|test_animation|test_runtime_injection|test_dynamic_link)$" --output-on-failure`。

完整套件：`ctest --test-dir build-release-dyn-clang64 --output-on-failure`。所有运行都记录新输出；既有测试报告不作为本任务通过证据。
