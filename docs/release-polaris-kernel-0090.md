# Polaris 0090 XNU kernel — experimental source/packaging component

Mi MIX 2S (`polaris`, SDM845) **kernel-only** release built by [CI 39381936308](https://github.com/luuu20309/ios16_on_polaris_mix2s/actions/runs/39381936308) from commit [`5c1c2bdc84be58052fbd304a8243158386ae46f3`](https://github.com/luuu20309/ios16_on_polaris_mix2s/commit/5c1c2bdc84be58052fbd304a8243158386ae46f3). The user confirmed one- and two-finger S3330 touch after a temporary boot of a locally packaged 0090 image. That observation is distinct from CI compile/link success; this release does **not** assert long-term stability.

| Attachment | Size | SHA-256 |
| --- | ---: | --- |
| `kernel.release.sweet` | 12,285,320 bytes | `9c19baf0c804992bc72dc0a436ca23a36688f2e22db49a168fb4da24c344c3df` |

The `.sweet` suffix is the XNU machine-configuration name retained by this port; **do not install it on a Redmi Note 10 Pro**. The attachment is an arm64 Mach-O **kernel, not an Android `boot.img`**, and cannot be flashed directly. No shim, proprietary DTB/firmware, Apple userspace/root filesystem, backup or device image is included. Later GPU diagnostic patches 0091–0093 and the CPU topology checker are **not part of this 0090 binary**. GPU acceleration, Metal and A75 eight-core bring-up have not been demonstrated on this phone.

Read the [酷安从零构建与刷入教程](https://github.com/luuu20309/ios16_on_polaris_mix2s/blob/main/docs/COOLAPK-POLARIS-INSTALL.md) before handling the attachment and the [Polaris engineering guide](https://github.com/luuu20309/ios16_on_polaris_mix2s/blob/main/docs/POLARIS-MIX2S.md). `fastboot flash boot kernel.release.sweet` is **wrong**. After personally building and verifying a *complete* device-matched Android boot image with shim, payload, ADT and stock DTB, its owner may choose `fastboot flash boot out/shim-polaris-xnu.img` (permanent and riskier than a temporary boot); this permanent path has not been independently validated here. `userdata` root-image installation is separately destructive and needs its own backup/explicit confirmation. Verify `product: polaris`, SHA-256, ADT, stock DTB and payload-size limit before any write. Retain CPU framebuffer as fallback.

This is a historical **0090** test milestone, not the latest development kernel. No Apple or Qualcomm proprietary binaries have been uploaded.
