#!/usr/bin/env python3
"""Load, inspect and assemble the canonical BMX SD-card layout.

``sd-layout.toml`` is deliberately the only hand-maintained description of
both the host-side SD contents and the target-side online-update path policy.
Source paths, staging profiles and host exclusions do not enter the device
projection, so changing where an unchanged binary is copied from never forces
a target rebuild.
"""

from __future__ import annotations

import argparse
import dataclasses
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import tempfile
import tomllib
from typing import Iterable, Mapping, NoReturn, Sequence


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LAYOUT_PATH = ROOT / "sd-layout.toml"
FORMAT = "bmx-sd-layout-v1"
SOURCE_INVENTORY_FORMAT = "bmx-sd-source-inventory-v1"
BOARDS = ("pi4", "pi5")
SCOPES = ("both", *BOARDS)
PROFILES = ("release", "debug")
MODES = ("config", "kernel", "keep", "metadata", "replace")

_ALIAS_RE = re.compile(r"[a-z][a-z0-9_-]{0,63}\Z")
_PLACEHOLDER_RE = re.compile(r"\{([a-z][a-z0-9_-]{0,63})\}")
_FAT_FORBIDDEN = frozenset('"*:<>?\\|')
_FAT_RESERVED = frozenset(
    {"CON", "PRN", "AUX", "NUL"}
    | {f"COM{number}" for number in range(1, 10)}
    | {f"LPT{number}" for number in range(1, 10)}
)


class SdLayoutError(ValueError):
    """A layout is malformed or cannot be assembled safely."""


def _fail(message: str) -> NoReturn:
    raise SdLayoutError(message)


def _exact_keys(
    value: object,
    required: set[str],
    where: str,
    *,
    optional: set[str] | None = None,
) -> dict[str, object]:
    if not isinstance(value, dict):
        _fail(f"{where} must be a TOML table")
    optional = optional or set()
    missing = required - set(value)
    unknown = set(value) - required - optional
    if missing:
        _fail(f"missing field in {where}: {sorted(missing)[0]}")
    if unknown:
        _fail(f"unknown field in {where}: {sorted(unknown)[0]}")
    return value


def _text(value: object, where: str) -> str:
    if not isinstance(value, str) or not value:
        _fail(f"{where} must be a non-empty string")
    return value


def _boolean(value: object, where: str) -> bool:
    if not isinstance(value, bool):
        _fail(f"{where} must be true or false")
    return value


def _string_list(
    value: object,
    where: str,
    *,
    allowed: Iterable[str] | None = None,
    allow_empty: bool = False,
) -> tuple[str, ...]:
    if not isinstance(value, list) or (not value and not allow_empty):
        _fail(f"{where} must be a{' possibly empty' if allow_empty else ' non-empty'} array")
    result = tuple(_text(item, f"{where}[{index}]") for index, item in enumerate(value))
    if len(result) != len(set(result)):
        _fail(f"{where} contains a duplicate")
    if list(result) != sorted(result):
        _fail(f"{where} must be sorted")
    if allowed is not None:
        allowed_set = set(allowed)
        unknown = [item for item in result if item not in allowed_set]
        if unknown:
            _fail(f"unknown value in {where}: {unknown[0]}")
    return result


def _fat_path(value: object, where: str, *, maximum_bytes: int = 240) -> str:
    text = _text(value, where)
    try:
        encoded = text.encode("ascii", "strict")
    except UnicodeEncodeError:
        _fail(f"{where} is not printable ASCII: {text!r}")
    if (
        len(encoded) > maximum_bytes
        or text.startswith("/")
        or text.endswith("/")
        or any(
            ord(character) < 0x20
            or ord(character) > 0x7E
            or character in _FAT_FORBIDDEN
            for character in text
        )
    ):
        _fail(f"{where} is not a safe FAT path: {text!r}")
    for component in text.split("/"):
        if (
            component in {"", ".", ".."}
            or component != component.strip()
            or component.endswith(".")
            or len(component.encode("ascii")) > 255
            or component.split(".", 1)[0].upper() in _FAT_RESERVED
        ):
            _fail(f"{where} has an unsafe FAT component: {text!r}")
    return text


def validate_fat_path(value: object, where: str = "path", *, maximum_bytes: int = 240) -> str:
    """Public fail-closed validator shared by host path-policy consumers."""

    return _fat_path(value, where, maximum_bytes=maximum_bytes)


def _source_reference(value: object, where: str, aliases: Mapping[str, str]) -> str:
    text = _text(value, where)
    if text.startswith("/") or "\\" in text or "\0" in text:
        _fail(f"{where} must use alias/relative-path syntax")
    components = text.split("/")
    if components[0] not in aliases:
        _fail(f"{where} uses unknown source alias: {components[0]}")
    if any(component in {"", ".", ".."} for component in components):
        _fail(f"{where} is not a canonical source reference")
    return text


def _glob(value: object, where: str) -> str:
    text = _text(value, where)
    if (
        text.startswith("/")
        or text.endswith("/")
        or "\\" in text
        or "\0" in text
        or "[" in text
        or "]" in text
        or any(character in (_FAT_FORBIDDEN - {"*", "?", "\\"}) for character in text)
        or any(component in {"", ".", ".."} for component in text.split("/"))
    ):
        _fail(f"{where} is not a simple relative glob: {text!r}")
    for character in text:
        if ord(character) < 0x20 or ord(character) > 0x7E:
            _fail(f"{where} is not printable ASCII")
    return text


def _target_pattern(value: object, where: str) -> str:
    text = _text(value, where)
    if "*" not in text and "?" not in text:
        return _fat_path(text, where)
    pattern = _glob(text, where)
    if pattern.startswith("**/"):
        basename = pattern[3:]
        if "/" in basename or not basename:
            _fail(f"{where} supports **/ only before a basename pattern")
        return pattern
    if "/" in pattern:
        _fail(f"{where} file patterns must be top-level or **/basename")
    return pattern


@dataclasses.dataclass(frozen=True)
class LayoutRule:
    scope: str
    kind: str
    ordinal: int
    target: str
    mode: str
    source: str | None = None
    include: tuple[str, ...] = ()
    exclude: tuple[str, ...] = ()
    profiles: tuple[str, ...] = ()
    features: tuple[str, ...] = ()
    optional: bool = False
    enabled: bool = True
    report_missing: bool = True
    review_disabled_for: tuple[str, ...] = ()
    review_exclude_pi4: tuple[str, ...] = ()
    review_exclude_pi5: tuple[str, ...] = ()
    review_move_pi4: str | None = None
    review_move_pi5: str | None = None

    @property
    def provenance(self) -> str:
        return f"{self.scope}.{self.kind}[{self.ordinal}]"

    @property
    def stages_content(self) -> bool:
        return self.enabled and (self.source is not None or self.kind == "directory")


@dataclasses.dataclass(frozen=True)
class SdLayout:
    path: Path
    sources: tuple[tuple[str, str], ...]
    rules: tuple[LayoutRule, ...]

    @property
    def source_map(self) -> dict[str, str]:
        return dict(self.sources)


@dataclasses.dataclass(frozen=True)
class PlannedFile:
    target: str
    source: Path
    source_reference: str
    mode: str
    size: int
    sha256: str
    provenance: str


@dataclasses.dataclass(frozen=True)
class StagePlan:
    board: str
    profile: str
    features: tuple[str, ...]
    files: tuple[PlannedFile, ...]
    directories: tuple[str, ...]
    missing_optional_targets: tuple[str, ...]


@dataclasses.dataclass(frozen=True)
class AssemblyResult:
    destination: Path
    entries: tuple[PlannedFile, ...]
    directories: tuple[str, ...]
    missing_optional_targets: tuple[str, ...]


@dataclasses.dataclass(frozen=True)
class StageOverride:
    kind: str
    scope: str
    target: str
    provenance: str
    original_target: str | None = None


@dataclasses.dataclass(frozen=True)
class StagePathProvenance:
    target: str
    source_reference: str | None
    mode: str
    rule: LayoutRule

    @property
    def provenance(self) -> str:
        return self.rule.provenance


@dataclasses.dataclass(frozen=True)
class SourceInventoryFile:
    """One physical regular file visible through one or more source aliases.

    ``source_reference`` is the deterministic display/default reference.  Rules
    are matched against every item in ``source_references`` so overlapping (or
    deliberately identical) source roots never duplicate the physical file and
    never lose the alias spelling used by the TOML rule.
    """

    source_reference: str
    source_references: tuple[str, ...]
    source_path: Path
    size: int
    sha256: str


@dataclasses.dataclass(frozen=True)
class StageReviewEntry:
    """Effective TOML decision for one source-to-target mapping.

    A source file may occur more than once when it intentionally feeds multiple
    SD targets.  A completely unmapped source occurs once with ``target=None``
    and ``effective_mode="excluded"``.
    """

    source_reference: str
    source_path: Path
    target: str | None
    effective_mode: str
    configured_mode: str | None
    can_restore: bool
    provenance: str | None
    scope: str | None
    restore_scope: str | None
    kind: str | None
    excluded_reason: str | None
    size: int
    sha256: str


