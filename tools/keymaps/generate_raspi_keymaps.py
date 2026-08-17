#!/usr/bin/env python3
import argparse
import re
from pathlib import Path


HOST_FLAG_CONTINUATION = 32
HOST_FLAG_SHIFT = 128
HOST_FLAG_DESHIFT = 16
EMULATED_SHIFT_FLAGS = 1 | 2 | 4 | 8 | HOST_FLAG_DESHIFT
HOST_FLAGS = HOST_FLAG_CONTINUATION | HOST_FLAG_SHIFT | 512 | 1024

MACHINES = {
    "c128": {
        "source": "tools/keymaps/raspi/c128/rpi_pos.vkm",
        "sym": "third_party/vice-3.10/data/C128/gtk3_sym.vkm",
        "out_dir": "c128",
    },
    "vic20": {
        "source": "tools/keymaps/raspi/vic20/rpi_pos.vkm",
        "sym": "third_party/vice-3.10/data/VIC20/gtk3_sym.vkm",
        "out_dir": "vic20",
    },
    "plus4": {
        "source": "tools/keymaps/raspi/plus4/rpi_pos.vkm",
        "sym": "third_party/vice-3.10/data/PLUS4/gtk3_sym.vkm",
        "out_dir": "plus4",
    },
    "pet": {
        "sym": "third_party/vice-3.10/data/PET/gtk3_buuk_sym.vkm",
        "out_dir": "pet",
    },
}

REFERENCE = {
    "us": "tools/keymaps/raspi/c64/rpi_pos.vkm",
    "de": "tools/keymaps/raspi/c64/rpi_pos_de.vkm",
}

US_LAYOUT = [
    ("LeftBracket", "bracketleft", False),
    ("LeftBracket", "at", True),
    ("SemiColon", "semicolon", False),
    ("SemiColon", "colon", True),
    ("Comma", "comma", False),
    ("BackSlash", "backslash", False),
    ("Equals", "equal", False),
    ("Equals", "plus", True),
    ("Period", "period", False),
    ("Dash", "minus", False),
    ("Dash", "underscore", True),
    ("SingleQuote", "apostrophe", False),
    ("SingleQuote", "quotedbl", True),
    ("Slash", "slash", False),
    ("Space", "space", False),
    ("Insert", "sterling", False),
    ("RightBracket", "bracketright", False),
    ("RightBracket", "asterisk", True),
]

for digit, shifted in [
    ("0", "parenright"),
    ("1", "exclam"),
    ("2", "at"),
    ("3", "numbersign"),
    ("4", "dollar"),
    ("5", "percent"),
    ("6", "asciicircum"),
    ("7", "ampersand"),
    ("8", "asterisk"),
    ("9", "parenleft"),
]:
    US_LAYOUT.append((digit, digit, False))
    US_LAYOUT.append((digit, shifted, True))

for letter in "abcdefghijklmnopqrstuvwxyz":
    US_LAYOUT.append((letter, letter, False))

DE_LAYOUT = [
    ("LeftBracket", "at", False),
    ("SemiColon", "colon", False),
    ("Comma", "comma", False),
    ("Comma", "semicolon", True),
    ("BackSlash", "numbersign", False),
    ("BackSlash", "apostrophe", True),
    ("Pound", "numbersign", False),
    ("Pound", "apostrophe", True),
    ("Equals", "sterling", False),
    ("Period", "period", False),
    ("Period", "colon", True),
    ("Dash", "plus", False),
    ("Dash", "question", True),
    ("SingleQuote", "semicolon", False),
    ("Slash", "minus", False),
    ("Slash", "underscore", True),
    ("Space", "space", False),
    ("Insert", "sterling", False),
    ("RightBracket", "plus", False),
    ("RightBracket", "asterisk", True),
    ("KP_BackSlash", "less", False),
    ("KP_BackSlash", "greater", True),
]

for digit in "0123456789":
    DE_LAYOUT.append((digit, digit, False))
DE_LAYOUT.append(("0", "equal", True))
DE_LAYOUT.append(("3", "numbersign", True))
DE_LAYOUT.append(("7", "slash", True))

for letter in "abcdefghijklmnopqrstuvwxyz":
    symbol = {"y": "z", "z": "y"}.get(letter, letter)
    DE_LAYOUT.append((letter, symbol, False))

LAYOUTS = {
    "us": US_LAYOUT,
    "de": DE_LAYOUT,
}


def strip_comment(line):
    return re.split(r"\s*(?:#|/\*)", line, maxsplit=1)[0].strip()


def parse_entry(line):
    line = strip_comment(line)
    if not line or line.startswith("!"):
        return None
    parts = line.split()
    if len(parts) < 4:
        return None
    try:
        return parts[0], int(parts[1]), int(parts[2]), int(parts[3], 0)
    except ValueError:
        return None


def parse_symbol_map(repo, rel_path, seen=None):
    path = repo / rel_path
    seen = set() if seen is None else seen
    if path in seen:
        return {}
    seen.add(path)

    symbols = {}
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = strip_comment(raw)
        if not line:
            continue
        if line.startswith("!INCLUDE"):
            include = line.split(maxsplit=1)[1]
            symbols.update(parse_symbol_map(repo, str(path.parent.relative_to(repo) / include), seen))
            continue
        entry = parse_entry(raw)
        if entry is None:
            continue
        key, row, col, flags = entry
        symbols.setdefault(key, (row, col, flags))
    return symbols


