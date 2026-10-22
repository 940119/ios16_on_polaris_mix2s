# 查到的 SDM845 A630 HFI 数据与适用边界（2026-10-22）

在 Linux 主线提交 [`7be4bc52fd9e0e55dc9a2e0232b89427d2c4b614`](https://github.com/torvalds/linux/tree/7be4bc52fd9e0e55dc9a2e0232b89427d2c4b614) 找到**真实驱动源码中的 A630 fallback 表**，并非由 DTS 带宽数字推测：[a6xx_hfi.c:761–796](https://github.com/torvalds/linux/blob/7be4bc52fd9e0e55dc9a2e0232b89427d2c4b614/drivers/gpu/drm/msm/adreno/a6xx_hfi.c#L761-L796) 写明 A630 GMU 不做总线 scaling，发送一档 DDR “off” 及两档 CNOC。具体源码常量：

| 组 | 地址（依次） | 命令数据 | 等待位掩码 |
|---|---|---|---|
| DDR | `0x50000`, `0x5005c`, `0x5000c` | off: `0x40000000` ×3 | `0x07` |
| CNOC | `0x50034`, `0x5007c`, `0x5004c` | off: `0x40000000`, `0x00000000`, `0x40000000`; on: `0x60000001`, `0x20000001`, `0x60000001` | `0x05` |

[主线芯片表:947–968](https://github.com/torvalds/linux/blob/7be4bc52fd9e0e55dc9a2e0232b89427d2c4b614/drivers/gpu/drm/msm/adreno/a6xx_catalog.c#L947-L968) 的 A630 条目包含 Polaris v2 `0x06030001`，固件 `a630_gmu.bin` / `a630_sqe.fw` 和 `a630_zap.mdt`，**没有** `.bcms`；因此 [HFI 分派:813–836](https://github.com/torvalds/linux/blob/7be4bc52fd9e0e55dc9a2e0232b89427d2c4b614/drivers/gpu/drm/msm/adreno/a6xx_hfi.c#L813-L836) 会落到上述 fallback。主线的[通用按 BCM aux 计算方法](https://github.com/torvalds/linux/blob/7be4bc52fd9e0e55dc9a2e0232b89427d2c4b614/drivers/gpu/drm/msm/adreno/a6xx_gmu.c#L1605-L1686) 对**具有** `.bcms` 的型号才使用；不能把其他型号的 SH0/MC0/ACV 地址/比例移植进 A630。

**关键差异：**[MiCode Polaris GPU DTS](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/polaris-o-oss/arch/arm64/boot/dts/qcom/sdm845-gpu.dtsi#L92-L113) 指定 13 档 GPU DDR 和 2 档 GMU CNOC，[MiCode GMU 初始化](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/polaris-o-oss/drivers/gpu/msm/kgsl_gmu.c#L684-L750) 从该设备的 `msm_bus_scale_query_tcs_cmd_all` 动态得到完整 TCS 数据；主线表没有这 13 档 DDR 动态投票，不能宣称它等同于 Polaris vendor HFI 数据。MiCode [总线拓扑](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/polaris-o-oss/arch/arm64/boot/dts/qcom/sdm845-bus.dtsi#L38-L87) 证明 apps-RSC 上有 `SH0/MC0/ACV` 名称，却不提供**本机** command-db 地址/aux 或实际聚合查询结果。主线固定数值是有价值的**带出处的启动参考/比较基线**，不是 XNU 可直接发送给这台设备的已验证命令。

下一步工程选择：如果验证本机 firmware 接受“无 DDR scaling”模式，可以在严格设备门槛和隔离测试中对照主线表做最小 GMU/HFI 启动；若要保留 Polaris 厂商 13 档 DDR，必须通过合法 owner 查询取得本机 command-db 与 bus 输出。无论哪种方式，仍缺真实的 XNU 电源/时钟、GMU、PAS13 和 SMMU 后端；没有 GPU 执行证据。不要提交从 GPL-2.0 Linux 源码复制的 C 驱动到 APSL/XNU；本研究只记录数值和来源。
