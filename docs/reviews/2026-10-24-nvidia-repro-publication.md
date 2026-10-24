# NVIDIA 复现材料发布记录

用户授权建立独立公开仓库 `YzK0741/vulkan-descriptor-heap-repro`，用于自行向 NVIDIA 反馈。未代为向 NVIDIA 提交反馈。

仓库：https://github.com/YzK0741/vulkan-descriptor-heap-repro

源码提交：`31bc23fd240256efa1242997d8d095320d369a37`。基于 deren 本地 `a777f7d5b80b666c2eddbaff7244161eacd44701` 的源代码快照，不依赖这个未公开的上游 ref。快照只增加 GUI heap 查询及 `DEREN_REPRO_WORKAROUND` 开关，没有移入 logo/启动窗口等当前工作区修改。原项目没有提交或推送。

## 复现范围

大型 Flowers 资产已缩成自行生成的单点 ASCII PLY。渲染器仍有正常的占位几何及其他 pass，因此这是**可稳定复现的应用级缩减案例，尚不是独立 Vulkan 最小程序，也未证明驱动责任**。独立小程序尝试了 heap/legacy descriptor、真实 ImGui、多帧、WSI、Slang heap graphics/compute、独立 command pool 等组合，仍未触发；这一限制在公开 README 与 NVIDIA_REPORT 中置顶说明。

## 验证

从源码快照另建干净构建目录，以 Clang 23.1.3、Release、`VR_NATIVE_ARCH=OFF`、`VR_DIAGNOSTICS=OFF`、14 jobs 完成构建。Vulkan SDK 1.4.357.0，Slang 2026.13.1，RTX 4060 Laptop GPU，NVIDIA 617.42，设备 Vulkan 1.4.351。

| 路径 | control GUI 亮像素 | workaround GUI 亮像素 | 验证错误 |
|---|---:|---:|---:|
| GPU，8 帧 | 0 | 1843 | 两者均 0 |
| CPU，8 帧 | 0 | 1854 | 两者均 0 |

独立二进制包在移除 MSYS2 PATH 后仍复现：0 / 1876，均退出 0，验证层明确启用且无验证错误。文本包含动态帧率，因此完整图像与亮像素数量可有少量变化。缺少验证层的负例被脚本明确拒绝，不会当成零错误。

## 分发

Windows 包：`vulkan-descriptor-heap-repro-windows-x64.zip`，约 3.82 MiB，98 个文件。

SHA-256：`64a826fd2292eb1908c04c08d9860e8a76cb1568a7194c5fc4e22bd0ecad4a10`。

显式文件清单排除二进制目录的运行日志、Vulkan loader 和显卡驱动；只分发应用 DLL、libc++、SPIR-V、运行脚本、场景、已脱敏证据与许可证。独立审查检查了所有 97 个负载文件的 manifest 哈希、许可证和 ZIP 日志隐私，未留下问题。

工作区临时 GUI 诊断开关已撤销，GUI 插件重新构建，源文件恢复到诊断前相同 SHA-256。可复现程序中的开关仅留在独立仓库源码快照中。
