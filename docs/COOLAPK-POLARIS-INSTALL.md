# 酷安发布稿｜小米 MIX 2S 运行 iOS 16：内核下载与安全测试说明

> 本文可复制到酷安发布；最后核对日期、Release 链接和本机机型再发布。**这是研发阶段，不是“一键刷机包”或可日用 ROM。** Release 只提供 XNU Mach-O 内核，**不能把它直接 `fastboot flash boot`**。截至 2026-10-23，用户确认 0090 临时镜像到达 iOS 锁屏并支持单／双指；GPU 加速、A75 八核运行尚未在 MIX 2S 上验证。

## 下载什么

- 项目：[luuu20309/ios16_on_polaris_mix2s](https://github.com/luuu20309/ios16_on_polaris_mix2s)
- 0090 内核候选 Release：[polaris-kernel-0090](https://github.com/luuu20309/ios16_on_polaris_mix2s/releases/tag/polaris-kernel-0090)
- 内核附件 `kernel.release.sweet`：**12,285,320 字节**；SHA-256：`9c19baf0c804992bc72dc0a436ca23a36688f2e22db49a168fb4da24c344c3df`。`.sweet` 是沿用的 XNU 机器配置名称，并不意味着这个工件适用于 Redmi Note 10 Pro。
- 该内核来自 [XNU CI 39381936308](https://github.com/luuu20309/ios16_on_polaris_mix2s/actions/runs/39381936308)，源码提交 `5c1c2bdc84be58052fbd304a8243158386ae46f3`。**CI 成功是编译/链接证据；0090 真机触摸结果另来自用户测试。**
- Release **不包含** Android boot.img、原厂 DTB、iOS 根镜像／IPSW、专有固件、设备密钥／备份。其他提交（例如 GPU 0091–0093、CPU 预检）**没有**包含在 0090 内核里，也没有 GPU 驱动。

## 先读：这个文件不能直接刷

XNU 内核只是多组件之一。要从 fastboot 临时启动 MIX 2S，还需按[项目 Polaris 完整构建说明](<POLARIS-MIX2S.md>)，使用**自己设备匹配的原厂 DTB**、本机生成的 Polaris ADT、boot shim、由 Apple 正式渠道取得并在本地构建的 iPhone X 16.7.16 用户态及 HFS+ 根卷。boot shim 的 ramdisk 容纳打包后的内核/ADT/启动参数，原厂 DTB 则由 shim 打包流程提供；内核单独不是 Android boot.img。

**任何 `fastboot flash boot kernel.release.sweet`、`fastboot flash recovery ...`、交叉刷别人的 `boot.img` 都是错误操作。** 不要重锁 bootloader，也不要拿 Redmi Note 10 Pro 的 DTB、触摸供电或 userdata 参数用于 MIX 2S。仓库及本文均不提供 Apple 用户态或能直接刷入的 iOS ROM。

## 零写入检查（先做这部分）

1. 明确手机确实是小米 MIX 2S（`polaris`，非 MIX 2、MIX 3、Redmi Note 10 Pro），电量和 USB 连接可靠；PC 已正确安装 Google platform-tools。
2. **先离线备份**你的 `userdata` 和逐机校准/分区数据（包括 `persist`、`modemst1/2`、`fsg`、`fsc`、`boot`、`dtbo`、`vbmeta`）；妥善留在本机，不上传酷安、网盘公开目录或 GitHub。确认能恢复再继续。
3. 从上述 Release 下载内核，在本机校验 SHA-256。Windows PowerShell：

```powershell
(Get-FileHash .\kernel.release.sweet -Algorithm SHA256).Hash.ToLower()
```

Linux/macOS：

```sh
sha256sum kernel.release.sweet
```

结果不等于 `9c19baf0c804992bc72dc0a436ca23a36688f2e22db49a168fb4da24c344c3df`，立即停止，不打包、不启动。

4. 只读核对 bootloader 已解锁且设备唯一。进入 fastboot 后执行 `fastboot devices` 和 `fastboot getvar product`，必须看到**仅一台设备**、`product: polaris`。命令可能回显序列号，**不要把完整输出发到公开评论区**。连接异常时不要盲试写分区命令。

## 构建个人临时测试镜像（适合有 XNU／Android 打包经验的开发者）

依照[Polaris 工程指南](<POLARIS-MIX2S.md>)生成 6GB 安全上限 ADT 和自己的 boot shim 镜像。`tools/mkadt.py --polaris --ram 6` 生成的是 Polaris 专用树；将下载的内核置于本地工作目录、用 `tools/mkpayload.py` 包装，并显式指定经过测试的 `cpus=4`、`sweettouch=0 sweetkeys=0` 和与 0090 对应的实验触摸选项。`--check-polaris-cpus` 会验证 8 个 CPU 节点及命令行，但**不会启动 A75**。完整示例和实际启动参数参见工程指南，不从本帖子随意拼接缺项命令。

打包前强制检查 payload 小于 ABL ramdisk→DTB 间隔 **0xDEA000（14,589,952 字节）**，逐字节解包比对 ramdisk，确认所用原厂 DTB 是**这部手机对应的来源**。这次发布的 `kernel.release.sweet` 只对这套已知 0090 代码作说明；自己重新打包后的 image SHA-256 必然与作者测试镜像不同，必须由打包者自行检验。不同底层系统和存储布局不保证成功。

## 启动与“刷入”到底有何区别

- **首选临时启动**：当你已有与**自己手机**匹配、容量/解包均检查通过的完整 Android boot 镜像，取得该设备操作者的明确同意后，才从 fastboot 使用 `fastboot boot <你的完整polaris镜像.img>`。该命令不改写 boot 分区，但 iOS 仍可能对已准备的 `userdata` 作写入，**不等于零风险**。
- **根卷写入是破坏性的**：要真正启动 iOS 用户态，原指南使用 `userdata` 中的 HFS+ 根卷。`fastboot flash userdata` 会删除原有 Android 数据；本文**没有提供可公开通用的 root 镜像，也不建议新手照抄这条命令**。只有独立备份、核对镜像和分区、亲自确认愿意清空数据后，按[完整工程指南](<POLARIS-MIX2S.md>)自行制作/写入。
- **绝不因为 Release 里有内核就刷 boot**：本发布没有完整 boot.img；即使自制镜像临时启动成功，也不代表适合写入 boot 分区。

若卡在 fastboot、Apple logo、panic 或无触摸：停止后续写入，返回备份可恢复的 Android/recovery 环境；拍照记录不含序列号的报错并阅读[进度/已知错误](<../README.md>)与 [NOTES 实验记录](<../NOTES.md>)。历史 0088 地址 NACK、0089 F12 无效帧与 GPIO99 复位的问题已在 0090 路线上处理，但本项目仍缺完整 0090 真机 ring；新机型/新固件并无通过保证。

## 当前无法保证的内容

GPU、Metal、Wi‑Fi、蓝牙、蜂窝、音频、相机、传感器、GPS、Face ID/SEP、完整十指、A75 八核运行和日常安全性均**未在此工件中实现/验证**。屏幕仍由 CPU 软件渲染；GPU 0091/0092 的元数据 CI 是后续独立工作，0093 默认拒绝继续。不要宣称“流畅满帧 GPU”“八核已修复”或“完美移植”。遇到问题请附内核来源提交、机型/内存档位、镜像哈希、启动参数（移除设备 ID）及经脱敏的日志；**绝不上传原始分区备份、Apple 镜像和固件**。

**免责声明：**仅供自有设备上经知情同意的研究与开发；刷机有丢失数据及无法启动风险。本文没有授权作者或其他人替你刷机，不含可直接刷入的通用 ROM。
