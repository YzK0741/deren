# Subtilis glTF Cluster DAG Converter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans inline to implement this plan task-by-task. Preserve the current-session native execution method and current-checkout authorization. One fresh whole-change reviewer after implementation; no per-task agents. Steps use checkbox syntax for tracking.

**Goal:** 实现可独立构建和验收的 CPU 多核 glTF cluster DAG 转换器，保留标准LOD0，在deren消费feature之前完成资产闭环。
**Architecture:** 无RHI的CPU模块负责wire codec、validated DAG、cut参考和构建。工具保留glTF原DOM并追加扩展，串行/多核共用确定性构建算法。先通过手工DAG形成文件闭环，再自动构建和并行化。
**Tech Stack:** CMake4.3、C++23 modules、Clang64/libc++、vessel/mimalloc、meshoptimizer v1.3、fastgltf/simdjson、yyjson0.12.0。无CUDA/Vulkan依赖。
**Spec:** `docs/superpowers/specs/2026-10-21-subtilis-gltf-design.md`（修订提交 `f6586564`）。代码基线 `07e365bd`，计划时HEAD `f6586564`。

## Global Constraints

- 本阶段不修改runtime/visibility/HZB，不把旧唯一父森林直接写盘，不加入CUDA或通用后端框架。后续deren消费feature单独实施。
- profile=`shared-vertices`，静态OPAQUE TRIANGLES，引用原顶点；POSITION/NORMAL必需，UV按spec规则处理。skin/morph/MASK/BLEND/未建模属性原样保留并报告跳过。
- cluster最多85三角形/256顶点，目标group8、ratio0.5、max rounds16、误差0.01×primitive对角线、normal/UV权重0.1/0.01。磁盘数量不绑定运行时4096可见项容量。
- 版本1 little-endian；header128B、descriptor32B；CLUSTERS32B、GROUPS48B；u64外层范围检查不能绕过内部u32限制，NONE=0xffffffff。
- meshoptimizer保持 `9e1f07b159d3cb777f1c67ed31fc11fd117986f4`；yyjson固定0.12.0 / `8b4a38dc994a110abaec8a400615567bd996105f`，仅vendor src/yyjson.c、src/yyjson.h、LICENSE及来源/manifest，不引入运行时联网构建。
- 项目容器用vessel，第三方分配接入一次性mimalloc适配；正常错误为typed result，系统OOM沿用fast_panic。不能因并发临时切换meshoptimizer全局allocator。
- 线程数、计时和调度批次不进入payload/hash。相同环境下串行/多核产物逐字一致。每次reduction边界验证，不以删除原三角形或全回退掩盖失败。
- 保留用户未跟踪文件，当前master继续修改；本地提交，不推送。一次只跑一个CMake构建，`-j12`；5h剩余<=5%时记录并暂停。重证据逐文件SHA256核对后归档D盘。

## Review Focus

1. 输入mesh被多个node共享且部分实例skinned：不重复烘焙、不丢实例、不把本应跳过的资产变成静态；Task3/6的实例与skin混合夹具验证。
2. 未知扩展直接引用buffer或拥有外部资源：保留DOM不代表可随意重排索引；Task3/6验证glTF保留、GLB拒绝危险打包、required扩展错误。
3. 合法DAG的跨组输出与相等误差、近面/缩放：不能用树递归或扫描中修补决定导致重复/漏绘；Task2/5测试阈值cut与独立替换覆盖。
4. 超大/损坏payload以及量化/稀疏输入：必须先检查范围和解码语义，再分配/散列；Task1/3覆盖溢出、别名section、NaN和accessor窗口。
5. 并发完成乱序、工作集退让、重复转换/写盘失败：身份稳定、源文件不改、最终目标完整；Task6/7验证字节一致与故障注入。

## 文件与边界

