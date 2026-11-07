"""Copia a una carpeta aparte todo lo que se puede publicar del port, sin nada derivado del juego.

Uso: python -I tools/export_github.py [destino]   (por defecto D:/Ports Swtch/kof2002um-nx-github)

Lo que NO se copia nunca: game/, romfs/, src/recompiler/generated/ (código traducido de default.xex),
los .nro/.elf/.nacp (llevan ese código compilado), .nxpack/.nxidx, el proyecto de Ghidra, cache/, logs,
out/ y los backups .bak*. Las dependencias del SDK (thirdparty/) van como lista de commits fijados y
tools/fetch_thirdparty.py las baja.
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent          # kof2002um-nx/
PORTS = ROOT.parent                                     # D:/Ports Swtch
DEST = Path(sys.argv[1]) if len(sys.argv) > 1 else PORTS / "kof2002um-nx-github"
SDK = ROOT / "tools" / "rexglue-sdk"

BANNED_SUFFIXES = {".nro", ".elf", ".nacp", ".nxpack", ".nxidx", ".xex", ".log", ".exe", ".o", ".a"}
copied = []


def ok(path: Path) -> bool:
    name = path.name
    return ".bak" not in name and path.suffix.lower() not in BANNED_SUFFIXES and path.is_file()


def put(src: Path, rel: str):
    if not ok(src):
        return
    dst = DEST / rel
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    copied.append(rel)


def tree(src_dir: Path, rel_dir: str, patterns=("*",)):
    for pat in patterns:
        for p in src_dir.rglob(pat):
            if p.is_file():
                put(p, f"{rel_dir}/{p.relative_to(src_dir).as_posix()}")


def git(*args, cwd=SDK):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, check=True).stdout


if DEST.exists():
    # Solo se borra el contenido exportado; la carpeta .git del destino se conserva.
    for child in DEST.iterdir():
        if child.name != ".git":
            shutil.rmtree(child) if child.is_dir() else child.unlink()
DEST.mkdir(parents=True, exist_ok=True)

# 1. La app: manifiesto, CMake y el código propio (hooks del port, bench).
rec = ROOT / "src" / "recompiler"
for f in ("CMakeLists.txt", "CMakePresets.json", "kof2002um_nx_manifest.toml"):
    put(rec / f, f"src/recompiler/{f}")
tree(rec / "src", "src/recompiler/src")

# 2. El SDK de ReXGlue modificado: archivos versionados + los nuevos, sin submódulos ni out/.
gitlinks = {}
for line in git("ls-files", "-s").splitlines():
    meta, path = line.split("\t", 1)
    mode, sha = meta.split()[:2]
    if mode == "160000":
        gitlinks[path] = sha
files = set(git("ls-files").splitlines()) | set(git("ls-files", "-o", "--exclude-standard").splitlines())
for rel in sorted(files):
    if rel in gitlinks or rel.startswith("out/") or rel.startswith("thirdparty/") and rel.count("/") > 1:
        continue
    put(SDK / rel, f"tools/rexglue-sdk/{rel}")
urls = {}
cur = None
for line in (SDK / ".gitmodules").read_text(encoding="utf-8").splitlines():
    line = line.strip()
    if line.startswith("path ="):
        cur = line.split("=", 1)[1].strip()
    elif line.startswith("url =") and cur:
        urls[cur] = line.split("=", 1)[1].strip()
pins = DEST / "tools" / "rexglue-sdk" / "thirdparty_pins.txt"
pins.parent.mkdir(parents=True, exist_ok=True)
pins.write_text("".join(f"{p} {urls.get(p, '?')} {s}\n" for p, s in sorted(gitlinks.items())), encoding="utf-8")
copied.append("tools/rexglue-sdk/thirdparty_pins.txt")

# 3. Herramientas propias.
put(ROOT / "tools" / "kof_stfs_extract.py", "tools/kof_stfs_extract.py")
put(ROOT / "tools" / "export_github.py", "tools/export_github.py")
put(ROOT / "tools" / "fetch_thirdparty.py", "tools/fetch_thirdparty.py")
tree(ROOT / "tools" / "ghidra-scripts", "tools/ghidra-scripts", ("*.java", "*.py"))
tree(ROOT / "tools" / "xctd", "tools/xctd", ("*.c", "*.h", "*.py"))
tree(ROOT / "tools" / "switch", "tools/switch")

# 4. Documentación, estado y configuración de la Switch.
tree(ROOT / "docs", "docs")
put(ROOT / "PORT_STATUS.md", "PORT_STATUS.md")
put(ROOT / "switch" / "kof2002um-nx-60fps" / "kof2002um_nx.toml", "switch/kof2002um_nx.toml")

# 5. Configs del bench automático (devkitnx).
tree(PORTS / "devkitnx" / "configs", "bench/devkitnx-configs", ("_nl_*.json",))

# 6. Archivos de la raíz del repo (README, .gitignore, avisos) viven en tools/github/.
tree(ROOT / "tools" / "github", ".")

# Control final: nada grande ni con extensión prohibida.
big = [r for r in copied if (DEST / r).exists() and (DEST / r).stat().st_size > 5 * 1024 * 1024]
print(f"{len(copied)} archivos copiados a {DEST}")
if big:
    print("REVISAR, archivos de más de 5 MB:", *big, sep="\n  ")
