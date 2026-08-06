#!/usr/bin/env python3
"""Build generated SD inputs and assemble one BMX board stage from sd-layout.toml.

The final stage is never populated piecemeal by this module.  Board/profile
transformations and generated files are written below a private temporary
``generated/<board>`` root first.  The SD layout assembler then creates the
destination from an empty directory using only declarations in sd-layout.toml.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from typing import Iterable, Sequence


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LAYOUT = REPO_ROOT / "sd-layout.toml"
BOARDS = ("pi4", "pi5")

BUILD_PROFILE_TEXT = """Build profile: {profile}

release:
- no staged UART diagnostics
- no second-stage firmware UART logging

debug:
- enables staged UART diagnostics
- uses board-specific second-stage UART settings described in UART-DEBUG.txt
- enables BMC64 serial logging through enable_serial=1
- leaves networking disabled by default; enable it from the BMX Network menu
"""

UART_DEBUG_TEXT = """UART debug enabled for BMX debug staging.

Common settings:
- enable_uart=1
- enable_serial=1
- networking remains off unless enabled from the BMX Network menu

Board-specific settings:
- Pi 4 / Pi 400 uses init_uart_baud=115200 and init_uart_clock=48000000.
  Second-stage firmware UART logging stays disabled because its noisy output can
  corrupt or interleave kernel and VICE boot profiling output. Early EEPROM
  output additionally requires BOOT_UART=1 in the EEPROM configuration.
- Pi 5 / Pi 500 enables uart_2ndstage=1. BMX uses Circle serial device 0,
  GPIO14/15 on the 40-pin header. Pi 5 also has a dedicated 3-pin JST UART.

Wiring for a 3.3V USB-TTL adapter:
- Pi pin 6  -> GND
- Pi pin 8  -> adapter RX
- Pi pin 10 -> adapter TX (optional for log capture only)

