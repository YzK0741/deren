# 渲染图组合构建机制

日期：2026-10-16。状态：组合构图基线及现有后处理适配已实施，ownable 命名按用户确认。下文保留设计说明；实际接口见 compose.cppm，验收见 [实施记录](../../reviews/2026-10-16-render-graph-composition.md)。主链绘制迁移仍为下一阶段。

**2026-10-18 现状核对：最后一句已过期。** 主链绘制已经迁到同一张组合图，"下一阶段" 是
2026-10-16 的排期，不是今天的待办：`runtime.post_graph.cpp` 现在是**全帧**构图入口，
最多 21 条 lane，覆盖 deferred lighting、scene、cluster、shadow、TAA、gbuffer debug、
transparent、character forward、toon screen rim、goo rim、Gaussian、ray-traced shadow 与
visibility raster/resolve（`source/engine/runtime/runtime.post_graph.cpp:30-47`）。
旧路径不再存在：`runtime.frames.cppm:876-878`（"THE GRAPH IS THE ONLY RECORDER NOW ... no
legacy cluster sort, no legacy shadow pass, no legacy scene"）与 `:3147-3150`
（"NO LEGACY HALF IS LEFT TO FALL BACK TO"）。

第 4 节给出的执行层接口**按原样落地**，可逐项对照：`ownable` / `graph_appender` /
`pass_recorder`（`compose.cppm:14`、`:24`、`:29`）、`execution::builder`（`:75`）、
`append`（`:176`）、`export_resource`（`:192`、`:200`）、`bind_pass`（`:218`）、
`bind_barrier_only`（`:230`）、`retain`（`:239`）、`prepare`（`:247`），以及 §269 说明的
`detail::composition_access`（`:264`）。

## 1. 目标和范围

让场景、光照、时间处理和后处理向同一个 builder 登记 pass，最终统一编译依赖、裁剪和同步。资源继续在图外创建，由 resource_cluster、既有 generation owner 和 lease 管理；旧路径保留到对应部分验收完成。

本次仅调整构图与录制组织，不迁移主链的实际绘制，不改变 TAA 算法，不增加多队列、并行构图、图缓存、GPU 资源池、别名分配或新的通用调度器。现有 Boost.Graph 编译基础保持不变。截图后最终交接暂时仍是独立图边界。

成功标准：现有后处理能通过可组合入口运行；两个模块共享一个资源 ref 后可统一生成跨模块屏障；插入、裁剪或重排 pass 不会错配录制动作；构图错误和缺失动作在发出图命令前失败；资源和动作数据能保留到 GPU 完成。

## 2. 当前限制与方案选择

当前 post::compile(description) 同时创建 builder、导入资源、追加 pass、export 和 compile，适合作为独立入口，但无法直接接受主链已产生的 HDR ref。runtime.post_graph.cpp 使用固定 ios/pass 数组和 pass_index 分发，这个约定不能跨模块扩展。

选择一个描述 builder 加一个薄的执行组合层。相较长期保留多张小图，它减少图间状态交接；相较将 RHI 回调直接放入 CPU builder，它继续允许纯 CPU 测试和诊断。

| 层 | 职责 | 不负责 |
| --- | --- | --- |
| render_graph::builder | ref、参数快照、访问、内容版本、依赖、裁剪、同步计划 | RHI 对象、录制动作、提交 |
| execution::builder（新增） | 组合描述、lease 导入绑定、动作登记、准备 | GPU 资源创建、帧槽调度、自动提交 |
| execution::prepared_graph（扩展） | 保有准备结果，按编译顺序录制屏障和动作 | 判断 GPU 完成、自动发布历史 |
| runtime／调用方 | 功能选择、资源准备、提交、状态发布和帧槽保留 | 重新推导图已接管范围的屏障 |

不新增与 prepared_graph 并行的 prepared_frame/program 对象体系。

## 3. 构图函数和模块边界

