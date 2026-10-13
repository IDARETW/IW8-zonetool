"""Audit a Replay 1.20 col_map TAG0 after an ACTS fastfile export.

World roots may be hknpCompressedMeshShape objects (with convex brush/glass
instances at +0x70) or hknpCompoundShape objects (instances at +0x48). Resolve
the root through the TAG0 type table before following its typed ITEM reference.
"""

import argparse
import json
import struct
from pathlib import Path


def section(data: bytes, parent: tuple[int, int], name: bytes) -> tuple[int, int]:
    start, length = parent
    position = start + 8
    while position + 8 <= start + length:
        child_length = struct.unpack_from(">I", data, position)[0] & 0x0FFFFFFF
        if child_length < 8 or position + child_length > start + length:
            raise ValueError(f"invalid TAG0 section near {position}")
        if data[position + 4 : position + 8] == name:
            return position, child_length
        position += child_length
    raise ValueError(f"TAG0 is missing {name.decode()}")


def varuint(data: bytes, position: int, end: int) -> tuple[int, int]:
    if position >= end:
        raise ValueError("truncated TAG0 type table integer")
    prefix = data[position]
    position += 1
    if prefix & 0x80 == 0:
        length, value = 1, prefix & 0x7F
    elif prefix & 0xC0 == 0x80:
        length, value = 2, prefix & 0x3F
    elif prefix & 0xE0 == 0xC0:
        length, value = 3, prefix & 0x1F
    elif prefix & 0xF8 == 0xE0:
        length, value = 4, prefix & 0x07
    elif prefix & 0xF8 == 0xE8:
        length, value = 5, prefix & 0x07
    elif prefix == 0xF8:
        length, value = 6, 0
    elif prefix & 0xF8 == 0xF0:
        length, value = 8, prefix & 0x07
    elif prefix == 0xF9:
        length, value = 9, 0
    else:
        raise ValueError("invalid TAG0 type table integer")
    if position + length - 1 > end:
        raise ValueError("truncated TAG0 type table integer")
    for _ in range(length - 1):
        value = value << 8 | data[position]
        position += 1
    return value, position


def type_identities(data: bytes, type_section: tuple[int, int]) -> list[str]:
    strings_section = section(data, type_section, b"TSTR")
    strings = []
    position = strings_section[0] + 8
    end = strings_section[0] + strings_section[1]
    while position < end:
        terminator = data.find(b"\0", position, end)
        if terminator < 0:
            raise ValueError("invalid TAG0 type string table")
        strings.append(data[position:terminator].decode("utf-8"))
        position = terminator + 1

    names_section = section(data, type_section, b"TNA1")
    position = names_section[0] + 8
    end = names_section[0] + names_section[1]
    count, position = varuint(data, position, end)
    if not 0 < count <= 65535:
        raise ValueError("invalid TAG0 type count")
    names = ["NULL"] + [""] * (count - 1)
    parameters: list[list[tuple[bool, int]]] = [[] for _ in range(count)]
    for type_index in range(1, count):
        name_index, position = varuint(data, position, end)
        parameter_count, position = varuint(data, position, end)
        if name_index >= len(strings) or parameter_count > 64:
            raise ValueError("invalid TAG0 type name")
        names[type_index] = strings[name_index]
        for _ in range(parameter_count):
            parameter_name, position = varuint(data, position, end)
            value, position = varuint(data, position, end)
            if parameter_name >= len(strings):
                raise ValueError("invalid TAG0 template parameter name")
            parameter_kind = strings[parameter_name]
            if not parameter_kind or parameter_kind[0] not in "tv":
                raise ValueError("invalid TAG0 template parameter kind")
            is_type = parameter_kind[0] == "t"
            if is_type and value >= count:
                raise ValueError("invalid TAG0 template type")
            parameters[type_index].append((is_type, value))

    identities = [""] * count
    identities[0] = "NULL"
    building = set()

    def identity(type_index: int) -> str:
        if identities[type_index]:
            return identities[type_index]
        if type_index in building:
            raise ValueError("recursive TAG0 type identity")
        building.add(type_index)
        identity_name = names[type_index]
        if parameters[type_index]:
            args = [
                identity(value) if is_type else str(value)
                for is_type, value in parameters[type_index]
            ]
            identity_name += "<" + ", ".join(args) + ">"
        building.remove(type_index)
        identities[type_index] = identity_name
        return identity_name

    for type_index in range(1, count):
        identity(type_index)
    return identities