def _parse_rule(
    raw: object,
    *,
    aliases: Mapping[str, str],
    scope: str,
    kind: str,
    ordinal: int,
) -> LayoutRule:
    common_optional = {
        "enabled", "features", "optional", "profiles", "report_missing",
        "review_disabled_for",
    }
    if kind == "file":
        value = _exact_keys(
            raw, {"mode", "to"}, f"{scope}.file[{ordinal}]",
            optional=common_optional | {
                "from", "review_move_pi4", "review_move_pi5",
            },
        )
    elif kind == "tree":
        value = _exact_keys(
            raw, {"mode", "to"}, f"{scope}.tree[{ordinal}]",
            optional=common_optional | {
                "exclude", "from", "include", "review_exclude_pi4",
                "review_exclude_pi5",
            },
        )
    else:
        value = _exact_keys(
            raw, {"mode", "to"}, f"{scope}.directory[{ordinal}]",
            optional={"enabled", "features", "profiles", "review_disabled_for"},
        )
    where = f"{scope}.{kind}[{ordinal}]"
    mode = _text(value["mode"], f"{where}.mode")
    if mode not in MODES:
        _fail(f"unknown mode in {where}: {mode}")
    source = (
        _source_reference(value["from"], f"{where}.from", aliases)
        if "from" in value else None
    )
    target = (
        _target_pattern(value["to"], f"{where}.to")
        if kind == "file" and source is None
        else _fat_path(value["to"], f"{where}.to")
    )
    if source is not None and kind == "directory":
        _fail(f"{where} cannot have from")
    if source is None and kind == "file" and mode != "keep" and "*" in target:
        # Device-only forward-compatible patterns are also useful for files
        # controlled by the updater (DTBs); never make them stage sources.
        pass
    include = ()
    exclude = ()
    if kind == "tree":
        if "include" in value:
            include = tuple(
                _glob(item, f"{where}.include[{index}]")
                for index, item in enumerate(
                    _string_list(value["include"], f"{where}.include")
                )
            )
        if "exclude" in value:
            exclude = tuple(
                _glob(item, f"{where}.exclude[{index}]")
                for index, item in enumerate(
                    _string_list(
                        value["exclude"], f"{where}.exclude", allow_empty=True,
                    )
                )
            )
    profiles = (
        _string_list(value["profiles"], f"{where}.profiles", allowed=PROFILES)
        if "profiles" in value else ()
    )
    features = (
        _string_list(value["features"], f"{where}.features")
        if "features" in value else ()
    )
    optional = _boolean(value["optional"], f"{where}.optional") if "optional" in value else False
    enabled = _boolean(value["enabled"], f"{where}.enabled") if "enabled" in value else True
    report_missing = (
        _boolean(value["report_missing"], f"{where}.report_missing")
        if "report_missing" in value else True
    )
    review_disabled_for = (
        _string_list(
            value["review_disabled_for"], f"{where}.review_disabled_for",
            allowed=BOARDS, allow_empty=True,
        )
        if "review_disabled_for" in value else ()
    )
    review_exclude_pi4 = ()
    review_exclude_pi5 = ()
    if kind == "tree":
        for board_name in BOARDS:
            field = f"review_exclude_{board_name}"
            parsed = (
                tuple(
                    _glob(item, f"{where}.{field}[{index}]")
                    for index, item in enumerate(
                        _string_list(
                            value[field], f"{where}.{field}", allow_empty=True,
                        )
                    )
                )
                if field in value else ()
            )
            if board_name == "pi4":
                review_exclude_pi4 = parsed
            else:
                review_exclude_pi5 = parsed
    review_move_pi4 = (
        _fat_path(value["review_move_pi4"], f"{where}.review_move_pi4")
        if "review_move_pi4" in value else None
    )
    review_move_pi5 = (
        _fat_path(value["review_move_pi5"], f"{where}.review_move_pi5")
        if "review_move_pi5" in value else None
    )
    if kind == "directory" and (optional or not report_missing):
        _fail(f"{where} does not support optional/report_missing")
    if kind == "directory" and mode != "keep":
        _fail(f"{where} must use mode = \"keep\"")
    if source is None and optional and mode != "kernel":
        _fail(f"{where} uses optional without a source")
    if review_disabled_for and scope != "both":
        _fail(f"{where}.review_disabled_for is valid only in scope both")
    if (review_exclude_pi4 or review_exclude_pi5) and scope != "both":
        _fail(f"{where} board-specific review excludes require scope both")
    if review_move_pi4 is not None or review_move_pi5 is not None:
        if kind != "file" or source is None:
            _fail(f"{where} board review moves require one staged file rule")
        if scope == "pi4" and review_move_pi5 is not None:
            _fail(f"{where}.review_move_pi5 conflicts with pi4 scope")
        if scope == "pi5" and review_move_pi4 is not None:
            _fail(f"{where}.review_move_pi4 conflicts with pi5 scope")
        if review_move_pi4 == target or review_move_pi5 == target:
            _fail(f"{where} review move must differ from to")
    return LayoutRule(
        scope=scope, kind=kind, ordinal=ordinal, target=target, mode=mode,
        source=source, include=include, exclude=exclude, profiles=profiles,
        features=features, optional=optional, enabled=enabled,
        report_missing=report_missing,
        review_disabled_for=review_disabled_for,
        review_exclude_pi4=review_exclude_pi4,
        review_exclude_pi5=review_exclude_pi5,
        review_move_pi4=review_move_pi4,
        review_move_pi5=review_move_pi5,
    )


def load_sd_layout(path: str | Path = DEFAULT_LAYOUT_PATH) -> SdLayout:
    """Load and strictly validate one canonical TOML layout."""

    layout_path = Path(path)
    try:
        with layout_path.open("rb") as handle:
            document = tomllib.load(handle)
    except (OSError, tomllib.TOMLDecodeError) as exc:
        raise SdLayoutError(f"cannot load SD layout {layout_path}: {exc}") from exc
    top = _exact_keys(
        document, {"default", "format", "sources"}, "layout",
        optional=set(SCOPES),
    )
    if top["format"] != FORMAT:
        _fail("unsupported SD layout format")
    if top["default"] != "deny":
        _fail("SD layout default must be deny")
    raw_sources = top["sources"]
    if not isinstance(raw_sources, dict) or not raw_sources:
        _fail("sources must be a non-empty table")
    sources: list[tuple[str, str]] = []
    for name, template_value in raw_sources.items():
        if not isinstance(name, str) or not _ALIAS_RE.fullmatch(name):
            _fail(f"invalid source alias: {name!r}")
        template = _text(template_value, f"sources.{name}")
        if "\0" in template or "\n" in template or "\r" in template:
            _fail(f"sources.{name} contains a forbidden character")
        unknown_brace = re.sub(_PLACEHOLDER_RE, "", template)
        if "{" in unknown_brace or "}" in unknown_brace:
            _fail(f"sources.{name} contains a malformed placeholder")
        sources.append((name, template))
    aliases = dict(sources)
    rules: list[LayoutRule] = []
    for scope in SCOPES:
        raw_scope = top.get(scope, {})
        if not isinstance(raw_scope, dict):
            _fail(f"{scope} must be a table")
        unknown_kinds = set(raw_scope) - {"file", "tree", "directory"}
        if unknown_kinds:
            _fail(f"unknown entry kind in {scope}: {sorted(unknown_kinds)[0]}")
        for kind in ("file", "tree", "directory"):
            raw_rules = raw_scope.get(kind, [])
            if not isinstance(raw_rules, list):
                _fail(f"{scope}.{kind} must be an array of tables")
            rules.extend(
                _parse_rule(
                    raw, aliases=aliases, scope=scope, kind=kind, ordinal=index,
                )
                for index, raw in enumerate(raw_rules)
            )
    if not rules:
        _fail("SD layout contains no entries")
    layout = SdLayout(layout_path.resolve(), tuple(sources), tuple(rules))
    _validate_static_layout(layout)
    _validate_device_mode_consistency(layout)
    return layout


def _applies(rule: LayoutRule, board: str, profile: str, features: set[str]) -> bool:
    return (
        rule.enabled
        and rule.scope in {"both", board}
        and board not in rule.review_disabled_for
        and (not rule.profiles or profile in rule.profiles)
        and set(rule.features).issubset(features)
    )


def _effective_target(rule: LayoutRule, board: str) -> str:
    if board == "pi4" and rule.review_move_pi4 is not None:
        return rule.review_move_pi4
    if board == "pi5" and rule.review_move_pi5 is not None:
        return rule.review_move_pi5
    return rule.target


def selected_layout_entries(
    layout: SdLayout | str | Path,
    board: str,
    profile: str = "release",
    features: Iterable[str] = (),
) -> tuple[LayoutRule, ...]:
    """Return enabled host entries for one board/profile/feature selection."""

    loaded = load_sd_layout(layout) if not isinstance(layout, SdLayout) else layout
    if board not in BOARDS:
        _fail(f"unsupported board: {board}")
    if profile not in PROFILES:
        _fail(f"unsupported profile: {profile}")
    feature_set = set(features)
    if any(not _ALIAS_RE.fullmatch(feature) for feature in feature_set):
        _fail("feature names must use lowercase alias syntax")
    return tuple(
        rule for rule in loaded.rules if _applies(rule, board, profile, feature_set)
    )


def feature_targets(
    layout: SdLayout | str | Path,
    feature: str,
    board: str,
    profile: str = "release",
) -> tuple[str, ...]:
    """List reportable exact file targets selected by one feature.

    This lets generators render MISSING-ROMS.txt from the canonical layout
    without maintaining another ROM inventory.
    """

    if not _ALIAS_RE.fullmatch(feature):
        _fail(f"invalid feature name: {feature}")
    entries = selected_layout_entries(layout, board, profile, {feature})
    result: list[str] = []
    seen: set[str] = set()
    for rule in entries:
        if (
            feature not in rule.features
            or rule.kind != "file"
            or "*" in rule.target
            or "?" in rule.target
            or not rule.report_missing
            or rule.target in seen
        ):
            continue
        seen.add(rule.target)
        result.append(rule.target)
    return tuple(result)


def stage_path_provenance(
    layout: SdLayout | str | Path,
    board: str,
    target: str,
    profile: str = "release",
    features: Iterable[str] = (),
) -> StagePathProvenance | None:
    """Resolve one effective final target back to its source and TOML rule."""

    safe = _fat_path(target, "stage provenance target")
    matches: list[StagePathProvenance] = []
    for rule in selected_layout_entries(layout, board, profile, features):
        effective = _effective_target(rule, board)
        if rule.kind in {"file", "directory"}:
            if effective == safe and (rule.source is not None or rule.kind == "directory"):
                matches.append(StagePathProvenance(
                    safe, rule.source, rule.mode, rule,
                ))
            continue
        if rule.source is None or not safe.startswith(effective + "/"):
            continue
        relative = safe[len(effective) + 1:]
        if _selected_by_tree(rule, relative, board):
            matches.append(StagePathProvenance(
                safe, f"{rule.source}/{relative}", rule.mode, rule,
            ))
    if len(matches) > 1:
        _fail(f"stage target {safe} has multiple provenance rules")
    return matches[0] if matches else None


def kernel_targets(
    layout: SdLayout | str | Path,
    board: str,
    *,
    include_optional: bool = False,
) -> tuple[str, ...]:
    """Return stable kernel targets, deriving requiredness from optional=false."""

    loaded = load_sd_layout(layout) if not isinstance(layout, SdLayout) else layout
    if board not in BOARDS:
        _fail(f"unsupported board: {board}")
    result = {
        rule.target for rule in loaded.rules
        if rule.scope in {"both", board}
        and rule.kind == "file"
        and rule.mode == "kernel"
        and rule.enabled
        and (include_optional or not rule.optional)
    }
    return tuple(sorted(result))


def kernel_machines(
    layout: SdLayout | str | Path,
    board: str,
    *,
    include_optional: bool = False,
) -> tuple[str, ...]:
    """Return kernel machine suffixes in their declared build order."""

    loaded = load_sd_layout(layout) if not isinstance(layout, SdLayout) else layout
    if board not in BOARDS:
        _fail(f"unsupported board: {board}")
    result: list[str] = []
    for rule in loaded.rules:
        if (
            rule.scope not in {"both", board}
            or rule.kind != "file"
            or rule.mode != "kernel"
            or (rule.optional and not include_optional)
        ):
            continue
        if "." not in rule.target:
            _fail(f"invalid kernel target: {rule.target}")
        machine = rule.target.rsplit(".", 1)[1]
        if machine not in result:
            result.append(machine)
    return tuple(result)


def _validate_static_layout(layout: SdLayout) -> None:
    for board in BOARDS:
        required = kernel_targets_unchecked(layout, board, include_optional=False)
        all_kernels = kernel_targets_unchecked(layout, board, include_optional=True)
        if len(required) < 7 or len(all_kernels) < len(required):
            _fail(f"{board} must define at least seven required kernels")
        bases: set[str] = set()
        machines: set[str] = set()
        for target in all_kernels:
            if "/" in target or ".img." not in target:
                _fail(f"invalid top-level machine kernel target: {target}")
            base, machine = target.rsplit(".", 1)
            if not machine.isascii() or not machine.isalnum():
                _fail(f"invalid kernel machine suffix: {target}")
            bases.add(base)
            machines.add(machine)
        if len(bases) != 1 or "c64" not in machines:
            _fail(f"{board} kernel targets must share one base and include c64")
    # Exact staged files may overlap a device-only pattern, but two source
    # mappings must never compete for one target in the same board.
    for board in BOARDS:
        seen: dict[str, LayoutRule] = {}
        for rule in layout.rules:
            if rule.scope not in {"both", board} or not rule.enabled or rule.source is None:
                continue
            if rule.kind != "file":
                continue
            key = rule.target.lower()
            previous = seen.get(key)
            if previous is not None:
                _fail(
                    f"duplicate/FAT-colliding {board} file targets: "
                    f"{previous.target} and {rule.target}"
                )
            seen[key] = rule
    for rule in layout.rules:
        if rule.review_move_pi4 is not None:
            _ensure_review_move_allowed(
                layout, rule, "pi4", rule.review_move_pi4,
            )
        if rule.review_move_pi5 is not None:
            _ensure_review_move_allowed(
                layout, rule, "pi5", rule.review_move_pi5,
            )
    _validate_known_stage_structure(layout)