- `cmake/deren_cpu_utility.cmake`：抽出已有vstd_lib/mimalloc_vendored/shared_utility目标定义，根工程和独立工具共用；用显式repo根路径，不依赖调用方CMAKE_SOURCE_DIR。保留原目标名、模块名、宏、平台链接和DLL导出规则。
- `cmake/deren_asset_dependencies.cmake`：共用已有meshoptimizer_vendored、fastgltf_vendored/simdjson的原源文件列表；添加yyjson_vendored。禁止调用上游会FetchContent的配置。
- `source/asset/subtilis/`：`types.cppm`、`codec.cppm/.cpp`、`digest.cpp`、`dag.cppm/.cpp`、`cut.cpp`、`builder.cppm/.cpp`、`topology.cpp`、`reduce.cpp`、`parallel.cpp`、`builder_internal.hpp`；模块名分别为`deren.asset.subtilis.types/codec/dag/builder`。
- `tools/subtilis/`：独立CMakeLists、`document.hpp/.cpp`、`accessor.cpp`、`package.cpp`、`commands.hpp/.cpp`、`main.cpp`；这些是CLI/IO实现，不导出yyjson/fastgltf类型到资产库。
- `source/tests/`：新增`test_subtilis_codec.cpp`、`test_subtilis_dag.cpp`、`test_subtilis_document.cpp`、`test_subtilis_dag_builder.cpp`、`test_subtilis_converter.cpp`、`test_subtilis_parallel.cpp`，共用`subtilis_dag_fixtures.hpp`。
- `docs/extensions/DEREN_subtilis/README.md`及`schema/mesh.primitive.DEREN_subtilis.schema.json`：正式wire约定与JSON schema。最终使用说明为`tools/subtilis/README.md`。
- `scripts/measure/measure_subtilis_converter.py`、`verify_subtilis_converter.py`：真实资产、串并行、Khronos校验和结果分析；重产物不进仓库。

## 公共类型约定

下列接口位于`deren::asset::subtilis`；`vector/string/array`指vessel别名。`result<T>`统一为`std::expected<T,error>`，`error`含`error_code code`、`string context`、可选primitive/group ID，不能混用RHI错误。

- `source_geometry`拥有position/normal `vector<array<float,3>>`、uv0 `vector<array<float,2>>`、`vector<uint32_t> indices`；非有限输入拒绝，坐标均为模型空间。
- `payload`拥有spec的clusters/groups/references/leaves/roots/indices、sourceHash/buildHash和canonical BUILD_META。只是一份未验证的解码数据。
- `validated_asset`只能通过验证工厂创建，独占拥有source_geometry和payload；对外const views，不暴露可变vector。builder返回的资产离开输入生命周期后仍有效。
- `limits`区分`max_input_bytes/max_payload_bytes/max_records`；`build_limits`另含`max_output_bytes/max_known_work_bytes`，不冒充第三方临时分配的严格总上界。
- `build_options`为Global Constraints的几何参数；`execution_options{threads, limits}`仅影响调度或整个primitive成功/失败，不改变已成功资产。
- `build_report`为各层计数/拒绝原因/阶段计时、峰值与环境；不写入构建身份中的线程数或计时。CLI退出码：0成功、2用法错误、3输入/校验错误、4构建/预算错误、5输出错误。

## Task 1 — 独立构建、wire codec与资产身份

**Files:** 新增上述两个cmake helper、`tools/subtilis/CMakeLists.txt`、types/codec/digest文件、`source/tests/test_subtilis_codec.cpp`、扩展README/schema；修改根`CMakeLists.txt`、`source/third_party/VERSIONS.md`；新增yyjson子树及manifest。

**Interfaces:**
- `result<vector<byte>> encode(payload const&, limits const&)`。
- `result<payload> decode(span<byte const>, limits const&)`；仅完成wire和结构形状检查，不能返回validated_asset。
- `result<digest> source_digest(source_geometry const&)`、`result<digest> build_digest(build_options const&)`（typed错误裁定见checkpoint）；固定字段顺序、长度前缀和LE编码见spec。SHA256适配仓库`test_goo_toon_math.cpp`的既有算法为流式实现，原测试不改为依赖新库。

