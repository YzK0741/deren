# RHI image/view 父引用

## 范围

本轮为后续纹理组提供父资源拥有引用，不实现 assign/load texture group。
`rhi::object` 保持只有不可变 interface_type 的共同根，不增加引用计数、设备归属或通用 retain。
`object_manager<T>` 仍是 factory 引用的 move-only 拥有者，以 release() 平衡一次创建。

新增 ABI 29 接口：

```cpp
std::shared_ptr<rhi::image> image::share();
std::shared_ptr<rhi::image> image_view::get_image() const noexcept;
```

Vulkan owned image 的 view 在创建时取得父 image 的共享拥有引用。
get_image() 复制这份引用，保持原 RHI 对象身份；调用者可以先释放 factory 引用，也可以先释放 view。
同时存活的 share() 引用共用控制块；共享控制块消失后，仍存在的 factory 引用允许再次取得共享引用。
控制块 deleter 在 backend 内调用 release()，最后一次 backend 引用释放时才删除 image 包装对象和分配器引用。
view 先销毁 VkImageView，再释放父引用；image 只弱引用共享控制块，不形成拥有环。

借用 swapchain image 的 share()、其 view 的 get_image() 均返回空。
不支持共享拥有能力的 backend 使用空返回值，调用方必须检查；当前 CPU probe 不实现该能力。
交换链 view 仍须在交换链重建前释放。api_core/device 和 backend DLL 必须覆盖所有资源引用的生命周期，
尤其不能在 DLL 卸载后再销毁其共享控制块的 weak_ptr。
本轮不改变提交资源的 GPU 保活机制；CPU 共享引用本身不能证明 GPU 使用已经完成。

## 验证

- 红灯：添加最小空实现后，真实设备测试在“owned view 必须保留父引用”断言失败，21 checks / 1 failed。
- 绿灯：最终真实设备测试 37 checks / 0 failed，覆盖原对象身份、控制块共用与重新创建、
  factory/view/父引用分别释放后继续使用、最后共享引用消失，以及借用交换链分支。
- 完整 Release 构建通过；CTest 20/20 通过。
- Full 渲染回归 14/14 匹配冻结 hash，两次运行无不确定结果，检查未发现 validation/log 问题。
  脚本显示的 12 项 CHANGED 是旧本地 baseline；本轮 verdict 取 frozen 比较（退出码 0）。
- 独立只读代码审核未发现需修改的问题。
