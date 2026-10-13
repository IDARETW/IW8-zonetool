"""Round-trip every imported attachment and weapon package in bounded batches."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess


# ACTS' structured Replay exporter follows these union members even when the
# stock attachment's discriminator selects another member. The same failures
# occur when the untouched assets are exported from global_stream_mp.ff. They
# do not indicate a fastfile load failure, so preserve them separately from
# real parser, relocation, or payload failures.
STOCK_EXPORTER_LIMITS={
    'fields/swaySettings/values/0/adv/torsoGoalViewSpeedToMaxDeadzone_graph',
    'fields/swaySettings/values/0/adv/gunGoalViewSpeedToOffset_graph',
    'fields/projectile/values/0/projExplosionEffect/particleSystemDef',
    'fields/projectile/values/0/projDudEffect/particleSystemDef',
    'fields/projectile/values/0/projTimeoutEffect/particleSystemDef',
    'fields/projectile/values/0/projDudSound/name',
    'fields/projectile/values/0/projBeaconEffect/particleSystemDef',
    'fields/projectile/values/0/projIgnitionSound/name',
    'fields/projectile/values/0/projIgnitionEffect/particleSystemDef',
}


def stock_exporter_limits(folder):
    manifests=list(folder.rglob('manifest.json'))
    journals=list(folder.rglob('assets.jsonl'))
    if not manifests or not journals:
        return None
    manifest=json.loads(manifests[0].read_text(encoding='utf-8-sig'))
    failed=[]
    for journal in journals:
        for line in journal.read_text(encoding='utf-8-sig').splitlines():
            record=json.loads(line)
            if record.get('status')=='failed':
                failed.append(record)
    if (not manifest.get('complete') or manifest.get('unavailable') or not failed or
            any(record.get('type')!='attachment' for record in failed)):
        return None
    issues=[issue for record in failed for issue in record.get('field_issues',[])]
    if not issues or any(issue.get('path') not in STOCK_EXPORTER_LIMITS or
                         not issue.get('reason','').startswith('Unreadable memory at ')
                         for issue in issues):
        return None
    return len(failed)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--library',type=Path,required=True)
    p.add_argument('--compiler',type=Path,required=True)
    p.add_argument('--acts',type=Path,required=True)
    p.add_argument('--game',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args()
    catalog=json.loads((a.library/'catalog.json').read_text())
    if a.out.exists():
        shutil.rmtree(a.out)
    assets=catalog['attachments']+catalog['packages']
    results=[]
    a.out.mkdir(parents=True,exist_ok=True)
    for index,start in enumerate(range(0,len(assets),50)):
        folder=a.out/f'batch-{index:03}'
        folder.mkdir(exist_ok=True)
        owned=[]
        for desc in assets[start:start+50]:
            root=json.loads((a.library/desc['file']).read_text())['root']
            owned.append({'pool':desc.get('pool',42),'name':desc['name'],'root':root})
        manifest={'format':'replay-weapon-build-v1',
                  'reference':str(a.library/'templates/iw8_acc_weapon_watch_analog_1.json'),
                  'name':f'iw8_cw_batch_{index}','display_name':f'Asset check {index}',
                  'loadout_slot':61,'attachments':False,'owned_assets':owned}
        (folder/'build.json').write_text(json.dumps(manifest))
        build=subprocess.run([str(a.compiler),'build-weapon','--project',str(folder/'build.json'),'-o',str(folder/'output')],capture_output=True,text=True,timeout=90)
        (folder/'build.log').write_text(build.stdout+build.stderr)
        result={'batch':index,'assets':[x['name'] for x in owned],'build_exit':build.returncode}
        if not build.returncode:
            files=[folder/'output'/f'iw8_cw_batch_{index}.ff',folder/'output'/f'iw8_cw_batch_{index}_common.ff']
            load=subprocess.run([str(a.acts),'--noUpdater','fastfile','-r','mw19replay','-g',str(a.game),
                '-a','weapon,attachment,animpkg,sfxpkg,vfxpkg','-o',str(folder/'roundtrip'),*map(str,files)],
                cwd=a.acts.parent,capture_output=True,text=True,timeout=90)
            (folder/'roundtrip.log').write_text(load.stdout+load.stderr)
            result['reload_exit']=load.returncode
            if load.returncode:
                limited=stock_exporter_limits(folder/'roundtrip')
                if limited is not None:
                    result['stock_exporter_limits']=limited
        results.append(result)
        if build.returncode or (result.get('reload_exit') and 'stock_exporter_limits' not in result):
            print(json.dumps({k:v for k,v in result.items() if k!='assets'}),flush=True)
        (a.out/'results.json').write_text(json.dumps(results,indent=2))
    passed=sum(r['build_exit']==0 and (r.get('reload_exit')==0 or 'stock_exporter_limits' in r)
               for r in results)
    print(json.dumps({'batches':len(results),'assets':len(assets),'passed':passed,
        'stock_exporter_limited_assets':sum(r.get('stock_exporter_limits',0) for r in results)}),
        flush=True)


if __name__=='__main__':
    main()
