# Polaris A630v2 headless 上电契约（未接入硬件）

该文档及 `tools/polaris_gpu_stages.py` 是 **host-only 前置/回退模型**，不是 XNU GPU 驱动、MMIO 后端或固件认证。未证明 command-db 所有权和 RPMh/BCM 投票前禁止将状态机阶段映射到寄存器写入；不能从 `PWR_ON=1` 推断获得 CX/GX 关闭权限。现有 0090 CPU framebuffer/触摸必须保持默认。

## 设备来源与不能混用的寄存器

- [MiCode SDM845 GPUCC](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/polaris-o-oss/drivers/clk/qcom/gpucc-sdm845.c#L264-L323) 给出 A630v2 的 180/257/342/414/520/596/675/710MHz 原始 clock 表；recovery KGSL 已观察非零 257–710MHz 七档。`v2` fixup 根据 `qcom,gpucc-sdm845-v2` 兼容字符串选择 [GFX 表及 GMU 500MHz 源](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/polaris-o-oss/drivers/clk/qcom/gpucc-sdm845.c#L606-L640)，不得从静态 DTS 的 210–600MHz 表推断实际设备版本。
- GPUCC/GFXCC 共享物理区 `0x05090000..0x05098fff`；[CX GDSCR `+0x106c`、CX HW_CTRL `+0x1540`、GX GDSCR `+0x100c`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/polaris-o-oss/arch/arm64/boot/dts/qcom/msm-gdsc-sdm845.dtsi#L188-L211)。它们不是两个可独立拥有/还原的寄存器域；CX 的 PM8998 S9 和 GX 根时钟/供电依赖参见 [板级 DTS](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/polaris-o-oss/arch/arm64/boot/dts/qcom/sdm845.dtsi#L4161-L4173)。只有本驱动实际取得的资源才能逆序释放；其他主控的票、已开域或被 HYP 管理的状态绝不擅自清除。
- [GPU SMMU](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/polaris-o-oss/arch/arm64/boot/dts/qcom/sdm845-gpu.dtsi#L268-L359) 基址 `0x05040000`，`qcom,protect=<0x40000 0xc000>` 是相对偏移，标明 CB5–7 的 HYP 保护范围，**不在** SMMU 的 `0x10000` reg 长度内；不要把 protect 偏移误认为合法的 MMIO 可访问区。GMU 用户/内核域与 3D core 域必须分别构建 IOVA、TLB/故障验证，不能沿用 HackiOS SC7280 的流编号。
- [GMU 依赖](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/polaris-o-oss/drivers/gpu/msm/kgsl_gmu.c#L1397-L1499) 先 CX GDSC、时钟/IRQ、临时 DDR 票，再 RPMh/PDC/GMU ITCM、HFI、DCVS。GX 经 GMU 请求，不能手工翻转 GDSCR 当作整体上电。`MAX_GMUFW_SIZE=0x2000` 是 8192 次 `u32` ITCM 写入，实际平铺固件 32KiB；已纳入离线预检，但 XNU 尚无装载后端。GMU 未空闲、HFI 请求未完成或 SMMU 故障时，应隔离设备而不是盲目关共享 CX。
- [PAS13 zap](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/polaris-o-oss/arch/arm64/boot/dts/qcom/sdm845.dtsi#L550-L554) 保留 `0x8c415000` 的 `0x2000`、`no-map`，不得当 BO/SMMU 映射内存或从 XNU 直接写入后假定 TZ 已认可。只有 PAS 签名、受控装载和真实 SQE/CP fence 后，才能进入呈现。

## 后续实现门槛

1. 现成 XNU 0091/0092 的双 ADT 板型和 opt-in 仅验证元数据。需要另证 command-db 所有权、精确 ARC/BCM 地址与 aux、各驱动电源投票和安全归属；无证据不得读 off-domain MMIO。
2. 每阶段提交后用明确超时/读回记录“本驱动取得的资源”；未取得资源不进入 `owned`。阶段失败按逆序清理，但 GPU/GMU 未确认 idle 则保留隔离和 CPU 回退。
3. 先 headless CP fence + 缓冲区实际写入读回，再接 Mesa/IOSurface，最后是屏幕呈现；完整编译不是成功标准。任何临时镜像启动/刷写先征求用户确认。
