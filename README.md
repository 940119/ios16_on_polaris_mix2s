# iOS 16 on Xiaomi Mi MIX 2S (`polaris`)

[简体中文](#简体中文) · [English](#english) · [开发记录](<NOTES.md>)

> **实验性 Mi MIX 2S 项目，非日用系统。** 截至 2026-10-23；没有此次更新后的真机测试。此页只统计 Polaris（SDM845／Adreno 630v2）的成果，不把 Redmi Note 10 Pro (`sweet`) 的成果算进来。原项目详细说明另存为 [Sweet 历史文档](<docs/SWEET-HISTORY.md>)。

## 简体中文

### MIX 2S 实现和证据

这是基于 XNU 8796／iPhone X iOS 16 用户态基础设施的 **Polaris 板级移植**：4×Cortex-A55 + 4×Cortex-A75、Synaptics S3330、1080×2160 面板、A630v2。它有独立 ADT、板型门控、触摸复位／读帧和 GPU 固件预检；不能复用 sweet 的触摸供电、寄存器或刷机参数。

“真机”= 临时启动结果或用户反馈；“CI”= 编译／链接；“离线”= 主机测试。三种证据不能互换。

| 功能 | 已验证或已完成 | 未验证／限制 |
| --- | --- | --- |
| 启动和触摸 | 0090 RELEASE 临时启动到 iOS 锁屏；用户确认 **单指、双指**触摸。S3330 使用 GPIO33/34 实验性软件 I²C、GPIO99 复位释放、连续 F12 帧。 | 未取得完整 0090 ring；十指、正式 SE14 硬件 I²C、长期稳定性未验证。用户表示自行刷入过工作镜像，代理未读回 boot 分区确认。 |
| CPU | Polaris ADT 有八个 MPIDR `0x000`–`0x700` 节点，XNU 上限为 8。**已测试镜像指定 `cpus=4`**；CPU1–3 的 PSCI CPU_ON／SGI 成功，最终 `4/4`。 | **没有 CPU4–7 四个 A75 在 MIX 2S 上运行的证据**。八核设备树和内核上限不等于八核已启动。 |
| 存储、界面和显示 | 复用 XNU UFS／HFS+ `userdata` 根和 iPhone X 用户态链路；CPU 软件合成／缩放到 ABL 遗留 framebuffer。 | 完整应用稳定性、独立面板初始化和 GPU 屏幕呈现未验证。 |
| GPU | Recovery Linux 只读报告 A630v2、257–710 MHz；本机六个固件组件通过离线预检。可选只读**固件元数据** XNU 0091/0092 在 [CI 39739222780](https://github.com/luuu20309/ios16_on_polaris_mix2s/actions/runs/39739222780) 编译／链接通过；26 个 GPU 主机测试通过。 | **没有 XNU GPU 上电、GMU/HFI、PAS13、SMMU、CP fence、GPU 渲染、Mesa/EAGL 或 Metal。** 0093 是默认关闭的初始化门槛，尚未 XNU 编译或真机执行，不能算 GPU 驱动。 |

### 目前的错误、阻塞及处理

| 阶段 | 实际错误／原因 | 当前进度 |
| --- | --- | --- |
| 0088 触摸 | 首次 I²C 地址写入失败，GPIO99 保持输出低电平，控制器可能仍在复位。 | 0089 单独释放 GPIO99 后读出 F01/F12 PDT，越过初始失败；未动 sweet 触摸供电。 |
| 0089 触摸 | 虽然 PDT 和 F12 Control23 可读，连续八帧数据无效，锁屏触摸仍无响应。 | 0090 改为单次有界连续读取最多 80 字节 F12 帧；随后用户确认单／双指可用。缺少完整 0090 真机 ring。 |
| “只有 4 核” | 已测 payload 明确写 `cpus=4`，日志为 `requested=4 selected=4 (limit=8)`；**不是已证实的 A75 PSCI 故障**。 | 已有八核 ADT/PSCI 上限；新增[八核拓扑校验](<tools/polaris_cpu_topology.py>)与 `mkpayload.py --check-polaris-cpus`，仅检查板型、MPIDR、连续 `cpus=1..8`。下一次获准的临时启动先试 `cpus=5`，查 CPU4 的 PSCI 返回、initCPU、SGI 和用户态，再试 6→8。 |
| 上游 CPU 失败回退 | 对照 [Luoxueowo/ios-polaris 固定版本](https://github.com/Luoxueowo/ios-polaris/tree/44556dec6995b8d9bb72717cf546b92b6c3dddda)，其 0065/0078 的 PSCI 与八核上限本仓库已有；0088 跳过失败核后仅用 `num_cpus-skipped` 上报，无法排除 CPU ID 空洞或晚到从核，XNU 还会丢弃 `PE_cpu_start` 错误。其 README 对 A75 是否启用也有矛盾。 | **未直接合并不安全的 0088 补丁**；先验证连续 CPU 前缀。完整对照见 [Polaris 指南](<docs/POLARIS-MIX2S.md>)。 |
| GPU 命令数据库 | 缺少已证明 XNU 归属的 ARC/BCM command-db 和这台机器的 13 档 DDR／2 档 CNOC HFI 实际投票。旧 0092 固定物理地址映射因归属不明已撤回。 | 仅完成来源核对与离线格式解析；静态 DTS 数值不是已聚合的 RPMh 命令。 |
| GPU 硬件后端 | CX/MX/GX 与共享 GPUCC/GFXCC 的所有权／回退、HYP 保护的 SMMU CB5–7、PAS13 的 8 KiB zap 区及 GMU/HFI 顺序尚未落实。 | 0093 在第一个未实现阶段拒绝并保留 CPU 显示。[安全实现契约](<research/polaris/gmu-secure-implementation.md>)定义后续有界 CP sentinel／fence 测试；**没有可交付的加速镜像**。 |
| 其他功能 | Wi‑Fi、蓝牙、蜂窝、音频、相机、传感器、GPS、SEP／Face ID 尚未实现或未在 Polaris 上验收。 | iOS 锁屏不是这些模块可用的证据。 |

### 下一步、代码与安全

1. **CPU：**保持已启动内核和 Polaris ADT 不变，只将临时镜像 `cpus=4` 改为 `cpus=5`；检查镜像容量和命令行，经用户确认后临时 `fastboot boot`。获取 CPU4 PSCI／SGI／用户态证据，再考虑 6→8；失败保留四核，不将坏核交给调度器。
2. **GPU：**先取得本机可信的 command-db/bus query 结果和 XNU 对 RPMh／GPUCC／SMMU／PAS13 的资源归属；实现可回退的上电路径，证明有界 CP fence 和缓冲区内容变化，然后才接 Mesa/EAGL、IOSurface 与显示。**编译成功不是 GPU 执行成功。**
3. **触摸：**获准测试时补采真机 0090 ring 与触点数据，验证 SE14 硬件 I²C 和全部触点。

更多细节：[Polaris 技术报告（中英）](<research/polaris/README.md>)、[Polaris 构建／CPU 指南](<docs/POLARIS-MIX2S.md>)、[逐次错误和真机日志交接](<NOTES.md>)、[XNU 补丁](<xnu/patches/>)、[CPU 测试](<tools/test_polaris_cpu_topology.py>)、[原 sweet 历史](<docs/SWEET-HISTORY.md>)。**下载：**[0090 内核 Release](https://github.com/luuu20309/ios16_on_polaris_mix2s/releases/tag/polaris-kernel-0090)（仅内核，不能直接刷 boot）及[酷安平台刷入／临时启动教程](<docs/COOLAPK-POLARIS-INSTALL.md>)。上游基础：[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)；硬件资料：[MiCode Polaris 内核](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/polaris-o-oss)。

```sh
# 离线测试；不需要专有固件，不证明真机运行
python -m unittest tools/test_polaris_cpu_topology.py tools/test_polaris_gpu_preflight.py tools/test_polaris_gmu_firmware.py tools/test_polaris_arc_db.py tools/test_polaris_bcm_db.py tools/test_polaris_bw_levels.py tools/test_polaris_gpu_stages.py -q
```

仓库不上传 Apple 镜像、Qualcomm 固件、设备转储、序列号或分区备份。对 `userdata` 写入会清除现有数据；新的临时启动与任何分区写入均须事先分别确认，保留 bootloader、recovery、modem、persist 和备份。

## English

### Mi MIX 2S implementation (2026-10-23)

This repository highlights the **Polaris/SDM845** board port of the XNU/iPhone X iOS 16 infrastructure. Sweet's historical build and flash instructions live in [a separate archive](<docs/SWEET-HISTORY.md>) and are **not** Mi MIX 2S instructions. This is experimental, not a secure daily-use OS; no distributable iOS image or working Polaris GPU accelerator is provided.

| Area | Evidence | Missing or unproven |
| --- | --- | --- |
| Boot/touch | Temporary 0090 RELEASE boot reached the lock screen; the user confirmed one- and two-finger S3330 input. | Full ten-contact reliability, hardware SE14 I²C, long-term stability, permanent-boot readback. |
| CPUs | Eight ADT nodes and an XNU limit of eight. The observed `cpus=4` image started four A55s; PSCI and SGI passed for CPUs1–3. | No log proves CPU4–7 (the A75s) boot on this Polaris phone. Four selected CPUs follow the boot argument, **not** an observed A75 failure. |
| Display | Software composition/scaling into the bootloader-inherited framebuffer. | GPU composition or independent panel initialization. |
| GPU | Recovery Linux identified A630v2. Device firmware was screened **locally** and excluded from Git. Read-only XNU firmware-metadata diagnostics compiled/linked in [CI 39739222780](https://github.com/luuu20309/ios16_on_polaris_mix2s/actions/runs/39739222780); 26 host GPU tests passed. | **No XNU GPU power, GMU/HFI, secure PAS13, SMMU mappings, CP fence, rendering, Mesa/EAGL, or Metal.** 0093 is an uncompiled, default-off gate that refuses at its first missing backend. |

### Known failures and why progress stopped

- **Touch 0088/0089:** The first I²C address write failed while GPIO99 was low. After 0089 reset release, F01/F12 were detected, but eight invalid frames still left touch unresponsive. A bounded contiguous F12 read in 0090 preceded the user's one-/two-finger confirmation; no full 0090 diagnostic ring was recovered.
- **CPU count:** The tested payload explicitly used `cpus=4` and logged `requested=4 selected=4 (limit=8)`. Comparing pinned [Luoxueowo/ios-polaris](https://github.com/Luoxueowo/ios-polaris/tree/44556dec6995b8d9bb72717cf546b92b6c3dddda) found no missing eight-core ADT/PSCI path. Its `0088` skip-failed-CPU patch is unsafe to import: XNU discards `PE_cpu_start` errors, subtracting a count cannot represent CPU-ID holes, and late cores remain unresolved. A new opt-in [topology/boot-argument preflight](<tools/polaris_cpu_topology.py>) does **not** prove CPUs are online. With approval, test `cpus=5`, then 6 and 8, and collect CPU_ON/SGI/userland evidence each time.
- **GPU:** A fixed-PA command-db map was withdrawn because XNU ownership was unproven. We still need genuine Polaris ARC/BCM/bus queries, owned reversible RPMh/GPUCC power, secure PAS13 and non-HYP SMMU mappings, GMU/HFI and bounded CP work. The `polarisgpu=1` 0093 gate refuses before hardware access and retains CPU display. [Technical report](<research/polaris/README.md>) and [implementation contract](<research/polaris/gmu-secure-implementation.md>) distinguish source facts, compilation, host tests and actual device execution.
- **Other hardware:** Wi‑Fi, Bluetooth, cellular, audio, camera, sensors, GPS and SEP/Face ID are absent or unvalidated on this build. A lock screen does not establish functionality.

**Next proof points:** On a user-approved *temporary* boot, try CPU4 using the known kernel/ADT with only `cpus=5` changed, then advance 6→8 if PSCI, SGI and userspace pass. For GPU, prove resource ownership and a bounded headless CP fence with changed buffer before Mesa/EAGL and screen presentation. Keep the CPU framebuffer fallback. Compilation is not execution.

Read the [Polaris build/CPU guide](<docs/POLARIS-MIX2S.md>), [full bilingual GPU notes](<research/polaris/README.md>), [experiment log](<NOTES.md>) and [Sweet historical record](<docs/SWEET-HISTORY.md>). Download the [0090 kernel-only Release](https://github.com/luuu20309/ios16_on_polaris_mix2s/releases/tag/polaris-kernel-0090) and read the [Coolapk install/safety guide](<docs/COOLAPK-POLARIS-INSTALL.md>) before trying to package it: the Mach-O file is **not flashable by itself**. The host-only test command above is reproducible without proprietary firmware. Never publish Apple images, device-identifying dumps, partition backups or Qualcomm proprietary blobs; ask separately before temporary boots and partition writes.
