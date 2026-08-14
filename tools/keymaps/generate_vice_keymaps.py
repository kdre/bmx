#!/usr/bin/env python3
"""Compile the pinned VICE GTK keymaps to BMX raw-USB-HID keymaps.

VICE's GTK maps are keyed by X keysyms.  BMX receives USB HID usages before
an operating system keyboard layout has translated them, so the source files
cannot be used directly.  This generator resolves the US and German keysyms
to a physical HID usage plus the VICE host-modifier flags while preserving the
original emulated matrix targets and flags.
"""

from __future__ import annotations

import argparse
import dataclasses
import difflib
import re
import sys
from collections import OrderedDict, defaultdict
from pathlib import Path


ALLOW_OTHER = 1 << 5
MAP_MOD_SHIFT = 1 << 7
ALT_MAP = 1 << 8
MAP_MOD_RIGHT_ALT = 1 << 9
MAP_MOD_CTRL = 1 << 10
HOST_MODIFIERS = MAP_MOD_SHIFT | MAP_MOD_RIGHT_ALT | MAP_MOD_CTRL


MACHINES = OrderedDict(
    (
        ("c64", "C64"),
        ("scpu64", "SCPU64"),
        ("c128", "C128"),
        ("vic20", "VIC20"),
        ("plus4", "PLUS4"),
    )
)

# VICE intentionally falls back to the generic business-UK map for the PET
# business-US and business-JP types.  Emit explicit aliases so changing PET
# keyboard type does not make VICE change the selected type during fallback.
PET_TYPES = OrderedDict(
    (
        (None, "buuk"),
        ("buuk", "buuk"),
        ("buus", "buuk"),
        ("bude", "bude"),
        ("bujp", "buuk"),
        ("grus", "grus"),
    )
)


@dataclasses.dataclass(frozen=True)
class Stroke:
    host: str
    modifiers: int = 0
    priority: int = 0


@dataclasses.dataclass
class Entry:
    symbol: str
    row: int
    column: int
    flags: int
    order: int


def add_level(
    table: dict[str, list[Stroke]],
    host: str,
    symbol: str | None,
    modifiers: int = 0,
    priority: int = 0,
) -> None:
    if symbol is not None:
        table.setdefault(symbol, []).append(Stroke(host, modifiers, priority))


def add_key(
    table: dict[str, list[Stroke]],
    host: str,
    base: str,
    shifted: str | None = None,
    altgr: str | None = None,
    altgr_shifted: str | None = None,
) -> None:
    add_level(table, host, base)
    add_level(table, host, shifted, MAP_MOD_SHIFT)
    add_level(table, host, altgr, MAP_MOD_RIGHT_ALT)
    add_level(
        table,
        host,
        altgr_shifted,
        MAP_MOD_RIGHT_ALT | MAP_MOD_SHIFT,
    )


def add_alias(
    table: dict[str, list[Stroke]],
    symbol: str,
    host: str,
    modifiers: int = 0,
    priority: int = 1,
) -> None:
    add_level(table, host, symbol, modifiers, priority)


