"""Deterministic smoke check for Replay resident SAB generation."""
from pathlib import Path
import json
import math
import shutil
import struct
import subprocess
import tempfile
import wave

from sound import generate


def main():
    root = Path(tempfile.gettempdir())/'replay-weapon-sound-check'
    if root.exists():
        shutil.rmtree(root)
    (root/'assets').mkdir(parents=True)
    wav = root/'assets'/'tone.wav'
    rate, frames = 22050, 5000
    with wave.open(str(wav), 'wb') as output:
        output.setparams((1, 2, rate, frames, 'NONE', 'not compressed'))
        output.writeframes(b''.join(struct.pack('<h', round(math.sin(i * math.tau * 440 / rate) * 12000))
                                    for i in range(frames)))
    bank = root/'custom.sabl'
    aliases = generate(root, [{'alias':'iw8_cw_check/fire','path':'assets/tone.wav',
                               'event':'sfx.sounds[0].fireSoundPlayer.name','preset':'weapon_player',
                               'volume':1.0,'pitch':1.0,'distance':25000,'looping':False}], bank)
    data = bank.read_bytes()
    assert struct.unpack_from('<I', data, 0)[0] == 0x23585532
    assert struct.unpack_from('<I', data, 20)[0] == 1
    assert struct.unpack_from('<Q', data, 32)[0] == len(data)
    result = {'format':'replay-sound-check-v1','bank_bytes':len(data),
              'frames':aliases[0]['frames'],'rate':aliases[0]['rate'],
              'asset_id':aliases[0]['asset_id']}
    acts = Path.home()/'AppData/Local/Temp/codex-acts-skin/build/bin/Release/acts.exe'
    if acts.is_file():
        process = subprocess.run([str(acts),'mw19soundtest',str(bank),str(aliases[0]['asset_id'])],
                                 capture_output=True,text=True,timeout=60)
        result.update(acts_exit=process.returncode, acts_output=(process.stdout+process.stderr)[-2000:])
        if process.returncode:
            raise RuntimeError(json.dumps(result,indent=2))
    result['status']='passed'
    print(json.dumps(result,indent=2))


if __name__ == '__main__':
    main()