def _validate_known_stage_structure(layout: SdLayout) -> None:
    """Reject statically known file/directory conflicts before staging.

    Source trees may legitimately share a destination when their selected
    members are disjoint, so member collisions remain a plan-time check.  An
    exact file can never, however, also be a declared/tree directory or an
    ancestor of another exact file/directory on a FAT volume.
    """

    for board in BOARDS:
        selected = tuple(
            rule for rule in layout.rules
            if rule.enabled
            and rule.scope in {"both", board}
            and board not in rule.review_disabled_for
        )
        files = tuple(
            (_effective_target(rule, board), rule)
            for rule in selected
            if rule.kind == "file" and rule.source is not None
        )
        directories = tuple(
            (_effective_target(rule, board), rule)
            for rule in selected
            if rule.kind == "directory"
            or (rule.kind == "tree" and rule.source is not None)
        )
        for target, rule in files:
            key = target.lower()
            for other_target, other in files:
                other_key = other_target.lower()
                if other is rule or not other_key.startswith(key + "/"):
                    continue
                _fail(
                    f"stage file {target} from {rule.provenance} is an ancestor "
                    f"of {other_target} from {other.provenance} for {board}"
                )
            for directory, directory_rule in directories:
                directory_key = directory.lower()
                if directory_key != key and not directory_key.startswith(key + "/"):
                    continue
                _fail(
                    f"stage file {target} from {rule.provenance} conflicts with "
                    f"directory {directory} from {directory_rule.provenance} "
                    f"for {board}"
                )


def kernel_targets_unchecked(
    layout: SdLayout, board: str, *, include_optional: bool,
) -> tuple[str, ...]:
    return tuple(sorted({
        rule.target for rule in layout.rules
        if rule.scope in {"both", board} and rule.kind == "file"
        and rule.mode == "kernel"
        and (include_optional or not rule.optional)
    }))


def _expand_source_roots(
    layout: SdLayout,
    board: str,
    overrides: Mapping[str, str | Path],
    aliases: Iterable[str] | None = None,
) -> dict[str, Path]:
    values = {key: os.fspath(value) for key, value in overrides.items()}
    values["board"] = board
    selected = set(layout.source_map) if aliases is None else set(aliases)
    roots: dict[str, Path] = {}
    for alias, template in layout.sources:
        if alias not in selected:
            continue
        if alias in overrides:
            rendered = os.fspath(overrides[alias])
        else:
            missing: set[str] = set()

            def replace(match: re.Match[str]) -> str:
                name = match.group(1)
                if name not in values:
                    missing.add(name)
                    return match.group(0)
                return values[name]

            rendered = _PLACEHOLDER_RE.sub(replace, template)
            if missing:
                _fail(
                    f"source alias {alias} needs binding(s): {', '.join(sorted(missing))}"
                )
        root = Path(rendered)
        if not root.is_absolute():
            _fail(f"resolved source alias {alias} is not absolute: {root}")
        roots[alias] = root
    unknown = set(overrides) - set(layout.source_map) - {
        placeholder
        for _alias, template in layout.sources
        for placeholder in _PLACEHOLDER_RE.findall(template)
    }
    if unknown:
        _fail(f"unknown source override/binding: {sorted(unknown)[0]}")
    return roots


def _resolve_source(reference: str, roots: Mapping[str, Path]) -> Path:
    alias, separator, relative = reference.partition("/")
    root = roots[alias]
    return root / relative if separator else root


def _matches_glob(path: str, pattern: str) -> bool:
    # fnmatch is deterministic and our strict grammar deliberately rejects
    # character classes.  For staging, '*' matching '/' is prevented by
    # applying basename-only patterns to the basename.
    if "/" not in pattern:
        return fnmatch.fnmatchcase(path.rsplit("/", 1)[-1], pattern)
    return Path(path).match(pattern)


def _selected_by_tree(
    rule: LayoutRule,
    relative: str,
    board: str | None = None,
    *,
    ignore_excludes: bool = False,
) -> bool:
    included = not rule.include or any(_matches_glob(relative, item) for item in rule.include)
    excludes = list(rule.exclude)
    if board == "pi4":
        excludes.extend(rule.review_exclude_pi4)
    elif board == "pi5":
        excludes.extend(rule.review_exclude_pi5)
    excluded = (
        False if ignore_excludes
        else any(_matches_glob(relative, item) for item in excludes)
    )
    return included and not excluded


def _regular_file_info(path: Path, where: str) -> tuple[int, str]:
    try:
        info = path.lstat()
    except OSError as exc:
        raise SdLayoutError(f"cannot inspect {where}: {path}: {exc}") from exc
    if not stat.S_ISREG(info.st_mode):
        _fail(f"{where} is not a regular file: {path}")
    digest = hashlib.sha256()
    size = 0
    try:
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                size += len(chunk)
                digest.update(chunk)
    except OSError as exc:
        raise SdLayoutError(f"cannot hash {where}: {path}: {exc}") from exc
    return size, digest.hexdigest()


def _add_target(
    files: dict[str, PlannedFile],
    fat_paths: dict[str, tuple[str, str]],
    planned: PlannedFile,
) -> None:
    key = planned.target.lower()
    previous = fat_paths.get(key)
    if previous is not None:
        _fail(
            f"duplicate/FAT-colliding stage target {planned.target} from "
            f"{planned.provenance}; already supplied by {previous[1]} as {previous[0]}"
        )
    files[planned.target] = planned
    fat_paths[key] = (planned.target, planned.provenance)


def plan_stage(
    layout: SdLayout | str | Path,
    board: str,
    source_overrides: Mapping[str, str | Path],
    profile: str = "release",
    features: Iterable[str] = (),
) -> StagePlan:
    """Resolve and hash a complete stage without writing the destination."""

    loaded = load_sd_layout(layout) if not isinstance(layout, SdLayout) else layout
    feature_set = set(features)
    entries = selected_layout_entries(loaded, board, profile, feature_set)
    roots = _expand_source_roots(loaded, board, source_overrides)
    files: dict[str, PlannedFile] = {}
    fat_paths: dict[str, tuple[str, str]] = {}
    directories: set[str] = set()
    missing_optional: set[str] = set()
    for rule in entries:
        effective_target = _effective_target(rule, board)
        if rule.kind == "directory":
            directories.add(effective_target)
            continue
        if rule.source is None:
            continue
        source = _resolve_source(rule.source, roots)
        if rule.kind == "file":
            if not source.exists() and not source.is_symlink():
                if rule.optional:
                    missing_optional.add(effective_target)
                    continue
                _fail(f"required source is missing for {rule.provenance}: {source}")
            size, digest = _regular_file_info(source, f"source for {rule.provenance}")
            _add_target(
                files, fat_paths,
                PlannedFile(
                    effective_target, source, rule.source, rule.mode, size, digest,
                    rule.provenance,
                ),
            )
            continue
        try:
            source_info = source.lstat()
        except OSError:
            if rule.optional:
                missing_optional.add(effective_target)
                continue
            _fail(f"required source tree is missing for {rule.provenance}: {source}")
        if not stat.S_ISDIR(source_info.st_mode):
            _fail(f"source tree is not a real directory for {rule.provenance}: {source}")
        # A selected tree always creates its destination, even when empty.
        directories.add(effective_target)
        for child in sorted(source.rglob("*"), key=lambda item: item.relative_to(source).as_posix()):
            relative = child.relative_to(source).as_posix()
            try:
                child_info = child.lstat()
            except OSError as exc:
                raise SdLayoutError(f"cannot inspect source tree member {child}: {exc}") from exc
            if stat.S_ISLNK(child_info.st_mode):
                _fail(f"source tree contains a symbolic link: {child}")
            if stat.S_ISDIR(child_info.st_mode):
                if not rule.include and not any(_matches_glob(relative, item) for item in rule.exclude):
                    directories.add(f"{effective_target}/{relative}")
                continue
            if not stat.S_ISREG(child_info.st_mode):
                _fail(f"source tree contains a special file: {child}")
            if not _selected_by_tree(rule, relative, board):
                continue
            target = _fat_path(
                f"{effective_target}/{relative}",
                f"resolved {rule.provenance} target",
            )
            size, digest = _regular_file_info(child, f"source for {rule.provenance}")
            _add_target(
                files, fat_paths,
                PlannedFile(
                    target, child, f"{rule.source}/{relative}", rule.mode,
                    size, digest, rule.provenance,
                ),
            )
            parent = target.rpartition("/")[0]
            while parent:
                directories.add(parent)
                parent = parent.rpartition("/")[0]
    for target, planned in files.items():
        parent = target.rpartition("/")[0]
        while parent:
            directories.add(parent)
            parent = parent.rpartition("/")[0]
        if target.lower() in {directory.lower() for directory in directories}:
            _fail(f"stage path is both a file and directory: {target}")
    # Catch FAT collisions among directories and file/directory ancestors.
    fat_directories: dict[str, str] = {}
    for directory in sorted(directories):
        _fat_path(directory, "resolved stage directory")
        key = directory.lower()
        previous = fat_directories.get(key)
        if previous is not None and previous != directory:
            _fail(f"FAT-colliding stage directories: {previous} and {directory}")
        if key in fat_paths:
            _fail(f"stage target is both file and directory: {directory}")
        fat_directories[key] = directory
    return StagePlan(
        board=board,
        profile=profile,
        features=tuple(sorted(feature_set)),
        files=tuple(files[path] for path in sorted(files)),
        directories=tuple(sorted(directories)),
        missing_optional_targets=tuple(sorted(missing_optional)),
    )


