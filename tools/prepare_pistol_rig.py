"""Join stock Replay pistol skeleton pieces for one attachment-free custom model.

Input exports supply bone transforms only. The mesh is supplied separately to
build-weapon. Receiver, slide and magazine become one model, not attachments.
"""
import argparse
import json
import math
from pathlib import Path


def multiply(a, b):
    x, y, z, w = a
    X, Y, Z, W = b
    return [w*X+x*W+y*Z-z*Y, w*Y-x*Z+y*W+z*X,
            w*Z+x*Y-y*X+z*W, w*W-x*X-y*Y-z*Z]


def rotate(q, v):
    return multiply(multiply(q, [*v, 0]), [-q[0], -q[1], -q[2], q[3]])[:3]


def combine(paths):
    rig = dict(bones=[], root_bones=1, parents=[], quats=[], translations=[],
               classification=[], bind_pose=[], rigid_bone=1)
    for path in paths:
        document = json.loads(path.read_text())
        if document['profile'] != 'replay-1.20' or document['pool'] != 'xmodel':
            raise ValueError('Expected Replay XModel: ' + str(path))
        strings = json.loads((path.parents[2] / 'script_strings.json').read_text())
        f = document['asset']['fields']
        if f['numRootBones'] != 1:
            raise ValueError('Expected one root per pistol piece')
        bones = [strings[v['value']] for v in f['boneNames']['values']][:f['numBones']]
        parents = bytes.fromhex(f['parentList']['bytes'])
        classification = bytes.fromhex(f['partClassification']['bytes'])
        for i, bone in enumerate(bones):
            if bone in rig['bones']:
                continue
            if i == 0:
                if rig['bones']:
                    raise ValueError('Disconnected skeleton piece: ' + bone)
                rig['bind_pose'].append(dict(quat=[0, 0, 0, 1], translation=[0, 0, 0], weight=2))
            else:
                parent = rig['bones'].index(bones[i - parents[i - 1]])
                q = f['quats']['values'][(i-1)*4:i*4]
                t = f['trans']['values'][(i-1)*3:i*3]
                rig['parents'].append(len(rig['bones']) - parent)
                rig['quats'].append(q)
                rig['translations'].append(t)
                length = math.sqrt(sum(v*v for v in q))
                local = [v/length for v in q]
                pose = rig['bind_pose'][parent]
                rotated = rotate(pose['quat'], t)
                rig['bind_pose'].append(dict(quat=multiply(pose['quat'], local),
                    translation=[a+b for a, b in zip(pose['translation'], rotated)], weight=2))
            rig['bones'].append(bone)
            rig['classification'].append(classification[i])
    if rig['bones'][:2] != ['j_gun', 'tag_pistol_offset'] or 'tag_flash' not in rig['bones']:
        raise ValueError('Pistol rig lacks its attachment or muzzle tag')
    return rig


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--receiver', type=Path, required=True)
    parser.add_argument('--slide', type=Path, required=True)
    parser.add_argument('--magazine', type=Path, required=True)
    parser.add_argument('--material', required=True)
    parser.add_argument('-o', '--output', type=Path, required=True)
    args = parser.parse_args()
    rig = combine([args.receiver, args.slide, args.magazine])
    rig['material'] = args.material
    # Kenney blaster-a: +Z is muzzle, +Y is up. Replay +X is muzzle,
    # +Z is up. An 8-inch blaster fits the 1911's pistol grip/animation rig.
    rig['transform'] = [[0, 0, 10, -8.8], [10, 0, 0, 0], [0, 10, 0, 1.2]]
    view = dict(rig, replace=['weapon_vm_pi_mike1911', 'weapon_vm_stream_pi'])
    world = dict(rig, replace=['weapon_wm_pi_mike1911', 'weapon_wm_stream_pi'])
    result = dict(format='replay-weapon-rig-v1', view_model=view, world_model=world)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{args.output}: {len(rig["bones"])} bones, no attachment assets')


if __name__ == '__main__':
    main()
