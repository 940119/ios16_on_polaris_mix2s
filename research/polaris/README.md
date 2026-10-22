# Mi Mix 2S (`polaris`) / 小米 MIX 2S 进展

[English](#english) · [简体中文](#简体中文)

## English

### Scope and honest status — 2026-10-22

This repository began as an iOS 16/XNU port to Redmi Note 10 Pro (`sweet`, SM7150). Polaris is a **separate** Snapdragon 845 board with Adreno 630v2, Synaptics S3330 and different power/clock/SMMU resources. The top-level [README](<../../README.md>) documents sweet; none of its addresses, touch rails, or flashing instructions are interchangeable with Polaris. We run iPhone X iOS 16 userspace, which is not distributed in this repository.

| Polaris area | Evidence today | Not proven |
| --- | --- | --- |
| Boot/UI | A temporarily booted RELEASE image with patch 0090 reached the iOS lock screen. User later reported flashing that image themselves; this was not verified by an agent reading back the boot partition. | Complete reliable boot, all apps, production stability. |
| Touch | On 0090 temporary boot the user confirmed **single- and two-finger input**. Synaptics S3330 on GPIO33/34 bit-banged I²C @0x20; a separately opted-in GPIO99 reset release corrected earlier address NACK. | Full ten-contact accuracy, gestures in all apps, durable hardware-I²C controller port; the exact F12 Data1 map remains partly inferred. |
| Display | Bootloader-inherited framebuffer and CPU composition remain the working fallback. | GPU composition or an independently initialized panel. |
| GPU | Device recovery Linux identified Adreno630v2 and 257–710 MHz; six firmware components were extracted **locally and kept out of Git**. An opt-in, read-only XNU firmware-metadata diagnostic compiled/linked in [CI run 39739222780](https://github.com/luuu20309/ios-sweet/actions/runs/39739222780). Host-only parsers and tests cover firmware, ARC/BCM metadata, v2 bandwidth cases and a bring-up ownership model. | **No XNU GPU power-on, GMU/HFI execution, PAS13, GPU SMMU mappings, CP fence, GPU drawing, or accelerated iOS display.** That CI run is compile evidence, not GPU execution. |

**No working GPU accelerator or ready-to-boot GPU-enabled image exists in this repo.** Nothing in these research tools should be mistaken for a driver that can render or boot the GPU. The most recent GPU changes are host-side and have not been kernel-built or phone-tested. No Metal implementation is claimed.

### Device bring-up timeline

1. Shim and ADT: `tools/mkadt.py --polaris --ram 6` builds a Polaris board-gated Apple Device Tree; the boot payload is checked against the ABL ramdisk-to-DTB gap (`0xdea000` bytes, observed for the tested 0090 image). `tools/mkpayload.py` supports named firmware blobs and `--max-payload-size`. These options only reserve bytes and check layout; they do **not** run GPU firmware.
2. Touch: patches `0084`–`0090` add SM7150 exclusion, Polaris clock diagnostics, board/boot-argument-gated Synaptics raw polling, reset release and bounded contiguous F12 reads. Earlier diagnostic boots failed at the first I²C address transaction while GPIO99 held the controller in reset; subsequent vendor-style 80-byte F12 reads led to the user-confirmed one-/two-finger result in 0090. Existing sweet SE7/LDO11 touch must never be enabled on Polaris.
3. GPU preparation: `0091` verifies named firmware ADT allocations and reserved-memory exclusions *only* with `polarisgpudiag=1` and dual Polaris/SDM845 board gates. `0092` explicitly **defers** command-db mapping: an earlier hard-coded physical map was withdrawn because ownership on XNU was not established. Neither patch reads off-domain GPU/GMU/SMMU registers or votes power.
4. Local recovery inventory found `a630_sqe.fw`, 32 KiB `a630_gmu.bin`, and signed zap MDT/segments. These are proprietary and excluded from Git. The flat A630 GMU firmware differs from HackiOS A660's typed block format; MiCode TCM_BOOT copies 8192 **32-bit words**, not 8192 bytes. Offline format screening is not signature verification or firmware execution.
5. Sources establish the power/clock/SMMU/PAS dependencies, but a working, owned XNU backend has **not** been implemented. The CPU framebuffer remains the default and the correct fallback.

For exact dated CI artifacts, checksums, phone observations and superseded hypotheses, consult [NOTES.md](<../../NOTES.md>); do not treat older entries as current claims.

### Hardware and driver boundary

- The MiCode [Polaris GPU DTS](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/polaris-o-oss/arch/arm64/boot/dts/qcom/sdm845-gpu.dtsi) names GPU `0x05000000`, GPU SMMU `0x05040000`, GMU `0x0506a000`, and GMU PDC `0x0b200000`; [GPUCC](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/polaris-o-oss/drivers/clk/qcom/gpucc-sdm845.c) and GFXCC share the physical window at `0x05090000`. CX/GX GDSCs are within that window, not independent owners. Do not read GMU registers while its power domain is off.
- The SMMU has HYP-protected CB5–7. The PAS13 `a630_zap` reserved area is only 8 KiB at `0x8c415000` and requires a secure loader, not an ordinary BO copy. GPUBW DDR, CNOC, ARC, CX/MX/GX rails and CPU/GMU clocks are separate dependencies. A GDSC `PWR_ON` bit does not confer exclusive ownership.
- [Polaris v2 DTS](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/polaris-o-oss/arch/arm64/boot/dts/qcom/sdm845-v2.dtsi#L550-L649) sets chip ID `0x06030001` with 710/675/596/520/414/342/257 MHz levels mapped to DDR cases 12/12/10/9/8/6/4. The base DTS's 210–600 MHz table is **not** the observed A630v2 table. [Bus DTS](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/polaris-o-oss/arch/arm64/boot/dts/qcom/sdm845-bus.dtsi) gives topology and named BCMs, not this phone's command-db addresses or aggregate votes.
- The [MiCode GMU driver](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/polaris-o-oss/drivers/gpu/msm/kgsl_gmu.c#L684-L784) queries a 13-case DDR and 2-case CNOC HFI table from the kernel bus driver. By contrast, the [upstream Linux A630 fallback](https://github.com/torvalds/linux/blob/7be4bc52fd9e0e55dc9a2e0232b89427d2c4b614/drivers/gpu/drm/msm/adreno/a6xx_hfi.c#L761-L796) provides a one-row DDR-off, two-row CNOC fixed table. Those upstream constants are a *source-backed comparison baseline*, not proof of values/permissions on this Polaris firmware and not a substitute for vendor 13-case scaling. See [mainline comparison](<mainline-a630-hfi-reference.md>).
- The [HackiOS XNU kernel](https://github.com/oosexgang/hackios-xnu-kernel/tree/8e61187098f2c26c5daee8f9f72b6416aa64b9ab) and [userland](https://github.com/oosexgang/hackios-xnu-userland/tree/906425b75e02b4f99eb5d7bb1ef7c9583472d772) offer staged rollback and Mesa/IOKit interface ideas for A642L/SC7280, **not** Polaris register values, firmware loader or ready-made GPU acceleration. See [porting matrix](<hackios-gpu-porting.md>).

### Where to look and how to validate without a phone

| File | Purpose |
| --- | --- |
| [`xnu/patches/0084`–`0090`](<../../xnu/patches/>) | Board-gated experimental Polaris touch patches; 0090 is the last user-confirmed single/two-finger version. |
| [`xnu/patches/0091`](<../../xnu/patches/0091-polaris-gpu-readonly-firmware-diagnostic.patch>) and [`0092`](<../../xnu/patches/0092-polaris-gpu-readonly-arc-command-db.patch>) | Read-only firmware metadata diagnostic and deferred ARC mapping; no power writes. |
| [`tools/polaris_gpu_preflight.py`](<../../tools/polaris_gpu_preflight.py>) and [`polaris_gmu_firmware.py`](<../../tools/polaris_gmu_firmware.py>) | Local-only firmware/layout/flat GMU format gates; never commit blobs. |
| [`tools/polaris_arc_db.py`](<../../tools/polaris_arc_db.py>) and [`polaris_bcm_db.py`](<../../tools/polaris_bcm_db.py>) | Parsers for explicitly supplied command-db bytes; no raw memory/device access. |
| [`tools/polaris_bw_levels.py`](<../../tools/polaris_bw_levels.py>) and [`polaris_gpu_stages.py`](<../../tools/polaris_gpu_stages.py>) | V2 bandwidth case and fail-closed staged ownership models; **host-only**, not a hardware driver. |
| [GPU bring-up contract](<a630v2-bringup-contract.md>), [bandwidth cases](<a630v2-bw-levels.md>), [BCM boundary](<bcm-vote-boundary.md>) | Source-linked constraints, rejection conditions, and what remains to be measured. |

Offline tests need only Python 3 (no proprietary firmware for unit tests):

```sh
python -m unittest tools/test_polaris_gpu_preflight.py tools/test_polaris_gmu_firmware.py tools/test_polaris_arc_db.py tools/test_polaris_bcm_db.py tools/test_polaris_bw_levels.py tools/test_polaris_gpu_stages.py -v
```

At the 2026-10-22 checkpoint, **26 host tests passed**. Actual device-extracted firmware passed the local preflight in the ignored workspace, but these files are not included here. Do not copy `out/`, partition backups, Apple IPSW/root images, logs containing serial numbers, or proprietary Qualcomm firmware into a public repository. `.gitignore` is not a reason to publish private artifacts via another route.

### Next milestones and collaboration

1. Establish an owned, read-only path to this Polaris firmware's command-db and bus-query results, or experimentally validate the upstream no-DDR-scaling fallback **only after** the base hardware dependencies are implemented. Never treat Linux Recovery's memory ownership as XNU ownership.
2. Implement separately gated Polaris RPMh/rails/GPUCC and reversible CX/GMU power with bounded readback; then actual A630 firmware/HFI and secure PAS13/SMMU mappings without touching protected context banks. Keep the CPU framebuffer as fallback.
3. In a user-approved, **temporary** `fastboot boot` test image only, demonstrate a bounded headless CP submission, completed fence and changed buffer contents. A kernel that compiles or boots is not a GPU test.
4. Add an isolated GPU buffer/submit/wait user client, Mesa/freedreno GLES/EAGL bridge and IOSurface/display presentation. The eventual user-visible acceleration must be measured on the device; no Metal claim.

The repository does not ship a Polaris-flashing recipe. Confirm the exact phone and image before any temporary boot; obtain separate explicit consent before partition writes. Preserve bootloaders, recovery, modem, persist and backup data.

## 简体中文

### 范围与真实状态（截至 2026-10-22）

本仓库最初是 Redmi Note 10 Pro（`sweet`，SM7150）的 iOS 16／XNU 移植；现在也包含小米 MIX 2S（`polaris`，SDM845／Adreno 630v2）的实验性适配。**这两款设备的设备树、触摸硬件、供电和 GPU 寄存器不能互换。** 根目录 [README](<../../README.md>) 中原有 sweet 构建和刷机示例不适用于 Polaris。仓库不分发 iOS 用户态、Apple 镜像、手机固件或设备转储。

| Polaris 模块 | 已有证据 | 尚未完成或证明 |
| --- | --- | --- |
| 启动与界面 | 临时启动 0090 RELEASE 镜像后到达 iOS 锁屏。用户后来表示自行刷入该工作镜像；代理没有读回 boot 分区验证。 | 所有应用可靠运行、量产级稳定性。 |
| 触摸 | 用户确认 0090 临时启动时**单指和双指触控可用**；S3330 通过 GPIO33/34 的临时软件 I²C、控制器地址 `0x20`，独立开关释放 GPIO99 复位。 | 完整十指、全局手势准确性、正式 SE14 硬件 I²C 和完全确定的 F12 Data1 布局。 |
| 显示 | CPU 合成并复制到 bootloader 遗留 framebuffer；这是当前默认回退路径。 | GPU 合成或独立初始化屏幕。 |
| GPU | Recovery Linux 报告 Adreno630v2 和 257–710 MHz；本机只读提取六个固件组件并置于 Git 忽略目录。可选的只读 XNU 固件元数据诊断在 [CI 39739222780](https://github.com/luuu20309/ios-sweet/actions/runs/39739222780) **编译和链接通过**；主机脚本检查固件、ARC/BCM 格式及 v2 档位。 | **未在 XNU 上给 GPU 供电、运行 GMU/HFI、执行 PAS13、建立 GPU SMMU 映射、提交 CP fence 或 GPU 渲染。** CI 不是 GPU 真机运行证据。 |

**本仓库当前没有可用的 Polaris GPU 加速驱动，也没有现成可启动 GPU 加速的镜像。** 当前图形仍由 CPU 完成；研究脚本不是硬件驱动。最新 GPU 主机工具未经过新的 XNU 编译或手机启动。没有 Metal 实现。

### 已完成的关键工作

1. **板型隔离与启动。** `tools/mkadt.py --polaris --ram 6` 生成与 sweet 不同的 ADT；`tools/mkpayload.py` 可为固件预留具名内存区域，并检查已观测到的 `0xdea000` ABL ramdisk→DTB 空间上界。这只检查打包，**不会启动 GPU 固件**。
2. **触摸。** `0084`–`0090` 补丁阻止 sweet SE7/LDO11 驱动跑在 Polaris 上，增加可选诊断、S3330 读帧和 GPIO99 复位释放。0090 改为一次连续读取最多 80 字节 F12 对象；用户确认单指、双指有效。更早失败的日志证明 GPIO99 保持复位时第一次 I²C 地址写入失败。该实验路径不是完备的正式触摸驱动。
3. **GPU 安全诊断。** `0091` 只有 `polarisgpudiag=1` 且同时匹配 Polaris 与 SDM845 时，才校验六个固件 ADT 条目和保留内存；`0092` 的最终版本只说明**推迟** command-db 映射。未经 XNU 内存归属证明的固定物理地址映射已撤回。两者都不会给 GPU 供电、访问掉电的 GMU 或改变 CPU 显示路径。
4. **本机固件与资料。** A630 GMU 固件是 32 KiB 平铺镜像，和 HackiOS A660 的分块格式不同。MiCode 中 `MAX_GMUFW_SIZE=0x2000` 表示 8192 次 **32 位字写入**，不是 8192 字节。固件文件只在本机 Git 忽略目录中，离线格式检查不等于签名校验或硬件运行。
5. **带宽档位与公开实数值。** Polaris v2 设备树将 710/675/596/520/414/342/257 MHz 分别映射到 DDR case 12/12/10/9/8/6/4，基础版的 210–600 MHz 不能误用。小米厂商驱动动态查询 13 档 DDR 和 2 档 CNOC；Linux 主线 A630 则有固定的一档 DDR-off／两档 CNOC HFI 表。主线固定值是有出处的参考，**尚未证明等同于本机 command-db 或小米的动态投票**。详见[主线 HFI 对照](<mainline-a630-hfi-reference.md>)、[档位依据](<a630v2-bw-levels.md>)和[BCM 边界](<bcm-vote-boundary.md>)。

逐次 CI、镜像哈希、真机观察和被推翻的早期猜测见 [NOTES.md](<../../NOTES.md>)；旧记录不可当作最新结论。

### 为什么还不能直接打开 GPU

MiCode [GPU 设备树](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/polaris-o-oss/arch/arm64/boot/dts/qcom/sdm845-gpu.dtsi) 列出 GPU、GPUCC、GMU/PDC、SMMU 以及供电依赖。GPUCC/GFXCC 共享一段物理窗口，SMMU 的 CB5–7 受 HYP 保护；PAS13 的 `a630_zap` 有单独的 8 KiB 保留区。CX 供电、时钟和 DDR 投票先于 GMU 固件/HFI；随后还要 GX、可信 SMMU 映射、SQE 与 CP。看到 GDSC 的 `PWR_ON` **不代表本驱动有权关掉它**。Recovery Linux 的内存归属也不能当作 XNU 的内存归属。

完整制表需要这台手机的 command-db 地址/BCM aux、总线拓扑、聚合和 HFI 查询输出；公开 DTS 只提供路径及档位输入。Linux 主线的固定 A630 表为最小启动提供研究线索，但绝不能声称已经在这台设备验证。HackiOS 的 A642L 状态机和 Mesa/IOKit 接口可参考，SC7280 的寄存器／电源／固件格式不能直接移植，详见[移植对照](<hackios-gpu-porting.md>)和[上电契约](<a630v2-bringup-contract.md>)。

### 目录、主机验证和下一步

| 路径 | 用途 |
| --- | --- |
| [`xnu/patches/0084`–`0090`](<../../xnu/patches/>) | Polaris 实验触摸补丁；0090 是最后一次获用户确认单／双指的版本。 |
| [`xnu/patches/0091`](<../../xnu/patches/0091-polaris-gpu-readonly-firmware-diagnostic.patch>)、[`0092`](<../../xnu/patches/0092-polaris-gpu-readonly-arc-command-db.patch>) | 固件只读元数据诊断与推迟 ARC 映射，**没有 GPU 上电**。 |
| [`tools/polaris_gpu_preflight.py`](<../../tools/polaris_gpu_preflight.py>)、[`polaris_gmu_firmware.py`](<../../tools/polaris_gmu_firmware.py>) | 本机固件、内存布局与 GMU 平铺格式检查。 |
| [`tools/polaris_arc_db.py`](<../../tools/polaris_arc_db.py>)、[`polaris_bcm_db.py`](<../../tools/polaris_bcm_db.py>) | 解析明确提供的离线 command-db 字节，不读取设备物理内存。 |
| [`tools/polaris_bw_levels.py`](<../../tools/polaris_bw_levels.py>)、[`polaris_gpu_stages.py`](<../../tools/polaris_gpu_stages.py>) | v2 带宽索引和资源归属阶段模型，仅供主机验证。 |
| [研究资料](<./>)、[详细交接](<../../NOTES.md>) | 源码依据、真机证据、限制及下一步。 |

```sh
python -m unittest tools/test_polaris_gpu_preflight.py tools/test_polaris_gmu_firmware.py tools/test_polaris_arc_db.py tools/test_polaris_bcm_db.py tools/test_polaris_bw_levels.py tools/test_polaris_gpu_stages.py -v
```

截至 2026-10-22，**26 项主机测试通过**。单元测试不需要专有固件；本机固件也曾通过独立预检，但未上传。后续要先取得受信任的 command-db／bus query 数据或单独验证主线简化模式，再实现可回退的 Polaris 电源、时钟、GMU/HFI、PAS13 与 SMMU 后端；先用经用户同意的临时 `fastboot boot` 验证有界 CP fence 和实际缓冲区变化，最后才接 Mesa／EAGL、IOSurface 与屏幕呈现。**完整编译通过不等于 GPU 加速成功。**

不要把设备转储、序列号、分区备份、Apple 镜像、Qualcomm 专有固件或构建产物传到公开 GitHub。仓库不提供 Polaris 刷机捷径；任何新测试镜像启动前确认设备与镜像，写分区前另征得明确同意，保留 bootloader、recovery、modem、persist 和备份。
