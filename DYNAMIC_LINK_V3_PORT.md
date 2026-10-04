# Codex v3 成果移植方案

> 依据：`github.com/machines-6657/deren` 的 `origin/codex/dynamic-link-v3`（7 个提交，`Codex <codex@local>`）。
> 本地检出于 `C:\Users\23530\Desktop\yzk\dl\deren`（裸 clone，工作区干净）。
> 所有"可落性"结论都有 `git apply --check` 实测；没跑的标**未验**。

---

## §0 一句话

**它的成果不是一条要合并的线，而是一组可以按内容移植的补丁**：四个真缺陷修复 + 一台更硬的门 + 一条加载器校验，我们**不合并分支**（无共同祖先），而是**逐条落补丁、逐条给见证、逐条过门**，因为它那份快照的基线**就是我们这棵树**（ABI 6），所以它的 diff 对我们的上下文基本有效（三条补丁里只有 3 个文件冲突，且都是我们自己改过的文件）。

---

## §1 对象与依据

| 它的提交 | 内容 | 我们是否要 |
|---|---|---|
| `948cc1a` | `load()` 拒绝空串与含 NUL 的路径 + 2 组回归 | ✅ 要 |
| `11fd95d` | 门重写（joiners 即失败、`--update` 只降不写、`--initialize`、`--app-object` 应用证据、反向依赖、过期消费者、原子写、`--report`）+ 27 个回归 + 基线 77→66 + CMake/CI 接线 | ✅ 要（**最高价值**） |
| `ab14ca6` | `main.cpp` 的 `application_window` RAII + `rhi.extension.cppm` 取消 `deren_ext_<ability>_v1` 要求 | ⚠️ 只要后半+部分前半（我们已有大半） |
| `ec39712` | **F1–F4 缺陷修复** + `tests/test_vulkan_capabilities.cpp`、`test_texture_upload_layout.cpp`、`generate_vulkan_query_fixture.py`、`vulkan/core/vma/texture_upload_layout.h` | ✅ 要 |
| `8a75911` | V3 方案 + P0 结果 + 在我们 `DYNAMIC_LINK_V2.md` 顶部加的评审修订说明 | 📖 读，决策后并入 |
| `9a650f3` | 保存我们给的 ABI 6 快照 + 从上游补 31 个文件 | ⛔ 无需（我们的树本来就是完整的） |
| `aaa99cb` | **v3 正式后端包架构**（80 文件 / +4768 行，自己标注 compile FAILING） | ⛔ 不移植，转作步 ④ 输入 |

**它的独立价值**：在**另一台机器**上复现了我们的测量 **66 symbols / 180 sites / 3 consumers / owning-STL 0**，并把基线棘轮到 66；另外它明确把"棘轮通过"与"翻转完成"分开（`--require-zero` 在 66 符号的树上按设计拒绝）。这是对我们度量的**外部验证**，保留引用。

---

## §2 前置（必须先做）：把我们的已验证状态提交掉

现状：**36 个修改 + 35 个未跟踪**，其中包含方案、尖刺、验证报告和 36 个文件的引擎迁移。移植是在这棵树上做增量修改——**没有提交就没有回滚点，也无法把"移植引入的问题"和"我们自己的问题"分开**。

建议拆成逻辑提交（顺序即依赖顺序），每笔提交后 `ctest` 14/14：

1. `contract: release() + object_manager + buffer_desc/buffer_usage/buffer_flag + device_address 拆分 + native_buffer`（abi 1→6 的契约侧，含 `rhi.core_desc.cppm`、`backend_entry.hpp`）
2. `backend: create_buffer 真实现 + owned_buffer + 设备地址能力 + native_buffer + 加载器 detach()`
3. `engine: buffer 面迁移到契约`（`vulkan/runtime/**` 与资源侧四模块）
4. `tests: 边界尖刺 + 加载器 detach 证明`
5. `docs: DYNAMIC_LINK_V2.md §15–§19.1 + docs/rhi/VERIFY_buffer_face.md + README Requirements`

