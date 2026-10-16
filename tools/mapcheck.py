"""mapcheck.py <built map directory> [--json] [--keep]

Load-time validation of a converted map package through Replay's own code, the
way the game would reject it:

  load      every asset of the main zone through Replay's loaders (ACTS test mode:
            "N assets loaded, M tested, F failed, U unavailable")
  preload   each zone through Replay's preload path with its postload stream plan
            recorded (ACTS_REPLAY_PRELOAD); a stream reservation overrun or a
            "Can't read" is the misaligned/overrun-stream failure that corrupted
            Greenband's map script (zt-55), and the plan must fit the capacity the
            zone header declares (XFile +0x08) because the game never bounds-checks it
  script    the compiled map script's string stack and bytecode walk the way the
            game links them: the walk must end exactly at the end of the code with
            the stack fully consumed, and every far call into the map's own file must
            name a function the file defines ("unknown function" at load otherwise)
  entities  gametype spawn and objective entity counts

Paths: ACTS (--acts or IW8_SIM_ACTS), the Replay 1.20 executable (--game or
IW8_SIM_GAME) and its Oodle DLL (--oodle or IW8_SIM_OODLE).
"""

import argparse
import collections
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

DEFAULTS = {
    "acts": r"D:\mw124\atian-cod-tools\build-workstation\bin\Release\acts.exe",
    "game": r"E:\IW8\Builds\1.20-replay\game_dx12_ship_replay.exe",
    "oodle": r"E:\IW8\Builds\1.20-replay\oo2core_7_win64.dll",
}

# Replay GSC opcode operand widths (verified against 311 stock scripts, see
# memory replay-gsc-script-conversion / simloader).
S1 = {1, 3, 20, 22, 40, 48, 52, 68, 69, 70, 71, 81, 82, 91, 98, 99, 104, 107, 113, 116, 140, 142, 147,
      161, 185, 186, 187, 188}
S2 = {11, 14, 25, 27, 28, 29, 30, 31, 32, 33, 34, 50, 80, 85, 102, 114, 126, 127, 128, 129, 130, 131}
S3 = {35, 51, 79, 132, 137, 152, 169, 170, 171, 172, 177, 178, 179, 180, 96, 117, 146, 150}
S4 = {4, 5, 7, 8, 23, 24, 37, 41, 44, 53, 56, 60, 72, 73, 76, 95, 101, 109, 110, 112, 122, 123, 134, 138,
      141, 148, 173, 174, 175, 176, 181, 182, 183, 184}
FIELD = {4, 7, 8, 24, 37, 44, 56, 60, 76, 109, 123, 141}
FAR = {96, 117, 146, 150, 5, 23, 101, 138}


class Report:
    def __init__(self, package):
        self.package = str(package)
        self.checks = []

    def add(self, name, status, detail, **extra):
        self.checks.append(dict(name=name, status=status, detail=detail, **extra))

    @property
    def passed(self):
        return all(check["status"] != "fail" for check in self.checks)


def run(command, env=None, timeout=2400):
    merged = dict(os.environ)
    merged.update(env or {})
    started = time.time()
    result = subprocess.run(command, capture_output=True, text=True, errors="replace", env=merged,
                            timeout=timeout)
    return result.stdout + result.stderr, time.time() - started


def main_fastfile(package):
    candidates = [p for p in package.glob("*.ff")
                  if not p.name.startswith(("srv_", "eng_", "ww_", "techsets_", "cn_", "jp_", "kr15_", "sa_"))]
    if len(candidates) != 1:
        raise SystemExit("package must hold exactly one main fastfile: " + ", ".join(p.name for p in candidates))
    return candidates[0]