- [x] RED：测试七分区roundtrip及字段固定offset；`headerBytes=128`、cluster32/group48、对齐/零padding；拒绝重叠、重复/未知section、truncated、u64 overflow、count×stride超限及不对齐输入。测试sourceDigest对-0/+0一致、索引绕序改变后不同；SHA256空串/abc/多块与Python hashlib一致。
- [x] 在实现前运行`cmake --build build-subtilis --target test_subtilis_codec -j12`并记录缺接口或断言失败；首次最小配置与测试目标随此交付建立。
- [x] 抽出原CPU依赖目标，按固定revision vendor yyjson并生成/核对manifest；实现显式字节读写，先验证再分配。JSON schema固定version=1/profile/shared-vertices和bufferView非负整数。
- [x] 配置：`cmake -S tools/subtilis -B build-subtilis -G Ninja -DCMAKE_BUILD_TYPE=Release`；构建/测试codec通过。独立配置设置`CMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE`仍成功，target图无promise/engine/CUDA。
- [x] 根Release重配置、构建和原58项CTest通过，证明helper抽取未改变现有工程；此后原工程全套仅在其构建配置或共享utility再次改变时重跑。
- [x] 本地提交：`feat: add the standalone Subtilis payload codec`。

## Task 2 — validated DAG、full geometry验证和CPU cut参考

**Files:** dag/cut文件、`subtilis_dag_fixtures.hpp`、`test_subtilis_dag.cpp`。

**Interfaces:**
- `result<void> validate_structure(payload const&, limits const&)`。
- `result<validated_asset> make_asset(source_geometry, payload, limits const&)`，完成source绑定、leaf多重集/绕序、每组外边界和bounds包含性校验。
- `cut_parameters`区分model_error/screen_error，含threshold、model/view/proj、extent、near_plane、max_clusters。
- `result<vector<uint32_t>> select_cut(validated_asset const&, cut_parameters const&)`，返回稳定cluster ID顺序；无效视图回LOD0，无法装下完整LOD0返回capacity。

- [x] RED：人工构造spec G0/G1→G2/G3真实交叉DAG及等几何不同三角剖分；阈值0/相等/两侧cut与独立区域覆盖参考一致，未选择粗细重叠表示。拒绝消费者互反错误、多producer、漏root、倒序dependency、下降误差、向内bounds和边界不一致。
- [x] RED：透视/正交、非均匀缩放、近面、NaN视图、容量不足；构造浮点闭合性不一致决定，必须保守禁止相应粗化，再一次性扫描cluster，不能中途更改决定。
- [x] 执行`ctest --test-dir build-subtilis -R '^test_subtilis_dag$' --output-on-failure`确认RED；实现immutable验证工厂和spec coarsen规则。bounds以CPU高精度重算并验证外向舍入，旧树worst-cut公式不复用。
- [x] 相同命令GREEN，并对固定seed的阈值/相机序列检查有效cut、无重复和closure；原输入销毁后validated_asset仍有效。
- [x] 本地提交：`feat: validate cluster DAGs and select consistent CPU cuts`。

## Task 3 — 保真文档读取、primitive身份与夹具文件闭环

**Files:** document/accessor文件、`test_subtilis_document.cpp`、`source/tests/fixtures/subtilis/`、扩展README/schema。

**Interfaces（tools命名空间）:**
- `primitive_key{uint32_t mesh, primitive}`，不使用node遍历序号。
- `result<document> read_document(path const&, limits const&)`；document移动拥有原DOM和buffer数据。
- `result<vector<primitive_key>> eligible_primitives(document const&)`；收集所有node/skin引用后判定，不重复展开实例。
- `result<source_geometry> extract_geometry(document const&, primitive_key)`。
- `result<void> attach_payload(document&, primitive_key, span<byte const>)`、`result<vector<byte>> serialize_embedded_glb(document const&)`：先支持已自包含GLB，供手工DAG夹具完整往返。

