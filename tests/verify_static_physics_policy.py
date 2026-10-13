"""Verify Replay PhysicsAsset policy in ACTS asset JSON and its Havok TAG0 body.

Usage: python tests/verify_static_physics_policy.py [--generic-static] ASSET_JSON...
The generic flag checks the selected 0/0, 0xB180 converter policy. Without it,
the checker accepts signed assets with different material and contents choices.
"""

import argparse
import json
import struct
from pathlib import Path


def section(data, parent, name):
    start, length = parent
    cursor = start + 8
    while cursor + 8 <= start + length:
        child_length = struct.unpack_from(">I", data, cursor)[0] & 0x0FFFFFFF
        if child_length < 8 or cursor + child_length > start + length:
            raise ValueError("invalid Havok section")
        if data[cursor + 4 : cursor + 8] == name:
            return cursor, child_length
        cursor += child_length
    raise ValueError(f"missing Havok section {name!r}")


def inspect(path, generic_static):
    fields = json.loads(path.read_text(encoding="utf-8"))["asset"]["fields"]
    data = bytes.fromhex(fields["havokData"]["bytes"])
    assert len(data) == fields["havokDataSize"]
    assert data[4:8] == b"TAG0"
    assert struct.unpack_from(">I", data)[0] & 0x0FFFFFFF == len(data)
    payload = section(data, (0, len(data)), b"DATA")
    index = section(data, (0, len(data)), b"INDX")
    item_section = section(data, index, b"ITEM")
    assert (item_section[1] - 8) % 12 == 0
    items = [
        struct.unpack_from("<III", data, offset)
        for offset in range(item_section[0] + 8, sum(item_section), 12)
    ]
    base = payload[0] + 8

    def reference(address):
        item_index = struct.unpack_from("<Q", data, address)[0]
        assert 0 < item_index < len(items)
        _, offset, count = items[item_index]
        assert offset < payload[1] - 8
        return base + offset, count

    def one_word(address):
        offset, count = reference(address)
        assert count == 1
        return struct.unpack_from("<I", data, offset)[0]

    use_category = fields["useCategory"]["value"]
    simulation_category = fields["simulationCategories"]["values"][0]["value"]
    contents = fields["bodyContents"]["values"][0]
    assert fields["numRigidBodies"] == 1
    assert struct.unpack_from("<I", data, base)[0] == use_category
    assert one_word(base + 0x90) == simulation_category
    system, _ = reference(base + 8)
    body, count = reference(system + 56)
    assert count == 1
    assert struct.unpack_from("<I", data, body + 0x10)[0] == contents
    quality = one_word(base + 0x10)
    material = one_word(base + 0x20)
    motion = one_word(base + 0x30)
    if use_category == 0 and simulation_category == 0:
        assert fields["containsDynamicBodies"] is False
        assert quality == 0xD0452309
        assert motion == 0xC356EC46
        assert struct.unpack_from("<H", data, body + 0x14)[0] == 0xFFFF
        assert data[body + 0x16] == 0xFF
        assert struct.unpack_from("<H", data, body + 0x88)[0] == 0xFFFF
        if generic_static:
            assert contents == 0xB180
            assert material == 0x4B02BC9D
    elif generic_static:
        raise AssertionError("expected a generic static-model PhysicsAsset")
    return {
        "name": fields["name"]["string"],
        "use_category": use_category,
        "simulation_category": simulation_category,
        "body_contents": hex(contents),
        "body_quality_crc": hex(quality),
        "material_crc": hex(material),
        "motion_crc": hex(motion),
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generic-static", action="store_true")
    parser.add_argument("assets", nargs="+", type=Path)
    args = parser.parse_args()
    for asset in args.assets:
        print(json.dumps(inspect(asset, args.generic_static), sort_keys=True))
