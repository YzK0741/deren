# 聚簇光照资源迁移记录 — 2026-10-13

状态：本阶段完成，正常运行路径已验证。改动仍在当前工作区，未提交、未推送。
这是已完成的 passchain 资源管理基础之后的第二组迁移；没有修改 RHI ABI 32 或 heap 槽位。

## 资源迁移

- cluster_pass 声明计数和索引两个 coherent storage buffer，每帧槽各一份。
  仍按 max_cluster_count=12288、cluster_light_capacity=32 分配：计数 49152 B，索引 1572864 B。
  调整窗口大小仅改变活动 grid，保持现有最大容量及 shader stride。
- 两个缓冲区均由 passchain registry 创建并持有 generation；pass 持有共享快照。
  runtime 的原 owning vector 改为借用句柄，保留原 heap 发布和资源表解析。
- 新增 mapped buffer 的 zero_on_create 策略，只在创建新 generation 时初始化。
  计数请求 zero_on_frame；索引帧间不重置，由 compute 写入实际使用的行。
- begin_frame 支持 preserved resource-id 列表。runtime 在 slot 等待完成、更新灯光事实后调用；
  没有活动 cluster dispatch 时跳过计数清零，保留之前每帧约 49 KB 写入的功能开关。
- 兼容资源链统一补齐缺少的 shadow/cluster 生产者，使用稳定的 chain-owned provider 对象。
  应用链与兼容链独立收集、初始化，并分别保存 submitted snapshots。
  切换 provider 时按原有 wait_idle 规则更新借用桥和 heap 地址。
- admission 检查完整 pair、帧槽 scope、大小、device address、storage_coherent 和初始化策略。
  不完整或不兼容的 pair 在 acquire 前拒绝，保持此前 heap 桥。
- cluster pass 的 hand-back buffer barriers 使用分发快照里的对应帧槽资源。

## 实际运行暴露并修复的旧问题

### Cluster dispatch 未推 heap 索引

新增 32 盏 demo light 的捕获，在冻结的迁移前版本中反复触发
`VUID-vkCmdDispatch-None-11376`：compute shader 使用 push [0,8)，但此前没有 push-data 调用。
没有灯光的原有捕获不会执行该 dispatch，因此此前没有覆盖这个问题。

light_cluster.slang 已定义 frame_slot/image_index；旧 cluster_io 和 record 却仍假定没有 push。
现在声明 8 B compute push，并在 dispatch 前通过现有 endpoint 推送空的 pass-specific payload，
由 host 附加 heap 索引。失败时不执行 dispatch。host 的空 payload 路径避免 memcpy(nullptr,0)。
现有 endpoint 还附加一个其他 stage 使用的第三 lane，不影响 shader 读取的前 8 B。

### 共享 shading 把浮点灯光数量按 uint 读取

CPU set_point_lights 写 float(count)，UBO 以 glm::vec4 保存；cluster shader 的 prefix 也读 float4。
但 shading.slang 原来把该 lane 定义成 uint：32 的浮点位模式会读成 0x42000000。

现已改为 float，直接遍历路径把数值转换成 int 并限定在 0..MAX_PUNCTUAL_LIGHTS。
字段仍占 4 B，所有偏移和 host 布局未变；MegaLights 使用同一 bounded helper。
该修复使 32 灯下聚簇与直接遍历两条路径的捕获逐像素一致。

## 验证

- Release 全量构建和 shader 编译成功。
- 最终 CTest：24/24，21.61 秒。
- 新 cluster portable 子测试：58 检查通过。
  覆盖非零填充的模拟分配被初始化、复用不重置索引、只清当前槽、显式跳过清零、
  unmapped 请求失败、应用/兼容生产者共存和切换，以及不完整/错误 usage pair 在 acquire 前拒绝。
- resource schema：600 检查通过。push 声明回归先观察到 599 检查中 1 项失败，再修复通过。
- shader source wire-type 回归：1588 检查通过；修复前有 1 项失败。
- 原 managed-resources / managed-frames 子测试继续通过，各 46 检查。
- 真实 Vulkan test_runtime_dyn：1164 检查通过。
- 最终 PBR 捕获：1280×720，40 帧，0.5 度/帧 sweep。
  deferred、shadow_single、transparent_blend、sponza 与冻结迁移前版本的 RGBA 像素全部一致。
- 最终 32 盏灯聚簇 on/off 两条路径的 RGBA 像素完全一致；六个最终场景全部无验证错误。
  旧 32 灯 cluster-on 捕获带已确认的 push 错误，作为问题证据保留，不能称为 clean baseline。
  旧 32 灯 brute-force 捕获使用错误灯光数量解码，也不能要求保留其错误输出。
- Gaussian CPU/GPU 各 12 帧成功且验证干净；两路径差异保持 44 pixels、最大 5/255，
  HDR RMSE 2.5238e-5。这是路径对比，不是两版本逐像素对比。
- 独立审查指出 custom provider 缺少 storage/coherence 限制，已加入 storage_coherent 检查
  和错误 usage 回归。复审资源生命周期、push 和 wire-type 修复，没有剩余重要发现。

## 证据与续接

- 当前文档：docs/managed_resources.md、docs/render_resource_inventory.md，已纳入手册。
- 持久捕获：docs/reviews/2026-10-13-cluster-managed-resources/。
  包括四组常规前后捕获、32 灯诊断及最终 on/off 捕获、配置、日志、脚本、pixels-final.json。
  清单包含文件、exe、backend DLL 和 shader 的 SHA256；二进制 hash 是归档时记录。
- 工作证据：build-release-dyn-clang64/cluster-resources-check/。
- frozen runtime.before 是本轮迁移前的可运行包，包含已完成的第一轮 shadow migration。
- 保留同工作区此前的离屏、阴影及第一阶段资源改动；不要按 git diff 全部重置。

下一步可迁移 light/head 等每帧常量缓冲；其他 pass、AS/imported 资源、自动 descriptor 发布
和自动屏障仍走原路径。本阶段不需要进一步补代码才能正常运行。
