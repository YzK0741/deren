# HZB M3 B2 验证记录

日期：2026-10-21。基线 `c2cf2945`，实施计划：`docs/superpowers/plans/2026-10-20-hzb-b2.md`。

本阶段让 visibility raster 和 resolve 真正消费 GPU compact 后的 meshlet，并用 GPU 更新既有 indirect command buffer。仍由 `DEREN_HZB_BUILD` + `DEREN_HZB_CULL` 显式开启，仅接受字节一致的静态历史和完整 host-cut run；共享表等其他来源保留原路径。未增加 RHI 接口、渲染 pass 或默认开启选项。

## 实现与回退

- row `meshlet_table=2` 和 push `base_vertex` bit1 同时发布，raster/resolve 使用同一 survivor 次序。
- 成功发布的 item 写完整 `{groups_x=survivors,1,1}`，包括零 survivor。无效 run 写 `UINT32_MAX` count，保留 host command；两个消费者均回到原 host-cut records。
- 图声明 cull 的 commands compute read/write，以及 survivors/results 的 mesh/compute reads；间接参数在 draw-indirect scope 消费。跨 pass 同步由图负责。workgroup 内读取 global flags 前使用 device-memory barrier。
- 既有 indirect buffer 同时申请 indirect 和 device_address，发布给 HZB 的 descriptor 是同一缓冲的别名。
- probe v2 显式发布 commands/results 的 shader_write→host_read，等待设备完成后导出既有 coherent indirect lane，并检查每个实际命令。item 第四字记录原 host dispatch count；旧版 probe 文件仍可验证。

## 真机正确性

RTX 4060 Laptop；Bistro geometry（4.2M triangles）；1280×720；subtilis=true；固定相机；每轮130帧；关闭 TAA、FXAA、SSAO、bloom、shadow。ON 正确性轮开启 Vulkan validation。

| 视点 | candidates → survivors | 剔除比例 | CPU/GPU 判断 | OFF/ON 像素差异 | probe |
|---|---:|---:|---|---|---|
| far `35,18,140,0,-83.071,0` | 50,808 → 14,473 | 71.51% | 50,808/50,808 | 0 pixels，max=0 | 通过，7,044 checks |
| near `35,8,42,0,-88,0` | 43,115 → 3,091 | 92.83% | 43,115/43,115 | 0 pixels，max=0 | 通过，6,978 checks |

probe 数量为移除 B2 无意义的 legacy padding 检查后的最终脚本结果。原始运行曾为7,045/6,979项，通过情况相同。flags、稳定压缩次序、per-item counts、stats 和实际命令均已验证。

移动相机（每帧yaw+0.02°）：有 history 后 static gate 仍关闭，OFF/ON changed=0,max=0。

临时仅在 shader 中强制所有非空 host-cut item 进入 overflow 分支：far/near 各130帧，分别记录374,874 / 366,231次 item overflow；两处截图均与相应 OFF 完全一致，changed=0,max=0。注入已恢复，正式 shader 已重新编译。生产代码无注入开关。

Vulkan 正确性轮无 VUID/Validation Error。驱动层枚举和 shader cache 无写权限的 INFO 属于环境信息。

## 性能复测

恢复正式 shader 后，无并发构建，OFF/ON 交错各3轮/视点；每轮130帧，取每轮129帧 GPU session avg，再跨轮取中位数。两组均 HZB_BUILD=1，仅 ON 设置 HZB_CULL=1；validation=false。原始数值见 `hzb-b2-performance.json`。

| 视点 | visibility raster OFF→ON | cull+raster+resolve OFF→ON | GPU frame OFF→ON |
|---|---|---|---|
| far | 23.42→9.20 ms（−60.72%） | 23.58→9.78 ms（−58.52%） | 24.47→10.62 ms（−56.60%） |
| near | 20.53→3.59 ms（−82.51%） | 20.89→4.16 ms（−80.09%） | 21.80→5.09 ms（−76.65%） |

GPU 阈值达到设计要求（near raster 至少−25%、GPU frame 至少−20%）。CPU post 最后60帧统计的跨轮中位数：far 4.57→4.48 ms，near 4.39→4.25 ms；未见 host 侧回退。笔记本频率/调度会影响绝对数值，这些是该设备的同会话测量。

far 三组配对图像差异均为0。near OFF1 与 ON1/ON2/ON3 均差异0；OFF2/OFF3 与 OFF1 则为同一组2901 pixels、max=10 的既有两变体抖动，和 B1 已记录的 OFF-vs-OFF 现象一致。没有把这种基线波动算作 HZB 画面误差，也没有放宽同变体的逐像素验收。

## 工程验证与限制

- Release 首轮58/58；探针同步修复后的最终完整测试仍为58/58（48.60s）。
- ASan/UBSan：`test_hzb`、`test_visibility_buffer`、`test_render_graph_visibility`、`test_subtilis_frame` 首轮4/4，探针同步修复后的最终4/4（5.15s）。
- 请求额外 ASan `test_runtime_dyn` 时，后端 DLL 链接失败：`-fno-sanitize=all` 的 DLL 链入带 sanitizer 的 `static_utility` 后出现未解析 `__asan_*` / `__ubsan_*`。本阶段没有改动 sanitizer 构建策略，不能报告该目标通过。
- 完整 clang-tidy 在 `bugprone-unchecked-optional-access` 对大型 `record_post_graph` 的数据流分析中崩溃。仅禁用该单项后对两个改动的 runtime 实现文件完成分析；现有 readability 噪音保留，未修改项目配置。聚合初始化中的空 range 被 `readability-trailing-comma` 误报；新增独立声明告警已修正。
- 独立完整审查：发现1项 Important，无 Critical/Minor。命令探针直接读 coherent buffer 缺 shader_write→host_read 发布；在既有 probe 的 host_reads barrier_group 同时发布 commands/results，修复后远/近 Vulkan validation 无 VUID/Validation Error，probe7044/6978项均通过。最终 far 对 OFF 像素差异0；near 与 OFF2/OFF3 同变体也为0（与另一 OFF 变体仍为已知2901px/max10）。一轮修复完成，不再启动第二轮审查。

范围限制：运动/TAA jitter 时关闭剔除；本阶段没有 current-frame recovery，也没有扩大 shared/import-time meshlet 来源。后续动态相机支持属于 phase C。

原始截图、日志、probe 和阶段构建记录归档到 `D:/deren-assets/hzb-b2`（captures 在其 `captures/` 子目录）；仓库仅保留本记录、性能 JSON 与实施计划。正式 shader 无临时 overflow 注入。