def check_package(package, report):
    main = main_fastfile(package)
    stem = main.stem
    missing = [f"{prefix}{stem}.ff" for prefix in ("srv_", "eng_", "ww_", "techsets_")
               if not (package / f"{prefix}{stem}.ff").exists()]
    header = main.read_bytes()[:0x30]
    magic = header[:8].decode("latin-1")
    if not magic.startswith("IWff"):
        report.add("package", "fail", f"{main.name}: not a Replay fastfile ({magic!r})")
    elif missing:
        report.add("package", "fail", "missing companion zones: " + ", ".join(missing))
    else:
        report.add("package", "ok", f"{main.name} {main.stat().st_size:,} bytes, magic {magic}, companions present")
    map_json = package / "map.json"
    if map_json.exists():
        try:
            meta = json.loads(map_json.read_bytes())
            report.add("metadata", "ok", json.dumps(meta)[:200])
        except Exception as error:  # noqa: BLE001
            report.add("metadata", "fail", f"map.json unreadable: {error}")
    return main


def parse_summary(text):
    match = re.search(r"Replay: (\d+) assets loaded, (\d+) tested, (\d+) failed, (\d+) unavailable", text)
    return tuple(int(x) for x in match.groups()) if match else None


def check_load(acts, game, oodle, main, out, report):
    text, seconds = run([acts, "--noUpdater", "fastfile", "-r", "mw19replay", "-g", game, "--oodle", oodle,
                         "--test", "-o", str(out / "test"), str(main)])
    (out / "test.log").write_text(text, encoding="utf-8")
    summary = parse_summary(text)
    if not summary:
        report.add("load", "fail", "ACTS produced no load summary; see test.log", seconds=round(seconds, 1),
                   tail=text[-1500:])
        return
    loaded, tested, failed, unavailable = summary
    failures = [line.strip() for line in text.splitlines() if re.search(r"\bfail|error|exception", line, re.I)][:20]
    status = "ok" if failed == 0 and unavailable == 0 else "fail"
    report.add("load", status, f"{loaded} assets loaded, {tested} tested, {failed} failed, {unavailable} unavailable",
               seconds=round(seconds, 1), failures=failures)


def check_preload(acts, game, oodle, package, out, report):
    for zone in sorted(package.glob("*.ff")):
        text, seconds = run([acts, "--noUpdater", "fastfile", "-r", "mw19replay", "-g", game, "--oodle", oodle,
                             "--test", "-o", str(out / f"preload-{zone.stem}"), str(zone)],
                            env={"ACTS_REPLAY_PRELOAD": "1"})
        (out / f"preload-{zone.stem}.log").write_text(text, encoding="utf-8")
        name = f"preload {zone.name}"
        overrun = [line.strip() for line in text.splitlines()
                   if re.search(r"Can't read|reservation exhausted|overrun|stream .* exceed", line, re.I)]
        if overrun:
            report.add(name, "fail", "stream overrun during preload: " + overrun[0][:300], seconds=round(seconds, 1))
            continue
        plans = list((out / f"preload-{zone.stem}").rglob("preload-plan.jsonl"))
        if not plans:
            report.add(name, "fail", "preload plan not recorded; see the log", seconds=round(seconds, 1),
                       tail=text[-800:])
            continue
        declared = struct.unpack_from("<Q", zone.read_bytes()[:0x30], 0x28)[0]
        entries = needed = 0
        with open(plans[0], encoding="utf-8") as handle:
            for line in handle:
                row = json.loads(line)
                entries += 1
                needed += 1 + 5 * len(row["deltas"])
        status = "ok" if needed <= declared else "fail"
        report.add(name, status, f"postload plan {entries} entries need {needed} bytes, header declares {declared}",
                   seconds=round(seconds, 1))