构图函数采用 append 命名，接受同一 CPU builder 及具名输入，返回具名输出。以下省略具体字段类型，但所有实际接口均返回 expected：

```cpp
post::append(render_graph::builder&, post::inputs const&, post::options const&)
    noexcept -> expected<post::outputs, graph_error>;
```

post::inputs 持有 HDR、frame、四级 Bloom、可选 LDR 和可选 LUT 的 texture_ref。post::options 只保存 Bloom 开关及 resolve 选择等构图条件，不保存初始布局、资源身份或 RHI 对象。

post::outputs 包含最终颜色 ref 和具名 pass_ref：四级 Bloom/clear、composite、可选 resolve。它们用于录制适配，不要求调用方了解该模块在全图的下标。输入无效、相互别名不允许或 resolve 所需资源缺失时正常报错。

```cpp
struct post::inputs {
    texture_ref hdr, frame;
    array<texture_ref, 4> bloom;
    optional<texture_ref> ldr, lut;
};
struct post::outputs {
    texture_ref color;
    array<pass_ref, 4> bloom;
    pass_ref composite;
    optional<pass_ref> resolve;
};
```

无 resolve 时 ldr 不参与访问；有 resolve 时必须提供 ldr。关闭 Bloom 时四个 bloom pass_ref 对应 clear 动作，composite 仍声明读取这些已清零的资源。实际执行适配根据已确定的 options 绑定相应动作，不从全图下标推断阶段。

append 不 import、不 export、不 compile、不记录 GPU 命令。其他模块采用相同规则，但使用各自的输入输出类型，不强制一个装下所有阶段字段的通用结构。

现有 post::compile(description) 保留：独立导入 → append → export 最终输出 → compile。纯编译测试继续使用它。其内部不再维护另一套 pass 生成代码。

后处理的 clear、bloom、composite、resolve 应逐步使用各自的参数类型；共享字段可以嵌套复用。不再通过一个 stage 枚举加运行期 switch 决定所有阶段的实际操作。迁移过程可短期保留旧参数，但新组合入口不依赖该枚举分发。

## 4. 执行组合层的接口

建议放在新模块 deren.engine.render_graph.compose，命名空间仍为 render_graph::execution，依赖已有 execute，避免 RHI 进入纯 CPU graph 核心。

```cpp
class execution::builder final {
public:
    explicit builder(shared_ptr<rhi::api_core>);

    expected<texture_ref, build_error> import_texture(
        resource_lease<rhi::image>, import_state, string_view name);
    expected<buffer_ref, build_error> import_buffer(
        resource_lease<rhi::buffer>, import_state, string_view name);

    // 同步调用 fn(CPU builder&)；将其 expected<T, graph_error>
    // 转换为 expected<T, build_error>，失败时记住首个错误。
    template<class F>
        requires graph_appender<F>
    auto append(F&& fn);

    expected<void, build_error> export_resource(texture_ref, texture_range = {});
    expected<void, build_error> export_resource(buffer_ref, buffer_range = {});
    expected<void, build_error> order_after(pass_ref, pass_ref);

    template<class Parameters, class Recorder>
        requires pass_recorder<remove_cvref_t<Recorder>, Parameters> &&
                 is_nothrow_constructible_v<remove_cvref_t<Recorder>, Recorder&&>
    expected<void, build_error> bind_pass(pass_ref, Recorder&&);
    expected<void, build_error> bind_barrier_only(pass_ref);

    template<class T>
        requires convertible_to<shared_ptr<T>, shared_ptr<void const>>
    expected<void, build_error> retain(shared_ptr<T>);

    expected<prepared_graph, build_error> prepare();
};
```

import_state 显式包含 initial_contents 和 resource_state，不提供隐含的“已有 sampled 内容”默认值。所有容器使用 utility::vessel；动作与内部共享对象通过项目 mimalloc allocator 分配。OOM 遵循既有 fast_panic，不引入异常恢复。