- [x] RED：同mesh被两node引用及静态/skin混用、非索引三角形、缺NORMAL/UV、TANGENT与未知属性；规范化量化/稀疏accessor及越界/坏stride/归一化有符号值；确认eligibility和sourceDigest输入符合spec。
- [x] RED：未知JSON嵌套对象/大整数、可选与required扩展、原数组ID和BIN前缀保持；原LOD0索引和材质/动画/node语义不变。被工具理解的DEREN扩展只从fastgltf解码用的临时required列表中排除，原DOM保留；真正未知required不得忽略。
- [x] `test_subtilis_document`先RED；IO用yyjson拥有原DOM，几何解码复用fastgltf原mesh/primitive索引和accessor工具，不经deren.gltf_loader展平。partial策略只跳过语义已知不适用项，不吞损坏core数据。
- [x] 附加Task2人工DAG，serialize→read→decode→make_asset→select_cut全链GREEN；core indices/attributes/节点引用与输入语义一致。量化位置的node变换不烘焙进模型空间数据。
- [x] 本地提交：`feat: preserve glTF documents and bind Subtilis payloads`。

## Task 4 — 叶聚类、边界拓扑与单次约束reduction

**Files:** topology/reduce/builder_internal文件、`test_subtilis_dag_builder.cpp`。

**Interfaces（builder_detail私有）:**
- `result<frontier> cluster_lod0(source_geometry const&, build_options const&)`：原顶点ID，源三角形多重集/绕序不变，空间聚类可重排三角形。
- `result<reduction_candidate> reduce_group(source_geometry const&, span<cluster_input const>, build_options const&, primitive_scale)`；返回私有粗索引/cluster和local_error，不直接分配全局ID或修改DAG。
- `boundary_signature boundary_of(source_geometry const&, span<uint32_t const>)`：有向外边界多重集和属性缝身份；unsafe拓扑给明确拒绝原因。

- [x] RED：曲面网格的leaf≤85tri/256vertex且精确覆盖源；两组共享边、UV缝/硬法线、重复位置不同属性、断开组件与非流形；合法简化真正减量，细粗外边界相同，非法/无进展候选保留原几何并报告。
- [x] RED：无NaN/越界/反向面、拒绝通过删源三角形伪造LOD0；normal/UV属性尺度固定于primitive，不随group层级缩小。max_rounds/ratio/error参数非法值拒绝。
- [x] `test_subtilis_dag_builder`先RED；初始及粗层聚类用`meshopt_buildMeshlets(max_vertices=256,max_triangles=85,cone_weight=0)`并还原到原始顶点ID，分区用`meshopt_partitionClusters`，简化用`meshopt_simplifyWithAttributes`。位置等价不焊接实际属性；锁当前组外边界，重算候选边界和bounds后才接受。第三方allocator只安装一次。
- [x] GREEN：上述语义与已知减量fixture通过；任务候选可独立构建/销毁，没有共享输出区写入。
- [x] 本地提交：`feat: build constrained cluster reduction candidates`。

## Task 5 — 串行DAG builder与最小可用CLI

**Files:** builder模块、commands/main文件、`test_subtilis_dag_builder.cpp`、`test_subtilis_converter.cpp`、`tools/subtilis/README.md`。

**Interfaces:**
- `result<build_result> build(source_geometry, build_options const&, execution_options const&)`；此任务threads=1，结果含validated_asset与build_report。
- `result<build_result> build_serial(source_geometry, build_options const&, build_limits const&)`作为稳定参考，Task7沿用同一候选/提交逻辑。
- `result<command> parse_arguments(span<string_view const>)`、`int run(command const&)`；command变体为build/inspect/validate/export_cut，各参数对应spec CLI；typed错误映射固定退出码。

