#!/usr/bin/env python3
"""
SA:COOP build script (llvm-mingw, i686).

    python tools/build.py              build everything (release)
    python tools/build.py client       build only coop.dll
    python tools/build.py --debug      build with -O0
    python tools/build.py --clean      remove the build output (build/release, build/debug)
    python tools/build.py dist         build everything and assemble dist/ (ready-to-play package)
    python tools/build.py --with DIR   also compile the .cpp files of DIR into coop.dll (the test bot)

The toolchain is expected in toolchain/llvm-mingw (see tools/get-toolchain.ps1).
"""
import argparse
import concurrent.futures as futures
import json
import os
import re
from array import array
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TC = ROOT / "toolchain" / "llvm-mingw" / "bin"
BUILD = ROOT / "build"

CC = TC / "i686-w64-mingw32-clang.exe"
CXX = TC / "i686-w64-mingw32-clang++.exe"
WINDRES = TC / "i686-w64-mingw32-windres.exe"

COMMON_DEFS = ["-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX", "-D_WIN32_WINNT=0x0601", "-D_CRT_SECURE_NO_WARNINGS"]
WARN = ["-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
        "-Wno-invalid-offsetof", "-Wno-unused-function"]


def lib(name, sources, includes=(), defines=(), cflags=()):
    return {"name": name, "kind": "objs", "sources": sources, "includes": list(includes),
            "defines": list(defines), "cflags": list(cflags)}


def glob(base, *patterns):
    out = []
    for p in patterns:
        out += sorted(str(x.relative_to(ROOT)).replace("\\", "/") for x in (ROOT / base).glob(p))
    return out


ENET = lib("enet", [f"third_party/enet/{f}.c" for f in
                    ("callbacks", "compress", "host", "list", "packet", "peer", "protocol", "win32")],
           includes=["third_party/enet/include"], cflags=["-w"])
MINHOOK = lib("minhook", ["third_party/minhook/src/buffer.c", "third_party/minhook/src/hook.c",
                          "third_party/minhook/src/trampoline.c", "third_party/minhook/src/hde/hde32.c"],
              includes=["third_party/minhook/include"], cflags=["-w"])

COMMON_INC = ["src", "third_party/enet/include"]  # src/common: code of all three binaries


def targets(extra=()):
    common = lib("common", glob("src/common", "**/*.cpp"), includes=COMMON_INC)
    client = {
        "name": "client", "kind": "dll", "out": "coop.dll",
        "sources": glob("src/client", "**/*.cpp") + list(extra),
        "rc": glob("src/client", "*.rc"),
        "includes": COMMON_INC + ["src/client", "third_party/minhook/include"],
        "defines": ["-DCOOP_CLIENT"], "cflags": [],
        "deps": [common, ENET, MINHOOK],
        "libs": ["-lws2_32", "-lwinmm", "-lgdi32", "-luser32", "-lkernel32", "-lole32", "-loleaut32", "-lshell32", "-luuid",
                 "-ldbghelp", "-lwinhttp"],
    }
    server = {
        "name": "server", "kind": "exe", "out": "coop-server.exe",
        "sources": glob("src/server", "**/*.cpp"),
        "rc": glob("src/server", "*.rc"),
        "includes": COMMON_INC + ["src/server"],
        "defines": ["-DCOOP_SERVER"], "cflags": [],
        "deps": [common, ENET],
        "libs": ["-lws2_32", "-lwinmm", "-lwinhttp", "-lbcrypt"],
        "ldflags": ["-municode"],
    }
    # vorbisFile.dll: ASI loader for a game that has none (installed by "install.bat" of the package)
    loader = {
        "name": "loader", "kind": "dll", "out": "vorbisFile.dll",
        "sources": glob("src/loader", "**/*.cpp"),
        "rc": glob("src/loader", "*.rc"),
        "includes": ["src"], "defines": [], "cflags": [], "deps": [],
        "libs": ["-lkernel32", "-luser32"],
        "ldflags": [str(ROOT / "src" / "loader" / "app" / "entry" / "vorbisFile.def")],
    }
    return {"client": client, "server": server, "loader": loader}


def parse_depfile(text: str):
    """Parse a make-style dependency file. Handles 'E:/' drive colons and '\\ ' escaped spaces."""
    text = text.replace("\\\r\n", " ").replace("\\\n", " ")
    m = re.search(r":\s", text)  # the rule separator is a colon followed by whitespace
    if not m:
        return []
    body = text[m.end():]
    files, cur, i = [], "", 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body) and body[i + 1] == " ":
            cur += " "
            i += 2
            continue
        if c.isspace():
            if cur:
                files.append(cur)
                cur = ""
        else:
            cur += c
        i += 1
    if cur:
        files.append(cur)
    return files


