#!/usr/bin/env python3
"""Applies a reviewed address map to the sources, the tests and the guard table.

Usage: port_apply.py <repository> <new game folder> <map.json> [overrides.json]

overrides.json is { "0xOLD": "0xNEW" }: the review's decisions, plus what no guard
row holds (data addresses, interior return addresses, the tests' derived values).
It stops, changing nothing, if a referenced guard row has no new address. Only
files of the repository change; the game is read. Afterwards: tools/port/port_check.py,
then build.ps1 and the tests.
"""
import hashlib
import json
import os
import re
import sys

from port_common import ROW, Game, guard_table, signature_path, sources

HEX = re.compile(r"0[xX]([0-9A-Fa-f]{5,8})(?![0-9A-Za-z_])|0[xX]([0-9A-Fa-f]{5,8})([uUlL]+)")
BARE = re.compile(r"(?<![0-9A-Za-z_])[0-9A-F]{6,7}(?![0-9A-Za-z_])")


def read_map(path):
    content = json.load(open(path, encoding="utf8"))
    return {int(old, 16): int(x["new"], 16) for old, x in content["map"].items()}


def main():
    if len(sys.argv) not in (4, 5):
        raise SystemExit(__doc__)
    root, game_folder, map_path = sys.argv[1:4]
    table_map = read_map(map_path)
    overrides = {}
    if len(sys.argv) == 5:
        overrides = {int(k, 16): int(v, 16) for k, v in json.load(open(sys.argv[4], encoding="utf8")).items()}
    full = {**table_map, **overrides}
    game = Game(game_folder)
    table = guard_table(root)
    guards = {rva for rva, _, _ in table}

    rewritten = {}

    def replace_hex(match):
        digits, suffix = (match.group(1), "") if match.group(1) else (match.group(2), match.group(3))
        value = int(digits, 16)
        if value not in full:
            return match.group(0)
        new = "%X" % full[value] if digits.upper() == digits else "%x" % full[value]
        return match.group(0)[:2] + new.rjust(len(digits) if digits.startswith("0") else 0, "0") + suffix

    # Hook names such as "hook:190B360" hold the address without a prefix.
    def replace_bare(match):
        value = int(match.group(0), 16)
        return "%X" % full[value] if value in full else match.group(0)

    unmapped = set()
    for path, text in sources(root, tests=True):
        for match in HEX.finditer(text):
            value = int(match.group(1) or match.group(2), 16)
            if value in guards and value not in full:
                unmapped.add(value)
        new = BARE.sub(replace_bare, HEX.sub(replace_hex, text))
        if new != text:
            rewritten[path] = new
    if unmapped:
        raise SystemExit("Referenced guards without a new address, nothing changed: " + ", ".join(hex(x) for x in sorted(unmapped)))

    rows, seen, dropped = [], set(), []
    for rva, _, comment in table:
        if rva not in full:
            dropped.append(rva)
            continue
        new = full[rva]
        if new in seen:
            continue
        seen.add(new)
        guard = game.read(new)
        if len(guard) != 16:
            raise SystemExit(f"{new:#x} is outside the binary, nothing changed")
        rows.append("    {0x%X, {%s}},%s" % (new, ",".join("0x%02X" % b for b in guard), " " + comment if comment else ""))

    for path, text in rewritten.items():
        open(path, "wb").write(text.encode("utf8", "surrogateescape"))
    path = signature_path(root)
    text = open(path, encoding="utf8", newline="").read()
    newline = "\r\n" if "\r\n" in text else "\n"
    first = ROW.search(text).start()
    start = text.rindex("\n", 0, first) + 1
    end = text.index("\n", list(ROW.finditer(text))[-1].end() - 1) + 1
    open(path, "w", encoding="utf8", newline="").write(text[:start] + newline.join(rows) + newline + text[end:])

    def sha256(file):
        return hashlib.sha256(open(file, "rb").read()).hexdigest()

    path = os.path.join(root, "src", "core", "common.hpp")
    text = open(path, encoding="utf8", newline="").read()
    text = re.sub(r'(ExpectedGameHash\[\] = ")[a-f0-9]{64}', lambda m: m.group(1) + sha256(game.assembly), text)
    text = re.sub(r'(ExpectedMetadataHash\[\] = ")[a-f0-9]{64}', lambda m: m.group(1) + sha256(game.metadata), text)
    open(path, "w", encoding="utf8", newline="").write(text)
    print(f"files={len(rewritten)} guards={len(rows)} unreferenced_dropped={len(dropped)}")
    print("Now update the client version strings: see tools/port/README.md")


if __name__ == "__main__":
    main()