def scan_source_inventory(
    layout: SdLayout | str | Path,
    board: str,
    source_overrides: Mapping[str, str | Path],
    aliases: Iterable[str] | None = None,
) -> tuple[SourceInventoryFile, ...]:
    """Hash every regular file below the resolved source roots exactly once.

    Source roots intentionally overlap (``repo`` is the broad fallback, while
    aliases such as ``sdcard`` and ``pi5_boot`` are more specific).  Physical
    paths are therefore deduplicated while every valid alias spelling is kept
    for subsequent TOML rule matching.  Missing roots are ignored so a cached
    Public/kernel inventory can also be built when the short-lived generated
    root is absent.
    """

    loaded = load_sd_layout(layout) if not isinstance(layout, SdLayout) else layout
    if board not in BOARDS:
        _fail(f"unsupported board: {board}")
    alias_order = {name: index for index, (name, _template) in enumerate(loaded.sources)}
    if aliases is None:
        selected_aliases = set(alias_order)
    else:
        requested = tuple(aliases)
        if len(requested) != len(set(requested)):
            _fail("source inventory alias filter contains a duplicate")
        unknown = set(requested) - set(alias_order)
        if unknown:
            _fail(f"unknown source inventory alias: {sorted(unknown)[0]}")
        selected_aliases = set(requested)
    roots = _expand_source_roots(
        loaded, board, source_overrides, selected_aliases,
    )
    aliases_by_file: dict[Path, dict[str, tuple[int, int, str]]] = {}
    for alias, _template in loaded.sources:
        if alias not in selected_aliases:
            continue
        configured_root = roots[alias]
        if not configured_root.exists() and not configured_root.is_symlink():
            continue
        try:
            root_info = configured_root.lstat()
        except OSError as exc:
            raise SdLayoutError(f"cannot inspect source root {alias}: {configured_root}: {exc}") from exc
        if stat.S_ISLNK(root_info.st_mode) or not stat.S_ISDIR(root_info.st_mode):
            _fail(f"source root {alias} is not a real directory: {configured_root}")
        root = configured_root.resolve()
        try:
            children = sorted(
                root.rglob("*"), key=lambda item: item.relative_to(root).as_posix(),
            )
        except OSError as exc:
            raise SdLayoutError(f"cannot enumerate source root {alias}: {root}: {exc}") from exc
        for child in children:
            relative_path = child.relative_to(root)
            if ".git" in relative_path.parts:
                continue
            relative = relative_path.as_posix()
            try:
                info = child.lstat()
            except OSError as exc:
                raise SdLayoutError(f"cannot inspect source inventory member {child}: {exc}") from exc
            if stat.S_ISDIR(info.st_mode):
                continue
            # The inventory represents candidate regular source files.  A
            # broad alias such as ``repo`` may also contain build/cache
            # symlinks which are neither candidates nor followed.  A symlink
            # selected by an active staging rule is still rejected later by
            # plan_stage's regular-file/tree validation.
            if stat.S_ISLNK(info.st_mode):
                continue
            if not stat.S_ISREG(info.st_mode):
                # Fail closed on devices/FIFOs/sockets: unlike a symlink they
                # are unexpected inside a release source tree, and silently
                # hiding them would make that malformed tree look reviewable.
                _fail(f"source inventory contains a special file: {child}")
            resolved = child.resolve()
            try:
                resolved.relative_to(root)
            except ValueError:
                _fail(f"source inventory member escapes root {alias}: {child}")
            reference = _source_reference(
                f"{alias}/{relative}", f"source inventory member {child}", loaded.source_map,
            )
            # Prefer the most specific root, then the declaration order.  Keep
            # the reference in the rank as a deterministic final tie breaker.
            rank = (-len(root.parts), alias_order[alias], reference)
            aliases_by_file.setdefault(resolved, {})[reference] = rank

    result: list[SourceInventoryFile] = []
    for source_path, ranked in aliases_by_file.items():
        references = tuple(sorted(ranked, key=lambda item: ranked[item]))
        size, digest = _regular_file_info(source_path, "source inventory file")
        result.append(SourceInventoryFile(
            source_reference=references[0],
            source_references=references,
            source_path=source_path,
            size=size,
            sha256=digest,
        ))
    return tuple(sorted(result, key=lambda item: (item.source_reference, os.fspath(item.source_path))))


def _source_inventory_document(
    inventory: Sequence[SourceInventoryFile],
) -> dict[str, object]:
    return {
        "format": SOURCE_INVENTORY_FORMAT,
        "files": [
            {
                "source_reference": item.source_reference,
                "source_references": list(item.source_references),
                "source_path": os.fspath(item.source_path),
                "size": item.size,
                "sha256": item.sha256,
            }
            for item in inventory
        ],
    }


def load_source_inventory(
    path: str | Path,
    layout: SdLayout | str | Path,
) -> tuple[SourceInventoryFile, ...]:
    """Load one strict persisted source-inventory cache without touching files."""

    loaded_layout = load_sd_layout(layout) if not isinstance(layout, SdLayout) else layout
    inventory_path = Path(path)
    try:
        document = json.loads(inventory_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise SdLayoutError(f"cannot load source inventory {inventory_path}: {exc}") from exc
    top = _exact_keys(document, {"files", "format"}, "source inventory")
    if top["format"] != SOURCE_INVENTORY_FORMAT:
        _fail("unsupported source inventory format")
    raw_files = top["files"]
    if not isinstance(raw_files, list):
        _fail("source inventory files must be an array")
    result: list[SourceInventoryFile] = []
    seen_paths: set[str] = set()
    seen_references: set[str] = set()
    aliases = loaded_layout.source_map
    for index, raw in enumerate(raw_files):
        where = f"source inventory files[{index}]"
        value = _exact_keys(
            raw,
            {"sha256", "size", "source_path", "source_reference", "source_references"},
            where,
        )
        primary = _source_reference(
            value["source_reference"], f"{where}.source_reference", aliases,
        )
        raw_references = value["source_references"]
        if not isinstance(raw_references, list) or not raw_references:
            _fail(f"{where}.source_references must be a non-empty array")
        references = tuple(
            _source_reference(item, f"{where}.source_references[{item_index}]", aliases)
            for item_index, item in enumerate(raw_references)
        )
        if len(references) != len(set(references)):
            _fail(f"{where}.source_references contains a duplicate")
        if references[0] != primary:
            _fail(f"{where}.source_reference must be the first source reference")
        raw_path = _text(value["source_path"], f"{where}.source_path")
        if "\0" in raw_path or "\n" in raw_path or "\r" in raw_path:
            _fail(f"{where}.source_path contains a forbidden character")
        source_path = Path(raw_path)
        if not source_path.is_absolute():
            _fail(f"{where}.source_path must be absolute")
        size = value["size"]
        if isinstance(size, bool) or not isinstance(size, int) or size < 0:
            _fail(f"{where}.size must be a non-negative integer")
        digest = value["sha256"]
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            _fail(f"{where}.sha256 must be a lowercase SHA-256")
        path_key = os.fspath(source_path)
        duplicate_reference = next(
            (reference for reference in references if reference in seen_references),
            None,
        )
        if path_key in seen_paths or duplicate_reference is not None:
            _fail(f"{where} duplicates a source inventory file")
        seen_paths.add(path_key)
        seen_references.update(references)
        result.append(SourceInventoryFile(
            source_reference=primary,
            source_references=references,
            source_path=source_path,
            size=size,
            sha256=digest,
        ))
    if result != sorted(
        result, key=lambda item: (item.source_reference, os.fspath(item.source_path)),
    ):
        _fail("source inventory files must be sorted")
    return tuple(result)


def save_source_inventory(
    inventory: Sequence[SourceInventoryFile],
    path: str | Path,
    layout: SdLayout | str | Path,
) -> None:
    """Atomically persist and pre-validate a cache of source file metadata."""

    loaded_layout = load_sd_layout(layout) if not isinstance(layout, SdLayout) else layout
    destination = Path(path)
    ordered = tuple(sorted(
        inventory, key=lambda item: (item.source_reference, os.fspath(item.source_path)),
    ))
    encoded = (
        json.dumps(
            _source_inventory_document(ordered),
            indent=2,
            sort_keys=True,
            ensure_ascii=True,
        )
        + "\n"
    ).encode("ascii")
    temporary = destination.with_name(f".{destination.name}.tmp-{os.getpid()}")
    try:
        with temporary.open("xb") as handle:
            os.fchmod(handle.fileno(), 0o600)
            handle.write(encoded)
            handle.flush()
            os.fsync(handle.fileno())
        if load_source_inventory(temporary, loaded_layout) != ordered:
            _fail("persisted source inventory does not match requested metadata")
        os.replace(temporary, destination)
        try:
            descriptor = os.open(destination.parent, os.O_RDONLY)
        except OSError:
            descriptor = -1
        if descriptor >= 0:
            try:
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
    finally:
        temporary.unlink(missing_ok=True)


def _rule_context_exclusions(
    rule: LayoutRule, board: str, profile: str, features: set[str],
) -> list[str]:
    reasons: list[str] = []
    if not rule.enabled:
        reasons.append("disabled")
    if board in rule.review_disabled_for:
        reasons.append("review-disabled")
    if rule.profiles and profile not in rule.profiles:
        reasons.append("profile")
    if not set(rule.features).issubset(features):
        reasons.append("feature")
    return reasons


def _tree_exclusion_details(
    rule: LayoutRule, relative: str, board: str,
) -> tuple[list[str], bool, str | None]:
    base_matches = [item for item in rule.exclude if _matches_glob(relative, item)]
    review_values = (
        rule.review_exclude_pi4 if board == "pi4" else rule.review_exclude_pi5
    )
    review_matches = [item for item in review_values if _matches_glob(relative, item)]
    reasons: list[str] = []
    if base_matches:
        reasons.append("tree-exclude")
    if review_matches:
        reasons.append("review-tree-exclude")
    can_restore = False
    restore_scope: str | None = None
    if len(base_matches) == 1 and base_matches[0] == relative and not review_matches:
        can_restore = True
        restore_scope = rule.scope
    elif len(review_matches) == 1 and review_matches[0] == relative and not base_matches:
        can_restore = True
        restore_scope = board
    return reasons, can_restore, restore_scope


def stage_review_inventory(
    layout: SdLayout | str | Path,
    board: str,
    source_overrides: Mapping[str, str | Path] | None = None,
    profile: str = "release",
    features: Iterable[str] = (),
    *,
    source_inventory: Sequence[SourceInventoryFile] | None = None,
) -> tuple[StageReviewEntry, ...]:
    """Derive all effective source decisions directly from ``sd-layout.toml``.

    Callers may cache ``scan_source_inventory`` and pass it back through
    ``source_inventory``; reclassifying modes after a TOML edit then performs no
    filesystem traversal or hashing.  Exactly one of ``source_overrides`` and a
    cached inventory must be supplied.
    """

    loaded = load_sd_layout(layout) if not isinstance(layout, SdLayout) else layout
    if board not in BOARDS:
        _fail(f"unsupported board: {board}")
    if profile not in PROFILES:
        _fail(f"unsupported profile: {profile}")
    feature_set = set(features)
    if any(not _ALIAS_RE.fullmatch(feature) for feature in feature_set):
        _fail("feature names must use lowercase alias syntax")
    if source_inventory is None:
        if source_overrides is None:
            _fail("stage review inventory needs source overrides or a cached inventory")
        inventory_files = scan_source_inventory(loaded, board, source_overrides)
    else:
        if source_overrides is not None:
            _fail("stage review inventory accepts source overrides or a cached inventory, not both")
        inventory_files = tuple(source_inventory)

    result: list[StageReviewEntry] = []
    device_projection = device_projection_document(loaded)
    for source_file in inventory_files:
        candidates: list[tuple[int, StageReviewEntry]] = []
        references = set(source_file.source_references)
        for rule in loaded.rules:
            if (
                rule.source is None
                or rule.scope not in {"both", board}
                or rule.kind == "directory"
            ):
                continue
            source_reference: str | None = None
            relative: str | None = None
            priority = 0 if rule.kind == "file" else 1
            if rule.kind == "file":
                if rule.source not in references:
                    continue
                source_reference = rule.source
            else:
                prefix = rule.source + "/"
                matching = sorted(
                    reference for reference in references if reference.startswith(prefix)
                )
                if not matching:
                    continue
                source_reference = matching[0]
                relative = source_reference[len(prefix):]
                if not _selected_by_tree(
                    rule, relative, board, ignore_excludes=True,
                ):
                    continue

            reasons = _rule_context_exclusions(
                rule, board, profile, feature_set,
            )
            can_restore = False
            restore_scope: str | None = None
            if rule.kind == "file":
                if reasons == ["disabled"]:
                    can_restore = True
                    restore_scope = rule.scope
                elif reasons == ["review-disabled"]:
                    can_restore = True
                    restore_scope = board
                target = _effective_target(rule, board)
            else:
                assert relative is not None
                tree_reasons, tree_restore, tree_scope = _tree_exclusion_details(
                    rule, relative, board,
                )
                reasons.extend(tree_reasons)
                if not _rule_context_exclusions(rule, board, profile, feature_set):
                    can_restore = tree_restore
                    restore_scope = tree_scope
                target = _fat_path(
                    f"{rule.target}/{relative}", f"resolved {rule.provenance} review target",
                )
            effective_mode = "excluded" if reasons else rule.mode
            if not reasons:
                device_mode = _device_mode_from_projection(
                    loaded, device_projection, target,
                )
                if device_mode != rule.mode:
                    _fail(
                        f"board-inconsistent device mode for {board} stage target "
                        f"{target}: TOML mapping is {rule.mode}, device mode is "
                        f"{device_mode or 'reject'}"
                    )
            candidates.append((priority, StageReviewEntry(
                source_reference=source_reference,
                source_path=source_file.source_path,
                target=target,
                effective_mode=effective_mode,
                configured_mode=rule.mode,
                can_restore=can_restore,
                provenance=rule.provenance,
                scope=rule.scope,
                restore_scope=restore_scope,
                kind=rule.kind,
                excluded_reason=",".join(reasons) if reasons else None,
                size=source_file.size,
                sha256=source_file.sha256,
            )))

        by_target: dict[str, list[tuple[int, StageReviewEntry]]] = {}
        for candidate in candidates:
            entry = candidate[1]
            assert entry.target is not None
            by_target.setdefault(entry.target.lower(), []).append(candidate)
        selected: list[StageReviewEntry] = []
        for target_candidates in by_target.values():
            active = [item for item in target_candidates if item[1].effective_mode != "excluded"]
            if len(active) > 1:
                descriptions = ", ".join(item[1].provenance or "?" for item in active)
                _fail(
                    f"source-to-target mapping has multiple active rules for "
                    f"{source_file.source_reference}: {descriptions}"
                )
            pool = active or target_candidates
            selected.append(min(
                pool,
                key=lambda item: (
                    item[0], item[1].provenance or "", item[1].source_reference,
                ),
            )[1])
        if selected:
            # Moving a member supplied by a tree creates two cooperating TOML
            # changes: an exact file mapping at the new target and an exclusion
            # from the original tree target.  They are one effective review
            # decision, not two independently editable rows.  Hiding the
            # supporting exclusion also prevents ``x`` from re-enabling the old
            # tree target while the moved exact mapping remains active.
            rules_by_provenance = {
                rule.provenance: rule for rule in loaded.rules
            }
            moved_origins = {
                rule.target.lower()
                for _, entry in candidates
                if entry.provenance is not None
                and (rule := rules_by_provenance[entry.provenance]).kind == "file"
                and _effective_target(rule, board) != rule.target
            }
            selected = [
                entry for entry in selected
                if not (
                    entry.effective_mode == "excluded"
                    and entry.kind == "tree"
                    and entry.can_restore
                    and entry.target is not None
                    and entry.target.lower() in moved_origins
                    and entry.excluded_reason in {
                        "tree-exclude", "review-tree-exclude",
                    }
                )
            ]
            result.extend(selected)
        else:
            result.append(StageReviewEntry(
                source_reference=source_file.source_reference,
                source_path=source_file.source_path,
                target=None,
                effective_mode="excluded",
                configured_mode=None,
                can_restore=False,
                provenance=None,
                scope=None,
                restore_scope=None,
                kind=None,
                excluded_reason="default-deny",
                size=source_file.size,
                sha256=source_file.sha256,
            ))
    active_targets: dict[str, StageReviewEntry] = {}
    for entry in result:
        if entry.target is None or entry.effective_mode == "excluded":
            continue
        target_key = entry.target.lower()
        previous = active_targets.get(target_key)
        if previous is not None:
            _fail(
                f"stage target {entry.target} has multiple active sources: "
                f"{previous.source_reference} ({previous.provenance or '?'}) and "
                f"{entry.source_reference} ({entry.provenance or '?'})"
            )
        active_targets[target_key] = entry
    return tuple(sorted(
        result,
        key=lambda item: (
            item.source_reference, item.target or "", item.provenance or "",
        ),
    ))


def _publish_tree(temporary: Path, destination: Path, *, replace: bool) -> None:
    backup = destination.with_name(f".{destination.name}.sd-layout-old-{os.getpid()}")
    if backup.exists() or backup.is_symlink():
        _fail(f"stale SD-layout backup exists: {backup}")
    had_destination = destination.exists() or destination.is_symlink()
    if had_destination:
        mode = destination.lstat().st_mode
        if stat.S_ISLNK(mode) or not stat.S_ISDIR(mode):
            _fail(f"stage destination is not a real directory: {destination}")
        if not replace and any(destination.iterdir()):
            _fail(f"stage destination is not empty: {destination}")
        if not replace:
            destination.rmdir()
        else:
            os.replace(destination, backup)
    try:
        os.replace(temporary, destination)
    except Exception:
        if backup.exists() and not destination.exists():
            os.replace(backup, destination)
        raise
    if backup.exists():
        shutil.rmtree(backup)


def _validated_destination(
    layout: SdLayout,
    board: str,
    source_overrides: Mapping[str, str | Path],
    destination: Path,
) -> Path:
    raw = Path(destination)
    if raw.exists() or raw.is_symlink():
        mode = raw.lstat().st_mode
        if stat.S_ISLNK(mode) or not stat.S_ISDIR(mode):
            _fail(f"stage destination is not a real directory: {raw}")
    canonical = raw.parent.resolve(strict=False) / raw.name
    if canonical == Path(canonical.anchor):
        _fail(f"refusing filesystem-root stage destination: {canonical}")
    roots = _expand_source_roots(layout, board, source_overrides)
    for alias, source_root in roots.items():
        source = source_root.resolve(strict=False)
        if canonical == source or canonical in source.parents:
            _fail(
                f"stage destination would replace source alias {alias}: "
                f"{canonical} vs {source}"
            )
    return canonical


def assemble_stage(
    layout: SdLayout | str | Path,
    board: str,
    source_overrides: Mapping[str, str | Path],
    destination: Path,
    profile: str = "release",
    features: Iterable[str] = (),
    *,
    replace: bool = False,
) -> AssemblyResult:
    """Build a complete stage privately, then publish it at ``destination``."""

    loaded = load_sd_layout(layout) if not isinstance(layout, SdLayout) else layout
    plan = plan_stage(loaded, board, source_overrides, profile, features)
    destination = _validated_destination(
        loaded, board, source_overrides, Path(destination),
    )
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(
        prefix=f".{destination.name}.sd-layout-new-", dir=destination.parent,
    ))
    try:
        for directory in plan.directories:
            (temporary / directory).mkdir(parents=True, exist_ok=True)
        for entry in plan.files:
            target = temporary / entry.target
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(entry.source, target)
            os.chmod(target, 0o644)
            size, digest = _regular_file_info(target, f"assembled target {entry.target}")
            if size != entry.size or digest != entry.sha256:
                _fail(f"assembled target verification failed: {entry.target}")
        _publish_tree(temporary, destination, replace=replace)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    return AssemblyResult(
        destination.resolve(), plan.files, plan.directories,
        plan.missing_optional_targets,
    )