执行层的 append(fn) 是复用纯 CPU append 的描述入口：同步借出内部 CPU builder，调用完成即结束借用，不保存 fn。回调必须传播构图错误，不得保留 builder 引用、调用 compile 或向其导入新的外部资源。通过该入口捕获 append 半途失败并封闭整次构图，避免错误返回后调用方仍拿到可准备的半张图。提前冻结底层 builder 会使 prepare 正常失败。执行层不接受外部随意修改过的 compiled_graph。

```cpp
auto post_outputs = frame.append([&](render_graph::builder& graph) noexcept {
    return post::append(graph, post_inputs, post_options);
});
```

第一版不增加 pass-sink 泛型体系，也不要求每个模块实现两套 append。使用方式为：通过 append(fn) 追加，然后将返回的 pass_ref 绑定动作。export/order_after 的转发入口也检查状态并记住失败。将来确有重复需求，再增加“登记参数并绑定动作”的便利函数。

## 5. 导入、身份与引用

执行层通过现有 scoped_synchronization 描述实际对象并取得规范身份、mip/layer/aspect 或 buffer byte size，然后调用 CPU import 并保存 lease。跨设备、空 lease、失效 frame snapshot、缺少能力均在导入时失败。

同一规范身份重复导入时：kind、shape、初始内容和初始状态必须一致；一致则返回同一 ref 并保留第一次的规范绑定，不一致则 conflicting_import。重复名称不构成身份，也不能覆盖第一次声明。额外 wrapper/view 的所有权由其使用者显式保留。

推荐在帧顶层或资源包的导入助手中集中导入一次。模块接收 ref，不能将上游输出重新声明成另一份初始状态。初始内容 undefined 不代表初始布局一定 undefined：CLEAR 可以丢弃内容，但 warm 资源仍须携带此前访问的执行前沿。

ref 只在所属 builder 内有效。为 CPU builder 增加窄的 contains(pass_ref) 查询，检查 incarnation 和范围；bind_pass 不能只取 index()。不公开 incarnation，也不赋予 ref GPU 所有权。

当前 ref 表示资源而非某个显式内容版本。生产者必须先 append，消费者后 append；编译器在已建立的内容关系中排程，不支持任意倒序声明后自动猜测写入来源。多个场景可以依次追加到同一张图，使用各自的具名 refs 和资源包。

## 6. 参数、录制动作与生命周期

### 6.1 Concepts 与编译期边界

复用现有 render_graph::pass_parameters，保留 visit_resources(parameter_visitor&) const noexcept 的约定。新增 concepts 位于 execution 组合层，不扩张成泛型调度器，也不要求改造整个项目的 callable API。

以下定义为实现目标；头文件需要 concepts、expected、functional、memory、type_traits，项目类型通过现有模块导入。

```cpp
// 规范的拥有值类型；只要求实际需要的移动构造和析构，
// 不要求默认构造、拷贝、赋值或比较。
template<class T>
concept ownable =
    std::is_object_v<T> &&
    std::same_as<T, std::remove_cvref_t<T>> &&
    std::is_nothrow_move_constructible_v<T> &&
    std::is_nothrow_destructible_v<T>;

namespace detail {
    template<class T>
    struct graph_append_result : std::false_type {};

    template<class T>
    struct graph_append_result<std::expected<T, graph_error>>
        : std::bool_constant<std::is_void_v<T> || ownable<T>> {};
}

// 不保存构图函数：允许其为有状态／mutable callable，
// 以具名左值调用一次；返回必须是按值的精确 expected 类型。
template<class F>
concept graph_appender = requires(F& fn, render_graph::builder& graph) {
    { std::invoke(fn, graph) } noexcept;
    requires detail::graph_append_result<
        decltype(std::invoke(fn, graph))>::value;
};

// 保存录制器：通过 const 引用反复访问拥有的 recorder。
// const 调用允许发出 GPU 命令，不代表函数无副作用。
template<class F, class Parameters>
concept pass_recorder =
    ownable<F> && ownable<Parameters> &&
    render_graph::pass_parameters<Parameters> &&
    requires(F const& fn, Parameters const& parameters,
             rhi::command_buffer& commands,
             execution::record_context const& context) {
        { std::invoke(fn, parameters, commands, context) }
            noexcept -> std::same_as<rhi::error>;
    };
```

