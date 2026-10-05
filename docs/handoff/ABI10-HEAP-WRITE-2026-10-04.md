# 阶段 1b：原生描述符写入

接续 `2e6efff`。ABI9 →10：vulkan_escape 追加 image/buffer heap write；不增加 C 导出入口，不宣布尚未实现的 portable descriptor_heap 能力。

- image 参数是 `vulkan_heap_image_desc` 值结构，包含结构大小、借用的 VkImage、flags、view type、format、四个 swizzle、aspect 和 mip/layer 范围。整数保留 Vulkan 公开枚举值，不把未知格式默认成 RGBA。
- 引擎从 VkImageViewCreateInfo 逐字段转出，后端逐字段重建；保留 VK_REMAINING_*、flags、component swizzle 和 descriptor type。当前产品调用都是 pNext=null，非空扩展链或错误 sType 明确返回 false，不能静默丢字段。
- buffer 传 offset/address/size/type；两类写入直接返回原来的 heap.write_* 结果，保留真实就绪和范围失败。
- 描述符/image 指针仅在同步调用中借用；写入不取得资源所有权。GPU 使用期间资源寿命仍由原有拥有者负责。
- rhi_face getter 标为 const：它只借用原有 core 引用，支持原本已经是 const 的 write_heap_buffer，未去掉录制方法的 const 约束。

验证日志位于 `D:\deren-workspace\work\abi8-followup`：

1. 新接口 producer RED：exit1，两个 pure virtual 缺实现；heap-write-red.log。
2. 首轮 GREEN 编译发现 const caller 的 getter 限定问题；修正后最终全目标构建 exit0；heap-write-green-final.log。
3. 新鲜边界/ratchet exit0，32 →30，删除 write_image/write_buffer，0 owning-STL；boundary-heap-write.json。
4. git diff --check exit0。

没有执行测试程序或 GPU。字段映射为静态核对，实际 Vulkan 描述符写入、拒绝路径和渲染仍需异机验证。二级命令缓冲录制/继承和 heap push/bind 保持原路径，留到下一独立批次。