def tree_digest(root: Path) -> str:
    """Hash a typed tree without timestamps or host permission bits."""

    root = Path(root)
    if root.is_symlink() or not root.is_dir():
        _fail(f"tree is not a real directory: {root}")
    digest = hashlib.sha256()
    for entry in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
        relative = entry.relative_to(root).as_posix()
        if relative == ".git" or relative.startswith(".git/"):
            continue
        mode = entry.lstat().st_mode
        if stat.S_ISLNK(mode):
            _fail(f"tree contains a symbolic link: {relative}")
        if stat.S_ISDIR(mode):
            digest.update(b"D\0" + relative.encode("utf-8") + b"\0")
        elif stat.S_ISREG(mode):
            size, file_digest = _regular_file_info(entry, "tree member")
            digest.update(
                b"F\0" + relative.encode("utf-8") + b"\0"
                + str(size).encode("ascii") + b"\0"
                + file_digest.encode("ascii") + b"\0"
            )
        else:
            _fail(f"tree contains a special entry: {relative}")
    return digest.hexdigest()


def _device_pattern(pattern: str, *, root: str | None, mode: str, where: str) -> dict[str, object]:
    if "/" in pattern:
        _fail(f"{where} cannot be represented by the target matcher: {pattern}")
    if "?" in pattern or pattern.count("*") > 1:
        _fail(f"{where} cannot be represented by the target matcher: {pattern}")
    if "*" not in pattern:
        return {
            "kind": "basename-exact", "mode": mode, "root": root,
            "value": pattern, "suffix": None,
        }
    prefix, suffix = pattern.split("*", 1)
    return {
        "kind": "basename-suffix" if not prefix else "basename-prefix-suffix",
        "mode": mode, "root": root,
        "value": prefix or None, "suffix": suffix,
    }


def _device_matcher(rule: LayoutRule) -> tuple[dict[str, object], ...]:
    if rule.kind == "tree":
        root = rule.target.split("/", 1)[0]
        if not rule.include:
            return ({
                "kind": "top-root", "mode": rule.mode, "root": root,
                "value": None, "suffix": None,
            },)
        return tuple(
            _device_pattern(
                pattern, root=root, mode=rule.mode, where=rule.provenance,
            )
            for pattern in rule.include
        )
    target = rule.target
    if target.startswith("**/"):
        return (_device_pattern(
            target[3:], root=None, mode=rule.mode, where=rule.provenance,
        ),)
    if "*" in target or "?" in target:
        return (_device_pattern(
            target, root=None, mode=rule.mode, where=rule.provenance,
        ),)
    return ({
        "kind": "exact-path", "mode": rule.mode, "root": None,
        "value": target, "suffix": None,
    },)


def _device_matcher_key(rule: Mapping[str, object]) -> tuple[str, str, str, str]:
    return (
        str(rule["kind"]), str(rule.get("root") or ""),
        str(rule.get("value") or ""), str(rule.get("suffix") or ""),
    )


def _device_matcher_matches(rule: Mapping[str, object], path: str) -> bool:
    root = rule.get("root")
    if root is not None and path.split("/", 1)[0] != root:
        return False
    basename = path.rsplit("/", 1)[-1]
    kind = rule["kind"]
    if kind == "exact-path":
        return path == rule["value"]
    if kind == "top-root":
        return True
    if kind == "basename-exact":
        return basename == rule["value"]
    if kind == "basename-suffix":
        return basename.endswith(str(rule.get("suffix") or ""))
    if kind == "basename-prefix-suffix":
        return (
            basename.startswith(str(rule.get("value") or ""))
            and basename.endswith(str(rule.get("suffix") or ""))
        )
    return False


_DEVICE_MATCHER_RANK = {
    "exact-path": 0,
    "basename-exact": 1,
    "basename-prefix-suffix": 2,
    "basename-suffix": 3,
    "top-root": 4,
}


def _device_matcher_order_key(
    matcher: Mapping[str, object],
) -> tuple[int, str, str, str, str]:
    return (
        _DEVICE_MATCHER_RANK[str(matcher["kind"])],
        *_device_matcher_key(matcher),
    )


def _basename_matchers_overlap(
    first: Mapping[str, object], second: Mapping[str, object],
) -> bool:
    def constraints(matcher: Mapping[str, object]) -> tuple[str | None, str, str]:
        kind = matcher["kind"]
        if kind == "basename-exact":
            return str(matcher["value"]), "", ""
        if kind == "basename-suffix":
            return None, "", str(matcher.get("suffix") or "")
        if kind == "basename-prefix-suffix":
            return (
                None,
                str(matcher.get("value") or ""),
                str(matcher.get("suffix") or ""),
            )
        raise AssertionError(f"unexpected basename matcher: {kind}")

    first_exact, first_prefix, first_suffix = constraints(first)
    second_exact, second_prefix, second_suffix = constraints(second)
    if first_exact is not None:
        if second_exact is not None:
            return first_exact == second_exact
        return (
            first_exact.startswith(second_prefix)
            and first_exact.endswith(second_suffix)
        )
    if second_exact is not None:
        return (
            second_exact.startswith(first_prefix)
            and second_exact.endswith(first_suffix)
        )
    return (
        (first_prefix.startswith(second_prefix) or second_prefix.startswith(first_prefix))
        and (first_suffix.endswith(second_suffix) or second_suffix.endswith(first_suffix))
    )