- [x] RED：合成网格自动产生跨组重新分配，至少一个producer的粗cluster被两个不同consumer使用；多根、无进展carry、终止轮数正确；不是手工夹具或“全部LOD0”冒充自动DAG。
- [x] RED：误差累计与LOD球嵌套；固定参数重复运行ID/payload稳定；每轮接受前验证边界，group拓扑顺序和互反引用一致。build后直接make_asset/full validation通过。
- [x] 先跑`test_subtilis_dag_builder` RED，再实现每轮frontier分区→候选→稳定提交→下一轮；分区单位为cluster，允许拆分上层生成集合。build返回拥有输入的资产，不引用调用者临时数据。
- [x] CLI RED→GREEN：对自包含GLB完成build、inspect、validate --full及多个model-error cut导出；inspect/validate输出与算法返回值交叉检查。未适用目标报告skip；零可转换目标非0；LOD0-only成功必须明示无收益。
- [x] CLI smoke：`deren-subtilis build fixture.glb -o cooked.glb --threads 1 --report build.json`、`deren-subtilis validate cooked.glb --full`、`deren-subtilis export-cut cooked.glb --error 0.01 -o cut.glb`均exit0。`--threads`为正整数，0或非法数值为用法错误；此任务缺省为1，Task7前只接受1，明确拒绝尚未实现的并行请求。
- [x] export-cut写标准indices/accessor，移除已失效DEREN声明（仅无剩余使用时移除extensionsUsed项），源材质/属性/node保持；不把整个cluster索引流当作一次正常绘制。
- [x] 本地提交：`feat: build cluster DAGs and expose the conversion CLI`。

## Task 6 — 完整资源包装、重复转换与失败原子性

**Files:** package/document/commands文件、`test_subtilis_converter.cpp`及fixtures。

**Interfaces:**
- `result<void> write_package(document const&, path const&, write_options const&)`，write_options含overwrite和gltf/glb输出类型；staging完成并full validate后发布。
- `result<conversion_report> convert_file(path const& input, path const& output, conversion_options const&)`，串行构建每个eligible primitive，之后Task7接入同接口。

- [x] RED：外部buffer/image、data URI、URI转义、输出新目录、多个buffer的GLB合并；已知引用可重定位，未知二进制语义拒绝危险GLB打包但保留glTF输出。测试>4GiB长度计算溢出用稀疏/计数夹具，不实际写满C盘。
- [x] RED：原地覆盖、已有目标、只读/短写/最终替换失败；源文件和已有目标字节不变，不遗留被当作成功产物的半包。目录输出先完成staging再发布，跨卷使用目的卷staging。
- [x] RED：source/buildHash相同且full valid则复用、不追加；过期source、不同参数、损坏已有payload明确拒绝并要求从源重建，不静默再append。strict/partial对构建失败和不适用目标分类符合spec。
- [x] `test_subtilis_converter`确认RED；实现有界资源读取、原索引保持的追加、MIME正确的image嵌入和事务式发布。参数/线程元数据仅放报告，资产构建参数保持canonical顺序。
- [x] GREEN：输入/输出资源逐项hash、core DOM语义、实例身份、临时文件清理和失败原子性全部通过。
- [x] 本地提交：`feat: package Subtilis glTF assets without losing source resources`。

## Task 7 — 确定性多核调度、工作集与阶段剖析

**Files:** parallel/builder文件、`test_subtilis_parallel.cpp`、commands/report输出。

**Interfaces:**
- `task_fn`与`commit_fn`均为`std::function<result<void>(size_t)>`；`result<void> run_ordered(size_t count, uint32_t threads, span<size_t const> estimates, size_t max_known_work_bytes, task_fn prepare, commit_fn commit)`为私有有界调度。调用者拥有按index预分配的候选槽，prepare仅写自己的槽，commit在调度线程按ID序读槽提交，任何槽不以重分配影响其他任务。
- `execution_options.threads`为进程单一并行预算，任务内不创建第二线程池；threads=1调用串行逻辑。本任务将CLI缺省值改为`min(max(hardware_concurrency(),1),12)`，显式正整数请求再受工作集预算限制。build/convert_file保持Task5/6签名。
- `build_report`含各阶段CPU累计/inclusive标记与总墙钟、已知工作集/第三方峰值的边界说明；线程数、时间、内存统计不进入wire payload/hash。

