# Third-party source references

This document records the external projects and source snapshots used by the
current BMX port. It distinguishes between sources that are vendored in
this repository, sources downloaded by build/staging scripts, and sources used
only as implementation references.

Last checked: 2026-07-19

## BMC64 upstream

| Item | Value |
| --- | --- |
| Role | Original upstream project and base for BMX |
| Upstream URL | https://github.com/randyrossi/bmc64 |
| Local remote | `upstream` |
| Base commit | `9c2772e094fd29dca6bf621268f003f64a3197a9` |
| Base commit URL | https://github.com/randyrossi/bmc64/commit/9c2772e094fd29dca6bf621268f003f64a3197a9 |
| Base commit subject | `Merge pull request #296 from aminch/mouse-click-fix-248` |
| Base commit date | 2025-07-26 |

The current branch is based on this upstream commit. BMX changes live on
top of it in this repository.

## VICE

### VICE 3.10

| Item | Value |
| --- | --- |
| Role | Active emulator core for the `vice310` branch |
| Local path | `third_party/vice-3.10` |
| Local import commit | `ae4d04aa6029ac2f1df83a051c5d2cf3573c611d` |
| Local import commit URL | https://github.com/kdre/bmx-private/commit/ae4d04aa6029ac2f1df83a051c5d2cf3573c611d |
| Version in tree | `3.10.0` (`third_party/vice-3.10/src/version.h`) |
| Upstream tag | `3.10.0` |
| Upstream tag commit | `4d283a2e7dd59b7e378524878e81ecc7826b700c` |
| Upstream source URL | https://github.com/VICE-Team/svn-mirror/tree/3.10.0 |

The VICE 3.10 source is vendored in the repository. BMX patches and Raspi
adapter work should be reviewed relative to the local import commit
`ae4d04aa6029ac2f1df83a051c5d2cf3573c611d`.

### TCPSER modem-core subset

| Item | Value |
| --- | --- |
| Role | Hayes-compatible modem command parser used by BMX RS232 Hayes Modem mode |
| Local path | `third_party/vice-3.10/src/rs232drv/tcpser` |
| Upstream URL | https://github.com/go4retro/tcpser |
| Upstream branch checked | `master` |
| Upstream commit checked | `fe7feff4862406b277e009d14c219f5d16cf1222` |
| Upstream commit URL | https://github.com/go4retro/tcpser/commit/fe7feff4862406b277e009d14c219f5d16cf1222 |
| License | GPL-2.0-or-later, as stated in the upstream README |
| Staged license file | `LICENCE.tcpser` |

BMX imports only the stable modem parser/core files from tcpser: `modem_core.*`,
`getcmd.*`, `phone_book.*`, `dce.h`, `line.h` and `nvt.h`. The original tcpser
POSIX application is not built. BMX supplies `bmx_tcpser_adapter.c` to connect
the modem core to VICE RS232/ACIA/Userport I/O and Circle sockets.

### CCGMS Future

| Item | Value |
| --- | --- |
| Role | C64 terminal utility included on the default BMX utility disk |
| Local path | `utils-disk/c64/ccgms.prg` |
| Upstream URL | https://github.com/mist64/ccgmsterm |
| Release asset URL | https://github.com/mist64/ccgmsterm/releases/download/v0.2/ccgms.prg |
| Version | `v0.2` |
| License | BSD-3-Clause |
| Staged license file | `LICENCE.ccgms` |

The Pi4/Pi5 staging scripts build `utils/c64/utils.d64` from
`utils-disk/c64/` using `c1541`. The generated disk image is staged onto the
boot partition. It is initially selected for drive 8, and can be changed or
disabled under `Drives > Default disk`. The generated `utils.d64` is not
stored in git.

## Circle and circle-stdlib

### circle-stdlib

| Item | Value |
| --- | --- |
| Role | Bare-metal C/C++ runtime and Circle integration used by Pi4 and Pi5 builds |
| Source archive | `third_party/source-cache/circle-stdlib-v20-a4fbed9b-full.tar.gz` |
| Archive SHA-256 | `e25e4c5087f608defb026a5eaa195a35f41838cd0286e1fb415449756ad6bce0` |
| Pi4 build path | `build/pi4/circle-variants/<profile>-<hash>/circle-stdlib` |
| Pi5 build path | `build/pi5/circle-variants/<profile>-<hash>/circle-stdlib` |
| Upstream URL | https://github.com/smuehlst/circle-stdlib |
| Version/tag | `v20` |
| Commit | `a4fbed9b369e8285e4a12b2bb0588511210b83a6` |
| Commit URL | https://github.com/smuehlst/circle-stdlib/commit/a4fbed9b369e8285e4a12b2bb0588511210b83a6 |

