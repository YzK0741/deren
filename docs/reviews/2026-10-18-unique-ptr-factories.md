# 资源工厂返回 unique_ptr（ABI 41）

日期：2026-10-18 · 批次：unique-ptr-factories · 前置：`2026-10-18-escape-device-handles`（ABI 40）

## 决策

资源创建不再让调用方记得"调用 release"：`rhi.api_core` 的每个 `create_*` 工厂
（swapchain / buffer / image / acceleration structure / micromap / sampler / shader / pipeline / query /
command buffer）和 `image::make_view` 都返回 **`rhi::object_ptr<T>`**，即

```cpp
template <typename object>
struct release_deleter {
    void operator()(object* const handle) const noexcept { if (handle) handle->release(); }
};
template <typename object>
using object_ptr = std::unique_ptr<object, release_deleter<object>>;
```

- 只有一个生命周期机制：**RAII 容器析构时调用契约的 `release()`**（release 不是 delete，backend 自己回答引用计数）。
- 调用方想要共享所有权就自己转：`std::shared_ptr<object>(std::move(owned))`——这是 C++ 标准库的答案，
  契约不再提供第二套所有权词汇。
- 删除了手写的 `object_manager<T>` 类（它之前承担同一职责、但多了一个需要解释的名字）；重命名为
  `object_ptr` 并直接变成 `std::unique_ptr` 别名。没有 zombie/generation/ownership 标记等新系统。

`make_command_buffer()` / `make_mergeable_command_buffer()` 仍然返回 `std::shared_ptr`：它们是**录制句柄**，
frame/engine 会长期共享它们（batch 的 retain 也基于共享引用）；共享所有权在这里是需求而不是可选项。

## 借用视图不再是"可容忍的释放"

上一版对借用视图（frame image / frame read-back buffer / frame command buffer）的 `release()` 是
"一次性 log + no-op"——这是对错误生命周期的容忍。现在工厂产品都有 RAII 容器，正常代码没有理由对视图调用
`release()`；把它当作前置条件违约：

- `frame_commands::release()`、`frame_image_slot::release()`、`frame_readback_slot::release()` 和
  `frame_snapshot::release()` 改为 `deren::utility::panic("...")`；
- `frame_image()->make_view()` 的**视图**仍是工厂产品（返回 `object_ptr<image_view>`），由容器释放。

## 迁移

- Vulkan backend：13 个工厂 + `make_view` 的返回类型；内部"backend 自己持有时"用
  `create_buffer_raw()`（`create_buffer(...).release()`）继续持有裸句柄；
- probe / 测试替身：工厂 override 返回 `object_ptr`，拒绝路径 `return {};`；
- engine/app：调用点去掉 `object_ptr<T>{...}` 包装（工厂已经返回容器），需要裸指针的地方 `.get()` /
  `.release()`；pipelines/resource_cluster 的所有权转移保持原来的语义；
- 测试：`test_acceleration_structures` 原先在末尾对裸指针显式 `release()`，迁移后容器已负责，删除这些
  双重释放（这是本批 device 路径唯一的功能性改动；发现时表现为 teardown 的 heap corruption）。

## ABI

**40 -> 41**：virtual 的返回类型是 ABI 的一部分，11 个工厂 + `make_view` 的返回类型变化 = 一次 bump。
历史写入 `rhi.contract.cppm`，三个 pin 同步 41；没有 by-value POD 变化，没有槽位移动。

## 验证

- `ctest`：**49/49 passed**；
- 真设备：`test_acceleration_structures --with-device` **506 checks PASS**（含修复后的 teardown）、
  `test_texture_group` **87**、`test_runtime_dyn` **1167**、`test_vulkan_capabilities` **39**；
  `test_gui_plugin` **35**；
- 冻结像素门禁：完整 OFF gate **15/15**、`-Visibility -Compare` **PASS**；
- doxygen **0 warnings**。

## 已知非本批问题

`test_backend_boundary_spike --with-device` 仍有一项既有失败（无 `native_window` 时 `present()` 返回
`unsupported`，spike 期望 `not_ready`）；本批未触碰该断言，其余 105 checks 通过。
