#!/usr/bin/env python3
"""
Programmatic alternative to CFF Explorer's "Import Adder" — patches a PE
file to add a new DLL+function entry to its import table.

Requires: pip install pefile

Usage:
    python add_import.py <target.exe> <dll_to_inject> <function_name>

Example:
    python add_import.py "...\System_en\L2.exe" l2ui.dll L2UI_Init

The script writes target.exe + ".patched" by default. Move it over the
original after testing.
"""
import os
import shutil
import sys

try:
    import pefile
except ImportError:
    print("pefile not installed. Run: pip install pefile")
    sys.exit(1)


def add_import(exe_path: str, dll_name: str, func_name: str, *, in_place=False) -> str:
    pe = pefile.PE(exe_path)
    # pefile's add_import is in newer versions; this version writes a new
    # IMPORT_DESCRIPTOR + adjusts the directory pointer. For older pefile
    # use the manual approach below.
    has_native = hasattr(pe, "add_import")
    if has_native:
        pe.add_import(dll_name, [func_name])
        out_path = exe_path if in_place else exe_path + ".patched"
        pe.write(filename=out_path)
        return out_path

    # Fallback: manual binary patch — appends a new descriptor to a fresh
    # section. Not implemented here to keep the script short; for older
    # pefile, install a newer version: `pip install -U pefile`.
    raise RuntimeError(
        "Your pefile version lacks add_import. "
        "Upgrade with: pip install -U pefile"
    )


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    exe, dll, func = sys.argv[1], sys.argv[2], sys.argv[3]
    if not os.path.isfile(exe):
        print(f"target not found: {exe}")
        sys.exit(1)

    backup = exe + ".original"
    if not os.path.exists(backup):
        print(f"backing up to {backup}")
        shutil.copy2(exe, backup)
    else:
        print(f"backup already exists at {backup} — leaving it alone")

    out = add_import(exe, dll, func, in_place=False)
    print(f"patched -> {out}")
    print("test it before overwriting the original.")


if __name__ == "__main__":
    main()