> 提交信息记录基线：`HEAD 8eaaa6a + 上述工作树`，`deren.exe sha256[:16] CF323C5ECCC58093`。

---

## §3 可落性实测（`git apply --check`，未写任何文件）

补丁生成：`git -C <dl clone> diff <commit>^ <commit> > <commit>.patch`，然后在我们的仓库 `git apply --check <patch>`。

| 补丁 | 文件数 | 结果 | 冲突点 |
|---|---|---|---|
| `948cc1a` | 2 | **exit 0** | 无 |
| `11fd95d` | 5 | exit 1 | 仅 `.github/workflows/ci.yml:101`（我们加过 warn 阶段的步骤） |
| `ec39712` | 10 | exit 1 | 仅 `CMakeLists.txt:849`（我们加过尖刺 target 等） |
| `ab14ca6` | 2 | exit 1 | `main.cpp:436`（**它的 main.cpp 是从上游 ABI 1 补回的**，与我们不同）；`promise/rhi/rhi.extension.cppm` 那半**可落** |

结论：**三条主要补丁可以"排除冲突文件后整块落地"，冲突只在我们自己改过的两类文件上**（CI/CMake 的接线、`main.cpp`）。这也说明移植的主要成本不在代码，而在**接线与见证**。

---

## §4 分步移植（每步：来源 / 落法 / 见证 / 门 / 回滚）

### P1 门硬化（`11fd95d`）— 最高优先

- **落法**：`git apply --exclude=.github/workflows/ci.yml 11fd95d.patch`，然后手工把 CI 那一步并入我们现有的 ci.yml（我们那一步是 warn 阶段，它的是强制；**合并结果应当只有它的强制版本**）。
- **基线文件**：补丁会把 `scripts/backend_boundary_baseline.mingw.json` 换成它的 66。**不得直接信任**：mangled 名字跨工具链不可比（脚本自己的注释就写了这条）。落完后立即在我们树上跑门；若报名字不匹配，就在**我们的**归档上重新 `--update`，并在方案里记下原因。
- **见证**：
  1. `python -m unittest tests/test_backend_boundary.py -v` → **27/27**（它的回归含 4 例"把后端归档冒充应用消费者"，修前 RED）；
  2. `check-backend-boundary` → `66 / 180 / 3 consumers / 0 owning STL`；
  3. 三条**反向见证**（证明门是门而不是装饰）：joiners 必须失败；`--update` 在集合未收缩时**拒绝且不写文件**；`--require-zero` 在 66 符号的树上**按设计拒绝**。
- **门**：全目标构建、`ctest`、`clang-format-check`、渲染哈希不变。
- **回滚**：单文件为主，`git restore` + 基线 JSON 还原。

### P2 加载器空串/NUL（`948cc1a`）

- **落法**：整块 `git apply`（**exit 0**）。
- **见证**：它的 2 组回归 + 我们已有的 `test_a_detached_library_is_not_unloaded` 全过；`test_dynamic_link` 检查数从 115 增至 119+。
- **为什么值得**：Windows 上路径含 NUL 会被 API 静默截断，**可能加载到另一个 DLL**——这是"拒绝而不是猜"的典型。

### P3 F1/F2/F3（`ec39712` 的 `init_utils.cppm` + `core.constructor.cppm` 部分）

