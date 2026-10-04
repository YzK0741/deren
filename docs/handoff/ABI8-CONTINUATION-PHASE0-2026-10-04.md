# ABI8 接续：阶段 0

基线：主仓库 `5eb8eed7e24d4c2f67d6b24419c48fc4930255b7`，分支 `codex/abi8-followup-2026-10-04`。本批恢复管线迁移后的完整构建，不代表正式动态后端或 GPU 验收完成。

## 修复

- `get_pipeline()` 的声明、定义、resolver 调用统一为 `pipelines::pipeline_handle`。注册表运行期只追加节点，唯一 clear 在析构中；内部 resolver 只读取创建后不变的 native handle。返回的指针仍是 runtime 生命周期内的借用，不能跨析构保存。
- G-buffer 的 mesh/meshlet 改走契约工厂。保留五目标格式、四个 opaque + 一个 additive、1x、mesh stage、深度测试和动态 depth-write；保留初始 render extent 的 viewport/scissor，原 resize 重同步路径不变。
- 将 `face` 和 `swap_chain_format` 从错误的 `frame_services` 初始化移回真正的 `pass_context`。
- toon-family builder 返回拥有错误文本的 `std::string`，避免从局部工厂结果返回悬空 `string_view`。错误路径的实际运行尚未验证。
- 保留上游 `<new>` 与 out-of-line 特殊成员修复；本机 clang 22.1.7 本轮未出现其历史 codegen 崩溃。

## 验证

构建目录 `D:\deren-workspace\work\build-upstream-abi8`，日志目录 `D:\deren-workspace\work\abi8-followup`。工具链：CMake 4.3.3、clang64 22.1.7、Vulkan headers 1.4.357。

1. configure：exit0。
2. 修改前 `cmake --build ... --parallel 2 -- -k 0`：exit1，复现三处返回类型、两处 G-buffer 赋值、两处错误字段，共七个诊断；`build-red.log`。
3. 修改后相同全目标构建：exit0，产品 exe 和所有配置中的目标编译/链接完成；`build-green.log`。未运行这些测试程序。
4. `check_backend_boundary.py --build-dir ... --list --report ...`：exit0，35 个符号、62 个引用位置、3 个消费者、0 owning-STL，集合是原 66 基线的子集。
5. 随后 `--update`：exit0，将仓库 baseline 收紧到 35；没有放宽消费者证据或比较规则。
6. `git diff --check`：exit0。

35 对 66 的收缩包含主仓库之前 ABI7/8 的工作，不能全部归因本批。因为基线源码不能完整编译，本轮没有可靠的“本批修改前实测符号集合”。GPU 渲染、resize、validation、错误路径和性能均未运行；保留用户要求的异机验收。

## 下一批

剩余分类：core 成员 18、descriptor_heap 8、原生 RAII 4、init_utils 3、模块初始化 1、VMA 1。先做 descriptor_heap 独立批次，特别处理二级命令缓冲录制：现有 command_list 仅覆盖主缓冲，不能把任意 VkCommandBuffer 强行冒充后端 frame_commands。

能力接口仍须给出真实签名和错误语义。允许通过明确的 Vulkan escape 处理原生描述符，禁止扩成全量 engine_device/engine_gpu 适配层。每批完整重建后重新测量，集合缩小才收紧 baseline。