def reference_layout(repo, layout):
    del repo
    return LAYOUTS[layout]


def preamble_from_existing(repo, machine):
    source = MACHINES[machine].get("source")
    if source is None:
        return pet_preamble()
    lines = []
    for raw in (repo / source).read_text(encoding="utf-8").splitlines():
        if raw.startswith("LeftBracket") or raw.startswith("# Generated from C64"):
            break
        lines.append(raw.rstrip())
    return lines


def pet_preamble():
    return [
        "# BMC64 generated PET business keyboard mapping",
        "# Derived from the C64 Pi 400/500 reference keymap.",
        "",
        "!CLEAR",
        "!LSHIFT 6 0",
        "!RSHIFT 6 6",
        "!VSHIFT LSHIFT",
        "!SHIFTL LSHIFT",
        "!LCBM 99 99",
        "!VCBM LCBM",
        "!LCTRL 99 99",
        "!VCTRL LCTRL",
        "",
        "Shift_L 6 0 2",
        "Shift_R 6 6 4",
        "CapsLock 6 0 64",
        "Tab 4 0 8",
        "ISO_Left_Tab 4 0 1",
        "BackSpace 4 7 8",
        "Delete 4 7 0",
        "Return 3 4 8",
        "Escape 9 4 8",
        "Home 8 4 8",
        "Left 0 5 1",
        "Right 0 5 8",
        "Up 5 4 1",
        "Down 5 4 8",
        "F1 2 0 8",
        "F2 8 0 8",
        "Page_Down 7 6 8",
        "!UNDEF Control_L",
        "!UNDEF Control_R",
        "",
    ]


def generate_pet_graphics_de(repo, output):
    symbol_map = parse_symbol_map(
        repo, "third_party/vice-3.10/data/PET/gtk3_grus_sym.vkm"
    )
    by_host = {}
    for host, symbol, host_shift in DE_LAYOUT:
        by_host.setdefault(host, []).append((symbol, host_shift))

    lines = [
        "# BMC64 PET graphics keyboard mapping for a German PC keyboard",
        "# The included base map is staged from rpi_grus_sym.vkm.",
        "",
        "!INCLUDE gtk3_grus_pos.vkm",
        "",
        "# Generated from C64 positional DE reference.",
    ]
    for host, entries in by_host.items():
        valid_entries = [
            (symbol, shift)
            for symbol, shift in entries
            if symbol in symbol_map
        ]
        for index, (symbol, shift) in enumerate(valid_entries):
            rendered = line_for(
                host,
                symbol,
                shift,
                symbol_map,
                index < len(valid_entries) - 1,
                deshift_host_shift=True,
            )
            if rendered:
                lines.append(f"{rendered}  # {symbol}")

    out_dir = output / "pet"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "rpi_graphics_pos_de.vkm").write_text(
        "\n".join(lines).rstrip() + "\n", encoding="utf-8"
    )


def line_for(host, symbol, host_shift, symbol_map, continuation, deshift_host_shift=False):
    if symbol not in symbol_map:
        return None
    row, col, flags = symbol_map[symbol]
    out_flags = flags
    if continuation:
        out_flags |= HOST_FLAG_CONTINUATION
    if host_shift:
        out_flags |= HOST_FLAG_SHIFT
        if deshift_host_shift and not (out_flags & EMULATED_SHIFT_FLAGS):
            out_flags |= HOST_FLAG_DESHIFT
    return f"{host} {row} {col} {out_flags}"


def generate(repo, output, machine, layout):
    symbol_map = parse_symbol_map(repo, MACHINES[machine]["sym"])
    refs = reference_layout(repo, layout)
    by_host = {}
    for host, symbol, host_shift in refs:
        by_host.setdefault(host, []).append((symbol, host_shift))

    lines = preamble_from_existing(repo, machine)
    if lines and lines[-1] != "":
        lines.append("")
    lines.append(f"# Generated from C64 positional {layout.upper()} reference.")

    for host, entries in by_host.items():
        valid_entries = [(symbol, shift) for symbol, shift in entries if symbol in symbol_map]
        for index, (symbol, shift) in enumerate(valid_entries):
            continuation = index < len(valid_entries) - 1
            rendered = line_for(
                host,
                symbol,
                shift,
                symbol_map,
                continuation,
                deshift_host_shift=(layout == "de"),
            )
            if rendered:
                lines.append(f"{rendered}  # {symbol}")

    if machine != "pet":
        lines.append("")
        lines.append("# Restore key mappings")
        lines.append("PageUp -3 0")
        if machine == "plus4":
            lines.append("CapsLock -4 1")
        else:
            lines.append("CapsLock 1 3 64")

    out_dir = output / MACHINES[machine]["out_dir"]
    out_dir.mkdir(parents=True, exist_ok=True)
    name = "rpi_pos_de.vkm" if layout == "de" else "rpi_pos.vkm"
    (out_dir / name).write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--machine", action="append", choices=sorted(MACHINES), default=[])
    args = parser.parse_args()

    machines = args.machine or ["vic20", "plus4", "pet"]
    for machine in machines:
        for layout in ("us", "de"):
            generate(args.repo, args.output, machine, layout)
        if machine == "pet":
            generate_pet_graphics_de(args.repo, args.output)


if __name__ == "__main__":
    main()
