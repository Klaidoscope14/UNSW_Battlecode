#!/usr/bin/env python3
"""make_zip.py NAME [--single] : build the submission zip fleet-NAME.zip (repo root) from fleet/.

Default: bot.toml + main.cpp + every fleet/*.hpp at the top level of the zip. This is the layout the docs specify
for C++: `include = [ "*.cpp", "*.hpp", ... ]`, and the zip's directory is on the include path.
--single: the same code as ONE main.cpp. Every `#include "x.hpp"` except helper.hpp is pasted in place, so the zip
holds bot.toml + main.cpp + helper.hpp, like the v2.x zips.

Either way, check it as the docs say: unzip into an empty directory and play a --sandbox game from there
(this script does that and prints the result)."""
import os, re, subprocess, sys, tempfile, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "fleet")


def amalgamate(path, seen):
    out = []
    for line in open(path).read().split("\n"):
        m = re.match(r'\s*#include "([^"]+)"', line)
        if m and m.group(1) != "helper.hpp":
            inc = os.path.join(SRC, m.group(1))
            if inc not in seen:
                seen.add(inc)
                out.append(f"// ===== {m.group(1)} =====")
                out += [l for l in amalgamate(inc, seen) if l.strip() != "#pragma once"]
            continue
        out.append(line)
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    name, single = sys.argv[1], "--single" in sys.argv
    out = os.path.join(ROOT, f"fleet-{name}.zip")
    files = {"bot.toml": open(os.path.join(SRC, "bot.toml")).read()}
    if single:
        files["main.cpp"] = "\n".join(amalgamate(os.path.join(SRC, "main.cpp"), set()))
        files["helper.hpp"] = open(os.path.join(SRC, "helper.hpp")).read()
    else:
        for f in sorted(os.listdir(SRC)):
            if f == "main.cpp" or f.endswith(".hpp"):
                files[f] = open(os.path.join(SRC, f)).read()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for f, text in files.items():
            z.writestr(f, text)
    size = os.path.getsize(out)
    print(f"{out}: {len(files)} files, {size / 1024:.0f} KB" + ("  TOO BIG (4 MB limit)" if size > 4 << 20 else ""))
    # The docs' check: unzip into an empty directory and play a sandbox game (the judge's compiler and meter).
    env = dict(os.environ, PATH=os.path.expanduser("~/.local/bin") + ":" + os.environ["PATH"], UNSWBC_NO_UPDATE="1")
    with tempfile.TemporaryDirectory() as tmp:
        zipfile.ZipFile(out).extractall(tmp)
        r = subprocess.run(["unswbc", "run", "maps/default_small.map", tmp, os.path.join(ROOT, "tools/testing/sparring/vib2"),
                            "--seed", "1", "--no-replay", "--sandbox"], cwd=ROOT, env=env, capture_output=True, text=True, timeout=900)
    txt = r.stdout + r.stderr
    res = [l for l in txt.splitlines() if re.search(r"wins|draw|team A points|error|fail", l, re.I)]
    print("sandbox check from the unzipped files:\n  " + "\n  ".join(res[:4]))
    if not re.search(r"team A wins|team B wins|draw", txt):
        sys.exit("CHECK FAILED - do not submit")


main()