def needs_rebuild(obj: Path, dep: Path, src: Path) -> bool:
    if not obj.exists() or not dep.exists():
        return True
    t = obj.stat().st_mtime
    if src.stat().st_mtime > t:
        return True
    for f in parse_depfile(dep.read_text(errors="ignore")):
        fp = Path(f)
        if not fp.is_absolute():
            fp = ROOT / fp
        try:
            if fp.stat().st_mtime > t:
                return True
        except OSError:
            return True
    return False


def compile_one(src, obj, flags, is_c):
    obj.parent.mkdir(parents=True, exist_ok=True)
    dep = obj.with_suffix(".d")
    cmd = [str(CC if is_c else CXX)] + flags + ["-MMD", "-MF", str(dep), "-c", str(ROOT / src), "-o", str(obj)]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    return src, r.returncode, (r.stdout + r.stderr).strip(), cmd


def build_flags(t, debug):
    opt = ["-O0", "-g"] if debug else ["-O2", "-g"]
    inc = [f"-I{ROOT / i}" for i in t["includes"]]
    return opt + ["-gcodeview", "-m32", "-march=i686", "-mfpmath=sse", "-msse2", "-fno-strict-aliasing",
                  "-ffunction-sections", "-fdata-sections"] + COMMON_DEFS + t.get("defines", []) + inc + t.get("cflags", [])


def compile_target(t, debug, pool, compdb):
    flags_c = build_flags(t, debug) + ["-std=c11"] + (["-w"] if "-w" in t.get("cflags", []) else WARN)
    flags_cpp = build_flags(t, debug) + ["-std=c++20"] + WARN
    objdir = BUILD / ("debug" if debug else "release") / "obj" / t["name"]
    jobs, objs = [], []
    for src in t["sources"]:
        is_c = src.endswith(".c")
        obj = objdir / (re.sub(r"[\\/:\s]+", "_", src).rsplit(".", 1)[0] + ".o")
        objs.append(obj)
        flags = flags_c if is_c else flags_cpp
        compdb.append({"directory": str(ROOT), "file": str(ROOT / src),
                       "arguments": [str(CC if is_c else CXX)] + flags + ["-c", str(ROOT / src)]})
        if needs_rebuild(obj, obj.with_suffix(".d"), ROOT / src):
            jobs.append(pool.submit(compile_one, src, obj, flags, is_c))
    ok = True
    for j in jobs:
        src, rc, out, cmd = j.result()
        status = "ok" if rc == 0 else "FAILED"
        print(f"  [{t['name']}] {src} {status}")
        if out:
            print(out)
        if rc != 0:
            ok = False
    return ok, objs


def file_version():
    """COOP_VERSION_STR of Version.h as the four numbers of a version resource: "0.2.7" -> "0,2,7,0"."""
    text = (ROOT / "src" / "common" / "config" / "version" / "Version.h").read_text(encoding="utf-8")
    nums = re.search(r'#define\s+COOP_VERSION_STR\s+"([\d.]+)"', text).group(1).split(".")
    return ",".join((nums + ["0"] * 4)[:4])


def set_pe_checksum(path: Path):
    """The PE header's checksum, as MSVC's /RELEASE fills it in (lld leaves 0): antiviruses' learned models count an
    empty one, like debug sections named /4, among a threat's marks."""
    data = bytearray(path.read_bytes())
    off = int.from_bytes(data[0x3C:0x40], "little") + 24 + 64  # IMAGE_OPTIONAL_HEADER32::CheckSum
    data[off:off + 4] = bytes(4)
    total = sum(array("H", bytes(data) + bytes(len(data) % 2)))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    data[off:off + 4] = ((total + len(data)) & 0xFFFFFFFF).to_bytes(4, "little")
    path.write_bytes(data)


