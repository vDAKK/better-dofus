#!/usr/bin/env python3
"""Checks the mod's native contract against a game build, without touching the game.

Usage: port_check.py <repository> <game folder> <dump.json>

Exit code 1 if a pinned hash, a guard, a named site or a raw native site is wrong.
Field offsets are only reported: a name found in a generic class has no fixed
offset in the dump. This is an offline check; it says nothing of live behaviour.
"""
import collections
import hashlib
import os
import re
import sys

from port_common import FIELD, RAW_NATIVE, SITE, Dump, Game, guard_table, sources


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    root, game_folder, dump_path = sys.argv[1:]
    game, dump = Game(game_folder), Dump(dump_path)
    failed = False

    def report(label, bad, total, fatal=True):
        nonlocal failed
        print(f"{'FAIL' if bad and fatal else ('WARN' if bad else 'OK  ')} {label}: {total - len(bad)}/{total}")
        for item in bad[:40]:
            print("     ", item)
        failed = failed or (bool(bad) and fatal)

    common = open(os.path.join(root, "src", "core", "common.hpp"), encoding="utf8").read()
    hashes = [("GameAssembly.dll", "ExpectedGameHash", game.assembly), ("global-metadata.dat", "ExpectedMetadataHash", game.metadata)]
    report("pinned hashes", [name for name, constant, path in hashes
                             if hashlib.sha256(open(path, "rb").read()).hexdigest() not in re.search(constant + r'\[\] = "[a-f0-9]{64}', common).group(0)], 2)

    table = guard_table(root)
    report("guards match the binary", [hex(rva) for rva, guard, _ in table if game.read(rva) != guard], len(table))
    report("guards are distinct", [hex(rva) for rva, n in collections.Counter(r for r, _, _ in table).items() if n > 1], len(table))
    guarded = {rva for rva, _, _ in table}

    sites = natives = 0
    unguarded, unresolved, raw_unguarded, field_bad = [], [], set(), []
    fields_total = 0
    offsets = collections.defaultdict(set)
    for row in dump.types:
        for name, value, _ in row["fields"]:
            offsets[name].add(value)
    for path, text in sources(root):
        name = os.path.basename(path)
        for match in SITE.finditer(text):
            rva, method, argc = int(match.group(3), 16), match.group(1), int(match.group(2))
            sites += 1
            if rva not in guarded:
                unguarded.append(f"{name} {method} {rva:#x}")
            elif rva not in {c[2] for c in dump.by_name.get((method, argc), [])}:
                unresolved.append(f"{name} {method}/{argc} {rva:#x}")
        for match in RAW_NATIVE.finditer(text):
            natives += 1
            rva = int(match.group(1), 16)
            if rva not in guarded:
                raw_unguarded.add(hex(rva))
        for match in FIELD.finditer(text):
            fields_total += 1
            if int(match.group(2), 0) not in offsets.get(match.group(1), ()):
                field_bad.append(f"{name} {match.group(1)} {match.group(2)}")
    report("named sites have a guard", unguarded, sites)
    report("named sites resolve to their method", unresolved, sites)
    report("raw native sites have a guard", sorted(raw_unguarded), natives)
    report("field contracts found in the dump", field_bad, fields_total, fatal=False)
    print("Offline only: live loading and rendering stay to verify." if not failed else "Port not ready.")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