def _device_matchers_overlap(
    first: Mapping[str, object], second: Mapping[str, object],
) -> bool:
    if first["kind"] == "exact-path":
        return _device_matcher_matches(second, str(first["value"]))
    if second["kind"] == "exact-path":
        return _device_matcher_matches(first, str(second["value"]))
    first_root = first.get("root")
    second_root = second.get("root")
    if (
        first_root is not None
        and second_root is not None
        and first_root != second_root
    ):
        return False
    if first["kind"] == "top-root" or second["kind"] == "top-root":
        return True
    return _basename_matchers_overlap(first, second)


def _validate_device_matcher_scopes(
    matchers: Sequence[Mapping[str, object]],
    boards_by_key: Mapping[tuple[str, str, str, str], set[str]],
    origins_by_key: Mapping[tuple[str, str, str, str], set[str]],
) -> None:
    """Reject board-local pattern precedence that leaks onto another board.

    More-specific patterns intentionally override broader policies in the
    canonical layout.  That remains safe when the winning rule applies to all
    boards covered by the lower-priority rule.  A board-local winner cannot be
    encoded by the source-independent device matcher and must fail closed.
    Exact paths are validated separately against every board's staged modes.
    """

    for index, first in enumerate(matchers):
        if first["kind"] == "exact-path":
            continue
        for second in matchers[index + 1:]:
            if (
                second["kind"] == "exact-path"
                or first["mode"] == second["mode"]
                or not _device_matchers_overlap(first, second)
            ):
                continue
            winner, loser = sorted(
                (first, second), key=_device_matcher_order_key,
            )
            winner_key = _device_matcher_key(winner)
            loser_key = _device_matcher_key(loser)
            if boards_by_key[winner_key].issuperset(boards_by_key[loser_key]):
                continue
            winner_origins = ", ".join(sorted(origins_by_key[winner_key]))
            loser_origins = ", ".join(sorted(origins_by_key[loser_key]))
            _fail(
                "board-inconsistent overlapping device matchers: "
                f"{winner_origins} ({winner['mode']}) overrides "
                f"{loser_origins} ({loser['mode']})"
            )


def device_projection_document(layout: SdLayout | str | Path) -> dict[str, object]:
    """Return the normalized, source-independent target policy semantics."""

    loaded = load_sd_layout(layout) if not isinstance(layout, SdLayout) else layout
    matchers: dict[tuple[str, str, str, str], dict[str, object]] = {}
    matcher_boards: dict[tuple[str, str, str, str], set[str]] = {}
    matcher_origins: dict[tuple[str, str, str, str], set[str]] = {}
    directories: set[str] = set()
    for rule in loaded.rules:
        if rule.kind == "directory":
            directories.add(rule.target.split("/", 1)[0])
            continue
        if rule.kind == "tree":
            directories.add(rule.target.split("/", 1)[0])
        if rule.mode != "kernel":
            for matcher in _device_matcher(rule):
                key = _device_matcher_key(matcher)
                previous = matchers.get(key)
                if previous is not None and previous["mode"] != matcher["mode"]:
                    _fail(
                        f"device matcher has conflicting modes: {key}: "
                        f"{previous['mode']} and {matcher['mode']}"
                    )
                matchers[key] = matcher
                matcher_boards.setdefault(key, set()).update(
                    BOARDS if rule.scope == "both" else (rule.scope,)
                )
                matcher_origins.setdefault(key, set()).add(rule.provenance)
        if "/" in rule.target and not rule.target.startswith("**/"):
            directories.add(rule.target.split("/", 1)[0])
    # Exact mappings already accepted by the winning broader matcher of the
    # same mode do not change target semantics and must not trigger a kernel
    # rebuild. Merely finding any same-mode matcher is insufficient: a more
    # specific conflicting matcher may win first (for example *.vkm replace
    # inside a broadly preserved machine directory).
    normalized: list[dict[str, object]] = []
    values = list(matchers.values())
    _validate_device_matcher_scopes(values, matcher_boards, matcher_origins)
    for matcher in values:
        if matcher["kind"] == "exact-path":
            broader = [
                other for other in values
                if other is not matcher
                and _device_matcher_matches(other, str(matcher["value"]))
            ]
            if broader:
                winner = min(broader, key=_device_matcher_order_key)
                if winner["mode"] == matcher["mode"]:
                    continue
        normalized.append(matcher)
    normalized.sort(key=lambda item: (
        *_device_matcher_order_key(item), str(item["mode"]),
    ))
    kernels = {
        board: {
            "required": list(kernel_targets_unchecked(loaded, board, include_optional=False)),
            "all": list(kernel_targets_unchecked(loaded, board, include_optional=True)),
        }
        for board in BOARDS
    }
    return {
        "format": "bmx-sd-device-projection-v1",
        "default": "deny",
        "directories": sorted(directories),
        "kernels": kernels,
        "rules": normalized,
    }


def device_projection_sha256(layout: SdLayout | str | Path) -> str:
    encoded = json.dumps(
        device_projection_document(layout), sort_keys=True, separators=(",", ":"),
        ensure_ascii=True,
    ).encode("ascii")
    return hashlib.sha256(encoded).hexdigest()


def _device_mode_from_projection(
    layout: SdLayout, projection: Mapping[str, object], path: str,
) -> str | None:
    for board in BOARDS:
        if path in kernel_targets_unchecked(layout, board, include_optional=True):
            return "kernel"
    raw_rules = projection["rules"]
    assert isinstance(raw_rules, list)
    for matcher in raw_rules:
        assert isinstance(matcher, dict)
        if _device_matcher_matches(matcher, path):
            return str(matcher["mode"])
    return None


def _device_mode_for_path(layout: SdLayout, path: str) -> str | None:
    return _device_mode_from_projection(
        layout, device_projection_document(layout), path,
    )


def _staged_modes_for_target(
    layout: SdLayout, board: str, target: str,
) -> set[str]:
    modes: set[str] = set()
    for rule in layout.rules:
        if (
            rule.source is None
            or not rule.enabled
            or rule.scope not in {"both", board}
            or board in rule.review_disabled_for
        ):
            continue
        effective = _effective_target(rule, board)
        if rule.kind == "file":
            if effective == target:
                modes.add(rule.mode)
            continue
        if rule.kind != "tree" or not target.startswith(effective + "/"):
            continue
        relative = target[len(effective) + 1:]
        if _selected_by_tree(rule, relative, board):
            modes.add(rule.mode)
    return modes


def _validate_device_mode_consistency(layout: SdLayout) -> None:
    """Reject host mappings whose effective device mode differs by board.

    The compiled device matcher is intentionally source- and scope-independent.
    An exact Pi-4 override must therefore not silently reclassify a Pi-5 tree
    member that is still staged with another mode.
    """

    # This also validates matcher representability and same-key conflicts.
    projection = device_projection_document(layout)
    exact_targets: set[str] = set()
    for rule in layout.rules:
        if rule.kind != "file" or "*" in rule.target or "?" in rule.target:
            continue
        exact_targets.add(rule.target)
        if rule.review_move_pi4 is not None:
            exact_targets.add(rule.review_move_pi4)
        if rule.review_move_pi5 is not None:
            exact_targets.add(rule.review_move_pi5)
    for board in BOARDS:
        for target in exact_targets:
            staged_modes = _staged_modes_for_target(layout, board, target)
            if len(staged_modes) > 1:
                _fail(
                    f"board-inconsistent staged modes for {board} target {target}: "
                    f"{', '.join(sorted(staged_modes))}"
                )
            if not staged_modes:
                continue
            staged_mode = next(iter(staged_modes))
            device_mode = _device_mode_from_projection(layout, projection, target)
            if device_mode != staged_mode:
                _fail(
                    f"board-inconsistent device mode for {board} target {target}: "
                    f"staged as {staged_mode}, device mode is "
                    f"{device_mode or 'reject'}"
                )


def _validated_layout(layout: SdLayout) -> SdLayout:
    _validate_static_layout(layout)
    _validate_device_mode_consistency(layout)
    return layout


def _ensure_review_move_allowed(
    layout: SdLayout,
    rule: LayoutRule,
    board: str,
    target: str,
) -> None:
    actual_mode = _device_mode_for_path(layout, target)
    if actual_mode != rule.mode:
        _fail(
            f"review move target is not accepted as mode {rule.mode}: {target} "
            f"(device mode: {actual_mode or 'reject'})"
        )
    key = target.lower()
    for other in layout.rules:
        if other is rule or other.source is None or other.scope not in {"both", board}:
            continue
        if other.kind == "file" and _effective_target(other, board).lower() == key:
            _fail(f"review move target collides with another staged file: {target}")
        if other.kind == "directory" and _effective_target(other, board).lower() == key:
            _fail(f"review move target collides with a staged directory: {target}")
        if other.kind == "tree":
            root = _effective_target(other, board)
            if target.startswith(root + "/"):
                relative = target[len(root) + 1:]
                if _selected_by_tree(other, relative, board):
                    _fail(
                        f"review move target overlaps staged tree "
                        f"{other.provenance}: {target}"
                    )


def _replace_rule(layout: SdLayout, old: LayoutRule, new: LayoutRule) -> SdLayout:
    return dataclasses.replace(
        layout,
        rules=tuple(new if rule is old else rule for rule in layout.rules),
    )


def _stage_rule_candidates(layout: SdLayout, scope: str, target: str) -> list[LayoutRule]:
    if scope not in SCOPES:
        _fail(f"invalid scope: {scope}")
    safe = _fat_path(target, "stage override target")
    candidates: list[LayoutRule] = []
    for rule in layout.rules:
        allowed_scopes = {"both"} if scope == "both" else {"both", scope}
        if rule.scope not in allowed_scopes or rule.source is None:
            continue
        effective = _effective_target(rule, scope) if scope in BOARDS else rule.target
        if rule.kind == "file" and effective == safe:
            candidates.append(rule)
        elif rule.kind == "tree" and safe.startswith(rule.target + "/"):
            relative = safe[len(rule.target) + 1:]
            if _selected_by_tree(rule, relative, scope, ignore_excludes=True):
                candidates.append(rule)
    # An explicit file rule is the stable representation of a single-member
    # override carved out of a tree.  Prefer it over the underlying tree so
    # later enable/mode edits remain unambiguous.
    exact = [rule for rule in candidates if rule.kind == "file"]
    return exact or candidates


def _next_rule_ordinal(layout: SdLayout, scope: str, kind: str) -> int:
    ordinals = [
        rule.ordinal for rule in layout.rules
        if rule.scope == scope and rule.kind == kind
    ]
    return max(ordinals, default=-1) + 1


def _append_layout_rule(layout: SdLayout, added: LayoutRule) -> SdLayout:
    scope_rank = {scope: index for index, scope in enumerate(SCOPES)}
    kind_rank = {kind: index for index, kind in enumerate(("file", "tree", "directory"))}
    return dataclasses.replace(
        layout,
        rules=tuple(sorted(
            (*layout.rules, added),
            key=lambda rule: (
                scope_rank[rule.scope], kind_rank[rule.kind], rule.ordinal,
            ),
        )),
    )


