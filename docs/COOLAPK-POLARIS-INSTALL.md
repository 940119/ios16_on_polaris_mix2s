# 小米 MIX 2S 运行 iOS 16：内核打包、iOS 构建与 boot / userdata 刷入教程

> **第一步：警告——本流程会永久清空手机数据。** `fastboot flash userdata` 会覆盖 Android 的用户分区（照片、聊天记录、应用及内部存储）；`fastboot flash boot` 会替换原有 Android 启动镜像，刷错可能无法正常启动。先在设备外完成并验证备份：`userdata`、原厂 `boot.img`、`dtbo`、`vbmeta`、`persist`、`modemst1/2`、`fsg`、`fsc`。不要上传含设备标识的备份。**只限自己已解锁 bootloader 的 Mi MIX 2S（`polaris`）；任何一步不确定就停下。不要重新锁 bootloader。**
>
> 本文是手工构建指南，**不是一键 ROM**。发布的 `kernel.release.sweet` 是 XNU Mach-O，不是 Android boot.img，**绝对不要执行 `fastboot flash boot kernel.release.sweet`**。要先将内核、Polaris ADT、启动参数与 boot shim 打包成完整 `out/shim-polaris-xnu.img`，核对后才可对 boot 分区执行 `fastboot flash boot`。旧有 0090 测试是临时 `fastboot boot`，**永久 flash boot 尚未由本教程独立验证**，其风险更高。GPU、八核 A75、日用稳定性均未验证。

## 0. 准备及资料来源