Both Pi4 and Pi5 build helpers extract the same pinned source archive into
configuration-isolated build directories so each board, toolchain and build
profile can be cached independently without re-downloading Circle sources.
BMX changes are then applied from `third_party/circle-stdlib-patches/`. On a
fresh build root, `build/<board>/circle-stdlib` is an active-tree convenience
symlink for patch and diagnostic work; target builds use the immutable variant
path directly. A pre-existing real directory at the convenience path is never
overwritten automatically.

### Circle submodule inside circle-stdlib

| Item | Value |
| --- | --- |
| Role | Bare-metal Raspberry Pi framework used by BMX |
| Source path in archive | `circle-stdlib/libs/circle` |
| Pi4 build path | `build/pi4/circle-variants/<profile>-<hash>/circle-stdlib/libs/circle` |
| Pi5 build path | `build/pi5/circle-variants/<profile>-<hash>/circle-stdlib/libs/circle` |
| Upstream URL | https://github.com/rsta2/circle |
| Version/tag | `Step51` |
| Commit | `6177984e30fac5e65582d171d43f1563368a94ac` |
| Commit URL | https://github.com/rsta2/circle/commit/6177984e30fac5e65582d171d43f1563368a94ac |
| Annotated tag object | `b4728d14738344d9b383585597776f926c95aec0` |

### Mbed TLS replacement inside circle-stdlib

| Item | Value |
| --- | --- |
| Role | TLS, X.509 and cryptographic implementation for the BMX updater |
| Upstream repository | https://github.com/Mbed-TLS/mbedtls |
| Release/tag | `mbedtls-3.6.7` (3.6 LTS) |
| Release date | 2026-07-07 |
| Release archive | https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2 |
| SHA-256 | `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6` |
| Source archive | `third_party/source-cache/mbedtls-3.6.7.tar.bz2` |
| Checksum registry | `third_party/source-cache/SHA256SUMS` |
| License | Apache-2.0 OR GPL-2.0-or-later |
| Staged license file | `licenses/mbedtls.txt` |

The official archive replaces Circle stdlib v20's bundled Mbed TLS 2.28.10
tree deterministically before compilation. Version 2.28 is no longer a
maintained branch; upstream states that the 3.6 LTS line is maintained until
March 2027. The build helper requires the repository-local archive, verifies
its pinned checksum, validates the archive layout and records its source hash
in each generated Circle tree. It never downloads source code.

Circle compatibility code remains in
`third_party/circle-stdlib-patches/0009-mbedtls-3.6.7-compat.patch`; the upstream
Mbed TLS source is not embedded into that patch.

## miniz raw-Deflate decoder

| Item | Value |
| --- | --- |
| Role | Allocation-free raw-Deflate decoder for the strict updater ZIP reader |
| Local path | `third_party/miniz` |
| Upstream | https://github.com/richgel999/miniz |
| Release | `3.1.2` |
| Release archive SHA-256 | `f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a` |
| License | `third_party/miniz/LICENSE` |

The vendored `miniz.c` and `miniz.h` are byte-identical upstream files. BMX
compiles only the narrow profile in `miniz_tinfl.c`; ZIP records, paths,
limits, CRC and signed inventory are validated by BMX itself. Provenance and
individual source hashes are recorded in `third_party/miniz/README.bmx.md`.

## Raspberry Pi firmware files

### Raspberry Pi boot firmware staged locally

| Item | Value |
| --- | --- |
| Role | Pi4/Pi400/CM4 and Pi5/Pi500 boot firmware copied by staging scripts |
| Local path | `third_party/raspberrypi-firmware` |
| Firmware repository | https://github.com/raspberrypi/firmware |
| Pinned commit | `dd28ccd67f16e60f5739dc779060be218b28eb1e` |
| Commit URL | https://github.com/raspberrypi/firmware/commit/dd28ccd67f16e60f5739dc779060be218b28eb1e |
| Shared license files | `boot/LICENCE.broadcom`, `boot/COPYING.linux` |
| Pi4 files | `start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb`, `bcm2711-rpi-400.dtb`, `bcm2711-rpi-cm4.dtb` |
| Pi5 files | `bcm2712-rpi-5-b.dtb`, `bcm2712-rpi-500.dtb`, `bcm2712d0-rpi-5-b.dtb`, `overlays/bcm2712d0.dtbo`, `overlays/uart0-pi5.dtbo` |

