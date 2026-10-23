# 小米 MIX 2S（polaris / SDM845）运行 iOS 16 —— 成品与刷入教程

本文对应仓库里的 polaris 移植改动。请先读完第 0 节，确认你拿到的是什么、能做什么、不能做什么。

> **最短路径（今天就能做）**：解锁 bootloader → `fastboot boot out/shim-polaris-boot.img` → 拍下红色诊断页 → 长按电源复位。这一步不改任何分区。完整 iOS 流程在第 4–8 节。
>
> 命令默认在仓库根目录执行。Windows 上用 **Git Bash**；如果系统里只有 `python` 没有 `python3`，把命令里的 `python3` 换成 `python`，`ipsw` 换成 `ipsw.exe`。

---

## 0. 先说实话

### 现在能直接给你的成品

`out/shim-polaris-boot.img`（本机已编译、已校验）：

| 项目 | 值 |
| --- | --- |
| 大小 | 6,479,872 字节 |
| sha256 | `4bea1e96b8653ce34ad1a69046c1fa9a8b82fef3d5d9a343b555ed1012a8559f` |
| 内容 | polaris 探测 shim + 原厂 14 个 DTB 拼接尾 + 空 ramdisk |
| 用途 | **仅用于 `fastboot boot` 临时启动，不写任何分区** |

它会接管屏幕，打印板型、内存范围、UFS GPT 分区表，约 3 分钟后自动重启（设计上回到 bootloader）。它**不是 iOS**，不会挂载或写入 userdata。

### 完整 iOS 需要你自己在另外两台机器上完成

1. **macOS（GitHub Actions 免费 runner）**：编译 XNU 内核。
2. **Linux**：用 iPhone X 的 iOS 16.7.16 IPSW 制作 HFS+ 根镜像。
3. **本机/Windows**：生成 ADT 和 payload、打包 boot.img、`fastboot` 刷入。

Apple 的根文件系统不能随仓库分发，XNU 也不能在 Windows 上编译，所以**完整的、可直接刷的 iOS 包无法在这里交付**。本教程把整条流水线写成可复制的命令。

### 关键限制（请务必读完）

- **polaris 的触摸和按键驱动还没有移植。** sweet 的触摸驱动是 Goodix GT9896（I²C `0xa84000`、地址 `0x5d`）；MIX 2S 是 Synaptics DSX 屏内触摸（I²C 地址 `0x20/0x2c`、IRQ GPIO 125、复位 GPIO 99），寄存器协议完全不同。按键也一样：sweet 是 PM6150/PM6150L，MIX 2S 是 PM8998/PMI8998。**所以即使完整 iOS 启动成功，也会停在锁屏，无法解锁、无法操作。** 现阶段它是“能点亮的实验品”，不是能用的手机系统。
- **没有 GPU。** 全部由 CPU 软件渲染。没有 Wi-Fi、蓝牙、基带、音频、相机、传感器、Face ID。
- **探测镜像没有在真实 MIX 2S 上验证过**（本环境没有设备）。它的作用是先花 3 分钟验证屏幕、DTB、UFS 三条最关键的路径。探测结果决定后面是否值得继续。
- 完整流程会**清空 Android 的 userdata（照片、应用、聊天记录全部丢失）**。只刷 userdata，不动 bootloader / modem / persist，但数据不可恢复。
- 解锁 bootloader 是前提。小米的解锁等待期（通常 168 小时）无法绕过。

---

## 1. 交付物与校验

文件都在仓库的 `out/`；探测镜像的副本在 `dist/polaris-mix2s/`（ADT/DTB 只放在 `out/`）：

```
out/shim-polaris-boot.img     # 探测镜像（先测这个）
out/polaris.adt               # 6GB 版 ADT 样例（当前不含 /product，完整流程必须重新生成）
out/polaris-8g.adt            # 8GB 版 ADT 样例（同上）
out/polaris-dtb.bin           # 从原厂 boot.img 拼出的 14 个 DTB
```

校验（Windows PowerShell）：

```powershell
Get-FileHash .\out\shim-polaris-boot.img -Algorithm SHA256
```

Git Bash / Linux：

```sh
sha256sum out/shim-polaris-boot.img
# 必须等于 4bea1e96b8653ce34ad1a69046c1fa9a8b82fef3d5d9a343b555ed1012a8559f
```

