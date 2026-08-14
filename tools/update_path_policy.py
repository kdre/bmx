#!/usr/bin/env python3
"""Compile the canonical SD-layout TOML into the BMX update path policy."""

from __future__ import annotations

import dataclasses
import functools
from pathlib import Path

from sd_layout import (
    BOARDS,
    DEFAULT_LAYOUT_PATH,
    LayoutRule,
    SdLayout,
    SdLayoutError,
    device_projection_document,
    device_projection_sha256,
    load_sd_layout,
    validate_fat_path,
)


DEFAULT_POLICY_PATH = DEFAULT_LAYOUT_PATH
POLICY_NAMES = frozenset(
    {"config-template", "kernel", "managed-replace", "metadata", "preserve"}
)
LEGACY_MACHINE_KERNEL_BASES = ("kernel7l.img",)
_MODE_POLICY = {
    "config": "config-template",
    "kernel": "kernel",
    "keep": "preserve",
    "metadata": "metadata",
    "replace": "managed-replace",
}


class PolicySpecError(ValueError):
    """The SD layout cannot be represented by the target v1 matcher."""


@dataclasses.dataclass(frozen=True)
class CompiledRule:
    kind: str
    policy: str | None
    root: str | None = None
    value: str | None = None
    suffix: str | None = None


@dataclasses.dataclass(frozen=True)
class MachineKernelMatch:
    base: str
    machine: str


@dataclasses.dataclass(frozen=True)
class UpdatePathPolicy:
    source_path: Path
    source_sha256: str
    rules: tuple[CompiledRule, ...]
    directory_roots: tuple[str, ...]
    kernel_bases: tuple[str, ...]
    kernel_board_families: tuple[str, ...]
    kernel_machines: tuple[str, ...]
    required_kernel_machines: tuple[str, ...]
    kernel_policy: str


def _error(message: str) -> None:
    raise PolicySpecError(message)


def _kernel_inventory(
    layout: SdLayout,
) -> tuple[tuple[str, ...], tuple[str, ...], tuple[str, ...]]:
    bases: list[str] = []
    board_machines: dict[str, list[tuple[str, bool]]] = {}
    for board in BOARDS:
        found: list[tuple[str, bool]] = []
        base: str | None = None
        for rule in layout.rules:
            if (
                rule.scope not in {"both", board}
                or rule.kind != "file"
                or rule.mode != "kernel"
            ):
                continue
            try:
                candidate_base, machine = rule.target.rsplit(".", 1)
            except ValueError:
                _error(f"invalid kernel target: {rule.target}")
            if base is None:
                base = candidate_base
            elif candidate_base != base:
                _error(f"{board} kernel targets use multiple bases")
            pair = (machine, not rule.optional)
            if pair not in found:
                found.append(pair)
        if base is None:
            _error(f"{board} has no kernel targets")
        bases.append(base)
        board_machines[board] = found
    pi4 = board_machines["pi4"]
    pi5 = board_machines["pi5"]
    if pi4 != pi5:
        _error("pi4 and pi5 kernel machine inventories differ")
    required = tuple(machine for machine, is_required in pi4 if is_required)
    optional = tuple(machine for machine, is_required in pi4 if not is_required)
    machines = required + optional
    if len(required) < 7 or "c64" not in required:
        _error("kernel inventory must contain at least seven required machines including c64")
    if len(set(machines)) != len(machines):
        _error("kernel inventory contains a duplicate machine")
    return tuple(bases), machines, required


def _split_star(pattern: str, where: str) -> tuple[str, str]:
    if "?" in pattern or pattern.count("*") != 1:
        _error(f"{where} cannot be represented by the target matcher: {pattern}")
    return tuple(pattern.split("*", 1))  # type: ignore[return-value]


def _compile_file(rule: LayoutRule) -> CompiledRule:
    policy = _MODE_POLICY[rule.mode]
    target = rule.target
    basename_anywhere = target.startswith("**/")
    pattern = target[3:] if basename_anywhere else target
    if "*" not in pattern and "?" not in pattern:
        return CompiledRule(
            kind="basename-exact" if basename_anywhere else "exact-path",
            policy=policy,
            value=pattern,
        )
    if "/" in pattern:
        _error(f"file pattern has an unsupported directory component: {target}")
    prefix, suffix = _split_star(pattern, rule.provenance)
    return CompiledRule(
        kind="basename-prefix-suffix",
        policy=policy,
        value=prefix,
        suffix=suffix,
    )


def _compile_tree(rule: LayoutRule) -> tuple[CompiledRule, ...]:
    policy = _MODE_POLICY[rule.mode]
    root = rule.target.split("/", 1)[0]
    if not rule.include:
        return (CompiledRule(kind="top-root", policy=policy, root=root),)
    result: list[CompiledRule] = []
    for pattern in rule.include:
        if "/" in pattern:
            _error(
                f"tree include cannot be represented by the target matcher in "
                f"{rule.provenance}: {pattern}"
            )
        if "*" not in pattern and "?" not in pattern:
            result.append(
                CompiledRule(
                    kind="basename-exact", policy=policy, root=root, value=pattern,
                )
            )
            continue
        prefix, suffix = _split_star(pattern, rule.provenance)
        kind = "basename-suffix" if not prefix else "basename-prefix-suffix"
        result.append(
            CompiledRule(
                kind=kind,
                policy=policy,
                root=root,
                value=prefix if prefix else None,
                suffix=suffix,
            )
        )
    return tuple(result)


