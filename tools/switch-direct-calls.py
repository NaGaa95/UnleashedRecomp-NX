#!/usr/bin/env python3
"""Let GCC inline calls between recompiled functions.

XenonRecomp (built with Clang, so with XENON_RECOMP_USE_ALIAS) emits every guest function as

    __attribute__((alias("__imp__sub_X"))) PPC_WEAK_FUNC(sub_X);
    PPC_FUNC_IMPL(__imp__sub_X) { ... }

and every call site as `sub_X(ctx, base);`. PPC_WEAK_FUNC is `weak, noinline` so that a hook in
the app (GUEST_FUNCTION_HOOK, PPC_FUNC overrides, ...) can replace sub_X at link time. The price
is that no call between recompiled functions can ever be inlined, not even inside one file and
not with LTO.

This post-pass rewrites `sub_X(ctx, base);` into `__imp__sub_X(ctx, base);` for every X that
has no hook. That is exactly the same function, but a normal one. Hooked functions keep going
through sub_X, and the indirect-call table (ppc_func_mapping.cpp) is never touched, so indirect
calls still reach hooks. Idea and numbers: nfsmw-nx tools/llamadas_directas.py (79,612 of
83,077 calls rewritten; with LTO and PGO, 30.3 -> ~34 FPS on that port).

An address counts as hooked if it appears anywhere in the app sources, the SWA API headers or
the recompiler configuration. That is more than strictly needed, and safe.

The pass is idempotent: it undoes its previous run (from ppc/direct_calls.txt) in memory and then
applies the current hook list, writing only files whose text changes, so adding a hook and
running it again is enough, and an unchanged run leaves every file (and Ninja) alone. Run it
after every XenonRecomp generation.

Usage:
    tools/switch-direct-calls.py                 rewrite (default)
    tools/switch-direct-calls.py --undo          restore the generated code
    tools/switch-direct-calls.py --verify-elf build/switch-app/UnleashedRecomp/UnleashedRecomp.elf [--nm aarch64-none-elf-nm]
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PPC_DIR = ROOT / "UnleashedRecompLib" / "ppc"
MANIFEST = PPC_DIR / "direct_calls.txt"
SHARED_HEADER = PPC_DIR / "ppc_recomp_shared.h"

# Functions the CPU profile found hot: their definitions become PPC_HOT_FUNC_IMPL (ppc_context.h), which
# GCC optimises harder and places together in .text.hot (better use of the instruction cache and TLB).
# Also idempotent; --no-hot (or SWITCH_HOT_FUNCTIONS=0) leaves every definition a plain one.
HOT_LIST = ROOT / "UnleashedRecompLib" / "config" / "hot_functions.txt"
PLAIN_DEFINITION = re.compile(r"^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\) \{$", re.MULTILINE)
HOT_DEFINITION = re.compile(r"^PPC_HOT_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\) \{$", re.MULTILINE)

HOOK_SOURCES = [
    ROOT / "UnleashedRecomp",
    ROOT / "UnleashedRecompLib" / "config",
]
HOOK_SUFFIXES = {".cpp", ".cc", ".c", ".h", ".hpp", ".inl", ".toml"}

# Guest code lives at 0x82000000-0x83FFFFFF in this game (e.g. sub_831B0B40).
ADDRESS = re.compile(r"(?<![0-9A-Fa-f])(8[23][0-9A-Fa-f]{6})(?![0-9A-Fa-f])")
CALL = re.compile(r"(?<![\w])sub_([0-9A-F]{8})\(ctx, base\);")
DIRECT_CALL = re.compile(r"(?<![\w])__imp__sub_([0-9A-F]{8})\(ctx, base\);")
DEFINITION = re.compile(r"PPC_(?:HOT_)?FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)\s*\{")

DECLARATIONS_BEGIN = "// BEGIN switch-direct-calls.py declarations (generated, do not edit)"
DECLARATIONS_END = "// END switch-direct-calls.py declarations"


def generated_sources() -> list[Path]:
    files = sorted(PPC_DIR.glob("ppc_recomp.*.cpp"), key=lambda p: int(p.name.split(".")[1]))
    if not files:
        sys.exit(f"No generated sources in {PPC_DIR}; run XenonRecomp first.")
    return files


def hooked_addresses() -> set[str]:
    hooked: set[str] = set()
    for base in HOOK_SOURCES:
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.is_file() and path.suffix in HOOK_SUFFIXES:
                text = path.read_text(encoding="utf-8", errors="ignore")
                hooked.update(match.upper() for match in ADDRESS.findall(text))
    return hooked


def read_manifest() -> set[str]:
    if not MANIFEST.exists():
        return set()
    return {line.strip() for line in MANIFEST.read_text().split() if line.strip()}


def write_if_changed(path: Path, text: str) -> bool:
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return False
    path.write_text(text, encoding="utf-8", newline="\n")
    return True


def strip_declarations(text: str) -> str:
    start = text.find(DECLARATIONS_BEGIN)
    if start < 0:
        return text
    # apply() separates the block with one blank line; take it out as well.
    if text[max(0, start - 2):start] == "\n\n":
        start -= 1
    end = text.find(DECLARATIONS_END, start)
    if end < 0:
        sys.exit(f"{SHARED_HEADER}: unterminated declaration block")
    end += len(DECLARATIONS_END)
    if end < len(text) and text[end] == "\n":
        end += 1
    return text[:start] + text[end:]


def restore_calls(text: str, previous: set[str]) -> tuple[str, int]:
    """The text with the calls rewritten by the previous run (the manifest) put back."""
    restored = 0

    def restore(match: re.Match) -> str:
        nonlocal restored
        if match.group(1) in previous:
            restored += 1
            return f"sub_{match.group(1)}(ctx, base);"
        return match.group(0)

    return DIRECT_CALL.sub(restore, text), restored


def read_hot_list() -> set[str]:
    if not HOT_LIST.exists():
        return set()
    hot: set[str] = set()
    for line in HOT_LIST.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            hot.add(line.upper())
    return hot


def mark_hot(text: str, hot: set[str]) -> tuple[str, int]:
    """The text with exactly the functions of `hot` defined as PPC_HOT_FUNC_IMPL."""
    marked = 0

    def plain(match: re.Match) -> str:
        return f"PPC_FUNC_IMPL(__imp__sub_{match.group(1)}) {{"

    def maybe_hot(match: re.Match) -> str:
        nonlocal marked
        if match.group(1) in hot:
            marked += 1
            return f"PPC_HOT_FUNC_IMPL(__imp__sub_{match.group(1)}) {{"
        return match.group(0)

    text = HOT_DEFINITION.sub(plain, text)
    return PLAIN_DEFINITION.sub(maybe_hot, text) if hot else text, marked


def undo(files: list[Path]) -> int:
    previous = read_manifest()
    restored = 0
    for path in files:
        text, count = restore_calls(path.read_text(encoding="utf-8"), previous)
        text, _ = mark_hot(text, set())
        restored += count
        write_if_changed(path, text)

    if SHARED_HEADER.exists():
        write_if_changed(SHARED_HEADER, strip_declarations(SHARED_HEADER.read_text(encoding="utf-8")))
    if MANIFEST.exists():
        MANIFEST.unlink()
    return restored


def apply(files: list[Path], hot: set[str]) -> None:
    sample = files[0].read_text(encoding="utf-8")
    if "__attribute__((alias(" not in sample:
        # Without alias mode the weak wrappers contain `__imp__sub_X(ctx, base);` themselves and
        # this pass could not tell them apart from rewritten call sites.
        sys.exit("Generated code does not use XENON_RECOMP_USE_ALIAS; refusing to rewrite.")

    # The previous run is undone in memory only: a file (or the shared header) whose final text
    # is what it already holds is not written, so Ninja does not recompile all the generated code
    # after every build. Undoing on disk first rewrote every file twice per run.
    previous = read_manifest()
    restored = 0

    hooked = hooked_addresses()
    defined: set[str] = set()
    texts = {}
    for path in files:
        text, count = restore_calls(path.read_text(encoding="utf-8"), previous)
        restored += count
        texts[path] = text
        defined.update(DEFINITION.findall(text))

    rewritten_targets: set[str] = set()
    calls = 0
    kept = 0
    changed_files = 0
    hot_marked = 0
    for path, text in texts.items():
        def rewrite(match: re.Match) -> str:
            nonlocal calls, kept
            address = match.group(1)
            if address in hooked or address not in defined:
                kept += 1
                return match.group(0)
            calls += 1
            rewritten_targets.add(address)
            return f"__imp__sub_{address}(ctx, base);"

        text, marked = mark_hot(CALL.sub(rewrite, text), hot)
        hot_marked += marked
        if write_if_changed(path, text):
            changed_files += 1

    # The shared header only declares sub_X; the rewritten calls need __imp__sub_X declared.
    header = strip_declarations(SHARED_HEADER.read_text(encoding="utf-8")).rstrip("\n") + "\n"
    if rewritten_targets:
        declarations = "".join(f"PPC_FUNC_IMPL(__imp__sub_{address});\n" for address in sorted(rewritten_targets))
        header += f"\n{DECLARATIONS_BEGIN}\n{declarations}{DECLARATIONS_END}\n"
    write_if_changed(SHARED_HEADER, header)
    write_if_changed(MANIFEST, "\n".join(sorted(rewritten_targets)) + "\n")

    print(f"switch-direct-calls: {calls} calls to {len(rewritten_targets)} functions made direct, "
          f"{kept} calls left through sub_X, {len(hooked)} hooked addresses respected, "
          f"{hot_marked} functions marked hot ({restored} previous rewrites undone first, {changed_files} files written).")


def verify_elf(elf: Path, nm: str) -> None:
    """Every sub_X in the final ELF that is not its own generated body is a hook; none may have been rewritten."""
    rewritten = read_manifest()
    if not rewritten:
        sys.exit("No manifest; nothing to verify.")
    output = subprocess.run([nm, str(elf)], check=True, capture_output=True, text=True).stdout
    # The guest functions are C++ functions, so their symbols are mangled (sub_82DFA2E8 is
    # _Z12sub_82DFA2E8R10PPCContextPh); plain names are accepted too. A generated sub_X is an alias
    # of __imp__sub_X, so it sits at the address of that body; a hook is a sub_X anywhere else.
    # The binding cannot tell them apart: the aliases are weak (W) in a normal link, but LTO turns
    # the ones that prevail into ordinary T symbols. __imp__sub_X may carry an LTO suffix
    # (__imp__sub_X.lto_priv.0), or be gone when a hook replaced it and nothing calls it.
    symbol = re.compile(r"^(?:_Z\d+)?sub_([0-9A-Fa-f]{8})(?![0-9A-Fa-f])")
    body = re.compile(r"^__imp__sub_([0-9A-Fa-f]{8})(?![0-9A-Fa-f])")
    functions: dict[str, set[int]] = {}
    bodies: dict[str, set[int]] = {}
    for line in output.splitlines():
        parts = line.split()
        if len(parts) != 3 or parts[1] not in ("T", "t", "W", "w"):
            continue
        match = body.match(parts[2])
        if match:
            bodies.setdefault(match.group(1).upper(), set()).add(int(parts[0], 16))
            continue
        match = symbol.match(parts[2])
        if match:
            functions.setdefault(match.group(1).upper(), set()).add(int(parts[0], 16))
    strong = {address for address, places in functions.items() if places - bodies.get(address, set())}
    if not strong:
        sys.exit("No strong sub_X symbols found in the ELF: the hook check would prove nothing.")
    bad = sorted(strong & rewritten)
    if bad:
        sys.exit("Hooked functions were called directly (hook bypassed): "
                 + ", ".join(f"sub_{address}" for address in bad[:20])
                 + (" ..." if len(bad) > 20 else ""))
    print(f"switch-direct-calls: OK, {len(strong)} hooks in the ELF, none of them bypassed.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--undo", action="store_true", help="restore the generated calls")
    parser.add_argument("--verify-elf", type=Path, help="check a linked ELF against the manifest")
    parser.add_argument("--nm", default="aarch64-none-elf-nm", help="nm to use with --verify-elf")
    parser.add_argument("--no-hot", action="store_true", help=f"do not mark the functions of {HOT_LIST.name} hot")
    args = parser.parse_args()

    if args.verify_elf:
        verify_elf(args.verify_elf, args.nm)
        return

    files = generated_sources()
    if args.undo:
        print(f"switch-direct-calls: {undo(files)} calls restored.")
    else:
        use_hot = not args.no_hot and os.environ.get("SWITCH_HOT_FUNCTIONS", "1") != "0"
        apply(files, read_hot_list() if use_hot else set())


if __name__ == "__main__":
    main()
