"""Native standalone-weapon CLI checks using a locally prepared Replay reference."""
import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('compiler', type=Path)
p.add_argument('reference', type=Path)
p.add_argument('rig', type=Path)
p.add_argument('model', type=Path)
p.add_argument('--material', type=Path)
a = p.parse_args()
base = [str(a.compiler.resolve()), 'build-weapon', '--reference', str(a.reference.resolve()),
        '--rig', str(a.rig.resolve()), '--model', str(a.model.resolve()),
        '--name', 'iw8_cw_cli_test', '--display-name', 'CLI Test']
with tempfile.TemporaryDirectory(prefix='custom-weapon-cli-') as directory:
    directory = Path(directory)
    def run(name, flags, error=None, use_material=True):
        output = directory/name
        if a.material and use_material and '--material' not in flags:
            flags = [*flags, '--material', str(a.material.resolve())]
        result = subprocess.run([*base, *flags, '-o', str(output)], text=True, capture_output=True)
        if error:
            assert result.returncode != 0, name + ': accepted bad input'
            assert error in result.stdout + result.stderr, result.stdout + result.stderr
            assert not list(output.glob('*.ff')), name + ': published invalid fastfile'
        else:
            assert result.returncode == 0, result.stdout + result.stderr
            names = {prefix + 'iw8_cw_cli_test' + suffix + '.ff'
                     for prefix in ('', 'techsets_', 'ww_', 'eng_') for suffix in ('', '_common')}
            assert {f.name for f in output.glob('*.ff')} == names, 'missing native companions'
            return {name: hashlib.sha256((output/name).read_bytes()).hexdigest() for name in names}
    flags = ['--rpm', '400', '--mag-size', '12', '--start-ammo', '36', '--max-ammo', '72',
             '--damage', '30', '--fire-sound-player', 'weap_decho_fire_plr',
             '--set', 'weapon.weapDef[0].lowAmmoWarningThreshold=0.25']
    assert run('repeat-a', flags) == run('repeat-b', flags), 'nondeterministic output'
    if a.material:
        run('incompatible-stock', [], 'unsupported vertex layout', use_material=False)
        custom = ['--material', str(a.material.resolve())]
        assert run('material-a', flags + custom) == run('material-b', flags + custom)
        material_dir = directory/'material-input'
        shutil.copytree(a.material.parent, material_dir)
        material_path = material_dir/a.material.name
        material = json.loads(material_path.read_text())
        pixels = material_dir/material['images'][0]['file']
        pixels.write_bytes(pixels.read_bytes()[:-1])
        run('short-image', ['--material', str(material_path)], 'image byte count')
        no_uv = directory/'no-uv.obj'
        no_uv.write_text('v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n')
        old_model = base[base.index('--model') + 1]
        base[base.index('--model') + 1] = str(no_uv)
        run('missing-uv', custom, 'requires OBJ UVs')
        no_uv.write_text('v 0 0 0\nv 1 0 0\nv 0 1 0\nvt .5 .5\nf 1/2 2/1 3/1\n')
        run('bad-uv-index', custom, 'texture index outside')
        base[base.index('--model') + 1] = old_model
    run('bad-rpm', ['--rpm', 'nan'], '--rpm must be')
    run('bad-mag', ['--mag-size', '-1'], 'stat out of supported range')
    run('overflow', ['--mag-size', '9999999999'], 'integer field out of range')
    run('bad-slot', ['--loadout-slot', '60'], '--loadout-slot must be')
    run('layout-change', ['--set', 'weapon.attachments[0].attachmentCount=1'], 'array or runtime index')
    run('identity-change', ['--set', 'weapon.szInternalName=iw8_pi_mike1911_mp'], 'identity and blueprint')
    run('bad-field', ['--set', 'weapon.this_does_not_exist=1'], 'field not present')
    run('duplicate', ['--name', 'iw8_pi_mike1911'], 'duplicate weapon option')
    listing = subprocess.run(base + ['--list-fields'], text=True, capture_output=True, check=True)
    assert 'weapon.weapDef[0].iFireTime\tint\t+0x140' in listing.stdout
    assert 'weapon.iClipSize\tint\t+0x1C0' in listing.stdout
    assert 'sfx.sounds[0].fireSoundPlayer.name' in listing.stdout
print('Standalone weapon CLI checks passed: deterministic output, stats, field listing and invalid-input rejection')
