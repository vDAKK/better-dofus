#!/usr/bin/env python3
"""Proposes where each guarded native address of the mod moved in a new game build.

Usage: port_map.py <repository> <new game folder> <new dump.json> <output map.json>

Read-only. An address is only proposed when the evidence below is strong enough;
the others go to "review", for a person to decide with a disassembler. Interior
call sites and unnamed helpers usually end there.
"""
import bisect
import collections
import json
import re
import sys

from port_common import CLASS, SITE, Dump, Game, guard_table, similarity, sources

NEAR = 0x400  # How far from the neighbours' shift a "near" proposal may be.


def named_sites(root, known):
    """rva -> [(method, argc, (namespace, class) or None)], from the mod's sources."""
    sites = collections.defaultdict(list)
    for _, text in sources(root):
        for match in SITE.finditer(text):
            rva = int(match.group(3), 16)
            if rva not in known:
                continue
            start = max(text.rfind(";", 0, match.start()), text.rfind("{", 0, match.start())) + 1
            before = text[start:match.start()]
            owner = None
            classes = CLASS.findall(before)
            if classes:
                owner = classes[-1]
            else:
                variable = re.findall(r"([A-Za-z_]\w*)\s*,\s*$", before)
                if variable:
                    for assigned in re.finditer(r"\b" + re.escape(variable[-1]) + r'\s*=\s*(?:api\.)?Class\(\s*"[^"]+"\s*,\s*"([^"]*)"\s*,\s*"([^"]+)"\s*\)', text):
                        owner = assigned.groups()
            sites[rva].append((match.group(1), int(match.group(2)), owner))
    return sites


def by_identity(guard, sites, dump):
    """The source's own method name, narrowed by its class or its guard."""
    best = None
    for name, argc, owner in sites:
        candidates = dump.by_name.get((name, argc), [])
        if owner:
            same = [c for c in candidates if c[1] == owner[1] and c[0] == owner[0]]
            if same:
                exact = [c for c in same if c[3] == guard]
                chosen = exact or same
                if len({c[2] for c in chosen}) == 1:
                    return "class", chosen[0][2], chosen[0][3]
        exact = [c for c in candidates if c[3] == guard]
        if not best and len({c[2] for c in exact}) == 1:
            best = ("unique-guard", exact[0][2], exact[0][3])
        elif not best and len({c[2] for c in candidates}) == 1 and similarity(candidates[0][3], guard) >= 10:
            best = ("unique-name", candidates[0][2], candidates[0][3])
    return best


def by_comment(guard, comment, dump):
    """The table's identity comment: // Namespace.Class.Method (...)"""
    if not comment:
        return None
    parts = comment[2:].split(":")[0].split("(")[0].strip().split(".")
    if len(parts) < 2:
        return None
    method, owner = parts[-1], parts[-2].split("`")[0]
    found = {rva for (c, m), rvas in dump.by_class.items() if m.split(".")[-1] == method and c.split("`")[0] == owner for rva in rvas}
    return found


class Shift:
    """Code keeps its order between builds: neighbours tell how far an address moved."""

    def __init__(self, pairs):
        self.pairs = sorted(pairs)
        self.keys = [old for old, _ in self.pairs]

    def predict(self, old):
        i = bisect.bisect_left(self.keys, old)
        return [self.pairs[j][1] + old - self.pairs[j][0] for j in (i - 1, i, i - 2, i + 1) if 0 <= j < len(self.pairs)]


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    root, game_folder, dump_path, output = sys.argv[1:]
    table = guard_table(root)
    game, dump = Game(game_folder), Dump(dump_path)
    guards = {rva: guard for rva, guard, _ in table}
    sites = named_sites(root, guards)
    result = {}

    for rva, guard in guards.items():
        found = by_identity(guard, sites.get(rva, []), dump)
        if found:
            result[rva] = {"new": found[1], "why": found[0], "similar": similarity(found[2], guard)}
    for rva, guard, comment in table:
        if rva in result:
            continue
        found = by_comment(guard, comment, dump)
        if not found:
            continue
        same = [x for x in found if game.read(x) == guard]
        chosen = same if len(set(same)) == 1 else (list(found) if len(found) == 1 else None)
        if chosen:
            result[rva] = {"new": chosen[0], "why": "comment", "similar": similarity(game.read(chosen[0]), guard)}

    trusted = ("class", "unique-guard", "comment", "near-name", "near-bytes")
    for _ in range(2):
        shift = Shift((old, x["new"]) for old, x in result.items() if x["why"] in trusted and x["similar"] >= 8)
        for rva, guard in guards.items():
            if rva in result or not shift.pairs:
                continue
            predicted = shift.predict(rva)
            best = None
            for name, argc, _ in sites.get(rva, []):
                for candidate in dump.by_name.get((name, argc), []):
                    distance = min(abs(candidate[2] - p) for p in predicted)
                    score = similarity(candidate[3], guard)
                    if distance <= NEAR and score >= 8 and (not best or (score, -distance) > (best[0], -best[1])):
                        best = (score, distance, candidate[2])
            if best:
                result[rva] = {"new": best[2], "why": "near-name", "similar": best[0]}
                continue
            # The same 16 bytes, once, right where the neighbours predict them.
            hits = set()
            for p in predicted:
                position = game.offset(p)
                if position is None:
                    continue
                low, high = max(0, position - NEAR), position + NEAR
                at = game.data.find(guard, low, high)
                while at >= 0:
                    hits.add(game.rva(at))
                    at = game.data.find(guard, at + 1, high)
            if len(hits) == 1:
                result[rva] = {"new": hits.pop(), "why": "near-bytes", "similar": 16}

    shift = Shift((old, x["new"]) for old, x in result.items() if x["why"] in trusted)
    review = []
    for rva, guard, comment in table:
        if rva in result:
            continue
        suggestion = None
        for p in shift.predict(rva)[:2]:
            position = game.offset(p)
            if position is None:
                continue
            for at in range(max(0, position - 0x2000), position + 0x2000):
                score = similarity(game.data[at:at + 16], guard)
                if not suggestion or score > suggestion["similar"]:
                    suggestion = {"new": game.rva(at), "similar": score}
        review.append({"old": hex(rva), "guard": guard.hex(), "comment": comment,
                       "sites": [s[0] for s in sites.get(rva, [])], "suggestion": suggestion and {"new": hex(suggestion["new"]), "similar": suggestion["similar"]}})

    targets = collections.defaultdict(list)
    for old, x in result.items():
        targets[x["new"]].append(hex(old))
    collisions = {hex(new): olds for new, olds in targets.items() if len(olds) > 1}
    weak = [hex(old) for old, x in result.items() if x["similar"] < 10]
    with open(output, "w", encoding="utf8") as stream:
        json.dump({"map": {hex(old): {**x, "new": hex(x["new"])} for old, x in sorted(result.items())},
                   "review": review, "collisions": collisions, "weak": weak}, stream, indent=1)
    print("proposed", len(result), dict(collections.Counter(x["why"] for x in result.values())))
    print("to review", len(review), "collisions", len(collisions), "weak guards", len(weak))


if __name__ == "__main__":
    main()