def common_strokes() -> dict[str, list[Stroke]]:
    table: dict[str, list[Stroke]] = {}
    direct = {
        "BackSpace": "BackSpace",
        "Caps_Lock": "CapsLock",
        "Control_L": "Control_L",
        "Delete": "Del",
        "Down": "Down",
        "End": "End",
        "Escape": "Escape",
        "F1": "F1",
        "F2": "F2",
        "F3": "F3",
        "F4": "F4",
        "F5": "F5",
        "F6": "F6",
        "F7": "F7",
        "F8": "F8",
        "F9": "F9",
        "F10": "F10",
        "F11": "F11",
        "F12": "F12",
        "Home": "Home",
        "Insert": "Insert",
        "KP_0": "KP_0",
        "KP_1": "KP_1",
        "KP_2": "KP_2",
        "KP_3": "KP_3",
        "KP_4": "KP_4",
        "KP_5": "KP_5",
        "KP_6": "KP_6",
        "KP_7": "KP_7",
        "KP_8": "KP_8",
        "KP_9": "KP_9",
        "KP_Add": "KP_Add",
        "KP_Decimal": "KP_Decimal",
        "KP_Divide": "KP_Divide",
        "KP_Enter": "KP_Enter",
        "KP_Multiply": "KP_Multiply",
        "KP_Subtract": "KP_Subtract",
        "Left": "Left",
        "Num_Lock": "NumLock",
        "Page_Down": "PageDown",
        "Page_Up": "PageUp",
        "Prior": "PageUp",
        "Print": "HID_46",
        "Return": "Return",
        "Right": "Right",
        "Scroll_Lock": "ScrollLock",
        "Shift_L": "Shift_L",
        "Shift_R": "Shift_R",
        "Sys_Req": "HID_46",
        "Tab": "Tab",
        "Up": "Up",
        "space": "Space",
    }
    for symbol, host in direct.items():
        add_level(table, host, symbol)

    add_level(table, "Tab", "ISO_Left_Tab", MAP_MOD_SHIFT)

    # X11 reports keypad navigation keysyms while Num Lock is off.  Raw HID
    # cannot see that translation, so retain them as lower-priority aliases of
    # the corresponding physical keypad key.
    keypad_aliases = {
        "KP_Insert": "KP_0",
        "KP_End": "KP_1",
        "KP_Down": "KP_2",
        "KP_Next": "KP_3",
        "KP_Left": "KP_4",
        "KP_Begin": "KP_5",
        "KP_Right": "KP_6",
        "KP_Home": "KP_7",
        "KP_Up": "KP_8",
        "KP_Page_Up": "KP_9",
        "KP_Delete": "KP_Decimal",
        "KP_Separator": "KP_Decimal",
        "KP_Tab": "Tab",
    }
    for symbol, host in keypad_aliases.items():
        add_alias(table, symbol, host)
    add_alias(table, "Clear", "NumLock")
    return table


def us_strokes() -> dict[str, list[Stroke]]:
    table = common_strokes()
    for letter in "abcdefghijklmnopqrstuvwxyz":
        add_key(table, letter, letter, letter.upper())
    for host, base, shifted in (
        ("1", "1", "exclam"),
        ("2", "2", "at"),
        ("3", "3", "numbersign"),
        ("4", "4", "dollar"),
        ("5", "5", "percent"),
        ("6", "6", "asciicircum"),
        ("7", "7", "ampersand"),
        ("8", "8", "asterisk"),
        ("9", "9", "parenleft"),
        ("0", "0", "parenright"),
        ("Dash", "minus", "underscore"),
        ("Equals", "equal", "plus"),
        ("LeftBracket", "bracketleft", "braceleft"),
        ("RightBracket", "bracketright", "braceright"),
        ("BackSlash", "backslash", "bar"),
        ("SemiColon", "semicolon", "colon"),
        ("SingleQuote", "apostrophe", "quotedbl"),
        ("BackQuote", "grave", "asciitilde"),
        ("Comma", "comma", "less"),
        ("Period", "period", "greater"),
        ("Slash", "slash", "question"),
    ):
        add_key(table, host, base, shifted)

    # The default US layout does not emit dead keysyms, but the aliases keep
    # the original VICE maps useful with the common US-international variant.
    for symbol, host, modifiers in (
        ("dead_acute", "SingleQuote", 0),
        ("acute", "SingleQuote", 0),
        ("dead_diaeresis", "SingleQuote", MAP_MOD_SHIFT),
        ("dead_circumflex", "6", MAP_MOD_SHIFT),
        ("dead_grave", "BackQuote", 0),
        ("dead_tilde", "BackQuote", MAP_MOD_SHIFT),
        ("dead_perispomeni", "BackQuote", MAP_MOD_SHIFT),
    ):
        add_alias(table, symbol, host, modifiers)

    # ISO US keyboards can report either usage for the backslash key.
    add_alias(table, "backslash", "Pound")
    add_alias(table, "bar", "Pound", MAP_MOD_SHIFT)
    return table