graph_appender 支持 expected<void, graph_error>，用于只追加副作用／交接 pass 的构图函数；其他返回值必须是符合 ownable 的输出结构。拒绝 bool、裸 outputs、其他 error 类型，以及 expected 的引用返回，避免隐藏错误转换和借用的局部结果。

append 不保存 F，因此允许引用捕获、mutable lambda、函数引用等；这不放宽禁止保存内部 builder 引用的约定。构图入口和适配 lambda 显式 noexcept，普通错误通过 expected 返回，OOM 仍按项目策略终止。给函数添加 noexcept 不会证明其内部安全，模块实现仍需遵守无异常约定。

pass_recorder 要求精确返回 rhi::error，拒绝 void/bool 等可转换替代；不使用 regular_invocable，因为录制本来就要修改命令缓冲区。Parameters 必须是规范值类型，同时满足已有资源访问枚举约定。这里只校验参数形状，绑定 pass 的实际 snapshot 类型仍由 prepare 检查。

bind_pass 使用 forwarding reference，在擦除前保存 remove_cvref_t<Recorder>。除 pass_recorder 外，再检查从本次 Recorder&& 实参构造存储值为 noexcept：支持 move-only recorder；左值只有在可无异常拷贝时接受，否则调用方须 std::move。不要求 recorder 可赋值或可默认构造。移动构造后 recorder 的存储位置固定，避免再次移动破坏内部 span。

retain 仅使用 shared_ptr<T> 到 shared_ptr<void const> 的转换约束，不另加 owner 概念层；允许现有 opaque retention token 和 const owner。裸指针、span、unique_ptr 或任意自称 owner 的类型不能直接传入此入口。retain 不承诺深 const，也不证明对象属于同一 GPU 设备。

### 6.2 运行期检查与所有权

bind_pass<Parameters>(ref, recorder) 将动作与原始 pass_ref 关联。Recorder 的 const 调用签名为：

```cpp
rhi::error operator()(Parameters const&,
                      rhi::command_buffer&,
                      execution::record_context const&) const noexcept;
```

Parameters 来自 CPU graph 已保存的 parameter_snapshot，不重新复制一份声明参数。prepare 对所有活跃绑定检查参数类型；类型不符在第一条图命令之前报错。无参数的纯屏障 pass 使用显式 bind_barrier_only，不能把漏绑定默认为空动作。

录制动作采用小型类型擦除：拥有 recorder 的不可变对象加调用 thunk，沿用已有参数快照的 allocator/类型标记思路。避免新增以 std::function 为中心的分配路径。不要在 public 接口暴露内部 erased storage。

编译前登记表按完整 ref 验证，编译后动作槽按原始 pass index 对应；execution_order 仅决定遍历顺序。模块局部序号、拓扑序位置和名称均不作为动作身份。重复绑定报错，禁止隐式覆盖。

Recorder 应拥有或共享拥有其 CPU 录制数据。额外 GPU 对象、views、描述符存储和上传资源可以由 recorder 持有 owner，或通过 retain 显式保留。捕获裸 this/span/指针不会自动延长生命周期，类型系统也不能证明这些捕获安全；生产适配必须审计。

现有 resolved_io 含有 span：需要在拥有它的 payload 地址稳定后建立这些 span，不能先在栈上解析，再移动含自指向 span 的对象。第一版允许 payload 中保存非 const RHI 句柄用于录制，但不允许动作借此修改图结构或发布跨帧状态。

