# Subtilis cluster DAG：glTF 扩展与 CPU 离线转换器

日期：2026-10-21。状态：**已按用户确认的 CPU-first 路线实施；Task 1-8 完成，真实资产、Khronos core validator、cut/LOD0、独立 CPU CI 与证据报告已记录；最终 whole-change review 待进行（由 Lead 执行）**。[实施计划](../plans/2026-10-21-subtilis-gltf.md)与[验收记录](../reviews/2026-10-21-subtilis-converter.md)为权威证据。代码基线 `07e365bd`。

## 1. 目标与阶段边界

用户确定的顺序：先定义 glTF 扩展并实现实际转换器，再为 deren 本体添加消费该资产的 feature。经 CUDA 收益讨论，用户确认首版先用 CPU 与多核并行完成，CUDA 不再是必做项。目标类似 UE 的 cluster DAG，不是整物体的离散 LOD0/1/2，也不把现有唯一父 group 森林直接固化成文件格式。

本阶段独立交付：格式文档和 JSON schema、CPU 编解码/校验/cut 参考、多核 CPU DAG 构建器、命令行转换/检查/验证/cut 导出、真实资产和性能证据。不修改现有 runtime/visibility/HZB，不加入自动导入构建、GPU runtime cut、软件光栅、streaming 或分页。

首版采用 CPU 构建：使用成熟的 meshoptimizer 完成聚类、分区和约束简化，按 primitive 和同一轮独立 group 做多核并行。先通过分阶段剖析确定热点，再决定是否值得做 CUDA 实验。CUDA 不影响首版交付和验收。

格式、工具入口与今后的 deren feature 均不依赖 CUDA。不提前建立通用后端框架；将来只在实测热点处提取最小替换接口。更换构建后端不自动提高资产质量或运行帧率，相关收益必须单独证明。

## 2. 现状与技术约束

- 现有 `subtilis_asset.cppm` 用连续 child group 范围、唯一父校验和树递归 cut；不适合直接消费新 DAG。保持原模块和已有格式无关的行为，本阶段不迁移旧 runtime。
- 已有 meshoptimizer 固定 v1.3 / `9e1f07b159d3cb777f1c67ed31fc11fd117986f4`，可复用分区、聚类、带属性简化等算法；复用源代码依赖，不跨编译器链接现有 engine 产物。
- deren 当前常用 Clang64/C++23 modules。工具独立 CMake 入口，CPU 层优先复用当前工具链，不因转换器强制改用 MSVC；普通 deren 构建和首版转换器都不查找或链接 CUDA。
- 宿主工程代码沿用项目 vessel/mimalloc、无异常错误处理；为工具单独组合所需 CPU 分配适配层，不拉入 engine/Vulkan。构建不临时下载依赖。
- 可选 CUDA 实验的环境记录：本机 `D:/cuda` 的 nvcc 13.4.92、`D:/vs` 的 MSVC tools 14.51.36231，Toolkit 自带 CUB 3.4.3。仅查询过版本，未作为首版前置验收；若启动实验再做编译/设备 smoke test，并确定与 CPU 工具之间最小 POD 接口和构建方式。没有确定收益前，不引入 CUDA 库、MSVC/nvcc 构建组合或跨编译器 ABI 边界。
- 保留用户未跟踪文件；继续遵守 5h 剩余 <=5% 时记录并暂停、构建最多并行12线程、重证据归档D盘的约定。

## 3. DAG 的准确语义

### 3.1 一次 reduction 是多对多替换

每个 reduction group `g` 有细 cluster 集合 `F(g)` 和生成的粗 cluster 集合 `C(g)`。构建器把整个 `F(g)` 合并、约束简化后重新聚类为 `C(g)`；它们是同一区域的两种表示。

同一次生成的多个粗 cluster 可以进入下一轮的不同 reduction group。例如：

```text
G0: {a,b} -> {u0,u1}       G1: {c,d} -> {v0,v1}
G2: {u0,v0} -> {w0}       G3: {u1,v1} -> {w1}
```

