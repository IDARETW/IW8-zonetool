"""Use the existing compiled frontend test driver to check v1/v2 lifetimes."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess


def verify(replay, output):
    spec = importlib.util.spec_from_file_location('selection_test', replay/'tools/verify_frontend_selection.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    driver = module.DRIVER
    # The current registration declarations use the native opaque dvar_t.
    # The stub only needs storage for the requested string, not a native dvar.
    driver = driver.replace('ReplayDvar', 'dvar_t')
    driver = driver.replace('#include "G_FrontEndScene_SetCameraFov.cpp"',
                            '#include "G_FrontEndScene_SetCameraFov.cpp"\n#include "XAnimCreateAnimation.cpp"')
    # V1 now lives in the compiled global_stream_mp. V2 is one optional native
    # animation companion owned by the same MP lifetime; no scene owns the deck.
    start = driver.index('    assert(queued[0] == "techsets_replay_frontend_skate");')
    end = driver.index('    assert(queued.back() == "global_stream_mp");', start)
    driver = driver[:start] + '''    assert(queued[0] == "replay_frontend_skate_v2_anims");
    for (const auto& name : queued) {
        assert(name != "techsets_replay_frontend_skate" && name != "replay_frontend_skate" &&
               name != "replay_frontend_skate_anims");
    }
''' + driver[end:]
    stubs = r'''
static bool customAvailable = true;
static std::vector<std::string> animationLookups;
static int animationParts;
extern "C" void* replay_rva_011aa890(int type, const char* name, bool createDefault) {
    assert(type == 7); animationLookups.emplace_back(name);
    const bool custom = !strncmp(name, "replay_frontend_skate", 21);
    assert(createDefault != custom);
    return custom && !customAvailable ? nullptr : &animationParts;
}
extern "C" void replay_rva_012b0820(int, const char*, ...) {}
static void checkClip(const char* expected) {
    unsigned char tree[0x40+16*4]{};
    auto* entry = reinterpret_cast<XAnimEntryReplay*>(tree+0x40)+3;
    entry->parent = 0x2345;
    animationLookups.clear(); customAvailable = true;
    XAnimCreateAnimation(tree, 3, "chr_menu_mp_squad_male_01");
    assert(animationLookups.size() == 1 && animationLookups[0] == expected);
    assert(entry->parts == &animationParts && entry->bindingIndex == 0xffff && entry->parent == 0x2345);
    assert(entry->nodeType == 1 && entry->lod == 5 && entry->numAnims == 0);
    animationLookups.clear(); customAvailable = false;
    XAnimCreateAnimation(tree, 3, "chr_menu_mp_squad_male_01");
    assert(animationLookups.back() == "chr_menu_mp_squad_male_01" && entry->parts == &animationParts);
    animationLookups.clear();
    XAnimCreateAnimation(tree, 3, "ordinary_anim");
    assert(animationLookups.size() == 1 && animationLookups[0] == "ordinary_anim");
    customAvailable = true;
}
'''
    driver = driver.replace('int main() {', stubs+'\nint main() {')
    insertion = r'''
    assert(!Com_FrontEndScene_GetSkateAnimationName());
    checkClip("chr_menu_mp_squad_male_01");
    animation("skateboardv2"); restartExpected("mp_frontend6", true);
    assert(!strcmp(Com_FrontEndScene_GetSkateAnimationName(), "replay_frontend_skate_v2/anim/operator_push"));
    checkClip("replay_frontend_skate_v2/anim/operator_push");
    animation("skateboardv2"); assert(events.empty());
    scene("mp_frontend3"); restartExpected("mp_frontend3", true, true);
    checkClip("replay_frontend_skate_v2/anim/operator_push");
    animation("skateboard"); restartExpected("mp_frontend3", true);
    checkClip("replay_frontend_skate/anim/operator_push");
    animation("skateboardv2"); restartExpected("mp_frontend3", true);
    animation("normal"); restartExpected("mp_frontend3", false);
    assert(!Com_FrontEndScene_GetSkateAnimationName());
    assert(rename("zone/replay_frontend_skate_v2_anims.ff", "zone/replay_frontend_skate_v2_anims.held") == 0);
    animation("skateboardv2"); assert(events.empty() && !Com_FrontEndScene_UseSkateAnimation());
    queued.clear(); DB_PendingZones_QueueZoneSet("global_stream_mp", 0x40, false, false, 1, 1);
    for (const auto& name : queued) assert(name != "replay_frontend_skate_v2_anims");
    queued.clear();
    assert(rename("zone/replay_frontend_skate_v2_anims.held", "zone/replay_frontend_skate_v2_anims.ff") == 0);
'''
    marker = '    assert(s_frontEndSceneSwitches == 2 && s_frontEndAnimationSwitches == 3);'
    if marker not in driver:
        raise ValueError('Baseline selection test changed; review before extending it')
    driver = driver.replace(marker, insertion+'\n    assert(s_frontEndSceneSwitches == 3 && s_frontEndAnimationSwitches == 7);')
    driver = driver.replace('assert(s_frontEndSceneSwitches == 3);', 'assert(s_frontEndSceneSwitches == 4);')
    output.mkdir(parents=True, exist_ok=True)
    (output/'zone').mkdir(exist_ok=True)
    for name in ('mp_frontend6', 'mp_frontendc3s5', 'replay_frontend_skate_v2_anims'):
        (output/'zone'/f'{name}.ff').write_bytes(b'Test file presence only; never deploy.')
    source = output/'driver.cpp'; source.write_text(driver)
    for placeholders, multi in ((False, False), (True, False), (True, True)):
        binary = output/('probe-multi.exe' if multi else 'probe-placeholders.exe' if placeholders else 'probe.exe')
        options = ['-DREPLAY_COMPILER_FRONTEND_PLACEHOLDERS'] if placeholders else []
        if multi: options += ['-DREPLAY_COMPILER_MULTI_FRONTEND']
        subprocess.run([r'C:\Program Files\LLVM\bin\clang++.exe', '-O2', '-std=c++20',
            '-D_CRT_SECURE_NO_WARNINGS', *options, '-DREPLAY_COMPILER_FRONTEND_PROBE="mp_frontend6"',
            '-DREPLAY_COMPILER_FRONTEND_INITIAL_SKATE=1', '-I', str(replay/'src'), str(source),
            '-o', str(binary)], check=True)
        subprocess.run([str(binary)], cwd=output, check=True)
    proof = dict(passed=True, configurations=3, animations=['normal', 'skateboard', 'skateboardv2'],
        coverage=['command dispatch and exact asset selection', 'native XAnim entry ABI',
                  'missing asset fallback', 'unrelated animation untouched', 'v1-v2-normal switching',
                  'v2 persists across scene switches', 'one board and preserved weapon requests',
                  'missing v2 file rejects command and is never queued', 'MP lifetime, no scene ownership'],
        limitations='Native scene shutdown and database services are stubbed; live visuals are a separate check.')
    (output/'verification.json').write_text(json.dumps(proof, indent=2)+'\n')
    print(json.dumps(proof, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--replay', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args(); verify(args.replay.resolve(), args.output.resolve())
