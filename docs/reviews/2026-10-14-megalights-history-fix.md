# MegaLights 历史有效性修复 — 2026-10-14

资源迁移后发现的原有时域有效性缺陷已修复。本次提交同时收录此前已验证的资源管理、离屏配置、阴影修复及性能记录。

## 修复

- runtime只在收集`megalights`阶段的temporal-copy成功结果后，提升当前输出图像的history-valid和resolved；trace录制本身不能证明resolve成功。
- temporal pass通过既有push block的`extents.w`传递有效位，shader只在历史有效且重投影坐标合法时读取history。push大小、布局及RHI ABI32不变。
- 功能开关变化清空所有输出历史；资源generation replacement继续清空历史。push失败时不再继续dispatch/copy或报告成功。
- 每帧在延迟渲染分支之外重置结果并清理不活跃的当前历史，覆盖G-buffer debug、unlit及其他跳过路径。修复审查发现的debug模式往返后读取旧历史的问题。
- 录制结束失败、提交失败及提交前发现异步present失败时，清除当前历史。提交成功后的present失败不撤销已提交的GPU拷贝。

## 回归和验证

- 原反馈缺陷：新测试修复前4/28失败，修复后28项通过。
- shader有效位传递/使用：修复前2/1594失败，修复后1594项通过。
- 提交失败路径：新增用例修复前1/31失败，修复后31项通过。
- debug模式往返：新增用例修复前10/40失败，最终40项通过。probe测试同时覆盖首次使用、无成功copy、无关stage、toggle、不同输出图像、replacement及无open-frame提交失败。
- 最终完整Release构建成功；CTest24/24通过，19.54秒；真实Vulkan runtime1164项通过，shader1594项通过。
- 最终13组场景各运行40帧、每帧环绕0.5度，同步验证干净。含32灯MegaLights+RT，以及0.67渲染比例MegaLights+RT+TAA。
- 相对于冻结的迁移后、修复前运行包，13组最终RGBA截图全部一致。此比较只说明这些捕获没有最终图像回归，不能据此证明时域累计数值或收敛质量；有效性协议由行为回归和shader分支检查验证。
- ML+RT+缩小比例+TAA最大化再恢复：两次resize通过，1280×720输出与恢复后client尺寸一致，同步验证干净。
- 独立只读审查发现debug跳过问题；修复后复审未发现其他具体缺陷。`git diff --check`通过。

## 证据

归档目录：[2026-10-14-megalights-history-fix](2026-10-14-megalights-history-fix/)。包含13组前后PNG、配置、日志、像素统计、resize证据、CTest日志、执行脚本及SHA256。
工作目录为`build-release-dyn-clang64/ml-history-check`，冻结包为其`runtime.before`；冻结包已包含17类图像资源迁移，携带相同toon FGD LUT。

归档PNG仅重新压缩IDAT，解压后的扫描行字节逐项断言相同，其余PNG块保留。
此前112张证据PNG同样无损压缩，从418,068,241字节减为38,100,115字节；记录见[压缩清单](2026-10-14-capture-compression.json)。二进制和shader摘要未改写。

这次收录的资源管理范围包括passchain生命周期/实例/共享generation、shadow与cluster buffers、light/head常量，以及G-buffer、HDR、scene-input、depth/velocity、TAA、LDR/Bloom、MegaLights与RT visibility图像。共享IBL/furnace、Gaussian/场景缓冲、AS、samplers及自动descriptor/barrier管理仍是后续工作。
