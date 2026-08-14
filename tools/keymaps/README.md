Keymap Runtime Sources
======================

`raspi/` contains the Raspberry Pi keyboard maps that are staged onto the SD
card for the enabled VICE machines. The `rpi_*.vkm` maps are hand-maintained
BMX/Pi maps. The `raspi_*.vkm` maps are checked-in raw-HID adaptations of the
original VICE symbolic and positional GTK maps.

Keep `sdcard/` for the minimal static boot tree (`config.txt`, `cmdline.txt`,
`machines.ini` and bootstat files). Keymaps live here because some are used as
generator input and should not be mixed with the pre-stage SD-card skeleton.

Every runtime source-to-target mapping is declared explicitly in
`sd-layout.toml`; the staging code has no second keymap inventory.
`generate_raspi_keymaps.py` remains the development tool that derives the
checked-in per-machine positional US/DE maps from the C64 reference maps in
`tools/keymaps/raspi/c64/`.

`generate_vice_keymaps.py` explicitly regenerates the checked-in
`raspi_{sym,pos}{,_de}.vkm` maps from the pinned VICE 3.10 data. They remain
static during normal builds. Run the generator only when the pinned VICE maps
or the HID translation need to change; CI/tests use `--check` to detect stale
generated files:

    python3 tools/keymaps/generate_vice_keymaps.py
    python3 tools/keymaps/generate_vice_keymaps.py --check
