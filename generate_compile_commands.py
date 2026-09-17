#!/usr/bin/env python3
"""Generate compile_commands.json for clangd/Zed by dry-running `make host`.

We don't hand-maintain a source list here: it would drift from the
Makefile's own APP_SRCS/LVGL_SRCS/etc. the moment either changed. Instead we
ask Make itself (via -n, "just print the recipes") what it would run for the
host build, and keep only the actual compiler invocations.
"""
import json
import os
import shlex
import subprocess
import sys

COMPILERS = {"gcc", "g++", "cc", "c++"}

print("\nGenerating Compile Commands...")

def main():
    cwd = os.getcwd()
    result = subprocess.run(
        ["make", "-Bnwk", "host"],
        cwd=cwd,
        capture_output=True,
        text=True,
    )

    entries = []
    for line in result.stdout.splitlines():
        line = line.strip()
        if not line or " -c " not in line:
            continue
        try:
            tokens = shlex.split(line)
        except ValueError:
            continue
        if not tokens or os.path.basename(tokens[0]) not in COMPILERS:
            continue
        if "-c" not in tokens:
            continue
        src = tokens[tokens.index("-c") + 1]
        if not (src.endswith(".c") or src.endswith(".cpp") or src.endswith(".cc")):
            continue
        entries.append({
            "directory": cwd,
            "arguments": tokens,
            "file": src,
        })

    if not entries:
        print(
            "generate_compile_commands.py: no compile commands captured from "
            "'make -Bnwk host' -- is the host build working? stderr:\n"
            + result.stderr,
            file=sys.stderr,
        )
        sys.exit(1)

    with open("compile_commands.json", "w") as out:
        json.dump(entries, out, indent=2)
    print(f"Generated compile_commands.json with {len(entries)} entries.")


if __name__ == "__main__":
    main()