def with_stage_path_mode(
    layout: SdLayout, scope: str, target: str, mode: str,
) -> SdLayout:
    """Change one exact effective target without widening a source tree.

    A tree member becomes an explicit file mapping and is excluded from its
    former tree rule.  Existing ``both`` mappings must be edited as ``both``;
    the source-independent device matcher cannot represent board-local modes
    for the same SD path.
    """

    if mode not in MODES:
        _fail(f"unknown stage mode: {mode}")
    safe = _fat_path(target, "stage mode target")
    candidates = _stage_rule_candidates(layout, scope, safe)
    if len(candidates) != 1:
        _fail(f"stage target {target} has {len(candidates)} editable source rules")
    rule = candidates[0]
    if scope != rule.scope:
        _fail(
            f"stage target {target} is declared in scope {rule.scope}; "
            f"change its mode using that scope"
        )
    if rule.kind == "file":
        if rule.mode == mode:
            _fail(f"stage target {target} already uses mode {mode}")
        return _validated_layout(
            _replace_rule(layout, rule, dataclasses.replace(rule, mode=mode))
        )

    assert rule.kind == "tree" and rule.source is not None
    if not safe.startswith(rule.target + "/"):
        _fail(f"stage target is not below editable tree {rule.provenance}: {safe}")
    relative = safe[len(rule.target) + 1:]
    if rule.mode == mode and not any(
        _matches_glob(relative, pattern) for pattern in rule.exclude
    ):
        _fail(f"stage target {target} already uses mode {mode}")
    source = _source_reference(
        f"{rule.source}/{relative}", "tree member source", layout.source_map,
    )
    exclusions = (
        rule.exclude
        if any(_matches_glob(relative, pattern) for pattern in rule.exclude)
        else tuple(sorted({*rule.exclude, relative}))
    )
    changed_tree = dataclasses.replace(rule, exclude=exclusions)
    disabled_for = set(rule.review_disabled_for)
    if any(_matches_glob(relative, item) for item in rule.review_exclude_pi4):
        disabled_for.add("pi4")
    if any(_matches_glob(relative, item) for item in rule.review_exclude_pi5):
        disabled_for.add("pi5")
    explicit = LayoutRule(
        scope=rule.scope,
        kind="file",
        ordinal=_next_rule_ordinal(layout, rule.scope, "file"),
        target=safe,
        mode=mode,
        source=source,
        profiles=rule.profiles,
        features=rule.features,
        optional=rule.optional,
        enabled=rule.enabled,
        report_missing=rule.report_missing,
        review_disabled_for=tuple(sorted(disabled_for)),
    )
    updated = _replace_rule(layout, rule, changed_tree)
    updated = _append_layout_rule(updated, explicit)
    return _validated_layout(updated)


def with_new_stage_mapping(
    layout: SdLayout,
    scope: str,
    source_reference: str,
    target: str,
    mode: str,
) -> SdLayout:
    """Add one explicit source-to-target mapping to the canonical layout."""

    if scope not in SCOPES:
        _fail(f"invalid scope: {scope}")
    if mode not in MODES:
        _fail(f"unknown stage mode: {mode}")
    source = _source_reference(
        source_reference, "new stage mapping source", layout.source_map,
    )
    safe = _fat_path(target, "new stage mapping target")
    boards = BOARDS if scope == "both" else (scope,)
    for board in boards:
        candidates = _stage_rule_candidates(layout, board, safe)
        if candidates:
            _fail(
                f"new stage target {safe} already has an editable source rule "
                f"for {board}: {candidates[0].provenance}"
            )
    explicit = LayoutRule(
        scope=scope,
        kind="file",
        ordinal=_next_rule_ordinal(layout, scope, "file"),
        target=safe,
        mode=mode,
        source=source,
    )
    updated = _append_layout_rule(layout, explicit)
    return _validated_layout(updated)


def with_stage_path_enabled(
    layout: SdLayout, scope: str, target: str, enabled: bool,
) -> SdLayout:
    """Enable/disable one exact staged target without changing device policy."""

    candidates = _stage_rule_candidates(layout, scope, target)
    if len(candidates) != 1:
        _fail(f"stage target {target} has {len(candidates)} editable source rules")
    rule = candidates[0]
    if rule.kind == "file":
        if scope in BOARDS and rule.scope == "both":
            disabled = set(rule.review_disabled_for)
            (disabled.discard if enabled else disabled.add)(scope)
            return _replace_rule(
                layout, rule,
                dataclasses.replace(
                    rule, review_disabled_for=tuple(sorted(disabled)),
                ),
            )
        return _replace_rule(layout, rule, dataclasses.replace(rule, enabled=enabled))
    relative = target[len(rule.target) + 1:]
    board_specific = scope in BOARDS and rule.scope == "both"
    if board_specific:
        excludes = set(
            rule.review_exclude_pi4 if scope == "pi4"
            else rule.review_exclude_pi5
        )
    else:
        excludes = set(rule.exclude)
    if enabled:
        excludes.discard(relative)
    else:
        excludes.add(relative)
    if board_specific:
        field = (
            {"review_exclude_pi4": tuple(sorted(excludes))}
            if scope == "pi4"
            else {"review_exclude_pi5": tuple(sorted(excludes))}
        )
        return _replace_rule(layout, rule, dataclasses.replace(rule, **field))
    return _replace_rule(
        layout, rule, dataclasses.replace(rule, exclude=tuple(sorted(excludes))),
    )


def with_stage_path_moved(
    layout: SdLayout, scope: str, target: str, new_target: str,
) -> SdLayout:
    """Move one staged file without changing the compiled device policy.

    A member supplied by a tree is carved out as one same-mode exact file
    rule.  The carve-out is board-local when the requested move is
    board-local, so the other board continues to receive the member from the
    original tree.  Exact same-mode matchers are normalized out of the device
    projection and therefore do not force a target rebuild.
    """

    candidates = _stage_rule_candidates(layout, scope, target)
    if len(candidates) != 1:
        _fail(f"stage target {target} has {len(candidates)} editable source rules")
    rule = candidates[0]
    safe = _fat_path(new_target, "new stage target")
    current = _fat_path(target, "stage move target")
    if safe == current:
        _fail(f"new stage target is unchanged: {safe}")

    if rule.kind == "tree":
        if not current.startswith(rule.target + "/") or rule.source is None:
            _fail(f"stage target is not below editable tree {rule.provenance}: {current}")
        relative = current[len(rule.target) + 1:]
        if not rule.enabled or (
            scope in BOARDS and scope in rule.review_disabled_for
        ):
            _fail(f"tree member is not active for {scope}: {current}")
        if not _selected_by_tree(rule, relative, scope if scope in BOARDS else None):
            _fail(f"tree member is not active for {scope}: {current}")

        source = _source_reference(
            f"{rule.source}/{relative}", "tree member source", layout.source_map,
        )
        if scope in BOARDS and rule.scope == "both":
            field = (
                "review_exclude_pi4" if scope == "pi4"
                else "review_exclude_pi5"
            )
            exclusions = tuple(sorted({*getattr(rule, field), relative}))
            changed_tree = dataclasses.replace(rule, **{field: exclusions})
            explicit_scope = scope
            disabled_for: tuple[str, ...] = ()
        else:
            if scope != rule.scope:
                _fail(
                    f"tree member {current} is declared in scope {rule.scope}; "
                    f"move it using that scope"
                )
            exclusions = tuple(sorted({*rule.exclude, relative}))
            changed_tree = dataclasses.replace(rule, exclude=exclusions)
            explicit_scope = rule.scope
            disabled_for = rule.review_disabled_for

        explicit = LayoutRule(
            scope=explicit_scope,
            kind="file",
            ordinal=_next_rule_ordinal(layout, explicit_scope, "file"),
            target=current,
            mode=rule.mode,
            source=source,
            profiles=rule.profiles,
            features=rule.features,
            optional=rule.optional,
            enabled=rule.enabled,
            report_missing=rule.report_missing,
            review_disabled_for=disabled_for,
        )
        layout = _append_layout_rule(
            _replace_rule(layout, rule, changed_tree), explicit,
        )
        rule = explicit

    if rule.kind != "file":
        _fail(f"stage target is not a movable file: {target}")
    updates: dict[str, object] = {}
    boards = BOARDS if scope == "both" else (scope,)
    for board in boards:
        _ensure_review_move_allowed(layout, rule, board, safe)
        field = f"review_move_{board}"
        updates[field] = None if safe == rule.target else safe
    return _replace_rule(layout, rule, dataclasses.replace(rule, **updates))


def restore_stage_path_move(
    layout: SdLayout, scope: str, target: str,
) -> SdLayout:
    """Restore a previously moved exact file rule to its original target."""

    candidates = _stage_rule_candidates(layout, scope, target)
    if len(candidates) != 1:
        _fail(f"stage target {target} has {len(candidates)} editable source rules")
    rule = candidates[0]
    if rule.kind != "file":
        _fail(f"stage target is not a reversible moved file: {target}")
    boards = BOARDS if scope == "both" else (scope,)
    updates: dict[str, object] = {}
    for board in boards:
        field = f"review_move_{board}"
        if getattr(rule, field) is None:
            _fail(f"stage target is not moved for {board}: {target}")
        updates[field] = None
    return _replace_rule(layout, rule, dataclasses.replace(rule, **updates))


def stage_override_records(layout: SdLayout) -> tuple[StageOverride, ...]:
    result: list[StageOverride] = []
    for rule in layout.rules:
        if not rule.enabled:
            disabled_target = (
                _effective_target(rule, rule.scope)
                if rule.scope in BOARDS else rule.target
            )
            result.append(StageOverride(
                "disabled", rule.scope, disabled_target, rule.provenance,
            ))
        for board in rule.review_disabled_for:
            result.append(StageOverride(
                "disabled", board, _effective_target(rule, board), rule.provenance,
            ))
        for pattern in rule.exclude:
            result.append(StageOverride(
                "excluded", rule.scope, f"{rule.target}/{pattern}",
                rule.provenance,
            ))
        for board, patterns in (
            ("pi4", rule.review_exclude_pi4),
            ("pi5", rule.review_exclude_pi5),
        ):
            for pattern in patterns:
                result.append(StageOverride(
                    "excluded", board, f"{rule.target}/{pattern}",
                    rule.provenance,
                ))
        for board, moved in (
            ("pi4", rule.review_move_pi4),
            ("pi5", rule.review_move_pi5),
        ):
            if moved is not None:
                result.append(StageOverride(
                    "moved", board, moved, rule.provenance,
                    original_target=rule.target,
                ))
    return tuple(result)


def stage_overrides(layout: SdLayout) -> tuple[str, ...]:
    rendered: list[str] = []
    for override in stage_override_records(layout):
        if override.kind == "moved":
            rendered.append(
                f"{override.provenance} [{override.scope}]: move "
                f"{override.original_target} -> "
                f"{override.target}"
            )
        else:
            rendered.append(
                f"{override.provenance} [{override.scope}]: "
                f"{override.kind} {override.target}"
            )
    return tuple(rendered)


