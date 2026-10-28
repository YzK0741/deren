# CI 收尾与暂停检查点

用户要求：完成 CI 后暂停，不再推进后续 subtilis 实现。

**状态：CI 已全部通过，已按用户要求暂停。**

- 基础提交：`508769d5a8ad293f0e36fd4c42fc7e3c2cf92466`，已推送到 `origin/master`，包含用户此前七项提交。
- CI 收尾提交：`d928555020952b1478b0b5b44eb1c7973ee2e19d`，已推送。修复 fixture 遗漏 io.cmd，以及 acceleration_structures 构建清单遗漏；默认 CPU 模式不再强制构建设备 DLL，设备模式需显式构建后端。
- 首轮 [39311962445](https://github.com/YzK0741/deren/actions/runs/39311962445)：Release/Debug/格式/tidy/POSIX 通过，sanitizer 测试暴露上述两处问题。
- 最新远程 CI：[39342839687](https://github.com/YzK0741/deren/actions/runs/39342839687)，整体 `completed / success`。三个 job 均成功：POSIX；ASan + UBSan；Release 构建/CTest/Debug 完整构建/clang-tidy（含格式、边界与运行包步骤）。已核对 head 为 `d9285550`。
- 本地最终：完整 Release 构建、53/53 测试；完整 CI 等同 ASan/UBSan 52/52；31 个 CI 清单源文件的 clang-tidy（warnings-as-errors）通过。
- 交付范围：验证后的底向上唯一父森林资产、保守确定性 CPU cut、M1a 完整 run 发布/回退；真实简化 builder、多级几何上传尚未实现。
- CI 修复：统一 MinGW/Clang BMI 的可选容器标注策略，保留编译器 ASan/UBSan；修复定点静态检查告警并移除已删除 better_pmr.cpp 的过期清单项。
- 记录：[实现与验证](2026-10-20-subtilis-m1b-foundation.md)、[计划](../superpowers/plans/2026-10-20-subtilis-m1b-foundation.md)。

最终结果已通过 GitHub CLI 核对。检查点仅保存在本地，不为记录结果另触发一次 CI；本轮未继续简化构建器或运行时多级上传。最后额度检查为 5h 已用 60%、剩余 40%，这次暂停依据用户指令。

恢复入口：固定 meshoptimizer 版本与许可证，先做保持 primitive/材质边界、锁区域外边界的索引简化构建器；通过 asset::make 后再考虑全驻留上传与视觉/性能验收。

保留未跟踪的 `.vscode/` 与旧 `2026-10-19-ci-checkpoint.md`，未纳入本轮提交。