G0/G1 的输出被跨组重新分区，G2/G3 都依赖下层组。这是首版必须真正生成、保存并消费的 DAG 见证，不能只用树形数据通过测试。

每个 cluster 保存：

- `producerGroup`：生成它的 reduction；LOD0 使用 `NONE=0xffffffff`。
- `consumerGroup`：随后消费它的 reduction；终止 cluster 使用 NONE。
- 独立的几何范围与几何包围球。

一个 cluster 最多有一个 producer 和一个 consumer，因为每一轮对 frontier 做分区；但一个 reduction 生成的多个 cluster 可以被多个后续组消费。因此**组依赖是 DAG**，隐式的粗细 cluster 替换也是多对多；无需把每一对粗细 cluster 展开成边。

group 与 cluster 都用稳定的本资产内 ID。group 按细到粗拓扑排序：若 `g` 的输入由 `p` 生成，必须 `p < g`。引用列表显式保存，不要求 cluster/group 在内存中形成连续子树。允许多个根，断开的组件或无法继续简化的区域不必强制并成一个根。

### 3.2 误差与 LOD 包围体

每个 group 有共同的 `lodSphere` 和非负有限的 `error`；该组所有输出共享同一组 LOD 判据。`lodSphere` 包含输入、输出几何，以及输入 cluster 的 producer group 的 LOD 球；group error 不小于任何依赖组的 error。

新误差以依赖的最大累计误差加本轮简化误差为基础，并做单调传播。定义为模型空间的 **LOD 选择度量**；带属性的 meshoptimizer 误差不能宣传成严格 Hausdorff 距离或逐像素保证。法线/UV 权重的长度尺度在 primitive 层固定，不能每层重新按包围体缩放。误差不存成相机距离或固定屏幕阈值。

几何包围球用于以后视锥/HZB；LOD 球用于选层，两者不能相互代替。浮点球写盘时向外取整，CPU full validator 重算包含性；首版以 CPU 高精度参考验证取整；未来 GPU 实验同样不得把可能向内舍入的结果直接出包。

### 3.3 独立 CPU cut 参考

对一次给定视图，**每个 group 只计算一次**投影误差 `E(g)`，定义 `coarsen(g) = E(g) <= threshold`。根外的虚拟 consumer 永不 coarsen，叶外的虚拟 producer 总视为已 coarsen。

选择 cluster `c` 的条件为：

```text
(producer(c) == NONE || coarsen(producer(c)))
    &&
(consumer(c) == NONE || !coarsen(consumer(c)))
```

前提是 coarsen 集合沿依赖向下闭合：`coarsen(g)` 为真时，其依赖组也必须为真。嵌套 LOD 球与单调 error 为支持的投影视图提供该性质；参考实现还必须显式验证/保守关闭违反闭合性的粗化决定，不能假定浮点计算永远保持严格关系。

先计算、修正完整 group 决定，再扫描 cluster 输出稳定 ID 顺序的 cut；不在扫描途中改变 group 决定。基本参考先不混入视锥/HZB，以便验证整个物体的替换覆盖。屏幕误差模式只接受已验证的标准透视/正交投影；近面穿越禁止相关粗化，异常视图返回 LOD0。

模型空间阈值模式直接用 group error，供命令行 cut 导出。相等阈值统一使用 `<=`，不存在的 producer/consumer 按上述规则处理。预算不足时整体返回 LOD0/明确错误，不输出被截断的 cut。不能沿用旧树的 worst-cut 求和公式作为 DAG 预算证明。

### 3.4 边界与拓扑

每次简化锁住当前组与组外几何之间的边界，并保留属性缝约束；下轮重新分组后，原内部边界可重新成为可简化区域。位置等价信息用于发现几何邻接，不等于焊接实际 UV/法线顶点。多组件、UV 缝、硬法线、非流形与重复面必须有专门用例。

