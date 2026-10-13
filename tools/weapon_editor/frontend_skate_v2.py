"""Import the supplied ten-second operator loop through the Replay scene writer.

Uses Replay's v1 spine presentation and weapon/head correction functions.
Bone rotations are absolute parent-local XYZW; translations are bind offsets
in native inches.
"""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import zipfile

import numpy as np


def import_clip(archive, replay, output):
    helper_path = replay / 'tools/build_frontend_skate.py'
    spec = importlib.util.spec_from_file_location('skate_corrections', helper_path)
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    unit, mul, inv, rotate = helper.unit, helper.mul, helper.inv, helper.rotate
    prefix = 'MW2019_v9_10s_Loop/'
    with zipfile.ZipFile(archive) as package:
        hashes = json.loads(package.read(prefix + 'SHA256.json'))
        def read(name):
            raw = package.read(prefix + name)
            if hashlib.sha256(raw).hexdigest() != hashes[name]:
                raise ValueError('Archive checksum mismatch: ' + name)
            return json.loads(raw)
        source = read('MW2019_Skate_v9_Full_10s.native.json')
        skeleton = read('operator_combined.skeleton.json')
        contact = read('validation/contact_seam_validation.json')
    coordinates = source['coordinate_system']
    if (source['format'] != 'replay-native-animation-preview-v1' or
        any(coordinates[k] != v for k, v in dict(handedness='right', up='+Z',
            forward='+X', left='+Y', length_unit='inch', quaternion_order='xyzw').items()) or
        source['fps'] != 30 or source['frames'] != 300 or source['duration'] != 10 or not source['loop']):
        raise ValueError('Unsupported animation coordinate system or loop timing')
    bones = skeleton['bones']
    names = [b['name'] for b in bones]
    lookup = {n: i for i, n in enumerate(names)}
    parents = [b['parent_index'] for b in bones]
    if len(lookup) != len(names) or any(p >= i or p < -1 for i, p in enumerate(parents)):
        raise ValueError('Invalid supplied bone hierarchy')
    tracks = {t['bone']: t for t in source['tracks']}
    if len(tracks) != len(source['tracks']) or set(tracks) != set(names):
        raise ValueError('Supplied tracks do not match the supplied skeleton')
    bind_t = [np.array(b['native_local_rest']['translation']) for b in bones]
    pinned = json.loads((replay / 'evidence/frontend-skate-operator-rig.json').read_text())['skeleton']
    for i, name in enumerate(pinned['names']):
        b = bones[lookup[name]]
        expected_parent = None if not i else pinned['names'][i-pinned['parents'][i-1]]
        if b['parent_name'] != expected_parent or (i and not np.allclose(
                bind_t[lookup[name]], pinned['translations'][i-1], atol=1e-5, rtol=0)):
            raise ValueError('Supplied rig differs from native Replay: ' + name)
    for name, track in tracks.items():
        for key, width, times in (('quaternions', 4, 'rotation_times'), ('translations', 3, 'position_times')):
            values = np.array(track[key], dtype=float)
            if (values.shape != (301, width) or not np.isfinite(values).all() or
                not np.allclose(track[times], np.arange(301)/30, atol=1e-7, rtol=0)):
                raise ValueError('Invalid samples/timebase: ' + name)
        qs = np.array(track['quaternions'])
        if np.any(np.linalg.norm(qs, axis=1) < 1e-9):
            raise ValueError('Zero quaternion: ' + name)
        if (not np.allclose(track['translations'][0], track['translations'][-1], atol=1e-5) or
            abs(unit(qs[0]) @ unit(qs[-1])) < .999999):
            raise ValueError('Open supplied loop: ' + name)

    # Measure soles from the same native weighted mesh used by the v1 viewer.
    # Fit the supplied 90 mm deck contacts to the existing deck, keeping ground
    # contacts and the authored knee bend plane. The board/wheels stay on ground.
    rig = json.loads((replay / 'assets/frontend-skate/preview/rig.json').read_text())
    vertices = np.array([[float(v) for v in line.split()[1:4]] for line in
        (replay / 'assets/frontend-skate/preview/world_model.obj').read_text().splitlines()
        if line.startswith('v ')])
    if len(vertices) != len(rig['vertex_weights']):
        raise ValueError('Preview mesh and native weights differ')
    feet = {}
    for side in ('le', 'ri'):
        groups = {}
        allowed = {f'j_ankle_{side}', f'j_ball_{side}'}
        for point, weights in zip(vertices, rig['vertex_weights'], strict=True):
            if len(weights) != 1 or weights[0]['bone'] not in allowed:
                continue
            n = weights[0]['bone']; b = bones[lookup[n]]['model_bind_pose']
            local = rotate(inv(unit(np.array(b['rotation_xyzw_raw']))), point-np.array(b['translation']))
            groups.setdefault(n, []).append(local)
        if not groups:
            raise ValueError('No native sole vertices for ' + side)
        feet[side] = {n: np.array(points) for n, points in groups.items()}
    def rotate_points(q, values):
        v = q[:3]; t = 2*np.cross(v, values)
        return values + q[3]*t + np.cross(v, t)
    def sole(side, world_q, world_p):
        points = np.concatenate([world_p[lookup[n]] + rotate_points(world_q[lookup[n]], local)
                                 for n, local in feet[side].items()])
        return points[np.argmin(points[:, 2])]

    stock_path = replay / 'assets/frontend-skate/preview/stock-animation.json'
    stock = json.loads(stock_path.read_text())
    stock_tracks = {t['bone']: t for t in stock['tracks']}
    extra_names = [n for n in stock_tracks if n not in tracks]
    if set(extra_names) != {'j_chest', 'j_nvg'}:
        raise ValueError('Unexpected missing native stock channels')
    # V1's lower/upper/spine4 world pitch fixed the upward-looking pose. Head
    # alignment alone merely makes the eyes follow an already elevated rifle.
    # Read the working native clip rather than assume a new rig's local axes.
    presentation_path = replay / 'assets/frontend-skate/project.json'
    presentation = json.loads(presentation_path.read_text())['animations'][0]
    presentation_tracks = {t['bone']: t for t in presentation['tracks']}
    spine_names = ('j_spinelower', 'j_spineupper', 'j_spine4')
    bind_q = [unit(np.array(b['model_bind_pose']['rotation_xyzw_raw'])) for b in bones]
    def world_rotations(rotations):
        world = []
        for i, parent in enumerate(parents):
            world.append(rotations[i] if parent < 0 else mul(world[parent], rotations[i]))
        return world
    def pitch_basis(rotation, index):
        # Factor Rz(yaw) Ry(pitch) Rx(roll) relative to the native model bind.
        delta = mul(rotation, inv(bind_q[index]))
        direction = rotate(delta, [1., 0., 0.])
        yaw = math.atan2(direction[1], direction[0])
        pitch = math.atan2(-direction[2], math.hypot(direction[0], direction[1]))
        yaw_q = helper.axis([0, 0, 1], yaw)
        roll_q = mul(inv(mul(yaw_q, helper.axis([0, 1, 0], pitch))), delta)
        return yaw_q, pitch, roll_q
    reference_world = world_rotations([unit(np.array(presentation_tracks[n]['quaternions'][0])) for n in names])
    source_world = world_rotations([unit(np.array(tracks[n]['quaternions'][0])) for n in names])
    reference_pitch = {n: pitch_basis(reference_world[lookup[n]], lookup[n])[1] for n in spine_names}
    source_pitch = {n: pitch_basis(source_world[lookup[n]], lookup[n])[1] for n in spine_names}
    # Retain pitch variation with the same soft five-degree stock-motion limit
    # used by v1. Use its actual rifle envelope to ease extreme imported looks,
    # without a hard clamp or changing authored wrist/finger/attachment tracks.
    weapon = lookup['tag_weapon_right']
    chain = []; index = weapon
    while index >= 0:
        chain.append(index); index = parents[index]
    reference_elevation = []
    for frame in range(len(presentation_tracks['j_head']['quaternions'])):
        rotation = np.array([0., 0., 0., 1.])
        for index in reversed(chain):
            rotation = mul(rotation, unit(np.array(presentation_tracks[names[index]]['quaternions'][frame])))
        reference_elevation.append(math.asin(float(np.clip(rotate(rotation, [1., 0., 0.])[2], -1, 1))))
    aim_min, aim_max = min(reference_elevation), max(reference_elevation)
    aim_center = (aim_min+aim_max)/2; aim_radius = (aim_max-aim_min)/2
    if not 0 < aim_radius < math.radians(20) or aim_max > math.radians(15):
        raise ValueError('V1 presentation no longer has a near-level rifle aim')
    board_names = ['tag_skateboard'] + [f'j_skate_wheel_{i:02}' for i in range(1, 5)]
    result = {n: dict(bone=n, translations=[], quaternions=[]) for n in names+extra_names+board_names}
    eye_error = head_error = contact_error = max_fit = 0.
    elevations = {n: [] for n in ('source_weapon', 'weapon', 'head')}
    grip_error = grip_rotation_error = 0.
    source_deck = contact['seam_coast_deck_top_m'] / coordinates['meters_per_unit']
    for frame in range(301):
        q = [unit(np.array(tracks[n]['quaternions'][frame])) for n in names]
        offsets = [np.array(tracks[n]['translations'][frame]) for n in names]
        world_q = []; world_p = []
        def forward():
            world_q.clear(); world_p.clear()
            for i, parent in enumerate(parents):
                world_q.append(q[i] if parent < 0 else mul(world_q[parent], q[i]))
                local = bind_t[i] + offsets[i]
                world_p.append(local if parent < 0 else world_p[parent]+rotate(world_q[parent], local))
        forward()
        for side in ('le', 'ri'):
            point = sole(side, world_q, world_p)
            amount = 1. if side == 'le' else helper.smooth(point[2]/source_deck)
            # Use the planted deck plane; the airborne pushing foot can travel
            # beyond the nose and must not inherit the nose's curved surface.
            shift = (4.15-source_deck)*amount
            max_fit = max(max_fit, abs(shift))
            hip, knee, ankle = [lookup[f'j_{joint}_{side}'] for joint in ('hip', 'knee', 'ankle')]
            start = world_p[hip].copy(); old_knee = world_p[knee].copy()
            ankle_q = world_q[ankle].copy(); target = world_p[ankle]+[0., 0., shift]
            a = np.linalg.norm(world_p[knee]-start); b = np.linalg.norm(world_p[ankle]-world_p[knee])
            vector = target-start; distance = np.linalg.norm(vector); direction = vector/distance
            if distance >= a+b or distance <= abs(a-b):
                raise ValueError(f'Deck fit exceeds leg reach: {side} frame {frame}')
            pole = old_knee-start; pole = unit(pole-direction*(pole @ direction))
            along = (a*a-b*b+distance*distance)/(2*distance)
            knee_target = start+direction*along+pole*math.sqrt(max(0., a*a-along*along))
            hip_q = mul(helper.align(old_knee-start, knee_target-start), world_q[hip])
            q[hip] = mul(inv(world_q[parents[hip]]), hip_q); forward()
            knee_q = mul(helper.align(world_p[ankle]-world_p[knee], target-world_p[knee]), world_q[knee])
            q[knee] = mul(inv(world_q[hip]), knee_q); forward()
            q[ankle] = mul(inv(world_q[knee]), ankle_q); forward()
            contact_error = max(contact_error, float(np.linalg.norm(world_p[ankle]-target)))
        original_world = world_q.copy()
        original_grip = [(mul(inv(world_q[weapon]), world_q[lookup[n]]),
                          rotate(inv(world_q[weapon]), world_p[lookup[n]]-world_p[weapon]))
                         for n in ('j_wrist_le', 'j_wrist_ri')]
        elevations['source_weapon'].append(math.degrees(math.asin(float(np.clip(
            rotate(world_q[weapon], [1., 0., 0.])[2], -1, 1)))))
        for n in spine_names:
            index = lookup[n]
            yaw_q, pitch, roll_q = pitch_basis(original_world[index], index)
            limit = math.radians(5)
            pitch = reference_pitch[n] + limit*math.tanh(.65*(pitch-source_pitch[n])/limit)
            orientation = mul(yaw_q, mul(helper.axis([0, 1, 0], pitch), mul(roll_q, bind_q[index])))
            q[index] = mul(inv(world_q[parents[index]]), orientation); forward()
        direction = rotate(world_q[weapon], [1., 0., 0.])
        elevation = math.asin(float(np.clip(direction[2], -1, 1)))
        target = aim_center + aim_radius*math.tanh((elevation-aim_center)/aim_radius)
        correction = helper.axis(np.cross([0., 0., 1.], direction), elevation-target)
        chest = lookup['j_spine4']
        q[chest] = mul(inv(world_q[parents[chest]]), mul(correction, world_q[chest])); forward()
        for n, (rotation, position) in zip(('j_wrist_le', 'j_wrist_ri'), original_grip, strict=True):
            index = lookup[n]
            actual = mul(inv(world_q[weapon]), world_q[index])
            grip_rotation_error = max(grip_rotation_error, math.degrees(2*math.acos(float(np.clip(
                abs(unit(actual) @ unit(rotation)), 0, 1)))))
            grip_error = max(grip_error, float(np.linalg.norm(
                rotate(inv(world_q[weapon]), world_p[index]-world_p[weapon])-position)))
        gun = helper.align_weapon_head(q, world_q, lookup, parents, forward)
        head_error = max(head_error, math.degrees(math.acos(float(np.clip(
            rotate(world_q[lookup['tag_eye']], [1.,0.,0.]) @ gun, -1, 1)))))
        for n in ('j_eyeball_le', 'j_eyeball_ri'):
            i = lookup[n]
            if parents[i] != lookup['j_head']:
                raise ValueError('Native eye parent differs')
            q[i], error = helper.align_weapon_eye(q[i], offsets[i], bind_t[i], world_q, world_p,
                lookup['j_head'], lookup['tag_weapon_right'], gun)
            eye_error = max(eye_error, error)
        for key, n in (('weapon', 'tag_weapon_right'), ('head', 'tag_eye')):
            elevations[key].append(math.degrees(math.asin(float(np.clip(
                rotate(world_q[lookup[n]], [1., 0., 0.])[2], -1, 1)))))
        for i, n in enumerate(names):
            result[n]['translations'].append(offsets[i].tolist())
            result[n]['quaternions'].append(unit(q[i]).tolist())
        for n in extra_names:
            t = frame/30 * stock['duration']/10
            rotation = helper.sample(stock_tracks[n], 'quaternions', t)
            translation = helper.sample(stock_tracks[n], 'translations', t)
            result[n]['quaternions'].append(rotation.tolist())
            result[n]['translations'].append([0.,0.,0.] if translation is None else translation.tolist())
        result['tag_skateboard']['translations'].append([0.,0.,0.])
        result['tag_skateboard']['quaternions'].append([0.,0.,0.,1.])
        for n in board_names[1:]:
            result[n]['translations'].append([0.,0.,0.])
            result[n]['quaternions'].append(helper.axis([0,1,0], -2*math.pi*40*frame/300).tolist())
    if (eye_error > .01 or head_error > 4 or contact_error > 1e-4 or
        grip_error > 1e-6 or grip_rotation_error > .001 or
        min(elevations['weapon']) < math.degrees(aim_min)-1e-6 or
        max(elevations['weapon']) > math.degrees(aim_max)+1e-6):
        raise ValueError('Native aim, grip or deck contact correction failed')
    for track in result.values():
        if (not np.allclose(track['translations'][0], track['translations'][-1], atol=1e-5) or
            abs(np.dot(track['quaternions'][0], track['quaternions'][-1])) < .999999):
            raise ValueError('Corrected loop is open: ' + track['bone'])
    clip = dict(format='replay-animation-source-v1', asset='replay_frontend_skate_v2/anim/operator_push',
        fps=30, duration=10., loop=True, asset_type=0, ik_type=0, finger_pose_type=0,
        tracks=list(result.values()), notetracks=[])
    output.mkdir(parents=True, exist_ok=True)
    project = dict(format='replay-scene-assets-v1', name='replay_frontend_skate_v2', models=[], animations=[clip])
    (output/'project.json').write_text(json.dumps(project, separators=(',', ':'))+'\n')
    unchanged = [n for n in names if all(np.allclose(result[n][key], tracks[n][key], atol=1e-6)
                                        for key in ('quaternions', 'translations'))]
    proof = dict(passed=True, archive=str(archive), archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
        source_animation_sha256=hashes['MW2019_Skate_v9_Full_10s.native.json'],
        upper_correction_source=str(helper_path), upper_correction_sha256=hashlib.sha256(helper_path.read_bytes()).hexdigest(),
        presentation_source=str(presentation_path), presentation_sha256=hashlib.sha256(presentation_path.read_bytes()).hexdigest(),
        spine_reference_pitch_degrees={n: math.degrees(p) for n, p in reference_pitch.items()},
        reference_weapon_elevation_degrees=[math.degrees(aim_min), math.degrees(aim_max)],
        elevation_degrees={n: dict(min=min(values), max=max(values), mean=float(np.mean(values)))
                           for n, values in elevations.items()},
        max_wrist_weapon_position_error_inches=grip_error, max_wrist_weapon_rotation_error_degrees=grip_rotation_error,
        stock_animation_sha256=hashlib.sha256(stock_path.read_bytes()).hexdigest(), frames=300, samples=301,
        fps=30, duration=10., loop_closed=True, bones=len(result), preserved_channels=unchanged,
        added_stock_channels=extra_names, max_head_weapon_angle_degrees=head_error,
        max_eye_target_error_degrees=eye_error, max_deck_fit_inches=max_fit, max_leg_target_error_inches=contact_error)
    (output/'import-validation.json').write_text(json.dumps(proof, indent=2)+'\n')
    return proof


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', required=True, type=Path)
    parser.add_argument('--replay', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    proof = import_clip(args.archive.resolve(), args.replay.resolve(), args.output.resolve())
    print(json.dumps({k: v for k, v in proof.items() if k != 'preserved_channels'}, indent=2))