- [x] RED：threads1/2/12、强制乱序完成、相邻任务拒绝、同mesh多实例与单大primitive；成功source/buildHash和encode(payload)逐字一致。被拒候选不消耗后续可见ID，不按完成顺序提交。
- [x] RED：已知workspace预算缩小导致降低并发或整primitive拒绝，不改变任何成功资产的reduction；allocator并发初始化、workspace复用和取消/收尾无泄漏。
- [x] `test_subtilis_parallel`先RED；用有界工作池/任务队列，粗粒度primitive优先，单大primitive在每轮group层并行；统一预算，不重复并行。保留build_serial为参考。
- [x] GREEN并跑整个独立工具CTest。计时测试只验证阶段完整/非负和total不重复累计，不断言机器相关加速或毫秒阈值。
- [x] 本地提交：`perf: parallelize Subtilis asset construction deterministically`。

## Task 8 — 真实资产、CI、文档与最终审查

**Files:** measure/verify脚本、新增`.github/workflows/subtilis-converter.yml`、`tools/subtilis/README.md`、`docs/reviews/2026-10-21-subtilis-converter.md`及结构化results；根README与`docs/subtilis.md`只更新工具状态/入口。独立CI不向现有VR_TEST_TARGETS/ci.yml列表混入工具测试，保持test_docs的现有契约。

- [ ] measure脚本RED→GREEN：从报告读分阶段计数，区分CPU累计/墙钟，缺阶段/缺轮次/无真实跨组DAG不得PASS；3轮serial/parallel中位数来自逐轮数据，不能只保留最好一轮。
- [ ] 对小资产、合成大网格和`D:/deren-assets/bistro/bistro-geometry.gltf`各跑threads1/12各3次，参数一致；记录CPU热点、峰值、完整输出大小、拒绝原因、真实DAG数量；不引入CUDA依赖或声称未测收益。
- [ ] Khronos core validator固定`2.0.0-dev.3.10`（官方release；单独工具依赖，不进普通CMake构建），校验普通输出与export-cut。使用官方CLI `gltf_validator --stdout <file>`，verify脚本解析numErrors，记录自有扩展unsupported info，不能忽略core errors；另运行`deren-subtilis validate --full`，两者都成功才接受。
- [ ] 按完整/LOD0/多个阈值cut检查场景、轮廓、材质缝和边界；LOD0几何多重集与core资源语义精确一致，简化画面变化单独报告。没有自动图像工具时保留标准GLB预览供直接审阅，不能宣称未检查的像素质量。
- [ ] 新增独立CPU工具CI：不安装Vulkan/CUDA可配置、构建并运行全部新测试；小fixture不下载Bistro。ASan/UBSan覆盖codec、DAG、builder、parallel和IO失败路径。根构建回归按Task1规则与最终集成改动范围运行。
- [ ] 一名fresh只读whole-change reviewer检查5个Review Focus及spec coverage；Critical/Important修复须有复现与GREEN，Minor记账；按executing-plans规则执行最终审查，不启动任务级agents或重复审查流程。
- [ ] 更新spec/计划状态和真实证据报告，本地提交最终文档。重产物归档D盘，核对每个SHA256后清理当前任务副本，保留恢复记录；不推送。

## 自查与交接

覆盖：spec1–2→Task1/8；DAG关系/误差/cut→Task2/5；边界/几何profile→Task3/4；wire/hash→Task1/2；保真与CLI→Task3/5/6；并行/预算/剖析→Task7/8。5个Review Focus均有具体负控或对照；所有跨任务接口以上述签名为准。

当前只完成计划，未创建生产文件或vendor依赖，所有任务仍未勾选。执行沿用当前会话native方式；该计划需用户审阅后再进入实现。推荐首先执行Task1–3，交付一个可验证手工DAG文件的最小闭环，再推进自动builder；这是实施节奏，不削减最终转换器范围。

依赖参考：[yyjson固定commit](https://github.com/ibireme/yyjson/commit/8b4a38dc994a110abaec8a400615567bd996105f)、[Khronos validator release](https://github.com/KhronosGroup/glTF-Validator/releases)。新依赖落盘后必须将实际文件SHA256写进manifest并检查，不能用版本名代替内容核验。