重新分组允许拆分上一组的粗 cluster 集合，但不能拆开单个 cluster。每次接受前重算细/粗表示的有向外边界多重集，按锁定的位置/属性缝身份比较；边界不同则拒绝这次reduction，保留原表示。非流形或无法可靠识别边界的区域不做猜测性简化。无进展的 group 不生成同义空 reduction；原 cluster 保留身份、继续参与后续可行分组或作为根。每次接受的 reduction 必须有三角形减量，限制轮数，避免无进展循环。

## 4. 首版几何 profile

采用 **`shared-vertices` profile**：cluster 索引引用所属 glTF primitive 的原顶点序号，简化不移动或新增顶点。原顶点属性和标准 LOD0 indices 不改写。

这是首版简化算法的明确约束，不是永久限制；以后引入移动顶点或 cluster-local 存储时新增 profile/major version，不偷偷更改本 profile 的索引含义。

支持静态、OPAQUE、TRIANGLES primitive。node 可以做普通刚体/缩放变换，数据在局部空间构建，多个实例共享资产。被任一 skin 实例使用的 mesh、含 morph target、MASK/BLEND、涉及尚未支持的几何解压/属性语义的 primitive 首版保留原始数据并报告跳过。

首版构建属性为 POSITION、NORMAL、TEXCOORD_0：POSITION/NORMAL 必需，缺 UV 且材质不需要 UV 时可补0；TANGENT及其他属性先保留在原文件，但含这些未建模简化约束的目标默认不转换，不能宣称其质量已覆盖。量化/稀疏 accessor 在支持的解码范围内统一成规范化值，不修改原 accessor。无索引输入按顶点顺序生成源三角形序列。

LOD0 先用成熟聚类函数生成空间连贯 cluster，三角形允许重新排序，但必须保持源三角形多重集和绕序。首个消费者 profile 每 cluster 最多85三角形、最多256唯一顶点，适配现有 mesh stage。磁盘 cluster 总数和 root 数不受当前每项4096个可见 meshlet 的限制；读取端在未来 feature 阶段检查自身容量。

## 5. glTF 外层约定

扩展暂称 `DEREN_subtilis`，挂在 `mesh.primitives[i].extensions`，不挂在 node。DEREN 是项目私有草案前缀；公开发布前办理前缀登记，不使用 KHR/EXT 命名。

```json
{
  "extensionsUsed": ["DEREN_subtilis"],
  "meshes": [{
    "primitives": [{
      "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
      "indices": 3,
      "extensions": {
        "DEREN_subtilis": {
          "version": 1,
          "profile": "shared-vertices",
          "bufferView": 23
        }
      }
    }]
  }]
}
```

示意中的 bufferView/accessor ID 必须在真实文件中定义。原 primitive 完整保留，扩展不加入 `extensionsRequired`；不理解它的查看器仍渲染普通 LOD0。

版本1只接受列出的 profile。未知版本/可选扩展失效不影响 core LOD0，但必须报告原因；若输入显式把不支持的扩展声明为 required，则遵守 glTF 的拒绝语义，不静默降级。

扩展 payload 的 bufferView 不设 ARRAY_BUFFER/ELEMENT_ARRAY_BUFFER target。只使用 glTF buffer 定位二进制，不另造网络 URL、绝对路径或 GPU 设备地址。

## 6. 二进制载荷 v1

### 6.1 编码与边界

全部字段 little-endian，浮点为 IEEE754 binary32，ID为u32、NONE为最大u32。文件偏移/长度/count 描述为u64，运行前检查到宿主 size_t 和实际ID范围的转换。所有计数乘法、偏移相加检查溢出，先验证范围再分配。格式不使用编译器 padding，也不通过 reinterpret_cast 读取不对齐输入。

header固定128字节：magic为8字节 `DRNSDAG\0`，major/minor为u16/u16，headerBytes为u32，totalBytes为u64，sectionCount/flags为u32/u32，sourceHash/buildHash各32字节，剩余32字节为0。major=1、minor=0、flags=0。JSON version与header major必须相同。