> `polaris.adt` / `polaris-8g.adt` 是用 `tools/mkadt.py --polaris` 生成的，但当前缺少 iPhone X 的 `/product` 节点（`research/ipsw-x/d22ap.adt` 不存在）。**完整 iOS 必须按第 5 节重新生成**，不要直接拿这两个去打包 payload。

---

## 2. 前置条件

- 小米 MIX 2S（代号 polaris，骁龙 845），**bootloader 已解锁**。
- Windows 10/11、Linux 或 macOS 任选一台做刷机；另需一台 Linux 做根镜像。
- Android platform-tools（fastboot/adb）：<https://developer.android.com/tools/releases/platform-tools>
- 原装或质量好的 USB 数据线。刷大镜像时优先用 USB 2.0 口，避免中途掉线。
- 电量 50% 以上。

确认机型与内存（Android 正常启动时）：

```sh
adb shell getprop ro.product.device     # 应为 polaris
adb shell getprop ro.product.model      # Mi MIX 2S
adb shell cat /proc/meminfo | head -1   # MemTotal 约 5.6GB=6GB 版，约 7.5GB=8GB 版
```

6GB 版和 8GB 版的高位内存大小不同。**默认用 6GB 参数，它在 6GB/8GB 机器上都安全**；只有确认是 8GB 才用 `--ram 8`。用错会导致内核访问不存在的内存，表现为秒重启或蓝屏。

### 备份（强烈建议）

进入 OrangeFox 等带 root 的 recovery 后，把下列分区拉到电脑（不要提交到 git）：

```sh
adb root
mkdir -p backup
for p in persist modemst1 modemst2 fsg fsc boot dtbo vbmeta; do
  adb pull /dev/block/bootdevice/by-name/$p backup/$p.img
done
```

`persist`、`modemst*`、`fsg`、`fsc` 是每台机器的校准/IMEI 数据，丢了很难恢复。**永远不要刷 bootloader、modem、persist。**

---

## 3. 第一步：启动探测镜像（不写分区，先做这个）

把手机关机，按住 **电源 + 音量下** 进入 fastboot，连 USB：

```sh
fastboot devices
# 应看到一行序列号 + fastboot

fastboot boot out/shim-polaris-boot.img
```

Windows `cmd` 里如果 `fastboot` 不在 PATH，就在 platform-tools 目录里执行 `fastboot.exe boot D:\path\to\shim-polaris-boot.img`；PowerShell 用 `.\fastboot.exe`。

### 预期屏幕内容

屏幕会先变红，然后出现黄色标签 + 白色文字的诊断页（3 倍放大的等宽字体）：

```
SHIM    polaris milestone 4: xnu loader
BOARD   Xiaomi Technologies, Inc. Polaris ...  ← 取自实机 DTB 的 model 字符串
RAM     hi=00000001 lo=00000000         ← 高位内存 0x100000000
RAM     MiB=xxxxx idx=0                 ← 这一段是实际可用大小
...
INITRD  lo=xxxxxxxx hi=xxxxxxxx
LOADER  no SWEETXNU payload
UFS     milestone 5 probe (read-only)
UFS     ver=... cap=...
UFS     hce=... hcs=...
UFS     is=... rsr=...
UFS     utrlba=... hi=...
UFS     dbr=... nutrs=...
UFS TUR rc=... ocs/st=...
UFS CAP rc=... ocs/st=...
UFS LUN0 lastlba=... blk=...
UFS MBR sig=... type=...
UFS GPT sig=...
UFS GPT entries=... lba=...
UFS PART userdata lba=... MiB=...
UFS PART ...
UFS     probe done, reset in ~3 min
```

要点：

- `BOARD` 显示 MIX 2S 的型号 → DTB 和 ABL 传递链路正常。
- `RAM hi=00000001` → 高位内存存在；`MiB=` 可以反推是 6GB 还是 8GB（6GB 版高位约 2GB 量级，8GB 版约 4GB 量级；以实机探测值为准）。
- `UFS PART userdata` → UFS 驱动能读到 LUN 0 的 GPT，并且 Android 的 userdata 分区名符合内核 `rd=ufsroot0` 的查找条件。
- 若这里显示 `LOADER no initrd in /chosen` 而不是 `INITRD ...`，说明 ABL 没有把空 ramdisk 传给 shim；不影响后面的 UFS 探测，但要记下来。
- 约 3 分钟后出现 `RESET in 20s` 并自动重启。**这是正常的**，说明探测流程走完了。通常回到 fastboot；若直接进了系统，按住电源 + 音量下即可。

