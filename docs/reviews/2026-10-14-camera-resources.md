# 共享相机常量资源迁移 — 2026-10-14

相机常量缓冲已从runtime独立所有权迁移至passchain，与light/head共用scene_constants_pass。
本轮不改shader、heap槽位、RHI ABI32、相机计算或同步时机；修改尚未提交。

## 实现

- 生产pass描述camera_ubo，272字节，frame_slot生命周期、per_frame_slot scope；uniform_coherent分配，storage和device_address flags，创建时初始化，不逐帧清零。
- select_resources允许独立选择camera/light/head。兼容链只补齐缺失类型，应用链提供同一类型时不重复创建。
- 构造阶段的output-only准备也包含camera，以满足后续场景设置时的heap发布；shadow、cluster、light/head保留延迟创建。
- runtime只保留borrowed camera handles与generation snapshot。替换沿用idle drain，所有描述先校验，再初始化、采用并发布。提交时仍持有应用/兼容两套snapshot，成功帧槽等待后才释放该槽的旧资源。
- 保留current_ubo、TAA jitter、每输出图像previous matrix及投影计算。上传从裸mapped memcpy改为既有write_frame_buffer，检查id、scope、slot、容量和coherent mapping。
- 禁止camera请求逐帧清零，避免begin_frame在相机上传后擦掉数据。错误描述和创建失败在acquire前返回失败，原heap绑定保留。

## 验证

- 行为回归在迁移前因camera不归生产pass所有而RED：2项检查中1项失败。迁移后GREEN55/55。
- 覆盖heap绑定与所有权一致、各帧槽写入隔离、generation复用、application→fallback切换、提交快照退役、过小buffer、zero_on_frame、错误scope、创建失败后重试及只声明camera。
- 原scene_constants回归60/60通过；完整Release构建成功；CTest24/24，19.77秒；真实Vulkan runtime1164/1164。
- 相对于97883342冻结运行包，13场景各40帧、每帧环绕0.5度，RGBA像素全部一致，同步验证干净。包含shadow、透明、Sponza、cluster开关、toon、TAA/FXAA/缩小比例、RT及32灯ML+RT。
- 缩小比例ML+RT+TAA最大化再恢复，两次resize通过；恢复输出1280×720与client一致，无同步错误。
- 独立只读审查未发现重要或严重缺陷，覆盖构造可用性、provider切换、admission、等待/heap替换、帧槽写入及销毁顺序；源代码diff检查通过。

## 证据与剩余范围

归档：[2026-10-14-camera-resources](2026-10-14-camera-resources/)，含配置、原始日志、前后PNG、像素统计、脚本、CTest及SHA256。
PNG仅无损重压缩IDAT，解压扫描行相同；shader/运行包摘要标识实际比较版本。
工作目录为build-release-dyn-clang64/camera-resources-check，runtime.before包含先前MegaLights历史修复。

下一批可迁移共享motion、skin/previous-skin和morph数据。它们存在setup写入、部分范围更新及历史CPU源，不能套用camera的每帧完整覆盖方式。
Gaussian地址缓冲/排序scratch、光追结构及其构建输入、IBL/furnace/材质共享纹理和samplers仍保留原有所有权。此批没有新增资源类型或自动barrier规则。