def link_target(t, debug, objs, dep_objs):
    outdir = BUILD / ("debug" if debug else "release") / "bin"
    outdir.mkdir(parents=True, exist_ok=True)
    out = outdir / t["out"]
    res_objs = []
    for rc in t.get("rc", []):
        res = BUILD / ("debug" if debug else "release") / "obj" / t["name"] / (Path(rc).stem + ".res.o")
        res.parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run([str(WINDRES), "-I", str(ROOT / "src"), "-D", f"COOP_FILEVERSION={file_version()}",
                            str(ROOT / rc), "-O", "coff", "-o", str(res)], cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout + r.stderr)
            return False
        res_objs.append(res)
    all_objs = objs + dep_objs + res_objs
    newest = max((o.stat().st_mtime for o in all_objs), default=0)
    listing = out.with_name(out.name + ".objs")  # what it was linked from: a build --with and one without differ
    linked = "\n".join(sorted(str(o) for o in all_objs))
    if out.exists() and out.stat().st_mtime >= newest and listing.exists() and listing.read_text() == linked:
        return True
    pdb = out.with_suffix(".pdb")
    cmd = [str(CXX), "-m32", "-static", "-g", "-gcodeview", f"-Wl,--pdb={pdb}", "-Wl,--gc-sections",
           "-o", str(out)] + t.get("ldflags", []) + [str(o) for o in all_objs] + t["libs"]
    if not debug:
        cmd.insert(1, "-Wl,--strip-debug")  # the DWARF sections (/4, /67...): the PDB holds what crash reports need
    if t["kind"] == "dll":
        cmd.insert(1, "-shared")
    if t.get("gui"):
        cmd.insert(1, "-mwindows")
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        print(f"  [{t['name']}] link FAILED")
        return False
    if not debug:
        set_pe_checksum(out)
    listing.write_text(linked)
    print(f"  [{t['name']}] -> {out.relative_to(ROOT)}")
    return True


def assemble_dist(debug):
    """dist/SA-COOP mirrors the install layout (copied over the game folder) + a zip of it."""
    src = BUILD / ("debug" if debug else "release") / "bin"
    dist = ROOT / "dist" / "SA-COOP"
    shutil.rmtree(dist, ignore_errors=True)
    (dist / "coop").mkdir(parents=True)
    shutil.copy2(src / "coop.dll", dist / "coop.asi")  # any ASI loader picks up *.asi
    for name in ("coop.pdb", "coop-server.exe"):
        if (src / name).exists():
            shutil.copy2(src / name, dist / "coop" / name)
    (dist / "coop" / "loader").mkdir()
    shutil.copy2(src / "vorbisFile.dll", dist / "coop" / "loader" / "vorbisFile.dll")  # install.bat puts it in place
    for f in (ROOT / "package").rglob("*"):
        if f.is_file():
            dst = dist / f.relative_to(ROOT / "package")
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, dst)
    licenses = dist / "coop" / "licenses"  # the binaries carry ENet and MinHook: their notices go along
    licenses.mkdir()
    shutil.copy2(ROOT / "LICENSE", licenses / "SA-COOP.txt")
    shutil.copy2(ROOT / "third_party" / "enet" / "LICENSE", licenses / "ENet.txt")
    shutil.copy2(ROOT / "third_party" / "minhook" / "LICENSE.txt", licenses / "MinHook.txt")
    shutil.copy2(ROOT / "third_party" / "fontawesome" / "LICENSE.txt", licenses / "FontAwesome.txt")  # the microphone icon
    archive = shutil.make_archive(str(ROOT / "dist" / "SA-COOP"), "zip", dist)
    print(f"dist assembled in {dist.relative_to(ROOT)} and {Path(archive).relative_to(ROOT)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", nargs="*", default=[])
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--with", dest="extra", action="append", default=[], metavar="DIR")
    args = ap.parse_args()
    if args.clean:
        for out in ("release", "debug"):  # only the output: build/ may hold other local folders
            shutil.rmtree(BUILD / out, ignore_errors=True)
        print("cleaned")
        return 0
    if not CXX.exists():
        print(f"toolchain not found: {CXX}\nrun: powershell -ExecutionPolicy Bypass -File tools/get-toolchain.ps1")
        return 1
    extra = [str(p.resolve()) for d in args.extra for p in sorted(Path(d).glob("**/*.cpp"))]
    all_t = targets(extra)
    want = [w for w in args.what if w != "dist"] or list(all_t.keys())
    compdb = []
    ok = True
    built_deps = {}
    with futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for name in want:
            t = all_t[name]
            if not t["sources"]:
                print(f"  [{name}] no sources yet, skipped")
                continue
            dep_objs = []
            for d in t.get("deps", []):
                if d["name"] not in built_deps:
                    good, objs = compile_target(d, args.debug, pool, compdb)
                    ok &= good
                    built_deps[d["name"]] = objs
                dep_objs += built_deps[d["name"]]
            good, objs = compile_target(t, args.debug, pool, compdb)
            ok &= good
            if good and ok:
                ok &= link_target(t, args.debug, objs, dep_objs)
    (ROOT / "compile_commands.json").write_text(json.dumps(compdb, indent=1))
    if ok and "dist" in args.what:
        assemble_dist(args.debug)
    print("BUILD OK" if ok else "BUILD FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
