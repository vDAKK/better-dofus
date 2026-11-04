"""What the port tools share: the game binary, the dump and the mod's sources."""
import collections
import glob
import json
import os
import re
import struct

ROW = re.compile(r"\{(0x[0-9A-Fa-f]+),\s*\{((?:0x[0-9A-Fa-f]{2},?\s*)+)\}\},?[ \t]*(//[^\r\n]*)?")
# A named native site: ... "Method", argc[)], 0xRVA
SITE = re.compile(r'"([^"\s]+)"\s*,\s*(\d+)\s*\)?\s*,\s*(0x[0-9A-Fa-f]{5,8})\b')
CLASS = re.compile(r'(?:\.|\b)Class\(\s*"[^"]+"\s*,\s*"([^"]*)"\s*,\s*"([^"]+)"\s*\)')
RAW_NATIVE = re.compile(r"(?:AddNativeHook|NativeAt)\(\s*(0x[0-9A-Fa-f]+)")
FIELD = re.compile(r'(?:FieldContract|RequireField|CheckField|CheckedField|FieldAt|VerifyField|ExpectField)\w*\(\s*[^;"]*?,\s*"([^"]+)"\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\)')


class Game:
    def __init__(self, folder):
        self.assembly = os.path.join(folder, "GameAssembly.dll")
        self.metadata = os.path.join(folder, "Dofus_Data/il2cpp_data/Metadata/global-metadata.dat")
        self.data = open(self.assembly, "rb").read()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        optional = struct.unpack_from("<H", self.data, pe + 20)[0]
        self.sections = []
        for i in range(struct.unpack_from("<H", self.data, pe + 6)[0]):
            vsize, va, rawsize, raw = struct.unpack_from("<IIII", self.data, pe + 24 + optional + 40 * i + 8)
            self.sections.append((va, max(vsize, rawsize), raw))

    def offset(self, rva):
        for va, size, raw in self.sections:
            if va <= rva < va + size:
                return raw + rva - va
        return None

    def rva(self, offset):
        for va, size, raw in self.sections:
            if raw <= offset < raw + size:
                return va + offset - raw
        return None

    def read(self, rva, size=16):
        position = self.offset(rva)
        return self.data[position:position + size] if position is not None else b""


class Dump:
    """The methods of a dump, by name and by class, generic instantiations included."""

    def __init__(self, path):
        content = json.load(open(path, encoding="utf8"))
        self.types = content["types"]
        self.by_name = collections.defaultdict(list)  # (method, argc) -> (namespace, class, rva, guard)
        self.by_class = collections.defaultdict(set)  # (class, method) -> rva
        self.by_rva = collections.defaultdict(set)
        for row in self.types:
            for method in row["m"]:
                if method["rva"] is not None and method["g"]:
                    self._add(row["ns"], row["n"], method["n"], len(method["p"]), method["rva"], method["g"])
        for owner, name, argc, rva, guard in content["generics"]:
            self._add("", owner, name, argc, rva, guard)

    def _add(self, namespace, owner, name, argc, rva, guard):
        self.by_name[(name, argc)].append((namespace, owner, rva, bytes.fromhex(guard)))
        self.by_class[(owner, name)].add(rva)
        self.by_rva[rva].add((owner, name))


def sources(root, tests=False):
    """(path, text) of the sources, except the guard table."""
    folders = ["src"] + (["tests"] if tests else [])
    for folder in folders:
        for path in glob.glob(os.path.join(root, folder, "**", "*.[ch]pp"), recursive=True):
            if os.path.basename(path) != "signatures.hpp":
                yield path, open(path, "rb").read().decode("utf8", "surrogateescape")


def signature_path(root):
    return os.path.join(root, "src", "core", "signatures.hpp")


def guard_table(root):
    """[(rva, guard, comment)] of src/core/signatures.hpp."""
    text = open(signature_path(root), encoding="utf8").read()
    return [(int(rva, 16), bytes(int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", body)), comment)
            for rva, body, comment in ROW.findall(text)]


def similarity(a, b):
    return sum(x == y for x, y in zip(a, b))