def de_strokes() -> dict[str, list[Stroke]]:
    table = common_strokes()
    for host in "abcdefghijklmnopqrstuvwxyz":
        symbol = {"y": "z", "z": "y"}.get(host, host)
        add_key(table, host, symbol, symbol.upper())

    for args in (
        ("BackQuote", "dead_circumflex", "degree", None, None),
        ("1", "1", "exclam", None, None),
        ("2", "2", "quotedbl", None, None),
        ("3", "3", "section", None, "sterling"),
        ("4", "4", "dollar", None, None),
        ("5", "5", "percent", None, None),
        ("6", "6", "ampersand", None, None),
        ("7", "7", "slash", "braceleft", None),
        ("8", "8", "parenleft", "bracketleft", None),
        ("9", "9", "parenright", "bracketright", None),
        ("0", "0", "equal", "braceright", None),
        ("Dash", "ssharp", "question", "backslash", "questiondown"),
        ("Equals", "dead_acute", "dead_grave", None, None),
        ("q", "q", "Q", "at", None),
        ("LeftBracket", "udiaeresis", "Udiaeresis", "dead_diaeresis", None),
        ("RightBracket", "plus", "asterisk", "asciitilde", None),
        ("Pound", "numbersign", "apostrophe", None, None),
        ("SemiColon", "odiaeresis", "Odiaeresis", None, None),
        ("SingleQuote", "adiaeresis", "Adiaeresis", "dead_circumflex", None),
        ("KP_BackSlash", "less", "greater", "bar", None),
        ("m", "m", "M", "mu", None),
        ("Comma", "comma", "semicolon", None, None),
        ("Period", "period", "colon", None, None),
        ("Slash", "minus", "underscore", None, None),
    ):
        add_key(table, *args)

    # Pi keyboards and generic ISO keyboards have both been observed using
    # 0x31 and 0x32 for the German number-sign position.
    add_alias(table, "numbersign", "BackSlash")
    add_alias(table, "apostrophe", "BackSlash", MAP_MOD_SHIFT)

    # Non-dead variants occur with XKB's de(nodeadkeys) layout.
    for symbol, host, modifiers in (
        ("asciicircum", "BackQuote", 0),
        ("acute", "Equals", 0),
        ("grave", "Equals", MAP_MOD_SHIFT),
        ("dead_tilde", "RightBracket", MAP_MOD_RIGHT_ALT),
        ("dead_perispomeni", "RightBracket", MAP_MOD_RIGHT_ALT),
        ("brokenbar", "KP_BackSlash", MAP_MOD_RIGHT_ALT),
    ):
        add_alias(table, symbol, host, modifiers)
    return table


LAYOUTS = {"us": us_strokes(), "de": de_strokes()}

# These X keysyms have no equivalent on a standard USB PC/Pi keyboard.  They
# remain available in the pinned GTK sources but are deliberately not guessed
# onto an unrelated physical key.
KNOWN_UNREPRESENTABLE = {
    "Find",
    "Help",
    "KP_F1",
    "KP_F2",
    "KP_F3",
    "KP_F4",
    "Linefeed",
    "sterling",  # represented on DE, unavailable on default US
}


def strip_comment(line: str) -> str:
    return re.split(r"\s*(?:#|/\*)", line, maxsplit=1)[0].strip()


def source_lines(path: Path, stack: tuple[Path, ...] = ()):
    if path in stack:
        raise ValueError(f"recursive keymap include: {path}")
    for number, raw in enumerate(
        path.read_text(encoding="utf-8", errors="replace").splitlines(), 1
    ):
        line = strip_comment(raw)
        if not line:
            continue
        if line.startswith("!INCLUDE"):
            fields = line.split(maxsplit=1)
            if len(fields) != 2:
                raise ValueError(f"{path}:{number}: malformed !INCLUDE")
            yield from source_lines(path.parent / fields[1], stack + (path,))
        else:
            yield path, number, line


