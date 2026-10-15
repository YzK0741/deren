# vessel 全部项目容器迁移

用户明确要求补齐标准容器迁移，沿用已批准的 vessel 别名、shared_utility 唯一 mimalloc 入口和 fatal OOM 策略。栈回溯暂留评估结果，本批不接入。

范围：source 下项目自有生产及测试 C++ 文件。排除 third_party、vstd 标准库实现和 reference 参考代码。动态容器/拥有字符串改为 vessel；固定 array 用同类型别名统一名称；span/string_view 保留借用语义，function/shared_ptr/expected 等不是本批容器替换对象。

- [x] 保存 dirty 基线，盘点剩余类型。
- [x] 提供 vessel::array，同步后端、RHI/GUI 契约、应用适配层和测试拥有容器；字符串流也使用项目 allocator。
- [x] 修正模块与 PUBLIC 链接依赖，RHI ABI33 / GUI ABI3，拒绝不匹配版本加载。
- [x] 编译验证并修正第三方具体类型适配，保持调用方拥有的容器使用 vessel。
- [x] 用 -j14 全量构建，完整 CTest26/26、针对性 ASan/UBSan3/3、真实 GPU 和同参数渲染比较，独立审查。
- [x] 更新使用说明、196 个源/头文件 census、验证记录。保留原有 dirty 工作，不提交或推送。

第三方借用接口优先 view；第三方明确要求 std 容器时在边界构造其类型，不能改其头或改变其契约。vessel 核心故障路径的普通 TU 不引入依赖它自身的容器，避免递归。Windows 子进程测试 TU 在正常路径导入 shared_utility 使用 vessel，注入故障后由固定缓冲诊断终止。

CMake 4.4.4 对目标选项列表合成不同 BMI，新增 promise -> shared_utility 后出现菱形路径冲突。将 sanitizer/warning 抑制保留为以目标名称约束的 source COMPILE_OPTIONS，使公开模块目标共享同一组公共编译选项。生成命令确认 promise/backend/DLL fixture 仍禁用 sanitizer，静态 probe 仍保留 instrumentation；ASan/UBSan 针对性构建与运行通过。源代码数值检查改为忽略空白，避免容器名字加长触发 clang-format 重排后误报。

完成证据：`docs/reviews/2026-10-15-vessel-complete.md`。
