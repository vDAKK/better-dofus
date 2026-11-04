#!/usr/bin/env python3
"""Read-only dump of an IL2CPP metadata v39 game, for porting the mod to a new build.

Usage: il2cpp_dump.py <game folder> <output.json>

The output describes the game's code: keep it outside the repository and never
publish it. Only files are read; no game process is accessed.
"""
import json
import os
import struct
import sys

TYPE_SIZE, METHOD_SIZE, FIELD_SIZE, PARAMETER_SIZE, IMAGE_SIZE = 82, 32, 12, 12, 36
STRINGS, METHODS, PARAMETERS, FIELDS, TYPES, IMAGES = 2, 5, 10, 11, 19, 20
PRIMITIVES = {1: "void", 2: "bool", 3: "char", 4: "sbyte", 5: "byte", 6: "short", 7: "ushort", 8: "int", 9: "uint",
              10: "long", 11: "ulong", 12: "float", 13: "double", 14: "string", 22: "IntPtr", 24: "nint", 25: "nuint",
              28: "object"}


def dump(game):
    metadata = open(os.path.join(game, "Dofus_Data/il2cpp_data/Metadata/global-metadata.dat"), "rb").read()
    binary = open(os.path.join(game, "GameAssembly.dll"), "rb").read()
    version = struct.unpack_from("<i", metadata, 4)[0]
    if version != 39:
        raise SystemExit(f"Metadata version {version}: this dump only knows the layouts of version 39")
    table = [struct.unpack_from("<III", metadata, 8 + 12 * i) for i in range(31)]

    def text(index):
        start = table[STRINGS][0] + index
        return metadata[start:metadata.find(b"\0", start)].decode("utf8", "replace")

    pe = struct.unpack_from("<I", binary, 0x3C)[0]
    base = struct.unpack_from("<Q", binary, pe + 48)[0]
    optional = struct.unpack_from("<H", binary, pe + 20)[0]
    sections = []
    for i in range(struct.unpack_from("<H", binary, pe + 6)[0]):
        p = pe + 24 + optional + 40 * i
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", binary, p + 8)
        sections.append((binary[p:p + 8].rstrip(b"\0").decode(), va, vsize, raw, rawsize))

    def offset(rva):
        for _, va, vsize, raw, rawsize in sections:
            if va <= rva < va + max(vsize, rawsize):
                return raw + rva - va
        return None

    def qword(position):
        return struct.unpack_from("<Q", binary, position)[0]

    def pointed(address):
        return offset(address - base) if address else None

    data = [s for s in sections if s[0] in (".data", ".rdata")]
    code = [s for s in sections if s[0] in (".text", "il2cpp")]

    def is_code(address):
        return any(va <= address - base < va + max(vsize, rawsize) for _, va, vsize, _, rawsize in code)

    images = []
    for i in range(table[IMAGES][2]):
        p = table[IMAGES][0] + i * IMAGE_SIZE
        first, count = struct.unpack_from("<HH", metadata, p + 8)
        images.append((text(struct.unpack_from("<I", metadata, p)[0]), first, count))

    # Il2CppCodeGenModule { char* name; u32 count; void** pointers; ... }
    names = {}
    for image, _, _ in images:
        needle = image.encode() + b"\0"
        position = binary.find(needle)
        while position >= 0:
            if not position or binary[position - 1] == 0:
                for _, va, _, raw, rawsize in sections:
                    if raw <= position < raw + rawsize:
                        names[base + va + position - raw] = image
            position = binary.find(needle, position + 1)
    modules = {}
    for _, _, _, raw, rawsize in data:
        for p in range(raw, raw + rawsize - 24, 8):
            image = names.get(qword(p))
            if image:
                count, pointers = qword(p + 8), pointed(qword(p + 16))
                if 0 < count < 500000 and pointers is not None:
                    first = qword(pointers)
                    if first == 0 or pointed(first) is not None:
                        modules[image] = (count, pointers)

    # Il2CppMetadataRegistration: located by its two counts of type definitions.
    type_count = table[TYPES][2]
    registration = None
    for _, _, _, raw, rawsize in data:
        for p in range(raw, raw + rawsize - 32, 8):
            if qword(p) == type_count and qword(p + 16) == type_count and pointed(qword(p + 8)) and pointed(qword(p + 24)):
                registration = p
                break
        if registration:
            break
    if not registration:
        raise SystemExit("Metadata registration not found: the layout changed, nothing written")
    field_offsets = pointed(qword(registration + 8))
    types_count, types = qword(registration - 32), pointed(qword(registration - 24))

    def definition_name(index):
        p = table[TYPES][0] + index * TYPE_SIZE
        namespace = text(struct.unpack_from("<I", metadata, p + 4)[0])
        return (namespace + "." if namespace else "") + text(struct.unpack_from("<I", metadata, p)[0])

    def type_name(index):
        if index < 0 or index >= types_count:
            return "?"
        p = pointed(qword(types + index * 8))
        if p is None:
            return "?"
        value = qword(p)
        kind = (struct.unpack_from("<I", binary, p + 8)[0] >> 16) & 0xFF
        if kind in PRIMITIVES:
            return PRIMITIVES[kind]
        if kind in (17, 18):
            return definition_name(value) if value < type_count else "?"
        return {29: "SZ[]", 21: "GENERIC", 19: "T", 30: "T"}.get(kind, f"k{kind}")

    rows = []
    for image, first, count in images:
        module = modules.get(image)
        for index in range(first, first + count):
            p = table[TYPES][0] + index * TYPE_SIZE
            field_first, method_first = struct.unpack_from("<II", metadata, p + 26)
            method_count, _, field_count = struct.unpack_from("<HHH", metadata, p + 58)
            field_table = qword(field_offsets + index * 8)
            field_table = pointed(field_table) if field_table else None
            fields = []
            for i in range(field_count):
                name, field_type, _ = struct.unpack_from("<III", metadata, table[FIELDS][0] + (field_first + i) * FIELD_SIZE)
                value = struct.unpack_from("<i", binary, field_table + i * 4)[0] if field_table else None
                fields.append([text(name), value, type_name(field_type)])
            methods = []
            for i in range(method_count):
                q = table[METHODS][0] + (method_first + i) * METHOD_SIZE
                token = struct.unpack_from("<I", metadata, q + 20)[0]
                rva = guard = None
                if module and 0 < (token & 0xFFFFFF) <= module[0]:
                    address = qword(module[1] + ((token & 0xFFFFFF) - 1) * 8)
                    if address:
                        rva = address - base
                        position = offset(rva)
                        guard = binary[position:position + 16].hex() if position else None
                parameter_first = struct.unpack_from("<I", metadata, q + 14)[0]
                parameters = [type_name(struct.unpack_from("<I", metadata, table[PARAMETERS][0] + (parameter_first + k) * PARAMETER_SIZE + 8)[0])
                              for k in range(struct.unpack_from("<H", metadata, q + 30)[0])]
                methods.append({"n": text(struct.unpack_from("<I", metadata, q)[0]), "rva": rva, "g": guard, "p": parameters,
                                "r": type_name(struct.unpack_from("<I", metadata, q + 6)[0])})
            rows.append({"img": image, "ns": text(struct.unpack_from("<I", metadata, p + 4)[0]),
                         "n": text(struct.unpack_from("<I", metadata, p)[0]), "m": methods, "fields": fields})

    # Generic instantiations are not in the modules' tables: the metadata
    # registration names them, the code registration holds their pointers.
    entry_count, entries = qword(registration - 48), pointed(qword(registration - 40))
    spec_count, specs = qword(registration - 16), pointed(qword(registration - 8))
    table_entries = [struct.unpack_from("<iiii", binary, entries + 16 * i) for i in range(entry_count)]
    needed = max(entry[1] for entry in table_entries) + 1
    # Il2CppCodeRegistration { ...; u64 genericMethodPointersCount; void** genericMethodPointers;
    # void** genericAdjustorThunks; u64 invokerPointersCount; void** invokerPointers; ... }
    found = []
    for _, _, _, raw, rawsize in data:
        for p in range(raw, raw + rawsize - 40, 8):
            count = qword(p)
            if needed <= count < 300000 and 0 < qword(p + 24) < 300000:
                candidate, thunks, invokers = pointed(qword(p + 8)), pointed(qword(p + 16)), pointed(qword(p + 32))
                if candidate and thunks and invokers:
                    sample = [qword(candidate + 8 * i) for i in range(0, min(count, 3000), 11)]
                    if all(is_code(x) or not x for x in sample) and sum(1 for x in sample if x) * 2 > len(sample):
                        found.append((count, candidate))
    if len(found) != 1:
        raise SystemExit(f"{len(found)} code registrations found instead of 1: the layout changed, nothing written")
    pointers = found[0]
    generics = set()
    for spec, pointer, _, _ in table_entries:
        if 0 <= spec < spec_count and 0 <= pointer < pointers[0]:
            address = qword(pointers[1] + 8 * pointer)
            if address:
                definition = struct.unpack_from("<i", binary, specs + 12 * spec)[0]
                q = table[METHODS][0] + definition * METHOD_SIZE
                owner = struct.unpack_from("<H", metadata, q + 4)[0]
                position = offset(address - base)
                generics.add((text(struct.unpack_from("<I", metadata, table[TYPES][0] + owner * TYPE_SIZE)[0]),
                              text(struct.unpack_from("<I", metadata, q)[0]), struct.unpack_from("<H", metadata, q + 30)[0],
                              address - base, binary[position:position + 16].hex()))
    return {"types": rows, "generics": sorted(generics)}


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    result = dump(sys.argv[1])
    with open(sys.argv[2], "w", encoding="utf8") as output:
        json.dump(result, output)
    methods = sum(len(row["m"]) for row in result["types"])
    print(f"types={len(result['types'])} methods={methods} generics={len(result['generics'])}")


if __name__ == "__main__":
    main()
