"""Downloads the ReXGlue SDK dependencies (tools/rexglue-sdk/thirdparty/) at the commits pinned in
tools/rexglue-sdk/thirdparty_pins.txt. Usage: python tools/fetch_thirdparty.py
"""
import subprocess
from pathlib import Path

SDK = Path(__file__).resolve().parent / "rexglue-sdk"
for line in (SDK / "thirdparty_pins.txt").read_text(encoding="utf-8").splitlines():
    path, url, sha = line.split()
    dst = SDK / path
    if (dst / ".git").exists():
        print(f"ok    {path}")
        continue
    print(f"fetch {path} @ {sha[:10]}")
    dst.mkdir(parents=True, exist_ok=True)
    run = lambda *a: subprocess.run(["git", *a], cwd=dst, check=True)
    run("init", "-q")
    run("remote", "add", "origin", url)
    run("fetch", "-q", "--depth", "1", "origin", sha)
    run("checkout", "-q", "FETCH_HEAD")
    run("submodule", "update", "-q", "--init", "--recursive", "--depth", "1")