> 探测镜像只读 UFS（源码里的 `test_ufs()` 只发 READ/TUR 命令），不写 userdata、不碰 boot 分区。想提前结束，长按电源约 12 秒。

### 排错

| 现象 | 含义 / 处理 |
| --- | --- |
| `fastboot: error: cannot load ...` | 当前目录不对，或路径写错；用绝对路径 |
| `FAILED (remote: 'Failed to load/authenticate boot image')` / 类似 | bootloader 没解锁，或该机型禁用了 `fastboot boot`；先完成官方解锁 |
| 命令返回 OKAY，但屏幕一直黑 | shim 可能仍在运行：等约 3.5 分钟看是否自动重启。若会重启说明代码在跑，只是屏幕路径/时序不对，把现象记下来；若不会重启，长按电源复位 |
| 屏幕有字但文字错位、斜条、重影 | 原厂 splash 的 stride 不是假设的 4320；记下照片，后面要改 `FB_STRIDE` 和 ADT 的 `stride` |
| 蓝屏 `PANIC ...` | shim 异常。拍照记录 `SLOT/ESR/ELR/FAR/STAGE`，这是最有用的调试信息 |
| 直接重启、没有红屏 | ABL 拒绝了镜像；看 `fastboot` 终端的报错 |

**请把 `BOARD`、`RAM` 各行、`UFS PART userdata` 和是否约 3.5 分钟自动重启这四项结果记下来。** 这些决定下一步的参数（6G/8G、UFS 根设备、显示是否可用）。

> 如果 `BOARD` 是别的机型或 `RAM` 明显不对，说明探测镜像里拼的是本仓库 `dtb/polaris_stock/` 的原厂 DTB 尾，和你的固件版本不一致。用你自己机器的原厂 boot.img 重新生成（`tools/polaris_dtb.py` 会解 gzip 内核、切出后面 14 个 DTB 并打印板型）：
>
> ```sh
> python3 tools/polaris_dtb.py --from-boot backup/boot.img out/polaris-dtb.bin
> PLATFORM=polaris shim/build.sh
> ```
>
> 正常输出应列出 14 个 `Xiaomi Technologies, Inc. Polaris ...` 板型，末尾是 `out/polaris-dtb.bin: 6278444 bytes, 14 DTBs`。然后再 `fastboot boot out/shim-polaris-boot.img` 测一次。原厂 DTB 的拼接必须保留 ABL 的 board-id/msm-id 选择机制，不能只留一个 DTB。

---

## 4. 第二步：在 GitHub Actions 上编译 XNU

内核必须在 macOS 上编译。仓库的 `.github/workflows/xnu.yml` 已经配置好 `macos-14`，会自动拉取 xnu-8796、按编号应用 `xnu/patches/`，并上传 `kernel.development.sweet`。

### 4.1 推到你的 fork

当前工作副本就是改好的仓库。先确认改动都在：

```sh
git status
git add .gitignore shim tools xnu/patches docs/POLARIS-MIX2S.md
git commit -m "polaris: Mi MIX 2S bring-up (probe, ADT, display patches)"
```

在 GitHub 上 fork `dipa-studio/ios-sweet`，然后：

```sh
git remote add fork https://github.com/<你的用户名>/ios-sweet.git
git push fork main
```

**关键检查**：fork 的 `xnu/patches/` 里必须有
`0082-sweet-polaris-display-scaling.patch` 和 `0083-sweet-polaris-early-fb.patch`。
CI 只编译这两个补丁生效后的内核；漏了它们，显示会按 sweet 的 1080×2400 逻辑走，MIX 2S 上会错位。

### 4.2 触发构建

网页（推荐，不依赖 token）：GitHub → 你的 fork → **Actions** → **xnu** → **Run workflow**。如果 fork 后 Actions 页提示工作流未启用，先点 **I understand my workflows, go ahead and enable them**。

- Branch: `main`
- `macos_version`: `13.5`
- `kernel_config`: `DEVELOPMENT`
- `machine_config`: `SWEET`（polaris 没有单独的 machine config；极性相关行为由 ADT + 启动参数开启）

或者用 `gh`：