- 源码：[项目主页](https://github.com/luuu20309/ios16_on_polaris_mix2s)；[0090 内核 Release](https://github.com/luuu20309/ios16_on_polaris_mix2s/releases/tag/polaris-kernel-0090)，下载 `kernel.release.sweet`（12,285,320 字节；SHA-256 `9c19baf0c804992bc72dc0a436ca23a36688f2e22db49a168fb4da24c344c3df`）。[构建来源 CI](https://github.com/luuu20309/ios16_on_polaris_mix2s/actions/runs/39381936308)，对应源码 `5c1c2bdc84be58052fbd304a8243158386ae46f3`。内核文件名沿用 SWEET 机器配置，但此项目的 0090 补丁和下列 ADT／boot image 是 Polaris 专用。
- 自己手机的原厂 `boot.img`：用于提取**匹配自己设备/固件的原厂 DTB**及系统恢复。不要下载陌生机主的 boot.img。本机 Android/Recovery 中使用自己可信的备份工具取得；不要发布备份。`tools/unpack_bootv2.py` 和 `tools/polaris_dtb.py` 用于提取/打包。DTB 错误不得继续。
- 一台运行 Linux 的 PC（建议 Ubuntu/Debian，至少 40GB 空闲）用于本地处理 Apple 官方 iPhone X `iPhone10,3` iOS 16.7.16 (20H392) IPSW，生成 HFS+ 根卷；另需 Python 3、Google platform-tools、`blacktop/ipsw`（提取 Apple 设备树）和 AArch64 交叉编译器或 Zig（编译 shim）。Windows 可用 Git Bash 做 shim/ADT 打包，Linux 做根卷。
- 本仓库**不分发** IPSW、Apple 用户态、iOS 根卷、Qualcomm 固件、原厂 DTB 或可直接刷的完整 boot.img。所有涉及它们的操作都在你本机完成。

从仓库根目录运行下列命令。本文中 `out/`、`research/ipsw-x/` 为本机工作目录，不要 `git add`/公开上传。文件名中的 `.sweet` 是编译配置名；**不能借此给 Redmi Note 10 Pro 刷 Polaris 镜像**。

## 1. 先备份、核对设备，再下载内核

先确认备份能从其他存储读回、原厂 `boot.img` 与本机固件匹配，电量和 USB 线稳定。进入 fastboot 后：

```sh
fastboot devices
fastboot getvar product
fastboot getvar unlocked
```

必须只有**一台**设备，`product: polaris` 且 bootloader 已解锁。不要把包含序列号的原始输出贴到酷安。若机器无法进入 fastboot 或已经报错，**此处停止**。不要猜分区名、不要给 `recovery`/`dtbo`/`vbmeta` 写测试镜像。

从 [Release](https://github.com/luuu20309/ios16_on_polaris_mix2s/releases/tag/polaris-kernel-0090) 下载内核并置于仓库根目录，核对 SHA-256：

```sh
sha256sum kernel.release.sweet
# 必须等于 9c19baf0c804992bc72dc0a436ca23a36688f2e22db49a168fb4da24c344c3df
```

Windows PowerShell 可用 `(Get-FileHash .\kernel.release.sweet -Algorithm SHA256).Hash.ToLower()`。不匹配立即停止。**不要直接 flash 这个内核文件**。

## 2. 用内核制作完整 Polaris boot.img

先在电脑上从**自己的**原厂 boot 备份提取 DTB。以下示例假设备份为 `backup/boot.img`；`--from-boot` 会扫描原厂内核所附 DTB 并列出机型信息。若未找到可信 DTB 或和本机板型不符，立刻停止，不要借用别人的 DTB：

```sh
mkdir -p out
python3 tools/unpack_bootv2.py backup/boot.img out/stock-boot-check
python3 tools/polaris_dtb.py --from-boot backup/boot.img out/polaris-dtb.bin
```

先从自己合法取得的 iPhone X IPSW 中取出 `DeviceTree.d22ap.im4p`，再提取**解封装 ADT**到 `research/ipsw-x/d22ap.adt`，供 `mkadt.py` 导入 `/product`。例如使用 `unzip` 从本机 IPSW 解出目标文件（实际路径以包内目录为准），并安装 [blacktop/ipsw](https://github.com/blacktop/ipsw/releases)：

```sh
mkdir -p research/ipsw-x
unzip -j /path/to/iPhone10,3_16.7.16_20H392_Restore.ipsw \
  'Firmware/all_flash/DeviceTree.d22ap.im4p' -d research/ipsw-x
ipsw img4 im4p extract research/ipsw-x/DeviceTree.d22ap.im4p \
  -o research/ipsw-x/d22ap.adt
python3 tools/mkadt.py out/polaris.adt --polaris --ram 6
```

6GB 的 RAM 上限配置适用于已验证的 6GB 流程；8GB 机型若要使用更多内存，参见[详细工程记录](<POLARIS-MIX2S.md>)，不要在首次刷写时改多个变量。**若 `mkadt.py` 报 `missing, no /product node` 则停止**，先检查 Apple ADT 提取路径/格式。不要把 Apple 文件提交到 Git。

0090 真机触摸测试的 payload 参数如下，整行保留（含禁用 sweet 触摸与 Polaris 的 GPIO99 复位开关），并设置容量上限及 CPU 拓扑预检：

```sh
python3 tools/mkpayload.py kernel.release.sweet out/polaris.adt out/polaris-payload.bin \
  --check-polaris-cpus --max-payload-size 0xdea000 \
  --cmdline 'debug=0xc serial=3 cpus=4 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=30 sweetfb_polaris=1 sweettouch=0 sweetkeys=0 polaristouchdiag=1 polaristouch=1 polaristouchhid=1 polaristouchreset=1'
PLATFORM=polaris shim/build.sh ../out/polaris-payload.bin
```

`shim/build.sh` 会选择本机 AArch64 GCC 或 Zig；先安装编译器并确认 `out/polaris-dtb.bin` 是从**当前手机**的原厂备份构建的。**警告：旧脚本只对超大 payload 发 warning；必须依赖上述 `--max-payload-size` 的硬拒绝，而不是忽略警告。** 打包后独立解包并比较 ramdisk：

```sh
python3 tools/unpack_bootv2.py out/shim-polaris-xnu.img out/check-polaris
cmp out/check-polaris/ramdisk out/polaris-payload.bin
sha256sum out/shim-polaris-xnu.img
```

确认输出是 Android boot.img v1、内含 shim 内核和完全一致的 payload ramdisk；**这才是能给 `fastboot` 用的镜像**，与 Release 的 Mach-O 内核是两个文件。不要复制别人生成的 boot.img 的 SHA 当作自己的校验值。

### 2.1 刷入 boot

推荐先以**同一份完整镜像**运行 `fastboot boot out/shim-polaris-xnu.img` 做临时启动、记录结果，能够回到 fastboot 且确认正常后，再考虑永久刷写。没有可用 iOS 根卷时未必能进入用户态，这不证明镜像正确。若你仍明确选择永久写入，重新核对只有一台 `product: polaris` 的设备、原厂 boot 备份确实可用，**由你本人执行**：

```sh
fastboot flash boot out/shim-polaris-xnu.img
```

这里只写 `boot`，不写 `recovery`、`dtbo`、`vbmeta`。刷成功只表示 fastboot 写入成功，**不是 iOS 启动成功**。刷完不要锁 bootloader。此步会替换 Android 原 boot，Android 可能无法启动；失败时回到已解锁 fastboot，使用自己此前备份的**原厂** `backup/boot.img` 恢复 `boot`（核对来源/哈希后再执行 `fastboot flash boot backup/boot.img`），必要时通过原厂完整固件恢复。不要用本文的 XNU 内核裸文件恢复 boot。

## 3. 在 Linux 本地构建 iOS 用户态根卷

**下面的处理在 PC 上，不读写手机。** 自行从 Apple 官方渠道获取匹配 iPhone X (`iPhone10,3`, D22AP) 的 iOS 16.7.16 (20H392) IPSW；不要将它或解出的 DMG 上传到项目/酷安。`tools/mkiosroot.sh` 输入分别为实际的 OS、SystemOS、AppOS 磁盘映像，**不能把字符串 `OS.dmg` 当作通用固定文件名**。从你取得的 IPSW `BuildManifest.plist` 核对三种角色与具体路径后，解压到本机，例如：

```sh
# 以下三个路径是你依据 BuildManifest.plist 识别并解压出的真实文件；不要按示例猜名称
OS_DMG=/path/to/OS.dmg
SYSTEMOS_DMG=/path/to/SystemOS.dmg
APPOS_DMG=/path/to/AppOS.dmg
sudo apt update
sudo apt install apfs-fuse hfsprogs rsync python3 android-sdk-libsparse-utils

tools/mkiosroot.sh "$OS_DMG" "$SYSTEMOS_DMG" "$APPOS_DMG" ios16-root.img 16G
sudo fsck.hfsplus -fy ios16-root.img
sudo mkdir -p /mnt/iosroot
sudo mount -t hfsplus -o loop,force ios16-root.img /mnt/iosroot
sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot
sudo umount /mnt/iosroot
img2simg ios16-root.img ios16-root.simg
sha256sum ios16-root.simg
```

`mkiosroot.sh` 把 iOS 系统卷和 cryptex 复制到**本地**区分大小写的 HFS+ 镜像并生成启动需要的目录；`sweet-root-tweaks.py` 名字是历史沿用，处理的是 iPhone X 用户态，并不会变出 MIX 2S 缺失的 GPU/基带/安全硬件。确认 fsck、mount、脚本、umount、img2simg 每步均成功，磁盘空间足够，并核对稀疏镜像对应的逻辑大小和实际分区容量。**任何失败都停下，不刷半成品。**

## 4. 最后刷入 userdata（真正清空 Android 数据）

再次确保备份已完成，boot 镜像来源和回退路径清楚，当前只有一台 `product: polaris`，`ios16-root.simg` 是**你自己本机构建且校验过**的文件，并明确接受清除 Android 数据。MIX 2S 的 `userdata` 要足以容纳 16GB HFS+ 卷；不同固件或存储布局需自行核对。仅在上述条件均满足且你本人确认时执行：

```sh
fastboot devices
fastboot getvar product
fastboot -S 512M flash userdata ios16-root.simg
```

等待每个分块返回 `OKAY`，**不要拔线或断电**。若 fastboot 连接卡住，不要重复 flash：先检查状态、重新进入 bootloader 并确认写入结果；必要时恢复原厂 boot、Android userdata/原厂固件。**刷 userdata 后，原 Android 数据不会自动回来。** 完成后可尝试启动前述完整 boot.img；没有把 XNU 镜像留在 boot 的话可用 `fastboot boot out/shim-polaris-xnu.img` 临时启动。永久刷 boot 不会绕过 iOS 根卷所需的数据写入。

## 5. 失败与功能边界

卡 Apple logo／panic／触摸无响应时**停止进一步写入**，保留错误照片和脱敏日志，先确认 0090 内核哈希、boot 镜像构成、ADT 是否含 `/product`、RAM 档位、GPIO99 复位开关及根卷的实际角色；可参照[项目现状和已知错误](<../README.md>)与 [NOTES](<../NOTES.md>)。不要公开设备序列号、分区备份、Apple 镜像和 Qualcomm 固件。若只能进 fastboot，先恢复自己保存的原厂 boot；已清空的 userdata 需用备份/原厂固件恢复，而不是再刷同一份未知 iOS 镜像。

0090 镜像曾被用户确认锁屏单／双指，**本教程组合出的 boot.img 与那次测试镜像未做逐字节一致性验证，更未证明永久刷 boot 后成功**。CPU 仍采用已测的 `cpus=4`；GPU/Metal、A75 八核、Wi‑Fi、蜂窝、蓝牙、音频、相机、传感器、GPS、SEP/Face ID、正式十指与日用安全性都没有完成或验证。整套系统绕过部分 Apple 安全机制，请勿存放私人数据。本文所有 `fastboot flash` 都是给机主自行核对后执行的指令，作者未在你的手机上运行它们。
