# RHI object 具体实现名称

## 接口与命名

ABI 30 将 `object` 的接口枚举改为公开、只读的具体实现名称：

```cpp
class object {
public:
    std::string_view const type;
protected:
    explicit constexpr object(std::string_view name) noexcept : type(name) {}
    ~object() = default;
};
```

删除 interface_type、interface_id、extension_interface_type() 和 type() getter。
每个 RHI 接口的构造函数要求具体实现提供名称；项目中的实现都使用静态字符串，
不分配字符串内存，也不依赖 DLL 两侧的字符串地址相同。对象仍通过具体接口的 release() 销毁。
名称的存储必须覆盖对象的生命周期，backend DLL 同样须覆盖其资源的生命周期。

常见名称：

| 实现 | type |
| --- | --- |
| Vulkan 根 | deren_api_core_vulkan |
| Vulkan owned image | deren_image_vulkan |
| Vulkan borrowed swapchain image | deren_frame_image_vulkan |
| Vulkan image view | deren_image_view_vulkan |
| Vulkan owned command buffer | deren_command_buffer_vulkan |
| Vulkan borrowed frame commands | deren_frame_commands_vulkan |
| 静态/DLL probe 根 | deren_api_core_probe |
| CPU probe image | deren_image_cpu_probe |

成员使用普通命名，无尾缀下划线：object_manager 的 owned_ 改为 owned；
Vulkan core 的 swapchain_view_ 改为 presentation_surface，避免与嵌套类型同名。
本轮 RHI 与 Vulkan core 中不再存在带尾缀下划线的成员。

Vulkan 中需要具体布局的转换使用具体名称进行检查，原有指针身份、image 注册表及设备归属检查保留。
名称不能证明设备归属、资源仍存活或任意输入的内存布局；后端仍须遵守 RHI 参数和能力查询契约。
extension_kind 继续用于能力请求；query_extension 的 typed helper 检查返回能力的 kind。
structure_type 继续描述参数结构的 next 链，与对象具体实现名称是不同的用途。

## 验证

- 红灯：旧实现的静态和 DLL 根都缺少 type 成员，新断言各失败一次（246 checks / 2 failed）。
- 新契约的只读 string_view 类型在编译时验证，静态/DLL 名称比较在 CTest 中验证。
- 完整 Release 构建通过；CTest 20/20 通过。
- 实际 Vulkan 资源测试 46/46 通过，覆盖具体类型名称、父 image 保活、借用 image、
  外来 image_view 在后端转换前返回 invalid_argument。
- 光追原生描述符堆 81/81、映射数组 84/84 通过；GPU 返回命中结果符合预期，
  外来 pipeline 被拒绝，validation 未报告 VUID。
- 独立只读审核未发现需修改的正确性问题；审核指出的旧成员尾缀已清理。

- Full 渲染回归 14/14 匹配冻结 hash，无不确定结果，validation/log 检查通过。
  输出中的 12 项 CHANGED 来自旧本地 baseline；冻结比较的 verdict 为 PASS，退出码 0。