- **落法**：从 `ec39712.patch` 里**只取** `vulkan/core/init_utils/init_utils.cppm`、`vulkan/core/core.constructor.cppm`、`tests/test_vulkan_capabilities.cpp`、`tests/generate_vulkan_query_fixture.py`（`CMakeLists.txt` 手工接线）。
- **缺陷内容**：F1 untyped 功能节点被挂在可选 micromap 之后；F2 属性链固定挂在可能不可达的前置节点后（RT handle size / AS scratch alignment 读回 0）；F3 选卡不检查强制能力，选完才在创建阶段崩。
- **见证**：CPU 回归 **37/37**（它用 fixture 替换 Vulkan 查询入口，从**真实源码**读查询/选卡实现，因此测的是产品代码而不是复制的算法）。
- **证据边界（必须照实写进文档）**：**本机无法行为验证** F1/F2/F3——我们的设备（NVIDIA 616.92）该有的都有，缺失路径根本不发生。这三条的效果只能在**别的设备**上显现；本机能给的证据止于 CPU 回归 + 我们自己的门不变。
- **门**：全套（构建/ctest/格式/边界/渲染哈希/尖刺）。

### P4 F4 压缩纹理上传布局（`ec39712` 的 vma 部分）

- **落法**：取 `vulkan/core/vma/vma.cppm`、新增 `vulkan/core/vma/texture_upload_layout.h` + `tests/test_texture_upload_layout.cpp`（同样手工接线）。
- **缺陷内容**：把"每块字节"当"每像素字节"——BC1 4×4 被算成 128 字节（应为 8），BC4 更离谱；另有 BC4/ETC/EAC 分组错误。
- **见证**：CPU 回归 **78/78**；另加两条我们能自己给的**算例锚点**（4×4 BC1 = 8 B；5×7 BC1、2 layers、3 mips = 96 B，各 mip 起点 0/64/80）。
- **必须同时查证并写明**：我们的 14 个门场景里**是否有任何材质使用压缩格式**。它自己诚实标注"没有证据表明当前材质用压缩格式"，因此 **F4 不能解释任何现有失败**；我们若查证同样结论，就照写，不把它包装成"修了一个渲染 bug"。

### P5 窗口所有权与能力入口（`ab14ca6`）— 拆成两件，分别裁决

- **P5a `rhi.extension.cppm`（可落的那半）**：取消 `deren_ext_<ability>_v1` C 函数表要求，一致性门改为逐位查 `query_extension()` 返回可用对象。
  **这是我们对 §6 裁决 #1 的答案候选**，但它动契约文件 ⇒ **单独一笔提交**，并在 `rhi.contract.cppm` 里记一句"是否跳 abi"的判断（删掉的是**要求**而非虚函数，按规则不跳；但要写明理由）。
- **P5b `main.cpp` 的 `application_window` RAII —— 已比对，结论：不移植它的类。**
  实测我们树里这条不变式**已经被强制**：`main.cpp:115` 的窗口对象先于 `runtime`（:543）构造、后于它析构；`core_options.native_window = window.window` 只借不拥有，`rhi.core_desc.cppm:64` 写明 `native_window` 是借用；`main.cpp:91-92` 的注释已经写了"runtime 还活着时拆窗口（或 GLFW）会把 surface 从它脚下抽掉"。它的类与我们的等价，移植只会产生**第二套表达**。
  **只取一行**：`#define GLFW_INCLUDE_NONE`（我们 `main.cpp` 里没有）。它让 `glfw3.h` 不再去拉 `<GL/gl.h>`——我们不链接 OpenGL，这行把"不需要 GL 头"写成事实而不是靠环境碰巧有。
  它的 `main.cpp` 与我们不同源（从上游 ABI 1 补回），**整块落必然错**。

### P6 文档与决策

- 把它的三条 V2 口径修订**逐条裁决**并写进我们的方案：
  1. **"计数棘轮即翻转证明" → 接受它的批评**：棘轮通过 ≠ 翻转完成，翻转门必须是 `--require-zero`（P1 落地后成为事实）；
  2. **"EXE 导入三个入口"**、**"PRIVATE 切断静态库传递依赖"** → 逐条给出我们的实测立场，接受或反驳都要写理由；
  3. 记录 provenance：每笔移植提交的 message 里写来源分支 + commit（`codex/dynamic-link-v3 @ <sha>`），便于审查与回话。
