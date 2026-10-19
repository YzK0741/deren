# RHI 共享所有权换成契约自己的容器（ABI 44）

日期：2026-10-18 · 批次：shared-object-migration · 提交：`1a317ca9`（容器替换，ABI 仍 43）+ `545773b7`（共享所有权迁移，ABI 44）· 前置：`2026-10-18-unique-ptr-factories`（ABI 41；`1a317ca9` 无独立 record）

## 决策：RHI 共享句柄不再用 `std::shared_ptr`

契约自己定义两个受 `object` 约束的容器（模板参数在管理成员上 `static_assert`，因为 `image_view` 有一处前向声明点）：

- `unique_object<T>`（上一批完成）：工厂返回的 one-reference RAII，析构调 `release()`；
- `shared_object<T>`（本批启用）：一个控制块，工厂那一个引用在最后一个拷贝处 `release()` 一次。提供：
  - 拷贝/移动、`get`/`operator->`/`*`/`bool`/`use_count`/`unique`/比较；
  - `borrow(T&)`：后端拥有的指针，不 drop 引用；
  - 嵌套 `weak`：`lock()`/`expired()`/`use_count()`，观察生命周期；
  - 别名构造：把借用的 RHI 指针绑定到引擎 owner 的 `std::shared_ptr` 生命周期（引擎 lease 的 `shared_ptr{chain, raw}` 形态；这里的 `std::shared_ptr` 是**引擎 owner 的类型**，不是 RHI 句柄——RHI 句柄上不再有 `std::shared_ptr`/`weak_ptr`），也支持绑定到另一个 `shared_object` 的控制块；
  - 派生 → 基类转换（后端具体对象 → 契约接口）。

**根对象也遵守同一规则**：`api_core` 增加 `virtual void release()`，`denen_make_api_core` 返回 `shared_object<api_core>`；后端入口 `new core` + `attach_self`，最后一个句柄 drop 时 `core::release(){ delete this; }`。probe 的注入工厂同样改。

## 迁移范围

| 面 | 迁移 |
| --- | --- |
| 契约 | `image::share()`、`image_view::get_image()`、`frame_resources::retain_image()`、`make_command_buffer()`、`mergeable_command_buffer_ref`、`texture_group_ref`、`api_core` 入口 → `shared_object`；新增 `api_core::release()` |
| 后端 | `core` 去掉 `enable_shared_from_this`，改 `self_owner` weak + `attach_self` + `release()`；`frame_snapshot` 用 `shared_object<frame_snapshot>::weak self` 自管、`keep_alive` 持有 root 共享句柄；`frame_resources` 从 root weak 取 keep-alive；`owned_image::share()` 复用同一控制块；`make_command_buffer` 由共享句柄直接持有工厂引用 |
| 引擎 lease | `resource_cluster_detail::owner<T>` 增加 `shared_object<T> shared` 依赖；`resource_lease::from_shared` 接受 `shared_object<T>`；`render_graph` 增加 `retain(shared_object<T>)`；runtime publication 容器换成共享句柄；frame command buffer 用 `borrow()` |
| probe / 测试 | probe root/snapshot/refs 全部换 shared 语义；fake `api_core` 补 `release()`；测试 weak 观察点换 `shared_object<T>::weak` / `shares_control`（替代 `owner_before`） |

## 三个被 ASan/门禁抓到的真实缺陷

1. **别名控制块的 drop 对象错误**：起初 `reset()` 用 `this->instance`（别名目标）作为 drop 的入参；一个「借用的 command buffer + api_core root 控制块」的别名会把 command buffer 当 root delete。控制块现在自己持有 `owned` 指针，drop 永远作用于它。
2. **drop 期间 weak 释放控制块（UAF）**：对象的析构可能销毁最后一个 `weak`（例如 root 的 `self_owner`），它看到 `strong==0 && weak==0` 就把控制块删了，随后 `reset()` 继续访问控制块。控制块增加 `dropping` 标志：drop 期间 weak 不删控制块，由 drop 之后的 `reset()` 删。**这两个缺陷分别被 ASan 的 heap-use-after-free 报告定位**，修复后 8 个 teardown 崩溃的测试全部恢复。
3. **`frame_snapshot::release()` 还是旧的 shared_ptr 语义（panic）**：快照的 owner 现在就是
   `shared_object`，最后一个拷贝 drop 时会调用 `release()`，旧实现把它当成误用而 panic；改为
   `delete this`（析构本来就从 core 的集合注销）。app 的 15 个 capture 因此恢复（这一条由像素门禁
   抓出，不是 ASan 报告）。

## ABI

**43 -> 44**：契约的虚拟返回类型从 `std::shared_ptr<T>` 换成 `shared_object<T>`（签名形状变化），`api_core` 追加 `release()` 虚函数。历史写入 `rhi.contract.cppm`，三个 ABI pin（`test_dynamic_link` / `test_acceleration_structures` / `test_runtime_dyn`）同步 44。已退役的值、POD 布局、工厂描述符不动。

## 验证

- `ctest`：**49/49 passed**（含此前因容器缺陷崩溃的 8 个测试）；
- 真设备：`test_texture_group` **88**、`test_runtime_dyn` **1167**、`test_acceleration_structures` **495**、`test_vulkan_capabilities` **39** checks PASS；`test_gui_plugin` **35**；
- 冻结像素门禁：完整 OFF gate **15/15**、`-Visibility` **PASS**（修复第 3 个缺陷后 app 的 15 个 capture
  全部恢复）；
- doxygen **0 warnings**；RHI validation audit、backend boundary 通过；
- spike 89 checks，只剩既有 `present()==not_ready` 失败（无 native_window，与本批无关）。