def walk_script(data):
    """Walk the compiled map script the way Replay links it. Returns a dict with
    the result; raises on a desync (the game would hit garbage opcodes)."""
    at = data.find(b".gsc\0")
    if at < 0:
        return None
    i = at + 5
    header = data.rfind(b"\xfe\xff\xff\xff\xff\xff\xff\xff", 0, i - 60)
    if header < 0:
        return dict(error="script header not found")
    clen, slen, blen = struct.unpack_from("<iii", data, header + 8)
    stack = zlib.decompress(data[i:i + clen])
    code = data[i + clen:i + clen + blen]
    if len(stack) != slen:
        return dict(error=f"string stack length {len(stack)} != declared {slen}")
    sp = 0

    def u32():
        nonlocal sp
        value = struct.unpack_from("<I", stack, sp)[0]
        sp += 4
        return value

    def cstr():
        nonlocal sp
        end = stack.index(b"\0", sp)
        value = stack[sp:end]
        sp = end + 1
        return value

    def name():
        value = u32()
        return ("id", value) if value else ("str", cstr().decode("latin-1"))

    rbx = 1
    next_function = 1
    functions = []
    far_calls = []
    while rbx < len(code):
        if rbx == next_function:
            size = struct.unpack_from("<i", stack, sp)[0]
            sp += 4
            functions.append((rbx, size, name()))
            next_function += size
        op = code[rbx]
        here = rbx
        rbx += 1
        if op in FIELD:
            value = struct.unpack_from("<I", code, rbx)[0]
            rbx += 4
            if value > 0xE2C0:
                name()
        elif op in FAR:
            file_id = u32()
            file_name = cstr().decode("latin-1") if file_id == 0 else ("id", file_id)
            far_calls.append((here, op, file_name, name()))
            rbx += 3 if op in (96, 117, 146, 150) else 4
        elif op in (177, 178, 179, 180):
            u32()
            rbx += 3
        elif op in (181, 182, 183, 184):
            u32()
            rbx += 4
        elif op in (95, 122):
            cstr()
            rbx += 4
        elif op == 98:
            cstr()
            rbx += 1
        elif op == 145:
            cstr()
            cstr()
            rbx += 8
        elif op == 86:
            cases = code[rbx] | code[rbx + 1] << 8
            rbx += 2
            for _ in range(cases):
                if struct.unpack_from("<I", code, rbx)[0] < 0x80000:
                    cstr()
                rbx += 7
        elif op in (166, 189):
            rbx += 1 + code[rbx]
        elif op == 19:
            rbx += 12
        elif op == 160:
            rbx += 5
        elif op in S1:
            rbx += 1
        elif op in S2:
            rbx += 2
        elif op in S3:
            rbx += 3
        elif op in S4:
            rbx += 4
        elif op > 0xBD:
            return dict(error=f"bad opcode {op} at code {here:#x} (stream desync)", functions=len(functions))
    return dict(code=len(code), walked=rbx, stack=len(stack), stack_used=sp, functions=functions,
                far_calls=far_calls)


def check_script(acts, game, oodle, main, out, report):
    text, seconds = run([acts, "--noUpdater", "fastfile", "-r", "mw19replay", "-g", game, "--oodle", oodle, "-d",
                         "--noAssetDump", "-o", str(out / "dump"), str(main)])
    decs = list((out / "dump").rglob("*.ff.dec"))
    if not decs:
        report.add("script", "fail", "could not dump the zone body (ACTS -d)", seconds=round(seconds, 1),
                   tail=text[-600:])
        return
    data = decs[0].read_bytes()
    try:
        result = walk_script(data)
    except Exception as error:  # noqa: BLE001
        report.add("script", "fail", f"bytecode walk desynced: {error}", seconds=round(seconds, 1))
        return
    if result is None:
        report.add("script", "warn", "the zone carries no map script", seconds=round(seconds, 1))
        return
    if "error" in result:
        report.add("script", "fail", result["error"], seconds=round(seconds, 1))
        return
    problems = []
    if result["walked"] != result["code"]:
        problems.append(f"walk ended at {result['walked']} of {result['code']} code bytes")
    if result["stack_used"] != result["stack"]:
        problems.append(f"string stack used {result['stack_used']} of {result['stack']} bytes")
    defined = {function[2] for function in result["functions"]}
    own = re.search(rb"(maps/mp/[a-z0-9_]+)\.gsc\0", data)
    own_name = own.group(1).decode() if own else None
    unresolved = []
    external = collections.Counter()
    for here, op, file_name, target in result["far_calls"]:
        if file_name == own_name or file_name == "":
            if target not in defined:
                unresolved.append(f"{file_name or '(empty file)'}::{target[1]!r} at {here:#x}")
        else:
            external[file_name if isinstance(file_name, str) else f"id {file_name[1]:#x}"] += 1
    if unresolved:
        problems.append("unresolved far calls into the map script: " + "; ".join(unresolved[:10]))
    report.add("script", "fail" if problems else "ok",
               "; ".join(problems) if problems else
               f"{len(result['functions'])} functions, {len(result['far_calls'])} far calls, walk and string stack consistent",
               seconds=round(seconds, 1),
               external_calls={k: v for k, v in external.most_common(20)})