- `README.md`：本轮已修一处（`VK_KHR_unified_image_layouts` 与 `VK_EXT_host_image_copy` **是硬要求**，我们自己的 panic 就是证据；同一 GPU 型号 616.92 可跑 / 560.70 三项全缺并具名 panic）。

---

## §5 不移植与另办

| 东西 | 处置 |
|---|---|
| `aaa99cb` v3 正式后端包架构（80 文件，compile FAILING） | **不移植**。它是一条不同架构；采用等于丢掉我们已验证的迁移。**转作步 ④ 输入**：`cmake/FormalBackendPackage.cmake`、`GenerateBackendIdentity.cmake`、`SharedGlfw.cmake`、`StageBackendPackage.cmake`、`scripts/windows/package_dynamic_backend.ps1`、`tests/shared_glfw/*`、`docs/superpowers/plans/2026-10-04-shared-glfw.md` 与我们 §4 的第四条不变式（DLL 活到进程结束）**直接相关**，应逐份读并只在步 ④ 采用。 |
| `9a650f3` 的"补回 31 个文件" | 无需：我们的树本来就完整（它的缺口来自 zip 快照）。 |
| 它的 `DYNAMIC_LINK_V3.md` | 读并折进我们的方案（P6），**不新建第三份并列文档**。 |

---

## §6 治理（避免两条线继续分叉）

1. **集成点是我们这棵树**：它的分支只作为**补丁来源**；`git merge` 永远不做（无共同祖先）。
2. **补丁从裸 clone 生成，不在我们的工作仓库里 fetch 外来分支**（保持 refs 干净）。
3. 每笔移植提交都带 provenance；P1–P4 落地后写一份**回话摘要**（哪几条接受、哪几条反驳、`--require-zero` 已采用），让对方不必重复。
4. 它那条 `aaa99cb` 若将来编译通过，**先当作步 ④ 的设计输入评审**，不与我们的迁移竞争同一条线。

---

## §7 风险与未验

| 风险 | 说明 | 处置 |
|---|---|---|
| 基线 JSON 跨工具链不可比 | 它的 66 是在它的构建上数的 | P1 里强制在我们归档上复核；不匹配就 `--update` 并记录 |
| 测试适配器依赖源码标记 | 它的 fixture 从真实源码抽取，**标记缺失会导致构建失败** | P3/P4 落地后若构建失败，先查标记再决定是否调整我们侧注释 |
| F1/F2/F3 本机不可行为验证 | 设备能力齐全 | 照实写"证据止于 CPU 回归"；不宣称"修好了渲染" |
| F4 是否影响现有画面 | 无证据表明当前材质用压缩格式 | 查证并写明；不夸大 |
| `ab14ca6` 的 main.cpp 与我们不同源 | 冲突已在 §3 实测 | 只取需要的部分，不整块落 |
| 双份维护 | 两条线仍在各自推进 | §6 的集成点与回话约定 |

**未验**：它的远程 CI 是否跑过（它自己说"远程 GitHub CI 尚未运行"）；它那台机器上的 17/17 CTest 我们无法复核；`aaa99cb` 是否真的只差编译（未读全量 diff）。

---

## §8 执行记录（2026-10-04，本方案已按此执行完毕）

