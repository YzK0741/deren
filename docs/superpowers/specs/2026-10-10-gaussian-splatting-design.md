# 高斯泼溅首版设计

日期：2026-10-10。状态：供评审，尚未实施。对应现有阶段计划 `../plans/2026-10-09-gaussian-splatting.md` 与[资料调研](../../research/2026-10-10-gaussian-splatting.md)。

## 1. 目标和路线选择

用户希望在现有 C++23、Slang、多后端边界的 renderer 中增加高斯泼溅。沿用此前范围：浏览已经训练好的静态资产，随后与 PBR 网格及卡渲角色组合。首版支持透视相机、SH 0..3 阶和多个高斯实例；训练、编辑、动态高斯、重光照和光追另立阶段。

本规格推荐普通实例化光栅，先以同步 CPU 排序建立正确性基准，再以同一输入输出约定加入 GPU 排序。

| 路线 | 得失与选择 |
| --- | --- |
| 普通实例化 quad + 全局深度排序 | 实现短、便于与现有 RHI 接入；投影重复计算，排序和覆盖开销较大。首版采用 |
| compute 分块光栅 | 可控制逐像素遍历、提前终止与任务分配；需要分桶、scan、排序和合成。基础版测量后决定 |
| mesh shader / 3DGRT | 前者可减少重复计算，后者支持次级视线；能力要求、资源和验证成本更高。后续按需求选择 |