随后每个section descriptor固定32字节：`type:u32, stride:u32, count:u64, offset:u64, length:u64`。offset相对payload开始，section起点8字节对齐；`length=count*stride`，不相互重叠、不与header/目录重叠。v1拒绝未知section type和重复type；演进时显式升级版本。padding为0，totalBytes等于bufferView长度。

### 6.2 必需分区

| type | 名称 | stride | 含义 |
|---|---|---:|---|
| 1 | INDICES | 4 | 所有cluster引用的u32顶点索引流，包含聚类后的LOD0与粗层 |
| 2 | CLUSTERS | 32 | firstIndex/indexCount/producerGroup/consumerGroup各u32，geometrySphere为4个f32 |
| 3 | GROUPS | 48 | fineFirst/fineCount/coarseFirst/coarseCount各u32，lodSphere为4个f32，error为f32，末尾12字节为0 |
| 4 | REFERENCES | 4 | group两类范围引用的cluster ID列表 |
| 5 | LEAVES | 4 | 全部LOD0 cluster ID，稳定顺序 |
| 6 | ROOTS | 4 | 全部无consumer cluster ID，稳定顺序 |
| 7 | BUILD_META | 1 | 严格UTF-8 JSON，含算法版本、参数、依赖版本、层数和无进展原因 |

空 GROUPS 合法，表示 LOD0-only 资产；仍需真实索引、leaf/root coverage，不能把空引用当成有效简化。生成这种资产必须在报告中明确无LOD收益。

cluster index窗口在INDICES范围内，顶点ID小于primitive顶点数；indexCount非0且为3的倍数。cluster/group数均小于NONE，顶点/索引/引用总数必须落在字段可表达范围；u64 section描述不允许绕过内部u32限制。一个 group 的引用不重复，输入/输出不相交且均非空；producer/consumer与反向引用必须完全一致。LEAVES恰好等于无producer集合，ROOTS恰好等于无consumer集合，所有cluster均参与合法终止路径。

payload不重复保存原顶点。INDICES不是普通 primitive.indices 的替代品，标准 LOD0索引保持原样。将来runtime适配器可拼成“原LOD0前缀+cluster索引”，在一次materialize中调整cluster偏移；不把这种GPU排布写成文件要求。

normal cone不是v1磁盘字段。未来feature materialize时可计算或禁用cone剔除，不能把本文件CLUSTERS直接上传成现有48B GPU meshlet。

### 6.3 指纹与失效

sourceHash为规范化模型空间属性、源三角形序列和构建所用primitive语义的SHA256。编码以固定profile标签和长度前缀区分字段，依次包含mode/顶点数、POSITION/NORMAL/有效TEXCOORD_0的f32 little-endian数据、u32源索引数和原始顺序的索引；缺UV按规则补0，accessor解码后统一为f32，规范化正负零，拒绝非有限值。纳入OPAQUE及所用UV集合等构建策略标签，明确记录所用属性集合；不散列C++ padding或文件排版。材质像素、node transform和外部文件路径不作为几何内容身份。

buildHash为算法/profile版本、全部几何构建参数、meshoptimizer revision、构建算法与依赖版本的规范化编码SHA256。线程数、调度批次、设备名、时间戳和性能数据放外部报告，不进入确定性载荷。执行预算不足时返回错误或跳过整个primitive，不因预算临时改变已接受的reduction，使同一算法和参数下的成功资产身份不受并行调度影响。指纹用于绑定和缓存，不代替结构/几何校验。

相同sourceHash/buildHash的已有有效payload复用，重复转换不再次追加。改变参数需要显式重建；首版对已带不匹配本扩展的文件报错，要求从源文件重建，避免未知扩展引用下冒险回收旧bufferView。

## 7. 转换器与文件保真