prepare 后，准备结果保有 core、编译图、活跃资源绑定、活跃录制动作及显式 retained owners。被裁剪 pass 的专属动作可释放；全局 retain 对象保守保留。调用方仍须将 retention 放入帧槽，直到 GPU 完成。record 返回不代表可以释放资源。

## 7. 准备、录制与兼容

状态为 collecting → preparing → prepared 或 failed。第一次 prepare 即封闭 builder，无论成功失败均不得再次追加或重新 prepare；重新构建使用新对象。构图阶段的首个错误保留，后续调用返回该错误，禁止使用半构建结果。

prepare 顺序：

1. 检查此前构图错误并冻结，调用 CPU compile，固定启用 synchronization。
2. 得到活跃 pass、输出及实际需要的资源集合。
3. 检查每个活跃 pass 有且只有一个合法动作／显式 barrier-only 绑定，参数类型匹配。
4. 复用 prepared_graph::prepare 的设备、资源形状、usage/stage、屏障 lowering 和全批次 validate。
5. 封装不可变动作表及额外 owners，一次性交付 prepared_graph。

以上过程不发 GPU 命令。死 pass 无需动作；给错图 ref 或重复绑定的错误仍在登记时立即失败，不因后续裁剪掩盖。

prepared_graph 增加 record(commands) 入口，内部对每个活跃 pass 执行原有身份/命令检查、屏障批次、对应动作。任一失败立即返回；调用方放弃该次部分录制并禁止提交，不在同一录制中回退旧路径。

现有 prepare(core, compiled_graph, bindings) + record(commands, callback) 作为低层入口保留。没有预绑定动作的旧 prepared_graph 不能用无 callback 的 record；已绑定动作的准备结果不能通过 callback 重载覆盖其动作。两条入口共用内部遍历和屏障实现。

不推断 history_valid、shadow_reuse 或 submit 成功。需要跨帧发布的结果仍作为调用方的候选状态，在实际提交成功后发布；present 失败不能撤销已提交状态。

准备结果对应本次资源和初始状态，不作为跨帧结构缓存。runtime 每帧重新构建；底层低层入口的重录仍由调用方保证初始状态成立，不能因 prepared_graph 尚存就假设可跨 acquire 重放。

## 8. 输出、顺序与外部边界

模块返回结果，帧顶层决定 export 最终颜色、历史等真实外部结果。默认不 export 中间 GBuffer/HDR，否则会阻止跨模块裁剪。导出当前只建立内容根，不自动请求最终布局。

append 函数内部可以为真实非资源顺序约束登记 order_after；不在所有相邻模块间强加边，以免掩盖漏声明。GPU 时间标记仍附着于已有录制位置，不为标记给整条链添加 side_effect 根。

本轮不改变截图后的独立 finish 图；也不增加 partial-record 游标。未来截图 copy 成为显式图 pass 后，再将最终 handoff 合并。图外保留的 upload、host publication、AS 等协议必须有明确交接范围，不能声称本机制已覆盖所有访问。

## 9. 错误与诊断

build_error 区分 graph_error 与执行/RHI 错误，补充组合层的 wrong_graph_ref、duplicate_recorder、missing_recorder、parameter_type_mismatch、sealed_builder 原因。错误携带可获得的原始 pass/resource index、拥有的名称及底层原因，不借用 builder 内字符串。

图 dump 继续描述纯图，不序列化 callable 或指针。组合层错误应能指出哪个活跃 pass 未绑定，不要求增加通用反射。普通失败返回 expected，OOM 使用已有致命策略。

## 10. 文件组织与迁移切口