def parse_source(path: Path) -> tuple[list[str], list[Entry], list[Entry]]:
    directives: OrderedDict[str, str] = OrderedDict()
    positive: list[Entry] = []
    negative: OrderedDict[tuple[int, int], Entry] = OrderedDict()
    order = 0

    for actual_path, number, line in source_lines(path):
        if line.startswith("!"):
            name = line[1:].split(maxsplit=1)[0]
            if name == "CLEAR":
                positive.clear()
                negative.clear()
                directives.clear()
                directives[name] = line
            elif name == "UNDEF":
                fields = line.split(maxsplit=1)
                if len(fields) != 2:
                    raise ValueError(f"{actual_path}:{number}: malformed !UNDEF")
                symbol = fields[1]
                positive[:] = [entry for entry in positive if entry.symbol != symbol]
                for target in list(negative):
                    if negative[target].symbol == symbol:
                        del negative[target]
            elif name in {
                "LSHIFT",
                "RSHIFT",
                "VSHIFT",
                "SHIFTL",
                "LCBM",
                "VCBM",
                "LCTRL",
                "VCTRL",
            }:
                directives[name] = line
            else:
                raise ValueError(f"{actual_path}:{number}: unsupported directive {line}")
            continue

        fields = line.split()
        if len(fields) < 3:
            raise ValueError(f"{actual_path}:{number}: malformed keymap entry")
        try:
            row = int(fields[1], 0)
            column = int(fields[2], 0)
            flags = int(fields[3], 0) if len(fields) > 3 else 0
        except ValueError as error:
            raise ValueError(f"{actual_path}:{number}: malformed keymap entry") from error
        entry = Entry(fields[0], row, column, flags, order)
        order += 1
        if row < 0:
            negative[(row, column)] = entry
            continue

        for index, current in enumerate(positive):
            if (
                current.symbol == entry.symbol
                and not current.flags & ALLOW_OTHER
                and not current.flags & ALT_MAP
            ):
                positive[index] = entry
                break
        else:
            positive.append(entry)

    return list(directives.values()), positive, list(negative.values())


def render_source(
    source: Path, layout: str, source_label: str | None = None
) -> tuple[str, set[str]]:
    directives, positive, negative = parse_source(source)
    strokes = LAYOUTS[layout]
    by_host: dict[str, list[tuple[Entry, Stroke]]] = defaultdict(list)
    missing: set[str] = set()

    for entry in positive:
        translated = strokes.get(entry.symbol, ())
        if not translated:
            missing.add(entry.symbol)
        for stroke in translated:
            by_host[stroke.host].append((entry, stroke))

    output = [
        "# BMX raw-HID adaptation of an original VICE GTK keymap.",
        f"# Source: {source_label or source.as_posix()}",
        f"# Host layout: {layout.upper()}",
        "# Generated by tools/keymaps/generate_vice_keymaps.py; do not edit.",
        "",
        *directives,
        "",
    ]

    for host in sorted(by_host, key=host_sort_key):
        candidates = deduplicate_candidates(by_host[host])
        candidates.sort(key=candidate_sort_key)
        for index, (entry, stroke) in enumerate(candidates):
            semantic = entry.flags & ~(ALLOW_OTHER | HOST_MODIFIERS)
            flags = semantic | (entry.flags & HOST_MODIFIERS) | stroke.modifiers
            if index + 1 < len(candidates):
                flags |= ALLOW_OTHER
            output.append(
                f"{host:<14} {entry.row:>3} {entry.column:>2} {flags:<5}"
                f" # {entry.symbol}"
            )

    if negative:
        output.extend(("", "# Special and joystick mappings"))
    for entry in negative:
        translated = strokes.get(entry.symbol, ())
        if not translated:
            missing.add(entry.symbol)
            continue
        # Negative-row mappings are singular slots in VICE.  Prefer the
        # standard-layout stroke over any compatibility alias.
        stroke = sorted(translated, key=lambda item: (item.priority, item.modifiers))[0]
        flags = (entry.flags & ~HOST_MODIFIERS) | stroke.modifiers
        suffix = f" {flags}" if flags else ""
        output.append(
            f"{stroke.host:<14} {entry.row:>3} {entry.column:>2}{suffix}"
            f" # {entry.symbol}"
        )

    return "\n".join(output).rstrip() + "\n", missing