```sh
gh auth login
gh workflow run xnu.yml -R <你的用户名>/ios-sweet --ref main \
  -f macos_version=13.5 -f kernel_config=DEVELOPMENT -f machine_config=SWEET
gh run list -R <你的用户名>/ios-sweet --workflow xnu.yml
```

成功后下载内核：

```sh
gh run download <RUN_ID> -R <你的用户名>/ios-sweet -n xnu-kernel -D xnu/out
ls xnu/out
# 应包含 kernel.development.sweet
```

如果构建失败，先确认 `0082-sweet-polaris-display-scaling.patch` 和 `0083-sweet-polaris-early-fb.patch` 确实在 fork 里、且是当前本地 `main` 完整推上去的（不是只推了部分文件）。

---

## 5. 第三步：提取 iPhone X 设备树（`/product`）

完整 iOS 的 `MobileGestalt` 需要真机 iPhone X 的 `/product` 属性。没有它，`mkadt.py` 会警告 `no /product node`，系统身份/激活路径会不完整。

- 机型：**iPhone X，iPhone10,3（全球版），D22AP**
- 系统：**iOS 16.7.16 (20H392)**

IPSW 从 <https://ipsw.me> 选择 iPhone10,3 / iOS 16.7.16 下载，或使用 Apple 的直链。只提取设备树可以不用下整包：

```sh
# 需要 blacktop/ipsw：https://github.com/blacktop/ipsw/releases
python3 tools/remotezip.py "<IPSW_URL>" DeviceTree.d22ap.im4p research/ipsw-x/dt
ipsw img4 im4p extract research/ipsw-x/dt/Firmware/all_flash/DeviceTree.d22ap.im4p \
  -o research/ipsw-x/d22ap.adt
```

或者下载整包后解压 `Firmware/all_flash/DeviceTree.d22ap.im4p`，再执行上面的 `ipsw img4 im4p extract`。

确认：

```sh
ls -l research/ipsw-x/d22ap.adt
python3 tools/mkadt.py out/polaris.adt --polaris --ram 6
# 这次不应再出现 "missing, no /product node"
```

---

## 6. 第四步：在 Linux 上制作 iOS 根镜像

> 需要 Linux、至少 40GB 空闲磁盘（脚本的临时目录会放 3 个 DMG 和 16GB 镜像）、root（`sudo`）。WSL2 的 loop mount 不可靠，建议原生 Linux 或云主机。空间紧张时可以设 `TMPDIR=/path/to/big/disk`。

安装依赖（Ubuntu/Debian）：

```sh
sudo apt update
sudo apt install -y apfs-fuse hfsprogs rsync python3 android-sdk-libsparse-utils
```

IPSW 里的三个磁盘镜像名字以 `BuildManifest.plist` 为准，先列出来：

```sh
unzip -l iPhone10,3_16.7.16_20H392_Restore.ipsw | grep -Ei '\.dmg$'
```

解出 `OS.dmg`、`SystemOS.dmg`、`AppOS.dmg`（名字如有后缀差异，按实际文件名替换）：

```sh
mkdir -p ipsw
unzip -o iPhone10,3_16.7.16_20H392_Restore.ipsw \
  'OS.dmg' 'SystemOS.dmg' 'AppOS.dmg' -d ipsw
```

制作并处理 16GB 根镜像：

```sh
tools/mkiosroot.sh ipsw/OS.dmg ipsw/SystemOS.dmg ipsw/AppOS.dmg ios16-root.img 16G

sudo fsck.hfsplus -fy ios16-root.img
sudo mkdir -p /mnt/iosroot
sudo mount -t hfsplus -o loop,force ios16-root.img /mnt/iosroot
sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot
sudo umount /mnt/iosroot

img2simg ios16-root.img ios16-root.simg
sha256sum ios16-root.simg
```

说明：

- `mkiosroot.sh` 生成区分大小写、带 journal 的 HFS+ 卷，把系统卷和两个 cryptex 放进同一个卷，并补齐 dyld cache 链接、system keybag、日志目录。
- `sweet-root-tweaks.py` 是 bring-up 用的通用改动（跳过 Setup、预置激活状态、移除基带守护进程、关闭看门狗、禁掉无 GPU 会崩溃的媒体路径等）。它名字里有 sweet，但对 iPhone X 16.7.16 的根卷同样适用。
- 脚本拒绝覆盖已存在的输出文件，这是故意的。
- 16G 镜像只占 userdata 开头；MIX 2S 的 userdata 远大于 16G（64G/128G/256G 版本都够）。