工具暂名 `deren-subtilis`，独立入口建议 `tools/subtilis/CMakeLists.txt`；公共格式/参考算法与构建器为无RHI的CPU库，不预建CUDA库或后端注册体系。此处目录是设计目标，不要求先重构现有engine模块。

宿主IO保存原glTF JSON DOM，原数组ID稳定，只在末尾追加本扩展需要的对象。不能通过现有drawable展平再导出。采用yyjson可写DOM，保留未知字段和整数精度，不自己手写通用JSON解析/序列化。依赖需在实施计划中固定revision、许可证与文件SHA256后vendor；不让构建下载最新版。

支持本地glTF/GLB和data URI，首版不抓取网络资源。原场景、实例、材质、纹理、动画与未转换primitive保留。GLB输入已嵌入的BIN内容保持前缀，新增数据追加；GLB文件总长受其u32长度字段限制，超过4GiB明确报错。

glTF输出复制/重定位已知本地资源，并新增专用bin，保持原buffer/accessor等索引不变。GLB输出优先打包成单文件；合并原buffers、嵌入images只对已知可安全重定位的引用做操作。含未知二进制引用语义的扩展时，禁止宣称可无损重打包；拒绝该GLB输出并建议保留glTF包结构。未知JSON字段的保留不等于理解其所有资源引用。

目标先写临时位置，经解码和full validation通过才发布最终输出；目录型输出先完成独立staging目录，单文件用同卷替换。源文件不原地覆盖，已有目标默认拒绝，`--overwrite`明确覆盖。构建错误、预算失败或写入失败不得发布部分资产。

首次构建参数：cluster上限85三角形/256顶点，目标group大小8个cluster，目标三角形比例0.5，最大轮数16；最大局部误差默认primitive包围盒对角线的0.01，normal/UV系数默认0.1/0.01，简化时属性长度尺度固定为该primitive对角线。分区API的group大小是目标，不是硬上限。零尺度和不适合简化的输入保留LOD0-only。全部几何构建参数进入metadata，执行参数只进外部报告；先用这些基线测量，再决定修改，不预先承诺更高压缩率。

## 8. CPU 并行、剖析与 CUDA 决策

### 8.1 多核调度与确定性

先在独立 primitive 之间并行；大型 primitive 的同一轮 reduction group 可在分区确定后并行。每个任务只读取不可变输入，拥有自己的简化、边界和重聚类scratch及候选输出。共享位置/属性顶点只读，不因组独立就并发修改全局几何。

DAG关系和稳定ID由调度线程按primitive/group固定顺序提交：先形成候选，再验证和提交；不以任务完成顺序决定cluster/group/root编号。每轮完成后才构造下一轮frontier。拒绝简化的输入保留原身份；验证、预算和错误路径不输出部分cut。

使用有界工作池和单一并行预算，避免“primitive线程池里再创建group线程池”导致超额线程。`--threads 1`提供串行对照，N线程复用同一算法；任务足够小时直接串行。meshoptimizer allocator只初始化一次，不在并发任务之间切换全局回调。

预算区分已知宿主缓冲、单任务scratch与最终输出；并行数和批次受可预估工作集预算约束，分配前检查已知大小。第三方分配也接入同一分配服务并计入峰值；无法证明严格总上界的meshoptimizer临时内存须如实标注，不能把输出预算冒充整个进程内存上限。无法承载任务时减少并发或明确失败，不删除几何来迎合容量；系统OOM遵循项目fast_panic约定。

### 8.2 首版必须提供的剖析

独立记录解析/规范化、源指纹、邻接/边界、聚类、分区、简化、重新聚类、DAG提交、full validation和写盘。CPU任务累计时间与端到端墙钟分别报告，累计时间不能直接相加冒充并行墙钟；可嵌套的计时标注inclusive/exclusive。

资产矩阵为小资产、合成大网格、原层级Bistro；串行和多核各至少3次，报告中位数及逐轮值、峰值宿主内存、输出体积、选层质量和无进展原因。源文件加载/IO与算法阶段分开，不用旧引擎加载增加10–15秒的历史记录冒充新DAG builder的分阶段数据。

