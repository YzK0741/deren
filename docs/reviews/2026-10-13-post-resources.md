# TAA history / LDR / Bloom 资源迁移 — 2026-10-13

本阶段完成，可正常构建和运行；未提交或推送。此前各阶段未提交修改保留。

## 实现

- output_targets_pass 扩展至13种图像：已有7种加TAA history、LDR、4级Bloom。runtime 对新增6种图像只持有borrowed handles和共享snapshot，不再自行分配。
- TAA history使用history生命周期，sampled/transfer-destination flags，不按帧清零；现有resolve后HDR→history拷贝与shader-read回交不变。
- taa_pass也声明同一history请求，与共享生产者合并到一个generation；bind_resources只在history generation变化时reset_history并清除history_written，普通reconcile保留历史。
- LDR和Bloom使用output_size生命周期。Bloom独立物理id追加在既有enum后，旧bloom family、heap槽位/stride和shader保持不变。
- Bloom宽高按max(1,render_extent>>(level+1))创建；runtime extent helper统一接受image指针，继续读实际图像extent，不复制尺寸公式。
- admission改为比较各类型的expected lifetime和expected extent；尺寸变化、producer切换、按槽retention和分配失败回滚沿用上一阶段规则。
- 其他图像、AS/imported资源、自动descriptor发布和自动barrier仍未迁移。RHI ABI32不变。

## 验证

- history heap绑定回归在实现前3checks中1项失败，实现后通过。
- post_resources42checks通过，含history分类和begin_frame保留、TAA history_valid首次0/随后1、替换generation后回到0且复用不再次reset、非法history分类拒绝、Bloom奇数尺寸/1x1下限和分配失败回滚。
  TAA行为测试使用portable command echo读取push参数，不声称执行真实shader。
- 扩展后的output_resources151checks、首帧失败重试7checks、resource schema603checks均通过。
- 完整Release构建成功；CTest24/24，25.16秒。原生Vulkan runtime1164checks通过。
- 十组前后捕获，各40帧，0.5度/帧sweep：deferred、shadow_single、transparent_blend、sponza、32灯clustered on/off、toon、TAA-on、0.67渲染比例TAA-on、TAA+FXAA。
  全部RGBA像素一致，全部同步验证干净。Bloom使用应用默认开启状态。
- 真实PBR场景启用TAA/FXAA，5秒后最大化、8秒后恢复并立即请求捕获；两次resize干净，输出1280×720匹配client尺寸，截图已检查。
  该检查使用timed脚本，不以Gaussian日志伪报渲染帧数。
- Gaussian CPU/GPU各12帧通过且验证干净；保留44差异像素、最大5/255、HDR RMSE2.5238316397326248e-5。此为两路径比较，不是两版本比较。
- 独立源代码审查覆盖generation retention、history替换/复用、partial fallback、尺寸与索引、采样/拷贝barrier，没有具体剩余发现。
- git diff --check通过。

## 记录与下一步

- 持久证据：docs/reviews/2026-10-13-post-resources/，含配置/日志/PNG、像素统计、脚本、CTest及SHA256。
  大型HDR EXR仍在工作目录，归档保留统计。
- 工作/冻结基线：build-release-dyn-clang64/post-resources-check/及runtime.before；基线已包含上一阶段G-buffer/HDR迁移与首帧错误路径修复，并带同一toon FGD LUT。
- managed_resources.md及render_resource_inventory.md已更新；管理文档仍在Doxygen手册输入中。
- 下一阶段建议MegaLights trace/resolve/history与RT visibility。随后再处理共享场景缓冲、Gaussian缓冲与AS/导入资源。
- 当前没有影响正常运行的半成品；不要整体重置工作区。
