# 共享场景表迁移 — 2026-10-14

九类缓冲已交给scene_tables_pass/passchain：material、instance、toon colour/lanes/rig、meshlet、mesh indirect commands、statistics与culled table。
之前camera/deformation迁移保持；这批仍未提交。

## 实现

- 构造时先准备受管资源，再写入默认toon值并注册material0。默认值初始化逻辑保留，runtime原分配调用移除。
- runtime保留borrowed handles/mappings和九个snapshot；应用与兼容链独立补齐类型，匹配producer合并。
- 所有表为单一全局allocation；后三类内部保留frame regions和既有shader/indirect offsets。indirect usage不变且无heap slot。
- material/instance schema改为实际device-wide，新增七个id追加在末尾；RHI ABI32与旧id/heap布局不变。
- admission在acquire/heap采用前检查类型、scope、lifetime、容量、mapping、flags及reset。新generation等待idle后完整复制旧内容；正常复用不清零/复制。import记录、toon默认值和累计counter均保留。
- frame resource table按instance0发布全部九类；提交snapshot继续保证退役顺序。

## 验证

- 基础回归RED4/27；扩展后GREEN178/178，覆盖九类所有权/heap、frame table发布、初始及尾部数据、重复producer、复用、handover/retirement、非法描述与创建失败恢复。
- resource schema608/608；完整Release构建成功，CTest24/24，20.03秒；真实Vulkan runtime1164/1164。
- 15场景各40帧，0.5度/帧环绕；动画使用固定0.025秒/帧。候选与冻结包RGBA全部一致且同步验证干净，含toon、透明、cluster、TAA、RT、MegaLights及动画skin refit。
- ML+RT+0.67比例+TAA最大化恢复两次通过，1280×720输出匹配client，无同步错误。
- 独立只读审查无具体代码缺陷；过时scope文档/注释已修正。diff检查通过。

证据：[2026-10-14-scene-tables](2026-10-14-scene-tables/)，含PNG、配置/原始日志、像素统计、脚本、CTest及SHA256。
PNG仅无损重压缩IDAT。工作/冻结包：build-release-dyn-clang64/scene-tables-check/runtime.before。
冻结包已包含camera/deformation和debug HDR同步修复；当前capture摘要记录采集时二进制。之后删除了不影响行为的旧scope注释。

持续目标下一批为Gaussian动态缓冲。共享纹理/采样器和RT结构仍未迁完，不能把全部pass迁移目标标为完成。
