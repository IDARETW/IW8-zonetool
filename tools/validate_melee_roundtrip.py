"""Compare stock and rebuilt base knives after both pass Replay's native loader.

Arguments are ACTS mw19replay/<zone> export directories. This checks gameplay
fields and dependencies independently of the writer's relocation representation.
"""
import argparse
import json
from pathlib import Path


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def normalize(directory):
    documents = list((directory / 'assets/weapon').glob('iw8_knife_mp.*.asset.json'))
    if len(documents) != 1:
        raise ValueError('Expected one exported base knife in ' + str(directory))
    asset = load(documents[0])['asset']
    if asset.get('issues') or any(asset.get(key, 0) for key in (
        'read_errors', 'unresolved_pointers', 'unresolved_unions', 'external_payloads'
    )):
        raise ValueError('Incomplete weapon export')
    strings = load(directory / 'script_strings.json')
    seen = {}

    def index(value):
        if isinstance(value, dict):
            if 'address' in value and ('values' in value or 'bytes' in value):
                seen[(value['address'], value.get('type'), value.get('count'))] = value
            for child in value.values():
                index(child)
        elif isinstance(value, list):
            for child in value:
                index(child)

    def clean(value, field=''):
        if isinstance(value, list):
            return [clean(child) for child in value]
        if not isinstance(value, dict):
            return value
        if 'string' in value:
            return value['string']
        if value.get('asset_reference'):
            return {'asset': value['name'].lstrip(','), 'type': value['type']}
        if 'count' in value and 'address' in value:
            if not value['count']:
                return None  # Empty allocated arrays and null arrays are equivalent.
            if value.get('status') == 'reference_already_exported':
                value = seen[(value['address'], value['type'], value['count'])]
            if value['type'] == 'scr_string_t':
                return [strings[v['value']] if v['value'] else None for v in value['values']]
            return value['bytes'] if 'bytes' in value else clean(value['values'])
        if 'value' in value:
            if field in ('stowTag', 'stowOffsetAttachTag'):
                return strings[value['value']] if value['value'] else None
            return value['value']
        return {key: clean(child, key) for key, child in value.items()}

    index(asset)
    return clean(asset['fields'])


def compare(stock, rebuilt, path=''):
    if type(stock) is not type(rebuilt):
        return [(path, stock, rebuilt)]
    if isinstance(stock, dict) and stock.keys() == rebuilt.keys():
        return [diff for key in stock for diff in compare(stock[key], rebuilt[key], path + '.' + key)]
    if isinstance(stock, list) and len(stock) == len(rebuilt):
        return [diff for i, (a, b) in enumerate(zip(stock, rebuilt))
                for diff in compare(a, b, path + f'[{i}]')]
    return [] if stock == rebuilt else [(path, stock, rebuilt)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stock', type=Path)
    parser.add_argument('rebuilt', type=Path)
    args = parser.parse_args()
    manifest = load(args.rebuilt / 'manifest.json')
    if not manifest['complete'] or not manifest['success'] or manifest['failed'] or manifest['unavailable']:
        raise SystemExit('Native loader validation failed')
    if manifest['tested'] != 5 or manifest['loaded_assets'] != 17:
        raise SystemExit('Unexpected knife package asset counts')
    diffs = compare(normalize(args.stock), normalize(args.rebuilt))
    expected = {
        '.weapDef[0].' + field + '.asset': ('weapon_' + suffix + '_me_soscar_knife', 'custom/knife_' + suffix)
        for field, suffix in (
            ('gunXModel', 'vm'), ('defaultViewModel', 'vm'),
            ('worldModel', 'wm'), ('defaultWorldModel', 'wm')
        )
    }
    if {path: (before, after) for path, before, after in diffs} != expected:
        raise SystemExit('Unexpected weapon differences:\n' + json.dumps(diffs, indent=2))
    print(json.dumps({'passed': True, 'native_assets_loaded': 17, 'custom_assets_validated': 5,
                      'weapon_differences': diffs}, indent=2))


if __name__ == '__main__':
    main()