首版的输出目标是正确的小场景及真实资产查看器，不承诺百万高斯的固定帧率。这里的全局中心深度排序是近似顺序，不等同于逐像素体积排序或原论文的完整 CUDA 渲染实现。[NVIDIA 光栅参考](https://nvpro-samples.github.io/vk_gaussian_splatting/deep-dives/rasterization_of_3d_gaussian_splatting/)

## 2. 模块与拥有关系

| 位置 | 职责 |
| --- | --- |
| `source/gaussian_loader/gaussian_loader.cppm/.cpp` | 文件读取、属性转换、CPU 资产与错误诊断；不依赖 engine 或 RHI |
| `source/engine/gaussian_splatting/` | GPU 资产、场景实例、全局绘制引用、排序、逐帧资源与统计 |
| `source/engine/pass/gaussian_splatting.cppm/.cpp` | 声明目标与深度输入、记录 pipeline、状态和 draw |
| `source/shaders/gaussian_raster.slang` | vertex/fragment 两个入口；数学共用文件按实际需要拆出 |
| `source/tests/` | 加载器、数学及排序 CPU 测试、GPU 小样本、冻结场景 |

CPU 资产不可变，可提前异步加载；GPU 创建在拥有对应设备的线程完成。GPU asset 可被多个 instance 共享。instance 是 `asset + transform + stable_id`，第一版接受平移、旋转和正的统一缩放；非统一缩放、镜像和非仿射变换明确拒绝。支持这些变换时须另行定义视角相关外观的语义。

高斯是独立的场景资源。现有每个 primitive 的 draw 不直接承担它的透明顺序；runtime 从 scene 中收集高斯实例，由一个专用 pass 共同处理。现有网格提交结构不用为每个高斯生成 primitive。

不让每个粒子成为 RHI 对象。位置、协方差、SH、索引都属于缓冲区，texture_group 仅在管线确有纹理输入时使用。引擎不得引入 Vulkan 句柄或 native escape。

## 3. PLY 输入契约

首版接受 PLY 1.0 的 ASCII 与 binary_little_endian，且只有一个 vertex element；不接受 big-endian、list 属性或含其他 element 的文件。未知的标量 vertex 属性可以跳过；重复属性、未知类型、缺失字段、截断与非法数量给出具体诊断。属性按名称定位，不要求文件中的物理顺序。

必需属性：`x/y/z`、`scale_0..2`、`rot_0..3`、`opacity`、`f_dc_0..2`。`f_rest_*` 必须是从 0 连续编号的完整序列，数量只能为 0、9、24、45，对应 SH 阶数 0、1、2、3。编号按数值排序。

按 INRIA 方言，尺度取 exp、透明度取稳定 sigmoid、四元数按 wxyz 归一化。零长度四元数与非有限值拒绝；尺度转换后溢出、下溢至零或不能产生有限协方差也拒绝。SH 的文件排列按通道展开，再转换成内部 coefficient-major RGB 排列。[输入语义依据](https://raw.githubusercontent.com/graphdeco-inria/gaussian-splatting/main/scene/gaussian_model.py)

`load_options` 提供最大文件字节数、粒子数和输出字节预算。首版建议默认 4,000,000 粒子、1 GiB 文件／输出预算，均可由调用者收紧或调整；不是硬件性能承诺。检查乘法溢出、属性 stride 和剩余文件长度后才能分配。错误至少携带分类、属性名及行号或字节位置。失败返回完整错误，不返回部分资产。

CPU 资产包含中心、正尺度、规范四元数、opacity、SH 和局部 bounds。bounds 使用下文定义的截断支持域。原始文件数据与转换后的内存预算分别核算。

## 4. GPU 数据和参数布局

先使用 float32。静态资源在创建时上传一次；模型变换和排序索引逐帧更新。

**几何 buffer：每粒子 48 字节，三个 float4。**

| 偏移 | 内容 |
| --- | --- |
| 0 | center.xyz、opacity |
| 16 | covariance.xx、xy、xz、0 |
| 32 | covariance.yy、yz、zz、0 |

协方差由规范四元数和尺度预计算：`Σ = R diag(s²) Rᵀ`。Shader 通过对称项恢复矩阵，用实例线性变换 A 得到 `A Σ Aᵀ`。

**SH buffer：每粒子 192 字节，`float coefficients[48]`。** 最多 16 个基函数，每个三个 RGB 标量；高于资产阶数的项为零。使用 scalar array，不用可能改变 stride 的 float3 数组。每资产的实际阶数写在 instance record 中。

**实例表：每项 160 字节。** geometry_address offset0、sh_address offset8、model matrix offset16、inverse model matrix offset80、sh_degree offset144、`uint padding[3]` offset148。地址是当前 RHI 的 device_address 能力，不是 Vulkan 类型；未来后端可用对应资源引用形式。实例表矩阵与项目的 CPU/Slang 存储约定一致，不能沿用示例的矩阵转置方式。

**绘制引用：每项 8 字节。** `uint instance_index + uint local_gaussian_index`。CPU 排序的是引用，不搬动静态属性。多个实例可以共享同一份资产。

**首版 push block：112 字节。**

| 偏移 | 字段 |
| --- | --- |
| 0 | view_projection，64 字节 |
| 64 | camera_position，float4 |
| 80 | draw_references_address，uint64 |
| 88 | instances_address，uint64 |
| 96、100 | viewport_width、viewport_height，float |
| 104、108 | visible_count、flags，uint |

所有 stride/offset 用 static_assert、SPIR-V reflection 和 GPU 字节模式探针验证。112 字节不占用现有材质 push block 或 texture_group token。GPU 投影使用从真实相机投影矩阵推导的像素空间 Jacobian；CPU 参考与 shader 同源约定，包含 Vulkan viewport 的 Y 方向。

每百万唯一粒子的基础 GPU 属性约 240 MB（十进制），每份百万绘制引用约 8 MB；实例表、上传暂存、CPU 数据和未来排序 scratch 另计。UI 必须报告实际总量。

## 5. CPU 基准及普通绘制

每帧按固定顺序完成：

1. 收集实例，校验总粒子数与 uint32 draw 上限，更新当前 frame slot 的实例表。
2. 为粒子中心计算世界位置和沿相机 forward 的深度。第一版仅剔除整个支持域明确位于视锥外的粒子；不能只测中心而切掉屏幕边缘的大椭圆。
3. 全部实例共同按深度从远到近排序，相同深度按 stable_instance_id 和 local index 排序；拒绝 NaN 深度。
4. 将引用上传当前 slot 的 coherent buffer；不使用上一次相机的异步排序结果。
5. 调用 `draw(6, visible_count, 0, 0)`，vertex shader 通过 SV_VertexID 构造两个三角形，通过 SV_InstanceID 查绘制引用。

vertex shader 读取属性，计算投影中心、二维协方差、椭圆基向量和 SH 颜色。SH 方向在资产局部空间取 camera 指向 center，即 `normalize(center - inverse_model * camera)`，与 NVIDIA 参考的方向一致；仍须用小样本验证具体基函数符号。颜色按参考约定加 0.5 并仅钳制负值，不提前将 HDR 值限制到 1。[方向依据](https://raw.githubusercontent.com/nvpro-samples/vk_gaussian_splatting/main/shaders/threedgs_raster.vert.slang)、[SH 参考](https://raw.githubusercontent.com/graphdeco-inria/gaussian-splatting/main/utils/sh_utils.py)

二维协方差加入 0.3 pixel² 对角正则项作为基础 3DGS 参数；它不能被称为完整 Mip-Splatting。第一版使用固定 3σ 支持域：fragment 令椭圆规范坐标 r 满足 `r² <= 9`，`alpha = min(0.99, opacity * exp(-r²/2))`，alpha 小于 1/255 丢弃。参考图必须记录同样的核截断、正则项和 alpha 阈值；阈值差异不是加载错误。

近裁面策略明确采用保守省略：支持域与近裁面相交的粒子不绘制并计入统计。第一版不承诺相机穿入高斯时连续无缺口；不允许以无限 quad 或 NaN 作为替代。后续改善近裁面投影时独立验收。

首版不预计算整个投影 buffer，以减少一个 compute pass 和同步环节。对六个顶点重复计算的成本单独测量；如果占比高，再加入 compute 投影或 mesh 路线。

## 6. 混合与现有帧流程

fragment 输出非预乘 `float4(color, alpha)`，复用现有 `blend_mode::alpha`。当前 Vulkan 实现的 RGB 公式为 `src.rgb * src.a + dst.rgb * (1-src.a)`，alpha 为 `src.a + dst.a * (1-src.a)`；不先把 color 乘 alpha。关闭背面剔除，启用深度测试，关闭深度写入。

基础查看模式关闭 TAA、景深和依赖表面深度的效果，保留现有曝光、tone mapping 以及可用的空间抗锯齿。所有颜色在 HDR 阶段合成，只经过一次现有后处理；SH 外观按 radiance 输入，不额外做 sRGB 解码。需要输入色彩元数据的其他方言另行声明。

混合阶段安排为：不透明网格／卡渲及其光照 → Gaussian pass → 现有透明 mesh pass → 后处理。读取不透明 depth attachment，LOAD 现有 HDR target。pass graph 声明 color attachment 的读改写依赖，以及 depth 从采样回到 attachment 后再恢复的转换，参考现有 transparent_pass 的资源交接。

该固定顺序只保证高斯与不透明网格的近似遮挡。透明网格与高斯无法这样获得统一深度顺序；含这类场景给出明确的功能限制，验收不将其算作正确混合。将来若支持，需要统一透明合成策略。

高斯出现时先明确禁用现有 TAA，并清除 history_valid；退出高斯模式也重新初始化历史。原 TAA 的表面 velocity/depth 与高斯不匹配，不能让它默认累积。后续可以引入独立高斯时域合成或覆盖率／反应性数据，另作设计。

## 7. 生命周期和失败行为

静态 GPU asset 与对应设备／backend DLL 使用现有根寿命约束。逐帧 instances、references 以 frame slot 为单位分配，只在该 slot 已完成后重写；不能以 swapchain image index 替代完成点。

帧快照保留它引用的全部 GPU asset。slot 完成且录制引用丢弃后释放；用户卸载资产不立即销毁正在被 GPU 使用的 buffer。当前 command buffer 的 texture-group 保活不自动扩展为任意 BDA buffer 保活，Gaussian owner 必须显式保留这些资产。

重新分配缓冲区只在安全 slot 上发布新地址，旧缓冲区由旧快照保留。首版正常绘制不调用 wait_idle 保活。resize 只影响帧参数及目标，静态属性不用重新上传。

加载失败保持原场景；GPU asset 创建失败不发布半成品。帧排序／上传失败时，该帧不录制 Gaussian draw，给出诊断并保持网格渲染；计数为零时完全跳过 pass。无高斯的旧场景不改变管线、TAA 或资源创建。

## 8. RHI 的最小演进

代码核对基于当前 master。基础 CPU 版可复用 buffer/device_address、draw、pipeline_desc、alpha blend、rendering scope 和 barrier，不新加高斯专用 RHI 类型。

GPU 阶段已确认的缺口：

- buffer_flag::indirect 已存在，但 command_buffer 只有 mesh indirect，缺普通 draw indirect。增加通用 `draw_indirect` 和对应 16 字节命令结构；用六顶点非索引 quad 不必同时新增 indexed indirect。
- buffer_use 缺少 indirect reader。追加 indirect_read，Vulkan 映射 DRAW_INDIRECT / INDIRECT_COMMAND_READ；不能把 shader_read 当作 indirect 参数同步。
- GPU radix passes 之间使用现有 shader_write → shader_read 同步；现有 Vulkan shader_read 覆盖 vertex/fragment/compute。新增 mesh 消费时再核对 mesh stage 覆盖。

第一轮 GPU 基线可按总粒子数排序，culled 条目使用 sentinel key，直接 draw 总数量并令无效实例退化；再加入 GPU 可见计数与 indirect draw，避免为了第一条 GPU 排序路径一次扩展所有命令。

shader 的资源入口集中在 Gaussian 共用访问层，当前 Vulkan 映射到 BDA，其他 API 将来映射它自己的资源访问。统一的是 RHI 和场景逻辑，不要求 Vulkan 为旧描述符模型提供回退。

## 9. 验证与验收

CPU：两种 PLY 编码、属性乱序、SH 阶数、重复／缺失属性、大小溢出、截断、非有限值、四元数、协方差及实例变换。排序检查正反方向、多个实例、相同深度与零粒子。

GPU：单粒子覆盖范围、各向异性旋转、SH 方向、两粒子重叠、不透明网格前后遮挡，以及 slot 复用／卸载保活。重叠例子固定为黑底、远蓝 alpha0.5、近红 alpha0.5，中心预期线性 RGB 为 (0.5, 0, 0.25)，用 float HDR readback 的明确容差验收。

真实资产首选 Bouquet of Flowers，记录固定相机、参考查看器设置、核参数和资产来源。现有 14 个 frozen 场景无高斯时全部保持原结果。新特性至少增加纯高斯、多实例和不透明网格混合场景。

性能记录总粒子数、可见数、近裁面省略数、分辨率、CPU 排序、上传、GPU 绘制和显存；GPU 阶段再加入键生成、排序与 scratch 统计。分别给出静止／移动相机的数据，不以异步滞后排序提高的 FPS 混淆画面质量。

首版完成的定义：真实 PLY 可查看，实例变换和相机运动有效，小样本符合数学预期；加载错误、卸载和 resize 安全，旧场景不回归。GPU 排序、任意透明 mesh 交叠、TAA、光追和重光照不作为基础查看器已完成的隐含承诺。