| 步 | 提交 | 落法与偏差 | 见证 |
|---|---|---|---|
| §2 前置 | 5 笔逻辑提交（contract → backend → engine → tests+gate → docs），每笔后 ctest 全过 | CMakeLists 按 hunk 拆进 1/2/4 三笔；ci.yml 整体进第 4 笔（core_desc 由第 1 笔先行入库）；截图 / probe dll / u2.log 不入库 | 树全程未变，ctest 14/14 × 5；exe sha `CF323C5ECCC58093` |
| P1 | `98ae679` | 补丁落 4 文件（排除 ci.yml），CI 由 warn 步骤**换成**强制 `check-backend-boundary` 目标；基线 66 逐名匹配本机归档，**未 --update** | 27/27；66/180/3/0；反向 ×3（冒充消费者 exit 1 / `--update` 集合变大拒绝且不写 / `--require-zero` 按设计拒绝）；ctest 15/15；渲染 core 集实际哈希 = §5 冻结值 |
| P2 | `1dac3e5` | 整块 exit 0 | 118 checks（补丁恰加 3 条：§4 的"119+"为预估偏差）；含 detach 见证；ctest 15/15 |
| P3 | `59e2c0b` | 取 init_utils/core.constructor + 两个测试文件；CMake/CI 手工接线（只挂 capabilities 测试） | 37/37；ctest 16/16；边界 66/180/3/0（符号集未动）；渲染不变；证据边界照 §4 写入提交 |
| P4 | `cb0c64c` | vma 子集 + 新头 + 纹理测试；fixture 生成器 P3 已接好（pre-P4 的 vma 也满足锚点） | 78/78（含 4×4 BC1=8 B、5×7/2/3=96 B、mip 起点 0/64/80 断言）；压缩格式查证：**14 场景无任何压缩纹理进入上传路径**（gltf 全解码 RGBA8） |
| P5a | `9561db1` | extension.cppm 那半整块可落；abi 判断记在 `rhi.contract.cppm` 的 `abi_version` 旁（不跳号 + 理由） | 纯注释改动；promise 重编；ctest 17/17 |
| P5b | `4e9c70d` 见下 | 只取 `GLFW_INCLUDE_NONE` 一行 | 构建/ctest/格式过 |
| P6 | 见 V2 §20 | 三条口径裁决（全部接受 + 实测立场）、F4 查证、回话摘要、provenance 全部入提交 | §20 即回话文档 |

### 最终门（收尾轮，全部通过）

构建 no-op；ctest **17/17**（含门回归 27/27、能力 37/37、纹理 78/78、加载器 118 checks）；
边界 **66/180/3/0**（基线未动）；clang-format-check exit 0；
尖刺 `--with-device` **36 checks / 0 failed**（`-DDEREN_BACKEND_SPIKE=ON` 一次性树，
SHARED 后端 + 真实设备：abi 6 握手、`query_extension(device_address)`、detach 策略全过——
F1/F2/F3 修复后的能力链在真硬件上成立）；渲染 **-Full 14/14 运行、flaky 0、FAIL 0**，
§5 冻结的 11 个哈希逐一**字节相同**（unlit/deformation 与存储参考一致，其余 9 个打印值逐一比对）。

### 执行中的一次计划外事件（与移植无关，照实记）

收尾轮 `-Full` 首跑出现一个 FAIL：`laevatain_no_sidecar` 以 0xC0000409 退出。诊断：这是
**门脚本的场景定义悬空**，不是移植回归——该场景按设计用"没有 `.toon.tsv` 的模型名"触发回退，
指向 `chars/laevatain.glb`；该资产早已改名为 `laevatain_goo.glb`（其余三个 laevatain 场景
随之改了路径，唯独它漏了，移植未触碰 .ps1/.glb/场景定义）。exe 的行为是**正确的拒绝**
（main.cpp:602 对缺模型 panic）。修复：构建树内复制 `laevatain_goo.glb` -> `laevatain.glb`
（chars/ 不入库，64 MB 不进仓库），场景机制原样恢复；复跑确定性哈希 `E9A2983BEB57D5C5`，
随后 14/14 全绿。**待办**：该场景的模型路径来源应当用配置显式表达而不是文件名巧合，
留给步 ④。

**未验（继承 §7）**：它那台机器的 17/17 与远程 CI 不可复核；`aaa99cb` 只留作步 ④ 输入。
**门的新事实**：强制边界门会因"消费者比后端旧 5 分钟"拒绝测量（P3/P4 各一次），处置是全目标干净重建——这条写进 V2 §20 裁决 1。