def deduplicate_candidates(
    candidates: list[tuple[Entry, Stroke]],
) -> list[tuple[Entry, Stroke]]:
    result: list[tuple[Entry, Stroke]] = []
    seen: set[tuple[int, int, int, int, int]] = set()
    for entry, stroke in candidates:
        semantic = entry.flags & ~ALLOW_OTHER
        effective_modifiers = (entry.flags & HOST_MODIFIERS) | stroke.modifiers
        key = (
            entry.row,
            entry.column,
            semantic & ~HOST_MODIFIERS,
            effective_modifiers,
            int(bool(semantic & ALT_MAP)),
        )
        if key not in seen:
            seen.add(key)
            result.append((entry, stroke))
    return result


def candidate_sort_key(candidate: tuple[Entry, Stroke]):
    entry, stroke = candidate
    modifiers = (entry.flags & HOST_MODIFIERS) | stroke.modifiers
    return (
        modifiers.bit_count(),
        modifiers,
        -stroke.priority,
        int(bool(entry.flags & ALT_MAP)),
        entry.order,
    )


HOST_ORDER = {
    **{letter: index for index, letter in enumerate("abcdefghijklmnopqrstuvwxyz", 10)},
    **{digit: index for index, digit in enumerate("1234567890", 40)},
}


def host_sort_key(host: str):
    return (HOST_ORDER.get(host, 100), host)


def jobs(repo: Path):
    vice_data = repo / "third_party/vice-3.10/data"
    for output_machine, source_machine in MACHINES.items():
        for kind in ("sym", "pos"):
            for layout in ("us", "de"):
                suffix = "_de" if layout == "de" else ""
                source = vice_data / source_machine / f"gtk3_{kind}{suffix}.vkm"
                target = repo / "tools/keymaps/raspi" / output_machine / (
                    f"raspi_{kind}{suffix}.vkm"
                )
                yield source, target, layout

    for target_type, source_type in PET_TYPES.items():
        for kind in ("sym", "pos"):
            for layout in ("us", "de"):
                suffix = "_de" if layout == "de" else ""
                source = vice_data / "PET" / f"gtk3_{source_type}_{kind}{suffix}.vkm"
                type_part = f"_{target_type}" if target_type else ""
                target = repo / "tools/keymaps/raspi/pet" / (
                    f"raspi{type_part}_{kind}{suffix}.vkm"
                )
                yield source, target, layout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail if checked-in generated maps are missing or stale",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="list source keysyms with no standard raw-HID equivalent",
    )
    args = parser.parse_args()
    repo = args.repo.resolve()
    failed = False
    all_missing: set[tuple[str, str]] = set()

    for source, target, layout in jobs(repo):
        content, missing = render_source(
            source, layout, source.relative_to(repo).as_posix()
        )
        unexpected = missing - KNOWN_UNREPRESENTABLE
        all_missing.update((layout, symbol) for symbol in missing)
        if unexpected:
            names = ", ".join(sorted(unexpected))
            print(f"{source}: untranslated {layout.upper()} keysyms: {names}", file=sys.stderr)
            failed = True
        if args.check:
            actual = target.read_text(encoding="utf-8") if target.exists() else ""
            if actual != content:
                print(f"stale generated keymap: {target.relative_to(repo)}", file=sys.stderr)
                diff = difflib.unified_diff(
                    actual.splitlines(),
                    content.splitlines(),
                    fromfile=str(target.relative_to(repo)),
                    tofile="generated",
                    n=2,
                )
                print("\n".join(list(diff)[:40]), file=sys.stderr)
                failed = True
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding="utf-8")

    if args.verbose and all_missing:
        summary = ", ".join(
            f"{layout.upper()}:{symbol}" for layout, symbol in sorted(all_missing)
        )
        print(f"unrepresentable source keysyms (expected): {summary}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