| 文件／模块 | 调整 |
| --- | --- |
| render_graph.cppm/.cpp | 增加 contains(pass_ref)，编译算法不变 |
| post.cppm | 抽出 refs/options → append → outputs，独立 compile 改为包装 |
| compose.cppm（新增） | execution::builder、导入与动作登记、准备编排 |
| execute.cppm | 复用内部录制循环，支持预绑定动作及 owners |
| runtime.post_graph.cpp | 生成稳定 payload，按具名 pass_ref 绑定，删除固定位置分发 |
| tests/CMake/CI | 组合构图、动作及生命周期测试 |

先迁移现有后处理来验证机制，保持 post_render_graph 默认关闭。独立后处理、Bloom-only 和既有纯 CPU 入口继续可用。通过验收后，下一项才是基础 GBuffer → lighting → post 共图；本设计不将其实现捆绑进来。

**2026-10-18 现状核对：** `post_render_graph` 这个配置键**已经不存在**——`config.example.toml`
里没有任何 render-graph 键（全文检索 "graph" 命中 0 次），图也不再可关：
"the composed render graph is the only recording path: there is no per-frame fallback any more"
（`source/application_configuration/application_configuration.cppm:255`）。所以"保持默认关闭"这句
不应再被当成现状或操作说明。表里 `runtime.post_graph.cpp` 一行只**部分**完成：绑定确实改成了
具名 `pass_ref`（`bind_pass(output->composite, …)`，`runtime.post_graph.cpp:906`），但录制载荷
仍是 21 槽的固定 `std::array` + `record(unsigned lane)`
（同文件 `:46-55`），"删除固定位置分发" 没有做完。GBuffer → lighting → post 共图已实现。

## 11. 验收场景

- 纯 CPU：两个 append 共享同一 ref，跨模块 RAW/WAR/WAW 正确；死模块能裁剪；仅最终 export 保留必要生产者。
- 导入：同物理别名一致去重，初始状态冲突失败，shape 来自实际对象，空/外设备/失效快照拒绝。
- 动作：插入额外 pass、拓扑重排后仍正确分发；死 pass 无动作可通过；活跃缺失/重复/错图同下标/类型不匹配在录制前失败。
- Concepts 编译探针：接受 noexcept appender、void 输出、move-only const recorder 和显式移动；拒绝错误返回类型、expected 引用返回、缺少 noexcept、mutable-only recorder、非 pass_parameters、引用／const 存储类型及不能从本次实参无异常构造的 recorder。使用 requires/static_assert 验证约束失败，避免依赖编译器诊断文本。
- 执行：barrier-only 不调用用户内容；回调错误停止后续动作；原有带 callback 入口仍工作；两种动作模式不能混用。
- 生命周期：销毁 builder 和调用方 payload 后 prepared 保有活跃 owners；retention 能跨 prepared 对象销毁保有 GPU 对象；最终释放无 cycle；span 不指向已移动的存储。
- 状态：封闭后拒绝修改，append 半途失败不产出可录制结果；失败不提交、不发布 warm/history。
- 生产：后处理三个 resolve × Bloom on/off，截图、新旧 PNG 对照、窗口 acquire/resize、同步验证和相关 ASan 回归；现有边界检查保持零。

自查：没有图内资源创建、没有把 ref 当内容版本、没有两套 post 构图实现、没有固定全图 pass 下标、没有默认空回调掩盖缺失绑定，也没有将录制成功当作提交成功。实现前仍需审阅此设计并另行制定实施计划。

实施说明：已完成设计确认及实施计划。由于 C++ named module 的声明归属规则，组合 builder 不通过跨模块 friend 操作 prepared_graph；由 execute 模块内的 detail::composition_access 连接两层，内部类型擦除存储不进入 prepared_graph 公共接口。空函数／成员指针是值域错误，append/bind_pass 在运行期拒绝并保留错误。

Concepts 不证明 ref 的 incarnation、设备归属、回调捕获的生命周期、资源访问是否完整，也不证明 noexcept 函数不会触发终止。这些仍分别由 contains/prepare、owner 保留和访问审计负责，不用模板约束替代运行期验证。
