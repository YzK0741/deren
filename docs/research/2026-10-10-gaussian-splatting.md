# 高斯泼溅资料调研

日期：2026-10-10。本文整理官方论文、项目和源码入口，并对照 deren 的已有计划提出选择依据。尚未下载、编译外部示例或实现高斯渲染。

## 最值得参考的 NVIDIA 项目

用户记忆中的项目是 [nvpro-samples/vk_gaussian_splatting](https://github.com/nvpro-samples/vk_gaussian_splatting)。它是 Vulkan 查看器和渲染技术试验平台，适合作为 deren 的首要实现参考。

其[光栅技术说明](https://nvpro-samples.github.io/vk_gaussian_splatting/deep-dives/rasterization_of_3d_gaussian_splatting/)给出了普通顶点与 mesh shader 路径，以及 GPU radix sort、异步 CPU 排序。基本过程为上传属性、生成可见列表与深度键、排序、按顺序绘制高斯覆盖区域。CPU 异步排序可能使用滞后的视角结果；画面稳定性与吞吐需要分别测量。该示例的全局排序与原论文的分块计算路径有区别。

建议按以下入口阅读源码，而非先通读整个应用框架：

| 源码入口 | 阅读目的 |
| --- | --- |
| [src/gaussian_splatting.cpp](https://github.com/nvpro-samples/vk_gaussian_splatting/blob/main/src/gaussian_splatting.cpp) | 帧内处理、排序调度和资源同步 |
| [shaders/dist.comp.slang](https://github.com/nvpro-samples/vk_gaussian_splatting/blob/main/shaders/dist.comp.slang) | 深度键、可见列表与间接参数 |
| [src/splat_sorter_async.cpp](https://github.com/nvpro-samples/vk_gaussian_splatting/blob/main/src/splat_sorter_async.cpp) | CPU 排序与结果交换 |
| [shaders/threedgs_raster.vert.slang](https://github.com/nvpro-samples/vk_gaussian_splatting/blob/main/shaders/threedgs_raster.vert.slang) | 协方差投影、屏幕椭圆和球谐颜色 |
| [shaders/threedgs_raster.frag.slang](https://github.com/nvpro-samples/vk_gaussian_splatting/blob/main/shaders/threedgs_raster.frag.slang) | 高斯覆盖率及输出 |
| [shaders/threedgs_raster.mesh.slang](https://github.com/nvpro-samples/vk_gaussian_splatting/blob/main/shaders/threedgs_raster.mesh.slang) | 按高斯生成 quad，研究减少重复计算 |

顶点源码已使用 Slang，但也使用 `vk::binding` 的普通资源绑定；矩阵计算采用向量乘矩阵形式。移入 deren 时需要重新映射资源、矩阵约定和 push 布局，不能因语言相同就直接复制。源码头同时保留 NVIDIA Apache-2.0 与所引用实现的 MIT 声明。[源码依据](https://raw.githubusercontent.com/nvpro-samples/vk_gaussian_splatting/main/shaders/threedgs_raster.vert.slang)

[Getting Started](https://github.com/nvpro-samples/vk_gaussian_splatting/blob/main/docs/getting-started.md)列出 Windows/Linux、Vulkan 1.4 SDK、C++20、CMake 3.22，以及 nvpro_core2。CUDA 用于可选的 NVML 监控，不是这里基础光栅渲染的必需依赖。文档提供预编译版本、PLY/SPZ 加载和关闭默认资产下载的构建选项。具体硬件能力仍需按所选路径检查。

## 论文与其他实现

| 资料 | 用途及优先级 |
| --- | --- |
| [3DGS 原论文与项目，SIGGRAPH 2023](https://repo-sam.inria.fr/fungraph/3d-gaussian-splatting/) | 必读：各向异性高斯、优化后的场景表示、可见性相关渲染。作为数学及资产格式来源 |
| [gsplat](https://github.com/nerfstudio-project/gsplat) | CUDA/Python 实现，适合参考数学、测试及离线训练工具；其 CUDA 运行时不直接进入 deren |
| [Mip-Splatting](https://github.com/autonomousvision/mip-splatting) | 研究尺度变化与抗锯齿。已有资产的训练与查看参数要匹配，不能简单把它理解为开启普通 MSAA |
| [nv-tlabs/3dgrut](https://github.com/nv-tlabs/3dgrut) | NVIDIA 的 3DGRT/3DGUT 官方实现；不是 NVlabs/3dgrut。研究光追、畸变相机与混合路线 |
| [HiGS，NVIDIA 2026](https://research.nvidia.com/labs/sil/projects/higs/) | 性能进阶：把宏分块排序与细分块光栅分离。后续大场景优化时阅读，先不承担完整移植 |
| [Vulkan Radix Sort / VRDX](https://github.com/jaesung-cs/vulkan_radix_sort) | NVIDIA 查看器使用的排序库；可作为 GPU 排序参考，但它的 Vulkan 接口不能直接放进 engine |
| [SPZ](https://github.com/nianticlabs/spz) | 后续压缩资产格式，首版 PLY 验证完成后再考虑 |

3DGRT 通过光追处理高斯粒子；3DGUT 用 Unscented Transform 支持复杂投影，相互匹配的表示可让主视线光栅化、次级视线光追。它们解决的问题比普通 3DGS 查看器更多，也带来不同训练和渲染约定。[官方说明](https://github.com/nv-tlabs/3dgrut)

HiGS 强调分块、排序、局部复用和负载均衡的共同设计，其性能数字来自指定 GPU 与场景，不能直接当作 deren 的目标帧率。[论文项目页](https://research.nvidia.com/labs/sil/projects/higs/)

## 光追与网格混合需要注意的区别

NVIDIA 的 [Vulkan 光追说明](https://github.com/nvpro-samples/vk_gaussian_splatting/blob/main/docs/deep-dives/ray_tracing_3d_gaussians.md)使用二十面体或 AABB 作为粒子代理，通过 any-hit 收集并排序一批命中，再按需继续追踪。any-hit 回调顺序不应当作深度顺序。网格先确定可见距离，再限制粒子追踪范围，是值得参考的混合策略。AABB 的大量重叠会使结构质量与遍历成本变得关键。

[混合路线说明](https://nvpro-samples.github.io/vk_gaussian_splatting/deep-dives/hybrid_rendering_3d_gaussians/)把主视线光栅化和次级视线光追组合起来；这是 deren 后续反射、折射和阴影的研究方向。

[光照与阴影说明](https://github.com/nvpro-samples/vk_gaussian_splatting/blob/main/docs/deep-dives/lighting_and_shadows.md)明确区分已有采集光照与真正的重光照，并讨论从累计透明度提取近似表面深度。球谐外观不能直接当作 PBR 反照率；多个透明层的平均深度也不能作为真实表面深度。

对 deren 的推论：第一版先验证与不透明网格的深度遮挡和颜色合成。透明网格、高斯、多层交叠、阴影和时域重建应分别验收，不能用一张近似深度图宣称全部问题已经解决。

## 测试资产

优先考虑 NVIDIA 的 Bouquet of Flowers；其[数据说明](https://github.com/nvpro-samples/vk_gaussian_splatting/blob/main/docs/datasets.md)提供下载入口和 CC BY-SA 4.0 标注。大型 City、Winter Garden 暂作压力测试候选，不作为第一张验证图。

原论文的预训练模型也是参照来源。应读取 `point_cloud` 子目录中的训练输出，`input.ply` 是原始 SfM 点云。资产记录要包含来源、训练方法、SH 阶数、抗锯齿选项、核函数、查看器设置与固定相机；相同 PLY 扩展名不能保证相同渲染语义。[数据与设置说明](https://raw.githubusercontent.com/nvpro-samples/vk_gaussian_splatting/main/docs/datasets.md)

另外自行生成单高斯、两个半透明高斯、退化尺度及近裁面小样本，用于确定性的数学与 GPU 验证。

如后续复制原版 INRIA 实现代码，应单独核对其研究／非商业许可；论文公式参考与代码引入是不同的工作。不要把示例仓库的许可默认延伸到所有依赖和数据。[原版许可](https://github.com/graphdeco-inria/gaussian-splatting/blob/main/LICENSE.md)

## 对 deren 的建议

这部分是基于上述资料与本地代码的工程判断，不是外部项目的承诺。

保留[现有实施计划](../superpowers/plans/2026-10-09-gaussian-splatting.md)的顺序：限定 PLY 方言、CPU 数学和排序基准、普通实例化光栅、GPU 筛选与排序、最后与网格合成。mesh shader、HiGS 和 3DGRT 按实测瓶颈与画面需求另立阶段。

高斯位置、协方差、透明度、SH、排序索引是缓冲区数据。texture_group 的 16 个槽位不应当用于表达整个高斯场景；仅在确有纹理输入的管线中使用它。绑定仍服从统一 RHI，具体 heap 模型留在 Vulkan 后端。

本地 RHI 已有实例化 draw、计算 dispatch 和 blend_mode 描述；开工前还应逐项核对排序所需的读写缓冲区、间接命令及屏障，尤其是目标的透明度混合公式。现在只确认了资料入口和现有接口，尚未验证完整高斯 pass 所需的所有能力。

首轮评估记录：分辨率、总高斯数、可见数、数据及临时缓冲显存、排序与绘制耗时。除参考查看器截图外，重叠小样本必须给出可解释的颜色结果。