---

## 7. 第五步：生成 ADT、payload 和最终 boot.img

以下命令在仓库根目录执行（Windows Git Bash / Linux 均可，只需 Python + Zig 或 aarch64 GCC）。

### 7.1 ADT

```sh
# 6GB 版（默认，兼容 6G/8G）
python3 tools/mkadt.py out/polaris.adt --polaris --ram 6

# 8GB 版（确认是 8GB 机器再用）
python3 tools/mkadt.py out/polaris-8g.adt --polaris --ram 8
```

### 7.2 payload

```sh
python3 tools/mkpayload.py \
  xnu/out/kernel.development.sweet out/polaris.adt out/polaris-payload.bin \
  --check-polaris-cpus \
  --cmdline 'debug=0xc serial=3 cpus=4 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=30 sweetfb_polaris=1 sweettouch=0 sweetkeys=0'
```

**CPU 核心数验证（不要自动跳到 8 核）。** 已有 0089 真机日志记录 `requested=4 selected=4 (limit=8)`，CPU1–3 的 PSCI CPU_ON 返回 0 并完成 SGI，最终 `4/4`；因此当前观测到 4 核是这条命令里的 `cpus=4` 所致，**没有 A75 失败的证据，也没有 A75 成功的证据**。`tools/mkadt.py` 已按 MiCode 的 MPIDR `0x000`–`0x700` 生成 4×A55 + 4×A75 节点，`0078` 将编译上限提升至 8。`--check-polaris-cpus` 在打包前只校验 ADT/命令行的连续 CPU 前缀，不证明 PSCI 已启动核心。优先复用已验证的内核和 ADT，仅将临时镜像的 `cpus=4` 改为 `cpus=5`，核查 CPU4 的 PSCI 返回值、进入 `initCPU`、SGI 确认和进入用户态；成功后再测试 `cpus=6`、最后 `cpus=8`。每次**临时 `fastboot boot` 都先由用户确认**，失败时保留 4 核镜像和持久日志，不把卡死的核心加入调度器。

