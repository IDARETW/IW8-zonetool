"""Exercise the native melee command with a local prepared fixture (no game launch)."""
import argparse
import copy
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler', type=Path)
    parser.add_argument('manifest', type=Path)
    args = parser.parse_args()
    compiler, manifest = args.compiler.resolve(), args.manifest.resolve()
    config = json.loads(manifest.read_text(encoding='utf-8'))
    for key in ('reference', 'model'):
        config[key] = str((manifest.parent / config[key]).resolve())
    reference = json.loads(Path(config['reference']).read_text(encoding='utf-8'))
    with tempfile.TemporaryDirectory(prefix='melee-inputs-') as temp:
        directory = Path(temp)

        def run(name, candidate, expected_error=None):
            source = directory / (name + '.json')
            output = directory / name
            source.write_text(json.dumps(candidate), encoding='utf-8')
            result = subprocess.run([str(compiler), 'build-weapon', str(source), '-o', str(output)],
                                    capture_output=True, text=True)
            package = output / 'custom_weapons.ff'
            if expected_error:
                assert result.returncode != 0, name + ': accepted invalid input'
                assert expected_error in result.stdout + result.stderr, name + ': wrong failure'
                assert not package.exists(), name + ': wrote a package after failure'
            else:
                assert result.returncode == 0, result.stdout + result.stderr
                return hashlib.sha256(package.read_bytes()).hexdigest()

        assert run('repeat1', config) == run('repeat2', config), 'Build is not deterministic'
        wrong_weapon = copy.deepcopy(config)
        wrong_weapon['weapon'] = 'iw8_another_mp'
        run('wrong-weapon', wrong_weapon, 'base iw8_knife_mp slot only')
        skeleton = copy.deepcopy(config)
        skeleton['view_model']['parents'].pop()
        run('bad-skeleton', skeleton, 'incomplete skeleton')
        relocation = copy.deepcopy(reference)
        relocation['root']['fixups'][0]['offset'] = 624
        relocation_path = directory / 'bad-reference.json'
        relocation_path.write_text(json.dumps(relocation), encoding='utf-8')
        bad_reference = copy.deepcopy(config)
        bad_reference['reference'] = str(relocation_path)
        run('bad-relocation', bad_reference, 'out-of-range melee relocation')
        for name, geometry, error in (
            ('bad-index', 'v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 4\n', 'face index outside vertices'),
            ('degenerate', 'v 0 0 0\nv 1 0 0\nv 2 0 0\nf 1 2 3\n', 'degenerate OBJ triangle'),
        ):
            mesh = directory / (name + '.obj')
            mesh.write_text(geometry, encoding='utf-8')
            bad_mesh = copy.deepcopy(config)
            bad_mesh['model'] = str(mesh)
            run(name, bad_mesh, error)
    print('PASS: deterministic native builds; unsupported weapon, malformed skeleton, relocation, '
          'OBJ index and degenerate geometry rejected without output')


if __name__ == '__main__':
    main()