优先判断瓶颈是否能够通过多核调度、减少重复拓扑分析、复用scratch和批处理解决；这些优化需要在相同质量/构建参数下比较。

### 8.3 CUDA 的进入条件

CUDA保留为后续测量驱动路线，不是首版必交付后端。CPU版本正确且有真实热点后，选择一个热点做有界实验：

- 邻接/排序/边界占显著时间时，验证批量GPU替换是否能缩短整次转换。
- 包围体/压紧占比很小时，不为这些轻量阶段单独搭GPU路径。
- 简化占主导时，明确只搬邻接的加速上限；若要GPU约束简化，另行设计拓扑合法性与冲突消解，不把CPU meshoptimizer称为GPU简化。

实验须同时统计H2D、kernel、D2H、同步、初始化和CPU剩余工作，比较完整墙钟与峰值显存/宿主内存，包含同资产的预热/冷启动及至少3轮CPU/GPU对照。不能只展示kernel加速倍数，也不能把分阶段上限推算写成实测。

只有在保持格式、DAG/边界与质量验收一致，并在目标工作负载上得到可重复的端到端收益后，才提议纳入正式后端；若收益不足，记录结论并停止扩展。具体工程收益门槛由实验方案结合实际转换时间确定，不在缺少基线时虚构一个加速承诺。

未来实验优先使用CUB等成熟排序/scan基础设施，并验证整数邻接一致、浮点bounds包含性及GPU错误/内存预算路径。首版不实现这些尚未测量的代码。

## 9. CLI 与可独立验收的产物

```text
deren-subtilis build input.glb -o cooked.glb --threads 12 --report build.json
deren-subtilis build input.glb -o reference.glb --threads 1
deren-subtilis inspect cooked.glb
deren-subtilis validate cooked.glb --full
deren-subtilis export-cut cooked.glb --error 0.01 -o preview.glb
```

`build`默认允许不适用primitive原样通过，但必须给出converted/skipped/LOD0-only数量；没有任何可转换目标时非0退出。`--strict`要求所有请求转换的目标成功，应用条件之外的skin/动画不被悄悄改写。

`validate`可在无GPU环境运行。结构验证检查范围、版本、互反关系、无环、误差与bounds；full验证重算sourceHash、leaf三角形多重集/绕序、每个reduction的边界等价、几何和LOD球包含性，并进行cut闭合性检查。轻量结构验证不能冒充几何正确性证据。

`export-cut --error`单位为模型空间，不解释为像素。导出普通glTF/GLB，保留材质、原属性及未转换对象；节点继续共享同一mesh cut，不烘焙node变换。导出所选cluster构成的索引作为正常primitive.indices，并移除本扩展/其已失效引用；由成熟查看器检查画面。本阶段只提供模型空间cut导出；屏幕误差的CPU参考用例带明确相机与extent，不增加交互编辑器。

报告包含各阶段耗时、峰值宿主内存、线程数、输入/输出字节、每层三角形/cluster/group/root数量、发生跨组重分配的数量、无进展原因、构建器和依赖版本。性能与质量报告外置，不污染资产身份。

## 10. 验收门槛

