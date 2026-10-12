# 高斯泼溅 GPU 帧准备与排序设计

日期：2026-10-11。状态：已于 2026-10-12 按批准方案实施并完成验收；结果见 [验收报告](../../reviews/2026-10-12-gaussian-gpu-frame.md)。

## 1. 目标与范围

用户明确选择提高帧率、尽量保持当前画质，并要求参考 NVIDIA 的实现。本阶段迁移逐帧粒子处理和排序到 GPU，保留普通六顶点光栅路径。mesh shader 属于下一阶段，以本阶段测量结果决定是否实施。

当前花束资产为 562,974 个粒子，SH degree 3。最近旋转验收日志中 CPU 排序约 65 ms，投影与 SH 约 25 ms；这是带验证的运行记录，不是关闭验证后的正式性能基线。现有静态属性已上传一次，逐帧 GPU buffer 也已按完成 slot 复用，因此本阶段不再把减少 GPU buffer 创建当作主要优化。

成功标准：正常 GPU 路径的 CPU 帧准备不遍历粒子、不排序、不计算逐粒子投影或 SH；CPU 只处理实例与相机。固定资产和相机协议下，端到端性能有可重复提升，既有画质、旋转和缩放验收继续通过。不预先承诺固定 FPS。

## 2. NVIDIA 参考与选择

参考入口：