def _rule_sort_key(rule: CompiledRule) -> tuple[int, str, str, str, str]:
    # Specific rules must precede broad top-root keep rules.  The target uses
    # first-match semantics, just like the host classifier below.
    rank = {
        "machine-kernel": 0,
        "exact-path": 1,
        "basename-exact": 2,
        "basename-prefix-suffix": 3,
        "basename-suffix": 4,
        "top-root": 5,
    }[rule.kind]
    return (
        rank,
        rule.root or "",
        rule.value or "",
        rule.suffix or "",
        rule.policy or "",
    )


def _compile_layout(path: Path) -> UpdatePathPolicy:
    try:
        layout = load_sd_layout(path)
        projection = device_projection_document(layout)
        projection_sha256 = device_projection_sha256(layout)
    except SdLayoutError as exc:
        raise PolicySpecError(str(exc)) from exc
    bases, machines, required = _kernel_inventory(layout)
    compiled: list[CompiledRule] = [
        CompiledRule(kind="machine-kernel", policy=None),
    ]
    raw_projection_rules = projection["rules"]
    if not isinstance(raw_projection_rules, list):
        _error("device projection rules are invalid")
    for item in raw_projection_rules:
        if not isinstance(item, dict):
            _error("device projection contains a non-object rule")
        mode = item.get("mode")
        if not isinstance(mode, str) or mode not in _MODE_POLICY:
            _error("device projection contains an invalid mode")
        compiled.append(CompiledRule(
            kind=str(item["kind"]),
            policy=_MODE_POLICY[mode],
            root=item.get("root") if isinstance(item.get("root"), str) else None,
            value=item.get("value") if isinstance(item.get("value"), str) else None,
            suffix=item.get("suffix") if isinstance(item.get("suffix"), str) else None,
        ))

    by_matcher: dict[tuple[str, str | None, str | None, str | None], CompiledRule] = {}
    for rule in compiled:
        key = (rule.kind, rule.root, rule.value, rule.suffix)
        previous = by_matcher.get(key)
        if previous is not None:
            if previous.policy != rule.policy:
                _error(
                    f"one device matcher has conflicting modes: {key}: "
                    f"{previous.policy} and {rule.policy}"
                )
            continue
        by_matcher[key] = rule
    rules = tuple(sorted(by_matcher.values(), key=_rule_sort_key))
    raw_directories = projection["directories"]
    if not isinstance(raw_directories, list) or not all(
        isinstance(item, str) for item in raw_directories
    ):
        _error("device projection directories are invalid")
    directory_roots = tuple(raw_directories)
    if not directory_roots:
        _error("layout defines no allowed directory roots")
    return UpdatePathPolicy(
        source_path=layout.path,
        source_sha256=projection_sha256,
        rules=rules,
        directory_roots=directory_roots,
        kernel_bases=bases,
        kernel_board_families=BOARDS,
        kernel_machines=machines,
        required_kernel_machines=required,
        kernel_policy="kernel",
    )


@functools.lru_cache(maxsize=1)
def _load_default_policy() -> UpdatePathPolicy:
    return _compile_layout(DEFAULT_POLICY_PATH)


def load_update_path_policy(path: str | Path | None = None) -> UpdatePathPolicy:
    """Load the canonical layout and compile its source-independent policy."""

    return _load_default_policy() if path is None else _compile_layout(Path(path))


def machine_kernel_match(
    path: str, policy: UpdatePathPolicy | None = None, *,
    include_legacy: bool = False,
) -> MachineKernelMatch | None:
    policy = policy or load_update_path_policy()
    if not isinstance(path, str) or "/" in path:
        return None
    bases = (*policy.kernel_bases, *LEGACY_MACHINE_KERNEL_BASES) \
        if include_legacy else policy.kernel_bases
    for base in bases:
        prefix = f"{base}."
        if path.startswith(prefix):
            machine = path[len(prefix):]
            if machine in policy.kernel_machines:
                return MachineKernelMatch(base, machine)
    return None


def classify_bmx_path(
    path: str, policy: UpdatePathPolicy | None = None, *,
    include_legacy_kernels: bool = False,
) -> str | None:
    """Classify one archive file, returning ``None`` for unknown paths."""

    policy = policy or load_update_path_policy()
    if not isinstance(path, str) or not path or path.endswith("/"):
        return None
    try:
        validate_fat_path(path, "candidate file path")
    except SdLayoutError:
        return None
    basename = path.rsplit("/", 1)[-1]
    top = path.split("/", 1)[0]
    for rule in policy.rules:
        if rule.kind == "machine-kernel":
            if machine_kernel_match(
                path, policy, include_legacy=include_legacy_kernels
            ) is not None:
                return policy.kernel_policy
            continue
        if rule.root is not None and top != rule.root:
            continue
        if rule.kind == "exact-path":
            matched = path == rule.value
        elif rule.kind == "top-root":
            matched = True
        elif rule.kind == "basename-exact":
            matched = basename == rule.value
        elif rule.kind == "basename-suffix":
            matched = basename.endswith(rule.suffix or "")
        elif rule.kind == "basename-prefix-suffix":
            matched = basename.startswith(rule.value or "") and basename.endswith(
                rule.suffix or ""
            )
        else:
            matched = False
        if matched:
            return rule.policy
    return None


def is_allowed_bmx_directory(
    path: str, policy: UpdatePathPolicy | None = None,
) -> bool:
    """Return whether an explicit ZIP directory has a declared target root."""

    policy = policy or load_update_path_policy()
    if not isinstance(path, str) or not path:
        return False
    clean = path[:-1] if path.endswith("/") else path
    if not clean:
        return False
    try:
        validate_fat_path(clean, "candidate directory path")
    except SdLayoutError:
        return False
    return clean.split("/", 1)[0] in policy.directory_roots