1. **格式闭环**：CPU encode/decode语义往返；损坏header、越界/重叠section、u64溢出、非法索引、NaN、循环/互反关系错误、漏根/重复引用均明确拒绝。离线解码不需要CUDA、Vulkan或engine。
2. **真实DAG**：至少一个自动构建资产发生§3.1的跨组分配；遍历不能退化成唯一父森林。独立CPU cut在阈值两侧、相等阈值、近面、缩放和标准透视/正交视图下验证依赖闭合和一致替换。
3. **边界**：共享边、UV缝、硬法线、多组件、退化/重复/非流形输入；不能通过删除源三角形、强行焊接或总回退LOD0来伪造通过。
4. **并行对照**：串行/多核构建的几何、DAG和稳定ID一致；相同构建环境下payload逐字一致。测试不同线程数、任务完成顺序、任务拒绝与预算退让，不能让并发顺序改变边界和最终资产。计时/线程数只进报告，不进入资产身份。
5. **glTF保真**：普通查看器忽略扩展仍正确显示原LOD0；转换前后core几何/材质/节点语义一致，实例不重复烘焙；未知字段、外部资源和已有扩展有保留/拒绝打包用例。生成文件经过Khronos validator，并单独运行本扩展validator；core validator不理解自有DAG语义。
6. **独立预览**：有多个误差cut输出，检查轮廓与材质缝、裂缝和三角形变化；原LOD0几何身份精确一致，简化结果按质量报告验收，不能要求与LOD0像素完全相同。
7. **真实场景**：小资产、合成大网格、原层级Bistro；串行/多核各至少3次测总墙钟、阶段耗时与内存，加载/IO单独标注。CPU热点及收益上限分析是首版交付；CUDA实验不阻塞首版验收。
8. **仓库回归**：普通deren构建/测试不新增CUDA依赖，现有Subtilis/HZB行为不变。无GPU CI运行格式、DAG与CPU构建用例。未来若有CUDA测试，必须明确设备运行，缺设备标skip，不能冒充PASS。

## 11. 建议实施分界

- A：独立CPU格式库、schema、DAG模型和cut参考，先用手工真实DAG夹具形成encode/decode/validate/export-cut闭环。
- B：meshoptimizer基础上的CPU DAG builder，验证重新分区、边界和真实资产，形成串行正确性基线。
- C：有界多核调度、scratch/预算复用、分阶段剖析及串行/多核一致性，形成可用的CPU转换器。
- D：无损glTF/GLB包装、完整转换矩阵、文档和工具发布验收。包装基础应在A阶段可用，D是复杂资源与真实文件的收尾，不是最后才验证输出可读。

A–D是同一个离线工具阶段的实施分界，不表示先把旧森林包装发布。首版完成后，根据§8的实测选择是否另开CUDA实验；格式不因是否采用CUDA而改变。后续单独设计deren的消费feature：加载payload、适配GPU数据、共享上传、DAG cut与现有visibility/HZB组合；旧格式/feature默认行为保持到那一阶段再决定。

## 12. 参考与设计自查

- UE本地参考：`D:/UnrealEngine-5.8.2-release/Engine/Source/Developer/NaniteBuilder/Private/ClusterDAG.h`、`ClusterDAG.cpp`的GroupTriangleClusters/ReduceGroup/FindCut，以及`Cluster.h`的GroupIndex/GeneratingGroupIndex。参考关系与误差传播，不把UE二进制结构用作本格式。
- [MSFT_lod](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Vendor/MSFT_lod/README.md)：节点/材质替换语义，不承载本DAG。
- [glTF前缀规则](https://github.com/KhronosGroup/glTF/blob/main/extensions/Prefixes.md)与[核心规格源码](https://github.com/KhronosGroup/glTF/blob/main/specification/2.0/Specification.adoc)。
- [CUDA 13.4 Windows编译器支持](https://docs.nvidia.com/cuda/cuda-installation-guide-microsoft-windows/)与[CUB](https://github.com/NVIDIA/cccl)。仅作为未来实验的环境资料，首版不启用CUDA。
- [yyjson](https://github.com/ibireme/yyjson)：可写JSON DOM、整数精度和自定义分配支持；仅用于新工具，不改变现有gltf_loader后端。

自查：首版明确为共享原顶点、多核CPU约束简化与实测剖析，CUDA延期且不增加预设后端框架，DAG切换条件有独立参考；普通LOD0保留，未知资源语义不作虚假保真承诺；格式不包含当前可见表容量/设备ABI。逐项实施计划已固定新依赖revision与构建边界，审阅后按计划实施。