- [帧内 distance、VRDX 和 draw 调度](https://github.com/nvpro-samples/vk_gaussian_splatting/blob/main/src/gaussian_splatting.cpp)。
- [深度键和计数生成](https://github.com/nvpro-samples/vk_gaussian_splatting/blob/main/shaders/dist.comp.slang)。
- [GPU radix sort 与间接绘制说明](https://nvpro-samples.github.io/vk_gaussian_splatting/deep-dives/rasterization_of_3d_gaussian_splatting/)。
- [VRDX 排序库](https://github.com/jaesung-cs/vulkan_radix_sort)。

采用 GPU 深度键、radix sort、间接绘制的组织方式。VRDX 封装在 Vulkan 后端，引擎不持有 Vulkan 句柄、不调用 native escape，也不引入 nvpro 的应用框架。

第一阶段排序总候选数：不可见粒子的键设为最大 sentinel，排序后可见引用自然位于前缀。每个输入有固定位置，避免 atomic append 导致相同深度的顺序取决于 GPU 调度。排序实际数量由 CPU 已知的候选总量提供，可见数量只用于 GPU 绘制；不需要 CPU 读回，也不需要新增 dispatch_indirect。

这与 NVIDIA 先压缩再排序可见粒子的做法有意不同：减少首轮 scan/压缩实现和稳定排序风险。相机面对少量可见粒子时仍排序所有候选，该局限通过单独的离屏场景计时量化；下一阶段再评估稳定压缩。

## 3. 画质与排序约定

保持当前 3σ 支持域、0.3 pixel² 正则项、Cholesky 椭圆基、alpha 上限 0.99、1/255 丢弃阈值、非预乘 alpha 合成、不透明中心深度测试和关闭深度写入。保留近裁面相交省略及基于正则化投影足迹的侧面裁剪，不复制 NVIDIA 的中心加 dilation 裁剪或 sqrt(8) 覆盖范围。静态属性保留 float32 和 SH 0..3，不压缩 SH、不降低阶数。

GPU 深度键使用有限的 float32 世界中心沿相机 forward 的距离，编码负深度为可升序排序的 uint32；不使用投影后的 NDC 深度，避免远处透视深度压缩带来的额外合并。规范化正负零；不可见键固定为 UINT32_MAX，有限可见键必须小于 sentinel。

原 CPU 参考按 double 深度排序。GPU float32 可能合并极接近的不同深度，不宣称与 CPU 每个引用完全一致。GPU 相同键按 stable_instance_id、local index 保持确定性：CPU 仅对实例 ID 排序并生成实例前缀，GPU 在固定输入位置写入键和值，VRDX 必须提供稳定排序。使用 GPU 实测验证稳定性；若选定版本不能满足，禁止启用该版本，不能以 nondeterministic atomic append 代替。

CPU double 数学与排序函数继续保留为独立参照，原公共函数的验证和错误契约不变。GPU 路径新增独立 frame 类型，不假装其 references() 是 CPU 排序输出，不把 CPU 已知候选数命名为 visible_count。

## 4. 帧数据与管线

静态 geometry、SH buffer 和实例记录基本布局不变。GPU 帧拥有：实例表、实例范围表、键 buffer、值 buffer、排序 scratch、最终 8 字节绘制引用、计数/状态 buffer，以及一个 16 字节非索引间接 draw 参数。

实例范围表按 stable ID 组织，包含原实例表索引和粒子前缀。每个固定 global slot 可以恢复 instance_index 和 local_gaussian_index；排序值是 global slot，最终展开为原有 draw_reference。实例表原索引不因 stable ID 顺序改变。

同一 graphics queue 的顺序：

1. CPU 检查实例、模型、相机、总数量、唯一 stable ID 和实际分配预算，更新安全 slot 的实例与范围表。
2. GPU reset 清零计数/错误标志和 draw 参数，写入 vertexCount=6、firstVertex=0、firstInstance=0。
3. GPU prepare 每粒子一次检查当前投影、支持域及有效颜色，生成深度键和值，统计 visible、outside、near_omitted；错误状态另记，不重复计入可见。
4. 后端 radix sort 排序固定数量的 key/value。
5. GPU finalize 展开可见前缀为 draw_reference，写 instanceCount。若任何粒子产生动态数值错误，instanceCount 写为零，整帧跳过高斯，避免发布部分有效图像。
6. 现有 Gaussian 光栅 pass LOAD HDR 与不透明 depth，通过 draw_indirect 绘制 GPU 可见数量，之后仍在现有透明网格和后处理之前。

vertex shader 本阶段仍计算投影和 SH，沿用原有输出，避免同时改变排序和绘制数学。CPU 的重复计算被移除；GPU prepare 的投影/SH 与 vertex 路径共享 Slang helper，不能维护两套易漂移的公式。后续 mesh 或预投影记录优化独立测量。

GPU prepare shader 与 camera/model/SH 的 float32 有限性检查必须覆盖当前绘制可产生非有限输出的条件。资产加载时完成静态验证；当前相机与模型造成的非有限值仍是逐帧 GPU 检查，不因资产有效就省略。

## 5. RHI 边界和依赖

新增普通非索引 draw_indirect 记录方法，参数为通用 buffer、offset、count、stride；定义 16 字节命令结构。Vulkan 后端校验句柄归属、indirect usage、范围和对齐。现有 recording 接口新增 virtual 需要提升 RHI ABI，同步更新所有实现、fake 和边界测试。

buffer_use 追加 indirect_read，映射 DRAW_INDIRECT / INDIRECT_COMMAND_READ。对实际读写角色补充 shader_read_write，确保 VRDX 对 key/value 的读取与写入均有正确访问域。新增枚举值不重编号旧值；不借 shader_read 冒充 indirect 参数读取。

排序通过新的可查询 RHI 能力提供，接口表达稳定 uint32 key/value 排序、最大数量、scratch 字节与用途/对齐、sorter 的拥有句柄及录制所需的契约资源。能力不带高斯概念，不暴露 VkBuffer 或 VkCommandBuffer。sorter 对象由 GPU 准备管线拥有，根设备寿命约束与现有 GPU 资产相同。

VRDX 仅作为 Vulkan 后端私有依赖。用户要求复制开源源码，已将 0.6.0、完整 commit `b0db22b0c65e17a9b803bc4daf20354898b820eb` 放入 `source/third_party/vrdx`，保留全部上游文件、MIT 许可和来源校验记录。采用上游生成头，避免普通项目配置时联网下载 Slang 或重新编译库 shader。接入前必须验证此版本的稳定性、scratch 要求、push descriptor、同步和 subgroup 能力要求。后端只在这些要求实际启用时宣布排序能力，不能根据显卡品牌猜测支持。

VRDX 会覆盖已绑定的 pipeline/layout/push 状态。调用后，finalize 和 raster 必须重新绑定自己的状态。排序包装器负责其内部各 pass 同步，调用者负责其前后的资源发布；责任在接口注释和 fake 录制测试中固定。

## 6. 生命周期、同步和预算

全部动态资源按 frame completion slot 拥有和复用，沿用已验证的 gaussian_frames 规则；不按 swapchain image index 重用。slot 未完成时不写实例、不重置计数、不重分配或销毁 scratch。asset、frame 和 sorter 必须活到引用它们的 GPU 提交完成。

prepare 前发布 reset 写入；排序前发布 prepare 的 shader write；finalize 前发布排序结果；绘制前分别发布引用的 shader read 与参数的 indirect read。slot 再次写入时覆盖前次读取依赖。必须校验 RHI role 到 Vulkan stage/access 的实际映射，不能仅凭函数名认为 barrier 有效。VRDX 实际使用 transfer 的版本，其内部 transfer 依赖必须由后端包装完整承担。

resize 更新当前相机、extent 和 attachment generation，容量足够时不重建静态属性或 scratch。GPU 准备与绘制必须使用同一帧相机参数，不使用上次相机的异步排序结果；保持 PLY 模式关闭 TAA 的现有规则。

预算按实际 buffer 大小累加：每 slot 的 keys 4N、values 4N、references 8N，加上 VRDX 查询的 scratch、实例/范围表、计数和间接参数。检查乘法、offset、uint32 count、最大 buffer 和所有 slot 总占用；不把 scratch 排除在性能路径的内存报告之外。稳态不进行粒子规模 CPU 分配或每帧 GPU buffer 创建，正常帧不调用 wait_idle。

零候选不 dispatch、不排序、不 draw。候选非零但全不可见时仍正常完成 GPU 管线，间接 instanceCount 为零，HDR/depth 的初始化继续由已有帧流程负责。

## 7. 回退和统计

支持 GPU 排序且管线创建成功时默认使用 GPU 路径。能力不足或初始化分配失败时使用当前同步 CPU 路径，并明确记录原因，不自动切换到 stochastic transparency。保留一个诊断用 CPU/GPU 选择入口，让测试能对同一场景做 A/B；不新增正常查看器的交互步骤。

录制中发生可返回错误则该帧跳过高斯并报告；GPU 发现动态错误时 finalize 将 draw 数量归零，诊断在该 slot 完成后读取。完成后的计数和时间属于之前提交的帧，标注 frame ID，不伪装为当前可见数。统计读回只发生在已有完成点，不额外阻塞 CPU 等待 GPU；需要 staging copy 的后端在提交内复制小型计数。

独立记录 CPU 实例准备、GPU prepare、GPU sort、GPU finalize、GPU raster 和整个帧耗时。统计开关关闭时不每帧打印日志，防止日志成本污染帧率。现有 lighting 的 GPU 时间区间不继续把高斯成本混在其中。

## 8. 验收

排序探针读回并验证 GPU 键/值的完整排列、升序、稳定性和引用展开；对相同输入 uint32 键，CPU 排序 oracle 与 GPU 精确比对。深度键生成另用 float32 CPU 数学参照检查，允许运算收缩带来的最多 4 ULP 深度差异；发生键合并或次序变化时单独报告，不把数学误差和排序错误混为一谈。CPU double 顺序另作精度差异统计。覆盖 0/1、workgroup 和 radix 分区边界、重复键、正负零、正负深度、随机输入、多实例乱序、相同深度、全不可见、不同 SH 阶数及预算拒绝。

GPU 数学/图像探针沿用现有五个小场景与 HDR RGB 最大绝对误差 0.005；新增双粒子极近深度、有明确顺序的多实例重叠、近远平面和四边足迹案例。相同 GPU 输入重复运行必须给出确定排序和图像。不能更新原 CPU 参考来掩盖 GPU 差异。

真实花束采用固定相机与连续旋转序列，对 CPU/GPU 同姿态进行 HDR 和最终 RGB 比较，报告最大误差、RMSE 与差异像素分布；HDR RMSE 上限 0.005，最终 RGB 每通道 RMSE 上限 1/255。超过阈值保留 CPU 默认路径并修正，不能静默放宽。投影边缘或近深度高对比样本还须人工检查，平均误差不能掩盖明显局部缺口或闪烁。

重跑同姿态 moving/still 比较、空 mesh/空粒子、三次 resize、asset 清除和替换、两个 in-flight slot；Vulkan synchronization validation 必须零错误。普通模型的 14 个 frozen 渲染基准继续匹配，不更新基准。

性能 A/B 使用相同 Release、1280×720、同资产和相机协议、关闭 validation/GUI/VSync/逐帧日志，在预热 60 帧后收集 300 帧，静止与持续旋转分别重复三次。报告 CPU 帧准备、各 GPU 阶段、端到端 median/p95 和所有 slot 内存。若 GPU 路径没有可重复的帧时间提升，不将其设为默认；开启 validation 的正确性验收单独运行。

完成所涉及的 headless 测试、shader 编译/验证、全套 CTest、格式与边界检查，并在真实设备上运行上述 GPU 探针。仅 fake RHI 通过不足以宣称排序正确或优化完成。

## 9. 后续阶段

本轮不加入 mesh shader、SH 压缩、降低 SH 阶数、size culling、异步旧相机排序、stochastic transparency 或分块光栅。测量完成后，按 GPU raster 和 sort 的实际占比选择 mesh shader 或可见列表稳定压缩；逐项复用同一验收协议。