def _quote(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def _array(values: Sequence[str]) -> str:
    return "[" + ", ".join(_quote(value) for value in values) + "]"


def _render_rule(rule: LayoutRule) -> list[str]:
    lines = [f"[[{rule.scope}.{rule.kind}]]"]
    if rule.source is not None:
        lines.append(f"from = {_quote(rule.source)}")
    lines.append(f"to = {_quote(rule.target)}")
    lines.append(f"mode = {_quote(rule.mode)}")
    if rule.include:
        lines.append(f"include = {_array(rule.include)}")
    if rule.exclude:
        lines.append(f"exclude = {_array(rule.exclude)}")
    if rule.profiles:
        lines.append(f"profiles = {_array(rule.profiles)}")
    if rule.features:
        lines.append(f"features = {_array(rule.features)}")
    if rule.optional:
        lines.append("optional = true")
    if not rule.enabled:
        lines.append("enabled = false")
    if not rule.report_missing:
        lines.append("report_missing = false")
    if rule.review_disabled_for:
        lines.append(f"review_disabled_for = {_array(rule.review_disabled_for)}")
    if rule.review_exclude_pi4:
        lines.append(f"review_exclude_pi4 = {_array(rule.review_exclude_pi4)}")
    if rule.review_exclude_pi5:
        lines.append(f"review_exclude_pi5 = {_array(rule.review_exclude_pi5)}")
    if rule.review_move_pi4 is not None:
        lines.append(f"review_move_pi4 = {_quote(rule.review_move_pi4)}")
    if rule.review_move_pi5 is not None:
        lines.append(f"review_move_pi5 = {_quote(rule.review_move_pi5)}")
    return lines


def render_sd_layout(layout: SdLayout) -> str:
    """Render deterministically; comments are intentionally not policy state."""

    lines = [f"format = {_quote(FORMAT)}", 'default = "deny"', "", "[sources]"]
    for name, template in layout.sources:
        lines.append(f"{name} = {_quote(template)}")
    for scope in SCOPES:
        for kind in ("file", "tree", "directory"):
            for rule in (item for item in layout.rules if item.scope == scope and item.kind == kind):
                lines.extend(["", *_render_rule(rule)])
    return "\n".join(lines) + "\n"


_RULE_HEADER_RE = re.compile(r"^\[\[(both|pi4|pi5)\.(file|tree|directory)\]\]$")
_MUTABLE_KEY_RE = re.compile(
    r"^\s*(?:exclude|enabled|review_disabled_for|review_exclude_pi4|"
    r"review_exclude_pi5|review_move_pi4|review_move_pi5)\s*="
)
_MODE_LINE_RE = re.compile(
    r'^(?P<prefix>\s*mode\s*=\s*)(?P<quote>["\'])'
    r'(?:replace|kernel|config|metadata|keep)(?P=quote)'
    r'(?P<suffix>\s*(?:#.*)?)$'
)


def _toml_inline_comment(line: str) -> str | None:
    """Return a TOML comment without mistaking ``#`` inside a basic string."""

    quoted = False
    escaped = False
    for index, character in enumerate(line):
        if escaped:
            escaped = False
            continue
        if quoted and character == "\\":
            escaped = True
            continue
        if character == '"':
            quoted = not quoted
            continue
        if character == "#" and not quoted:
            return line[index:]
    return None


def _review_neutral(rule: LayoutRule) -> LayoutRule:
    return dataclasses.replace(
        rule,
        exclude=(),
        enabled=True,
        review_disabled_for=(),
        review_exclude_pi4=(),
        review_exclude_pi5=(),
        review_move_pi4=None,
        review_move_pi5=None,
    )


def _surgical_neutral(rule: LayoutRule) -> LayoutRule:
    return dataclasses.replace(_review_neutral(rule), mode="replace")


def _review_lines(rule: LayoutRule) -> list[str]:
    result: list[str] = []
    if rule.exclude:
        result.append(f"exclude = {_array(rule.exclude)}")
    if not rule.enabled:
        result.append("enabled = false")
    if rule.review_disabled_for:
        result.append(f"review_disabled_for = {_array(rule.review_disabled_for)}")
    if rule.review_exclude_pi4:
        result.append(f"review_exclude_pi4 = {_array(rule.review_exclude_pi4)}")
    if rule.review_exclude_pi5:
        result.append(f"review_exclude_pi5 = {_array(rule.review_exclude_pi5)}")
    if rule.review_move_pi4 is not None:
        result.append(f"review_move_pi4 = {_quote(rule.review_move_pi4)}")
    if rule.review_move_pi5 is not None:
        result.append(f"review_move_pi5 = {_quote(rule.review_move_pi5)}")
    return result


def _surgical_review_render(
    original_text: str,
    original: SdLayout,
    updated: SdLayout,
) -> str | None:
    """Preserve comments/formatting for review fields, modes and new files."""

    if original.sources != updated.sources:
        return None
    original_rules = {
        (rule.scope, rule.kind, rule.ordinal): rule for rule in original.rules
    }
    updated_rules = {
        (rule.scope, rule.kind, rule.ordinal): rule for rule in updated.rules
    }
    if len(original_rules) != len(original.rules) or len(updated_rules) != len(updated.rules):
        return None
    if not set(original_rules).issubset(updated_rules):
        return None
    changed: dict[tuple[str, str, int], tuple[LayoutRule, LayoutRule]] = {}
    for key, before in original_rules.items():
        after = updated_rules[key]
        if _surgical_neutral(before) != _surgical_neutral(after):
            return None
        if before != after:
            changed[key] = (before, after)
    added = [
        rule for key, rule in updated_rules.items() if key not in original_rules
    ]
    if any(rule.kind != "file" for rule in added):
        return None
    for scope in SCOPES:
        old_count = sum(
            rule.scope == scope and rule.kind == "file" for rule in original.rules
        )
        ordinals = sorted(
            rule.ordinal for rule in added
            if rule.scope == scope and rule.kind == "file"
        )
        if ordinals != list(range(old_count, old_count + len(ordinals))):
            return None
    if not changed and not added:
        return original_text
    lines = original_text.splitlines()
    headers: list[tuple[int, str, str, int]] = []
    counters: dict[tuple[str, str], int] = {}
    for index, line in enumerate(lines):
        match = _RULE_HEADER_RE.fullmatch(line)
        if match is None:
            continue
        scope, kind = match.groups()
        key = (scope, kind)
        ordinal = counters.get(key, 0)
        counters[key] = ordinal + 1
        headers.append((index, scope, kind, ordinal))
    if len(headers) != len(original.rules):
        return None
    for header_index in range(len(headers) - 1, -1, -1):
        start, scope, kind, ordinal = headers[header_index]
        change = changed.get((scope, kind, ordinal))
        if change is None:
            continue
        before, rule = change
        end = headers[header_index + 1][0] if header_index + 1 < len(headers) else len(lines)
        body = lines[start + 1:end]
        cleaned: list[str] = []
        preserved_comments: list[str] = []
        skipping_array = False
        changed_mode_lines = 0
        for line in body:
            if skipping_array:
                comment = _toml_inline_comment(line)
                if comment is not None:
                    preserved_comments.append(comment)
                if "]" in line:
                    skipping_array = False
                continue
            if _MUTABLE_KEY_RE.match(line):
                comment = _toml_inline_comment(line)
                if comment is not None:
                    preserved_comments.append(comment)
                if "[" in line and "]" not in line:
                    skipping_array = True
                continue
            if before.mode != rule.mode:
                mode_match = _MODE_LINE_RE.fullmatch(line)
                if mode_match is not None:
                    line = (
                        f"{mode_match.group('prefix')}"
                        f"{mode_match.group('quote')}{rule.mode}"
                        f"{mode_match.group('quote')}"
                        f"{mode_match.group('suffix')}"
                    )
                    changed_mode_lines += 1
            cleaned.append(line)
        if before.mode != rule.mode and changed_mode_lines != 1:
            return None
        lines[start + 1:end] = [
            *_review_lines(rule), *preserved_comments, *cleaned,
        ]

    if added:
        # Existing file groups are non-empty in every valid BMX layout (kernel
        # requirements guarantee the board groups).  Insert before the trailing
        # blank/comment block so the following section's commentary stays with
        # that following section.
        insertions: list[tuple[int, list[str]]] = []
        rescanned: list[tuple[int, str, str]] = []
        for index, line in enumerate(lines):
            match = _RULE_HEADER_RE.fullmatch(line)
            if match is not None:
                rescanned.append((index, *match.groups()))
        for scope in SCOPES:
            rules = sorted(
                (
                    rule for rule in added
                    if rule.scope == scope and rule.kind == "file"
                ),
                key=lambda rule: rule.ordinal,
            )
            if not rules:
                continue
            matching_indices = [
                index for index, item_scope, item_kind in rescanned
                if item_scope == scope and item_kind == "file"
            ]
            if not matching_indices:
                return None
            last_header = matching_indices[-1]
            later_headers = [index for index, _scope, _kind in rescanned if index > last_header]
            boundary = min(later_headers, default=len(lines))
            insertion = boundary
            while (
                insertion > last_header + 1
                and (
                    not lines[insertion - 1].strip()
                    or lines[insertion - 1].lstrip().startswith("#")
                )
            ):
                insertion -= 1
            block: list[str] = []
            for rule in rules:
                block.extend(["", *_render_rule(rule)])
            insertions.append((insertion, block))
        for insertion, block in sorted(insertions, reverse=True):
            lines[insertion:insertion] = block
    return "\n".join(lines) + "\n"


def save_sd_layout(layout: SdLayout, path: str | Path | None = None) -> None:
    _validated_layout(layout)
    destination = Path(path) if path is not None else layout.path
    rendered: str | None = None
    if destination.is_file() and not destination.is_symlink():
        try:
            original_text = destination.read_text(encoding="utf-8")
            original = load_sd_layout(destination)
            rendered = _surgical_review_render(original_text, original, layout)
        except (OSError, UnicodeError, SdLayoutError):
            rendered = None
    encoded = (rendered if rendered is not None else render_sd_layout(layout)).encode("utf-8")
    temporary = destination.with_name(f".{destination.name}.tmp-{os.getpid()}")
    try:
        with temporary.open("xb") as handle:
            handle.write(encoded)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temporary, 0o644)
        candidate = load_sd_layout(temporary)
        if candidate.sources != layout.sources or candidate.rules != layout.rules:
            _fail("rendered SD layout does not match the requested semantics")
        # Loading already validates the complete projection.  Compute its hash
        # once more here so this pre-replace contract remains explicit even if
        # load-time validation is refactored later.
        device_projection_sha256(candidate)
        os.replace(temporary, destination)
        try:
            descriptor = os.open(destination.parent, os.O_RDONLY)
        except OSError:
            descriptor = -1
        if descriptor >= 0:
            try:
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
    finally:
        temporary.unlink(missing_ok=True)


def _parse_bindings(values: Sequence[str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for value in values:
        name, separator, path = value.partition("=")
        if not separator or not _ALIAS_RE.fullmatch(name) or not path:
            _fail(f"invalid --source value (expected name=/absolute/path): {value}")
        if name in result:
            _fail(f"duplicate --source value: {name}")
        result[name] = path
    return result


def _main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--layout", type=Path, default=DEFAULT_LAYOUT_PATH)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("check")
    sub.add_parser("device-sha256")
    machines = sub.add_parser("kernel-machines")
    machines.add_argument("--board", choices=BOARDS, required=True)
    machines.add_argument("--include-optional", action="store_true")
    digest = sub.add_parser("tree-digest")
    digest.add_argument("--tree", type=Path, required=True)
    assemble = sub.add_parser("assemble")
    assemble.add_argument("--board", choices=BOARDS, required=True)
    assemble.add_argument("--profile", choices=PROFILES, default="release")
    assemble.add_argument("--feature", action="append", default=[])
    assemble.add_argument("--source", action="append", default=[])
    assemble.add_argument("--destination", type=Path, required=True)
    assemble.add_argument("--replace", action="store_true")
    args = parser.parse_args(argv)
    try:
        layout = load_sd_layout(args.layout)
        if args.action == "device-sha256":
            print(device_projection_sha256(layout))
        elif args.action == "kernel-machines":
            for machine in kernel_machines(
                layout, args.board, include_optional=args.include_optional,
            ):
                print(machine)
        elif args.action == "tree-digest":
            print(tree_digest(args.tree))
        elif args.action == "assemble":
            result = assemble_stage(
                layout, args.board, _parse_bindings(args.source), args.destination,
                args.profile, args.feature, replace=args.replace,
            )
            print(
                f"assembled {args.board} stage: {len(result.entries)} files, "
                f"{len(result.directories)} directories"
            )
        return 0
    except (OSError, SdLayoutError, UnicodeError) as exc:
        print(f"sd_layout.py: error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(_main())