def check_entities(acts, game, oodle, main, out, report):
    text, seconds = run([acts, "--noUpdater", "fastfile", "-r", "mw19replay", "-g", game, "--oodle", oodle, "-a",
                         "map_ents", "-o", str(out / "ents"), str(main)], timeout=1200)
    files = [p for p in (out / "ents").rglob("*.json") if "map_ents" in p.as_posix()]
    if not files:
        report.add("entities", "warn", "MapEnts not exported", seconds=round(seconds, 1))
        return
    document = json.loads(files[0].read_bytes())
    entity_string = bytes.fromhex(document["asset"]["fields"]["entityString"]["bytes"]).decode("latin-1")
    entities = [dict(re.findall(r'(\d+)\s+"([^"]*)"', block)) for block in re.findall(r"\{([^{}]*)\}", entity_string)]
    classes = collections.Counter(entity.get("212") for entity in entities)
    spawns = {key: classes[key] for key in ("mp_tdm_spawn", "mp_tdm_spawn_allies_start", "mp_tdm_spawn_axis_start",
                                             "mp_dm_spawn", "mp_dom_spawn", "mp_sd_spawn_attacker",
                                             "mp_sd_spawn_defender", "mp_ctf_spawn")}
    status = "ok" if spawns["mp_tdm_spawn"] and spawns["mp_tdm_spawn_allies_start"] and spawns["mp_tdm_spawn_axis_start"] else "warn"
    report.add("entities", status, ", ".join(f"{k} {v}" for k, v in spawns.items() if v) or "no gametype spawns",
               seconds=round(seconds, 1), entities=len(entities))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("package", type=Path)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--keep", action="store_true", help="keep the ACTS outputs under <package>/_check")
    parser.add_argument("--acts", default=os.environ.get("IW8_SIM_ACTS", DEFAULTS["acts"]))
    parser.add_argument("--game", default=os.environ.get("IW8_SIM_GAME", DEFAULTS["game"]))
    parser.add_argument("--oodle", default=os.environ.get("IW8_SIM_OODLE", DEFAULTS["oodle"]))
    parser.add_argument("--skip", default="", help="comma list: load,preload,script,entities")
    args = parser.parse_args()
    package = args.package.resolve()
    if not package.is_dir():
        raise SystemExit(f"not a directory: {package}")
    for label, path in (("ACTS", args.acts), ("game", args.game), ("Oodle", args.oodle)):
        if not Path(path).is_file():
            raise SystemExit(f"{label} not found: {path} (set IW8_SIM_{label.upper()} or pass --{label.lower()})")
    skip = {s.strip() for s in args.skip.split(",") if s.strip()}
    report = Report(package)
    out = package / "_check" if args.keep else Path(tempfile.mkdtemp(prefix="mapcheck-", dir=package.parent))
    out.mkdir(parents=True, exist_ok=True)
    started = time.time()
    try:
        main = check_package(package, report)
        if "load" not in skip:
            check_load(args.acts, args.game, args.oodle, main, out, report)
        if "preload" not in skip:
            check_preload(args.acts, args.game, args.oodle, package, out, report)
        if "script" not in skip:
            check_script(args.acts, args.game, args.oodle, main, out, report)
        if "entities" not in skip:
            check_entities(args.acts, args.game, args.oodle, main, out, report)
    finally:
        if not args.keep:
            shutil.rmtree(out, ignore_errors=True)
    result = dict(package=report.package, passed=report.passed, seconds=round(time.time() - started, 1),
                  checks=report.checks, method="Replay's own loaders through ACTS; no game launched")
    if args.json:
        print(json.dumps(result, indent=1))
    else:
        for check in report.checks:
            print(f"[{check['status']:4}] {check['name']}: {check['detail']}")
        print("PASS" if report.passed else "FAIL", f"({result['seconds']}s)")
    return 0 if report.passed else 1


if __name__ == "__main__":
    sys.exit(main())