Pi4 staging also copies `armstub7-rpi4.bin` from
`third_party/raspberrypi-firmware/boot/pi4`. That binary is generated from the
Circle armstub source included in the `circle-stdlib` source archive.

### Raspberry Pi WLAN firmware staged locally

| Item | Value |
| --- | --- |
| Role | Pi4/Pi400/CM4 and Pi5/Pi500 WLAN firmware copied by staging scripts |
| Local path | `third_party/raspberrypi-firmware/wlan` |
| Firmware repository | https://github.com/RPi-Distro/firmware-nonfree |
| Pinned commit | `c9d3ae6584ab79d19a4f94ccf701e888f9f87a53` |
| Commit URL | https://github.com/RPi-Distro/firmware-nonfree/commit/c9d3ae6584ab79d19a4f94ccf701e888f9f87a53 |
| Files | `brcmfmac*.bin`, `brcmfmac*.txt`, `brcmfmac*.clm_blob` |

The Pi4 and Pi5 staging scripts copy boot and WLAN firmware from the worktree
and do not download firmware at staging time. They validate that the expected
Broadcom WLAN firmware files are present and fail instead of running the Circle
firmware downloader.

## Raspberry Pi Linux kernel reference

| Item | Value |
| --- | --- |
| Role | Reference source for the experimental Pi5KMS HDMI modeset implementation |
| Repository | https://github.com/raspberrypi/linux |
| Branch checked | `rpi-6.12.y` |
| Commit checked | `0a382e93f18ae5b8b7f10d62106b5480c2a0f1dd` |
| Commit URL | https://github.com/raspberrypi/linux/commit/0a382e93f18ae5b8b7f10d62106b5480c2a0f1dd |
| Relevant files | `drivers/gpu/drm/vc4/vc4_hdmi.c`, `drivers/gpu/drm/vc4/vc4_hdmi_phy.c`, `drivers/gpu/drm/vc4/vc4_hvs.c` |

This kernel tree is not vendored and is not linked into BMX. It was used
as a reference for Pi5 HDMI register programming, PHY setup and HVS scanout.
The BMX implementation lives in `src/pi5kms/`.

Useful reference links:

- https://github.com/raspberrypi/linux/blob/0a382e93f18ae5b8b7f10d62106b5480c2a0f1dd/drivers/gpu/drm/vc4/vc4_hdmi.c
- https://github.com/raspberrypi/linux/blob/0a382e93f18ae5b8b7f10d62106b5480c2a0f1dd/drivers/gpu/drm/vc4/vc4_hdmi_phy.c
- https://github.com/raspberrypi/linux/blob/0a382e93f18ae5b8b7f10d62106b5480c2a0f1dd/drivers/gpu/drm/vc4/vc4_hvs.c

## ARM GNU toolchains

| Board | Toolchain | URL |
| --- | --- | --- |
| Pi4/Pi400 | `arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi` | https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi.tar.xz |
| Pi5/Pi500 | `arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-elf` | https://developer.arm.com/-/media/Files/downloads/gnu/15.2.rel1/binrel/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-elf.tar.xz |

The build scripts download these into `.toolchains/` only if no matching
compiler is already available in `PATH`.

## CRT shader source

| Item | Value |
| --- | --- |
| Role | Shader source used by the BMC64 display path |
| Local path | `third_party/shaders` |
| Source note | Files carry copyright notice `Copyright (C) 2015-2016 davej` |

## Re-check commands

```sh
git remote -v
sha256sum third_party/source-cache/mbedtls-3.6.7.tar.bz2
tar -tzf third_party/source-cache/circle-stdlib-v20-a4fbed9b-full.tar.gz | grep '^circle-stdlib/libs/circle/Rules.mk$'
tar -tjf third_party/source-cache/mbedtls-3.6.7.tar.bz2 | grep '^mbedtls-3.6.7/include/mbedtls/version.h$'
grep MBEDTLS_VERSION_STRING build/pi4/circle-stdlib/libs/mbedtls/include/mbedtls/build_info.h
grep MBEDTLS_VERSION_STRING build/pi5/circle-stdlib/libs/mbedtls/include/mbedtls/build_info.h
git ls-remote https://github.com/raspberrypi/linux.git refs/heads/rpi-6.12.y
git ls-remote https://github.com/raspberrypi/firmware.git refs/heads/master
git ls-remote https://github.com/VICE-Team/svn-mirror.git refs/tags/3.10.0
git ls-remote https://github.com/go4retro/tcpser.git refs/heads/master
```