def audit(path: Path) -> dict:
    fields = json.loads(path.read_text(encoding="utf-8"))["asset"]["fields"]
    data = bytes.fromhex(fields["havokWorldShapeData"]["bytes"])
    if len(data) != fields["havokWorldShapeDataSize"] or data[4:8] != b"TAG0":
        raise ValueError("invalid or incomplete col_map TAG0")
    root = (0, len(data))
    payload = section(data, root, b"DATA")
    identities = type_identities(data, section(data, root, b"TYPE"))
    item_section = section(data, section(data, root, b"INDX"), b"ITEM")
    if (item_section[1] - 8) % 12:
        raise ValueError("invalid TAG0 ITEM table")
    items = [
        struct.unpack_from("<III", data, pos)
        for pos in range(item_section[0] + 8, sum(item_section), 12)
    ]
    payload_start = payload[0] + 8
    payload_size = payload[1] - 8

    def item(reference: int, stride: int) -> tuple[int, int, int]:
        if not 0 < reference < len(items):
            raise ValueError(f"invalid TAG0 item reference {reference}")
        type_and_flags, offset, count = items[reference]
        if not type_and_flags & 0x00FFFFFF or offset + count * stride > payload_size:
            raise ValueError(f"TAG0 item {reference} exceeds DATA")
        return payload_start + offset, count, type_and_flags

    def ref(offset: int) -> int:
        return struct.unpack_from("<Q", data, offset)[0]

    shape_array, shape_count, _ = item(ref(payload_start), 8)
    if shape_count != 1:
        raise ValueError(f"expected one world root shape, found {shape_count}")
    shape, root_count, root_type = item(ref(shape_array), 128)
    if root_count != 1:
        raise ValueError("world root shape item is not singular")

    root_type_id = root_type & 0x00FFFFFF
    if root_type_id >= len(identities):
        raise ValueError(f"world root has unknown TAG0 type {root_type_id}")
    world_root = identities[root_type_id]
    child_offset_by_type = {
        "hknpCompressedMeshShape": 0x70,
        "hknpCompoundShape": 0x48,
    }
    try:
        children_offset = child_offset_by_type[world_root]
    except KeyError as error:
        raise ValueError(f"unsupported world root TAG0 type {world_root}") from error

    child_array, child_count, child_type = item(ref(shape + children_offset), 112)
    if child_count == 0:
        raise ValueError(f"{world_root} has no shape instances")
    child_type_id = child_type & 0x00FFFFFF
    if child_type_id >= len(identities):
        raise ValueError(f"world root has unknown child TAG0 type {child_type_id}")
    child_type_name = identities[child_type_id]
    if child_type_name not in {
        "hknpShapeInstance",
        "hkFreeListArrayElement<hknpShapeInstance>",
    }:
        raise ValueError(f"unexpected {world_root} child type {child_type_name}")
    counts, count_entries, _ = item(ref(payload_start + 136), 4)
    if count_entries != 1 or struct.unpack_from("<I", data, counts)[0] != child_count:
        raise ValueError("shape-list child count does not match world root")

    tags, tag_count, _ = item(ref(payload_start + 104), 24)
    glass_tags = {}
    for tag_index in range(tag_count):
        contents, _, _, _, user_data = struct.unpack_from(
            "<IIH6sQ", data, tags + tag_index * 24
        )
        glass_id = (user_data >> 32) & 0xFFFF
        if glass_id:
            if contents != 0x04200001 or (user_data >> 19) & 0x1FFF != 39:
                raise ValueError(f"glass tag {tag_index} lacks Replay contents/material")
            glass_tags[tag_index] = glass_id
    child_glass_ids = []
    for index in range(child_count):
        child = child_array + index * 112
        tag_index = struct.unpack_from("<H", data, child + 88)[0]
        if tag_index in glass_tags:
            if not ref(child + 80):
                raise ValueError(f"glass child {index} has no shape reference")
            child_glass_ids.append(glass_tags[tag_index])
    expected = fields["totalGlassInitPieceCount"]
    if sorted(set(child_glass_ids)) != list(range(1, expected + 1)):
        raise ValueError("numbered glass child IDs do not cover GlassWorld pieces")
    return {
        "path": str(path),
        "world_root": f"{world_root}_with_shape_instances",
        "root_item_type_and_flags": f"0x{root_type:08x}",
        "havok_bytes": len(data),
        "convex_children": child_count,
        "shape_tags": tag_count,
        "glass_pieces": expected,
        "glass_child_instances": len(child_glass_ids),
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("colmap_exports", nargs="+", type=Path)
    args = parser.parse_args()
    for export in args.colmap_exports:
        print(json.dumps(audit(export), indent=2))
