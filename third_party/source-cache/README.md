# Source Cache

This directory stores pinned upstream source archives that BMX extracts into
board-local build directories. The archive contents are unmodified upstream
sources plus submodules; BMX-specific changes are kept as patches outside the
archive.

## circle-stdlib

`circle-stdlib-v20-a4fbed9b-full.tar.gz.part-*` contains a split copy of
`circle-stdlib-v20-a4fbed9b-full.tar.gz`. The build scripts concatenate the
parts into `build/source-cache/` before extraction and verify the original
archive SHA256 listed in `SHA256SUMS`.

The archive contains:

- `smuehlst/circle-stdlib` tag `v20`
- commit `a4fbed9b369e8285e4a12b2bb0588511210b83a6`
- initialized submodules, including Circle `Step51`
- no `.git` directories

Build scripts extract this single archive into configuration-keyed trees:

- `build/pi4/circle-variants/<profile>-<hash>/circle-stdlib`
- `build/pi5/circle-variants/<profile>-<hash>/circle-stdlib`

The extracted trees are separate because Pi4 and Pi5 use different Circle
configuration, toolchains and install directories. The shorter
`build/<board>/circle-stdlib` path is maintained as a convenience symlink to
the last completed configuration when that path is not already a real tree.

## Mesa V3D Offline Compiler

`mesa-24.2.8-7d908b5a-full.tar.gz.part-*` contains a split copy of the clean
Mesa source archive used by the desktop V3D offline compiler. The build script
reconstructs it in `build/host/v3d-offline/source-cache/`, verifies the archive
SHA256 from `SHA256SUMS`, and extracts it into a private host build tree.

The archive contains:

- Mesa tag `mesa-24.2.8`
- commit `7d908b5aed61a280411380e29814ad48336427a7`
- no `.git` directory or generated build products

BMX-specific compiler export, V3D 4.2/7.1 no-op shim selection, and host
compatibility changes remain as versioned patches under
`tools/v3d/offline-compiler/patches/mesa-24.2.8/`.

## Mbed TLS

Circle stdlib v20 embeds an old Mbed TLS source snapshot. BMX replaces it with
the official Mbed TLS 3.6.7 release archive before configuration and build.
The verified upstream archive is tracked here as `mbedtls-3.6.7.tar.bz2`, and
its SHA-256 is recorded in the adjacent `SHA256SUMS`. The version is pinned by
the archive name and build helper. The helper requires this repository archive,
verifies its checksum and single-root layout, and never downloads source code.

The upstream release is available at:

<https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2>

Its license is copied byte-for-byte to `sdcard/licenses/mbedtls.txt` so SD
staging does not depend on a generated Circle build tree.
