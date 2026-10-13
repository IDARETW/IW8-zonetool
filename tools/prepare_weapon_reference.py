"""Import matching Replay weapon and SFX exports for the native weapon compiler.

This authoring step uses local game data. It does not redistribute stock assets.
The resulting reference contains typed fields and relocations, never process pointers.
"""
import argparse
import json
from pathlib import Path

from prepare_melee_reference import prepare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('schema', 'weapon', 'strings', 'sfx', 'sfx-strings'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('-o', '--output', type=Path, required=True)
    args = parser.parse_args()
    read = lambda path: json.loads(path.read_text(encoding='utf-8'))
    schema = read(args.schema)
    weapon = prepare(schema, read(args.weapon), read(args.strings))
    sfx = prepare(schema, read(args.sfx), read(args.sfx_strings), root_type='WeaponSFXPackage')
    result = {'format': 'replay-weapon-reference-v1', 'source_weapon': weapon['source_weapon'],
              'source_sfx': sfx['source_weapon'], 'root': weapon['root'], 'sfx': sfx['root']}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(args.output)


if __name__ == '__main__':
    main()