对照 [Luoxueowo/ios-polaris 固定版本](https://github.com/Luoxueowo/ios-polaris/tree/44556dec6995b8d9bb72717cf546b92b6c3dddda)：它的 0065/0078 与本项目 PSCI 启动和八核上限基本一致，并没有额外的 A75 上电修复；其 README 同时声称 8 核和“A75 未启用”，不能代替真机日志。**不要直接合并其 0088 跳过失败 CPU 的补丁**：XNU `cpu_start()` 丢弃 `PE_cpu_start` 错误，`num_cpus - skipped` 无法排除中间失败造成的 CPU ID 空洞，超时后晚到的核心也未隔离。只有实现完整处理器状态／调度器排除机制，才能安全提供降级启动。

| 参数 | 作用 |
| --- | --- |
| `rd=ufsroot0 ufsrw=1 rootrw=1` | 根文件系统在 UFS 的 `userdata` 分区，可读写 |
| `cpus=4` | 已测试的 4 个 A55 核；ADT 和内核上限原本支持 8 核，不等于 4 个 A75 已经验证上线。 |
| `sweetfb_polaris=1` | 把 iPhone X 的 1125×2436 逻辑画面按高度缩放进 1080×2160 面板，左右各 41px 黑边 |
| `sweetfb_noconsole=1` | 不再把内核文字画到 GUI 上；panic 仍会画 |
| `sweetfb_hz=30` | CPU 拷贝刷新率 |
| `sweettouch=0 sweetkeys=0` | **polaris 必须关**：sweet 的触摸/按键驱动会去操作错误的 I²C/PMIC 地址 |
| `wdt=-1 wdt_test=no_user_checkin` | 关闭硬件看门狗 |
| `debug=0xc serial=3 keepsyms=1` | 调试与符号；没有串口，日志走持久化 ring |
| `ignition_level=1 launchd_unsecure_cache=1 unentitled_ios_sim_launch=1` | 允许未密封的用户态启动 |

### 7.3 打包

```sh
PLATFORM=polaris shim/build.sh ../out/polaris-payload.bin
```

输出：`out/shim-polaris-xnu.img`（不会覆盖探测镜像 `out/shim-polaris-boot.img`）。

脚本会检查 payload 大小：polaris 的 ramdisk 载入地址是 `0x1000000`，原厂 DTB 地址是 `0x1dea000`，中间只有 **14,589,952 字节（约 14.6MB）**。payload 达到或超过这个值会警告“可能覆盖 DTB”。正常 DEVELOPMENT 内核 + ADT 应远小于该值；如果出现警告，先不要刷，检查 `mkpayload.py` 的输入是不是把根镜像也塞进了 payload（根镜像应刷到 userdata，不放进 ramdisk）。

校验：

```sh
python3 tools/unpack_bootv2.py out/shim-polaris-xnu.img out/check-xnu
cmp out/check-xnu/ramdisk out/polaris-payload.bin
sha256sum out/shim-polaris-xnu.img
```

---

## 8. 第六步：刷入并启动（会清空 Android 数据）

> 到这里为止，之前的所有步骤都没有写手机。下面第一条 `fastboot flash userdata` 会**永久清空 Android 的用户数据**。确认照片/聊天记录已备份。

```sh
fastboot devices
fastboot -S 512M flash userdata ios16-root.simg
```

- 16GB 稀疏镜像通常需要 7–15 分钟。**不要拔线、不要碰电源键。**
- 小米的 fastboot 在大镜像刷完后有时会卡住；如果终端长时间无响应，长按电源 + 音量下重新进 fastboot，再继续。
- 只刷 `userdata`。**不要** `fastboot flash boot`、`flash dtbo`、`flash vbmeta`，**不要**重新锁 bootloader。

启动：

```sh
fastboot boot out/shim-polaris-xnu.img
```

- 这是临时启动，不写 boot 分区；重启就会回到 Android（如果 userdata 已被清空，Android 会重新初始化或无法启动，需要刷回原厂）。
- 首次启动在 Apple logo 阶段可能停留 5–10 分钟，polaris 比 sweet 更没有优化过，耐心等。
- 预期画面：1080×2160 面板，中间 998×2160 是 iOS 画面，左右各 41px 黑边（`sweetfb_polaris=1` 的效果）。
- **即使成功，也会停在锁屏**：没有触摸、没有可用按键，无法上滑解锁。这是当前移植的真实状态。

### 抓日志（没有串口）

内核把 console 写在 userdata 分区末尾的 4MiB ring 里。临时启动一个带 adb root 的 recovery（例如 OrangeFox 的 polaris 版本）：

```sh
fastboot boot recovery.img
adb devices
adb root
mkdir -p logs
tools/sweet-log-pull.sh logs/boot.txt
```

`logs/boot.txt` 里找：

- `hfs: mounted ... on device b(1, 0)`：根文件系统挂载成功
- `load_init_program: attempting to load /sbin/launchd`
- `CRASH ...` 与 `SWEET ...` 行
- 若在 `bsd_init` 就 panic，通常是根设备没找到或 HFS 镜像不对

Windows 上在 Git Bash 里跑同一脚本（需要 `adb` 和 `python3` 在 PATH）。

### 回到 Android

- 只做过探测：直接重启，不会动 Android。
- 刷过 userdata：用小米官方 fastboot 包 + MiFlash（**选 “clean all”，不要选 lock**）恢复；或只把原厂 `userdata.img` 刷回（会再次清空，这是预期）。

---

## 9. 目前还没移植完的部分（决定它能不能“用”）

| 模块 | polaris 状态 | 说明 |
| --- | --- | --- |
| 触摸 | **未移植** | 硬件是 Synaptics DSX（I²C `0x20/0x2c`，IRQ GPIO125，reset GPIO99）；sweet 驱动是 GT9896，协议/地址/供电都不同 |
| 按键 | **未移植** | MIX 2S 是 PM8998/PMI8998；sweet 驱动写的是 PM6150/PM6150L 的 PON/GPIO |
| 显示 | 探测中 | 1080×2160 splash + `sweetfb_polaris` 缩放补丁已就位；面板持续扫描/刷新时序仍需实机确认 |
| GPU | 无 | Adreno 630 无驱动，CPU 软件渲染 |
| Wi-Fi/蓝牙/基带/音频/相机/传感器/Face ID | 无 | 相关守护进程会启动失败或反复重启 |
| 安全 | 关闭 | AMFI 宽松、沙箱宽松、AppleKeyStore 桩；**不要放个人数据** |

下一步的移植顺序建议：

1. 先用探测镜像确认屏幕、内存、UFS 三项。
2. 跑通“内核 + 空 HFS 测试卷”到 mount 成功（可先把 `userdata` 刷一个 8MB 空 HFS 测试卷，成本低）。
3. 再试完整 iOS 根镜像到锁屏。
4. 最后才做 Synaptics 触摸 + PMIC 按键；没有这两样，系统只能看不能用。

---

## 10. 附录：polaris 关键参数

| 项目 | 值 |
| --- | --- |
| 平台 | Qualcomm SDM845（4×Cortex-A55 + 4×Cortex-A75） |
| 设备树身份 | iPhone X `D22AP` / `iPhone10,3` |
| 逻辑显示 | 1125×2436（iPhone X 画布） |
| 物理面板 | 1080×2160，stride 4320，BGRA |
| splash framebuffer | `0x9d400000`，长 36MiB（`0x2400000`） |
| UFS | `0x1d84000`，LUN 0，GPT 分区名 `userdata` |
| GIC | distributor `0x17a00000`，redistributor `0x17a60000` |
| 高位内存 | `0x100000000` + `0x7cc00000`（6GB 安全）/ `0xfcc00000`（仅 8GB） |
| 定时器 | 19.2MHz，虚拟定时器 PPI 19 |
| 串口 | 无（调试 UART 未引出），只能看屏幕或 userdata 日志 ring |

### 原厂 boot.img 头部（探测镜像已按此对齐）

```
header v1, pagesize 4096
kernel_addr  0x8000
ramdisk_addr 0x1000000
tags_addr    0x100
dtb_addr     0x1dea000
os_version   0x200001a2
```

14 个原厂 DTB 拼接后 6,278,444 字节，已逐字节比对并原样追加到内核段末尾。

---

## 11. 常见问题

**Q：探测镜像会不会把手机变砖？**
不会写 flash。它只读 UFS、写内存里的 framebuffer、最后通过 PSCI 复位。最坏情况是黑屏/重启，长按电源即可。

**Q：`fastboot boot` 报 `unknown command`？**
有些 fastboot 版本或锁定的 bootloader 不支持。确认 bootloader 已解锁，并使用 platform-tools 最新版。

**Q：能不能不刷 userdata，先只启动内核？**
可以先用一个小的空 HFS+ 测试卷（例如 8MB）刷到 userdata，验证内核、UFS、显示；这同样会清空 Android 数据，但比刷 16GB 快得多。完整 iOS 再换 16GB 根镜像。

```sh
# Linux
dd if=/dev/zero of=test-hfs.img bs=1M count=8
mkfs.hfsplus -v SweetRoot test-hfs.img
fastboot flash userdata test-hfs.img
```

这个测试卷里没有 `/sbin/launchd`，预期是内核打印 `hfs: mounted ...` 后 panic（找不到 init）；能看到 mount 就说明 UFS + HFS 路径通了。

**Q：为什么探测页 3 分钟后自动重启？**
`shim.c` 的探测路径故意等待 180 秒后复位，方便拍照记录，不需要按键。

**Q：`mkadt.py` 一直警告 `no /product node`？**
`research/ipsw-x/d22ap.adt` 不存在或路径不对。按第 5 节提取 iPhone10,3 的设备树，确认文件名就是 `d22ap.adt`。

**Q：8GB 机器用 6GB 参数会怎样？**
能用，只是浪费约 2GB 高位内存。反过来（6GB 机器用 `--ram 8`）会访问不存在的物理内存，**不要**。

**Q：`cpus=4` 和 `cpus=8`？**
先 `4`（A55 簇）确保稳定；能进系统后再改 `8` 试 A75 簇。polaris 的 CPU 拓扑已经写进 ADT（8 个节点、kryo385 兼容串）。

---

## 12. 结果回传

探测后请记录：

1. 屏幕照片（完整诊断页，尤其 `BOARD` / `RAM` / `UFS PART` 三处）。
2. `fastboot boot` 终端的完整输出。
3. 是否约 3.5 分钟自动重启；若蓝屏，拍 `PANIC` 页。
4. `MemTotal` 或机型存储版本（6GB / 8GB）。

有了这些，才能确定下一步是修显示时序、换 8GB 参数，还是补 UFS/DTB 细节。