Serial settings:
- 115200 baud
- 8 data bits
- no parity
- 1 stop bit
"""


class StageError(ValueError):
    """Raised when generated inputs cannot be produced without guessing."""


@dataclass(frozen=True)
class StageContext:
    board: str
    profile: str
    developer_mode: bool
    omit_roms: bool
    kernel_dir: Path
    stage_dir: Path
    layout_path: Path
    build_info: Path | None
    source_inventory: Path | None


@dataclass(frozen=True)
class KernelContract:
    base: str
    machines: tuple[str, ...]


def load_kernel_contract(context: StageContext) -> KernelContract:
    try:
        from sd_layout import SdLayoutError, kernel_targets
    except ImportError as exc:
        raise StageError("tools/sd_layout.py is unavailable") from exc
    try:
        targets = kernel_targets(context.layout_path, context.board)
    except SdLayoutError as exc:
        raise StageError(str(exc)) from exc
    bases: set[str] = set()
    machines: list[str] = []
    for target in targets:
        if "/" in target or ".img." not in target:
            raise StageError(f"invalid machine-kernel target in SD layout: {target}")
        base, machine = target.rsplit(".", 1)
        bases.add(base)
        machines.append(machine)
    if len(bases) != 1 or "c64" not in machines:
        raise StageError(
            f"{context.board} SD layout kernels must share one base and include c64"
        )
    return KernelContract(next(iter(bases)), tuple(machines))


def _require_regular(path: Path, description: str) -> None:
    try:
        mode = path.lstat().st_mode
    except OSError as exc:
        raise StageError(f"missing {description}: {path}") from exc
    if not stat.S_ISREG(mode):
        raise StageError(f"{description} is not a regular file: {path}")


def _read_ascii(path: Path, description: str) -> str:
    _require_regular(path, description)
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise StageError(f"cannot read {description}: {path}") from exc
    try:
        text = raw.decode("ascii", "strict")
    except UnicodeDecodeError as exc:
        raise StageError(f"{description} must be strict ASCII: {path}") from exc
    if "\r" in text or not text.endswith("\n"):
        raise StageError(f"{description} must use LF endings and end with LF: {path}")
    return text


def _write_bytes(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        path.write_bytes(content)
        path.chmod(0o644)
    except OSError as exc:
        raise StageError(f"cannot write generated file: {path}") from exc


def _write_text(path: Path, content: str) -> None:
    _write_bytes(path, content.encode("ascii"))


def _copy_required(source: Path, destination: Path, description: str) -> None:
    _require_regular(source, description)
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copyfile(source, destination)
        destination.chmod(0o644)
    except OSError as exc:
        raise StageError(f"cannot prepare {description}: {source}") from exc


def validate_kernel_inputs(context: StageContext, contract: KernelContract) -> None:
    """Validate the intermediate fallback and all stable machine kernels.

    The unsuffixed fallback is a build-time proof only.  It is deliberately not
    copied into generated inputs or the final SD stage.
    """

    base = contract.base
    fallback = context.kernel_dir / base
    c64 = context.kernel_dir / f"{base}.c64"
    _require_regular(fallback, f"{context.board} fallback kernel")
    _require_regular(c64, f"{context.board} C64 kernel")
    try:
        fallback_bytes = fallback.read_bytes()
        c64_bytes = c64.read_bytes()
    except OSError as exc:
        raise StageError(f"cannot read kernel inputs in {context.kernel_dir}") from exc
    if not c64_bytes:
        raise StageError(f"C64 kernel is empty: {c64}")
    if fallback_bytes != c64_bytes:
        raise StageError(
            f"fallback kernel must be byte-identical to the C64 kernel: {fallback}"
        )
    for machine in contract.machines:
        _require_regular(
            context.kernel_dir / f"{base}.{machine}",
            f"{context.board} {machine} kernel",
        )


def _insert_managed_lines(raw: str, lines: Sequence[str], *, pi4: bool) -> str:
    marker = "# END BMX MANAGED\n"
    if raw.count(marker) != 1:
        raise StageError("config.txt has no single canonical BMX managed block")
    inserted = "".join(f"{line}\n" for line in lines) + marker + "\n"
    if pi4:
        inserted += (
            "hdmi_force_hotplug:0=1\n"
            "hdmi_ignore_hotplug:1=1\n"
            "hdmi_drive:0=2\n"
            "hdmi_force_edid_audio:0=1\n"
        )
    return raw.replace(marker, inserted, 1)


def _replace_kernel_with_selector(raw: str, kernel_name: str, selector: str) -> str:
    begin_marker = "# BEGIN BMX MANAGED\n"
    end_marker = "# END BMX MANAGED\n"
    if raw.count(begin_marker) != 1 or raw.count(end_marker) != 1:
        raise StageError("config.txt has no single canonical BMX managed block")
    begin = raw.index(begin_marker) + len(begin_marker)
    end = raw.index(end_marker)
    if begin >= end:
        raise StageError("config.txt has an invalid BMX managed block")
    block = raw[begin:end]
    kernel_line = f"kernel={kernel_name}\n"
    if block.count(kernel_line) != 1:
        raise StageError("generated config must contain exactly one fallback kernel")
    if any(line.lstrip(" \t").startswith("include ") for line in block.splitlines()):
        raise StageError("fresh generated config unexpectedly contains an include")
    return raw[:begin] + block.replace(
        kernel_line, kernel_line + f"include {selector}\n", 1
    ) + raw[end:]


def _append_first_line_option(raw: str, option: str) -> str:
    first, separator, rest = raw.partition("\n")
    if not separator:
        raise StageError("cmdline.txt has no complete first line")
    return first + " " + option + separator + rest


def _set_first_line_option(raw: str, key: str, value: str | None) -> str:
    first, separator, rest = raw.partition("\n")
    if not separator:
        raise StageError("cmdline.txt has no complete first line")
    options = [
        option
        for option in first.split()
        if option.partition("=")[0] != key
    ]
    if value is not None:
        options.append(f"{key}={value}")
    return " ".join(options) + separator + rest


def render_config_and_cmdline(
    context: StageContext,
    contract: KernelContract,
    generated: Path,
) -> None:
    config = _read_ascii(REPO_ROOT / "sdcard/config.txt", "base config.txt")
    cmdline = _read_ascii(REPO_ROOT / "sdcard/cmdline.txt", "base cmdline.txt")
    kernel_name = f"{contract.base}.c64"

    if context.board == "pi4":
        config = _insert_managed_lines(
            config,
            (f"kernel={kernel_name}", "hdmi_group:0=1", "hdmi_mode:0=19"),
            pi4=True,
        )
        cmdline = re.sub(r"pi5kms=[^ ]* *", "", cmdline, count=1)
        if context.profile == "debug" and "enable_uart=1\n" not in config:
            config += (
                "\nenable_uart=1\n"
                "uart_2ndstage=1\n"
                "init_uart_baud=115200\n"
            )
        if context.profile == "debug" and "uart_2ndstage=" not in config:
            config += "uart_2ndstage=1\n"
        if context.profile == "debug" and "init_uart_baud=" not in config:
            config += "init_uart_baud=115200\n"
        if context.profile == "debug" and "init_uart_clock=" not in config:
            config += "init_uart_clock=48000000\n"
        config += (
            "\narm_64bit=0\n"
            "initial_turbo=0\n"
            "\n[pi4]\n"
            "armstub=armstub7-rpi4.bin\n"
            "max_framebuffers=2\n"
            "\n[cm4]\n"
            "otg_mode=1\n"
        )
    else:
        config = _insert_managed_lines(config, (f"kernel={kernel_name}",), pi4=False)
        if "gpiofanpin=" not in cmdline:
            cmdline = _append_first_line_option(cmdline, "gpiofanpin=45")
        if context.profile == "debug" and "enable_uart=1\n" not in config:
            config += "\nenable_uart=1\nuart_2ndstage=1\ndtoverlay=uart0-pi5\n"
        config += "\ninitial_turbo=0\n\n[pi5]\narm_64bit=1\n"

    if context.profile == "debug" and "enable_serial=" not in cmdline:
        cmdline = _append_first_line_option(cmdline, "enable_serial=1")
    cmdline = _set_first_line_option(
        cmdline, "developer_mode", "1" if context.developer_mode else None,
    )

    active = _replace_kernel_with_selector(config, kernel_name, "bmx-active-kernel.txt")
    selector = f"# BMX-KERNEL-SELECTOR-V2\nkernel={kernel_name}\n"
    _write_text(generated / "config.txt", active)
    _write_text(generated / "cmdline.txt", cmdline)
    _write_text(generated / "bmx-active-kernel.txt", selector)


def prepare_utils(generated: Path) -> None:
    command = [str(REPO_ROOT / "tools/build_utils_disks.sh"), str(generated)]
    try:
        subprocess.run(command, cwd=REPO_ROOT, check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise StageError("failed to build BMX utility disk images") from exc


def prepare_profile_files(context: StageContext, generated: Path) -> None:
    _write_text(
        generated / "BUILD-PROFILE.txt",
        BUILD_PROFILE_TEXT.format(profile=context.profile),
    )
    if context.profile == "debug":
        _write_text(generated / "UART-DEBUG.txt", UART_DEBUG_TEXT)


def prepare_build_info(context: StageContext, generated: Path) -> None:
    if context.build_info is None:
        return
    _copy_required(context.build_info, generated / "BMX-BUILD.json", "BMX build info")


def write_missing_rom_report(generated: Path, paths: Iterable[str]) -> None:
    missing = list(dict.fromkeys(paths))
    report = generated / "MISSING-ROMS.txt"
    if not missing:
        report.unlink(missing_ok=True)
        return
    text = (
        "Missing ROM files were not staged.\n\n"
        "Copy these files to the staged SD card tree before booting:\n"
        + "".join(f"  {path}\n" for path in missing)
        + "\nEach machine directory also contains ROMS.txt with the required machine ROMs.\n"
    )
    _write_text(report, text)


def prepare_generated_inputs(context: StageContext, generated_root: Path) -> Path:
    generated = generated_root / context.board
    generated.mkdir(parents=True, exist_ok=False)
    contract = load_kernel_contract(context)
    validate_kernel_inputs(context, contract)
    render_config_and_cmdline(context, contract, generated)
    prepare_utils(generated)
    prepare_profile_files(context, generated)
    prepare_build_info(context, generated)
    return generated


def source_overrides(context: StageContext, generated: Path) -> dict[str, Path]:
    """Return the one source-root binding set shared by scan and assembly."""

    return {
        "repo": REPO_ROOT,
        "kernels": context.kernel_dir,
        "generated": generated,
        "roms": REPO_ROOT / "third_party/vice-3.10/data",
    }


def persist_source_inventory(
    context: StageContext, generated: Path,
) -> None:
    """Persist every resolved SD-layout source for the TUI All view."""

    if context.source_inventory is None:
        return
    try:
        from sd_layout import (
            SdLayoutError,
            load_sd_layout,
            save_source_inventory,
            scan_source_inventory,
        )
    except ImportError as exc:
        raise StageError("tools/sd_layout.py is unavailable") from exc
    try:
        layout = load_sd_layout(context.layout_path)
        inventory = scan_source_inventory(
            layout,
            context.board,
            source_overrides(context, generated),
        )
        context.source_inventory.parent.mkdir(parents=True, exist_ok=True)
        save_source_inventory(
            inventory, context.source_inventory, layout,
        )
    except (OSError, SdLayoutError) as exc:
        raise StageError(f"cannot persist SD source inventory: {exc}") from exc


def assemble(context: StageContext, generated: Path) -> tuple[str, ...]:
    """Resolve the declarative layout and populate a fresh final stage."""

    try:
        from sd_layout import (
            SdLayoutError,
            assemble_stage,
            feature_targets,
            plan_stage,
        )
    except ImportError as exc:
        raise StageError("tools/sd_layout.py is unavailable") from exc

    overrides = source_overrides(context, generated)
    features = () if context.omit_roms else ("roms",)
    try:
        reportable_roms = feature_targets(
            context.layout_path,
            "roms",
            context.board,
            context.profile,
        )
        if context.omit_roms:
            missing_roms = reportable_roms
        else:
            plan = plan_stage(
                context.layout_path,
                board=context.board,
                source_overrides=overrides,
                profile=context.profile,
                features=features,
            )
            reportable = frozenset(reportable_roms)
            missing_roms = tuple(
                target
                for target in plan.missing_optional_targets
                if target in reportable
            )
        write_missing_rom_report(generated, missing_roms)
        # MISSING-ROMS.txt is itself generated from the selected feature set,
        # so inventory only after that final generated-source mutation.
        persist_source_inventory(context, generated)
        assemble_stage(
            context.layout_path,
            board=context.board,
            source_overrides=overrides,
            destination=context.stage_dir,
            profile=context.profile,
            features=features,
            replace=True,
        )
    except SdLayoutError as exc:
        raise StageError(str(exc)) from exc
    return tuple(missing_roms)


def _default_stage(board: str) -> Path:
    board_override = os.environ.get(f"{board.upper()}_STAGE_DIR")
    shared_override = os.environ.get("BMC64_STAGE_DIR")
    return Path(board_override or shared_override or REPO_ROOT / f"{board}-test/sdcard")


def _default_kernel_dir(board: str) -> Path:
    override = os.environ.get("BMC64_KERNEL_DIR")
    build_root = Path(os.environ.get("BMC64_BUILD_ROOT", REPO_ROOT / "build"))
    return Path(override) if override else build_root / board / "vice310-images"


def parse_args(argv: Sequence[str] | None = None) -> StageContext:
    parser = argparse.ArgumentParser(
        description="assemble a BMX SD stage directly from sd-layout.toml"
    )
    parser.add_argument("--board", choices=BOARDS, required=True)
    parser.add_argument("--profile", choices=("release", "debug"))
    parser.add_argument("--debug-uart", action="store_true")
    developer_mode = parser.add_mutually_exclusive_group()
    developer_mode.add_argument(
        "--developer-mode", dest="developer_mode", action="store_true",
        help="stage developer_mode=1 in cmdline.txt (local default)",
    )
    developer_mode.add_argument(
        "--no-developer-mode", dest="developer_mode", action="store_false",
        help="remove developer_mode from cmdline.txt",
    )
    parser.set_defaults(developer_mode=True)
    parser.add_argument("--omit-roms", action="store_true")
    parser.add_argument("--kernel-dir", type=Path)
    parser.add_argument("--stage-dir", type=Path)
    parser.add_argument("--layout", type=Path, default=DEFAULT_LAYOUT)
    parser.add_argument("--build-info", type=Path)
    parser.add_argument(
        "--source-inventory",
        type=Path,
        help="persist metadata for every resolved sd-layout.toml source alias",
    )
    args = parser.parse_args(argv)

    profile = args.profile or os.environ.get("BMC64_BUILD_PROFILE", "release")
    if args.debug_uart:
        profile = "debug"
    if profile not in ("release", "debug"):
        parser.error(f"unsupported build profile: {profile}")
    kernel_dir = (args.kernel_dir or _default_kernel_dir(args.board)).resolve()
    stage_dir = (args.stage_dir or _default_stage(args.board)).resolve()
    layout_path = args.layout.resolve()
    build_info = args.build_info.resolve() if args.build_info else None
    source_inventory = (
        args.source_inventory.resolve()
        if args.source_inventory else None
    )
    return StageContext(
        board=args.board,
        profile=profile,
        developer_mode=args.developer_mode,
        omit_roms=args.omit_roms,
        kernel_dir=kernel_dir,
        stage_dir=stage_dir,
        layout_path=layout_path,
        build_info=build_info,
        source_inventory=source_inventory,
    )


def main(argv: Sequence[str] | None = None) -> int:
    try:
        context = parse_args(argv)
        with tempfile.TemporaryDirectory(prefix="bmx-sd-generated-") as temporary:
            generated = prepare_generated_inputs(context, Path(temporary))
            missing_roms = assemble(context, generated)
    except StageError as exc:
        print(f"stage_bmx_sd.py: error: {exc}", file=sys.stderr)
        return 1
    if missing_roms:
        print(
            f"Skipped {len(missing_roms)} missing ROM file(s); see "
            f"{context.stage_dir / 'MISSING-ROMS.txt'}",
            file=sys.stderr,
        )
    print(
        f"Staged {context.board} {context.profile} SD tree at {context.stage_dir} "
        "from sd-layout.toml"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
