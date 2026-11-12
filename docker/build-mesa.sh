#!/usr/bin/env bash
# Compila el Mesa/NVK del port y deja el SDK en tools/mesa-switch-sdk (lib/libvulkan.a).
set -euo pipefail
MESA_COMMIT=1a8c1a66d6f          # mesa-switch, la base de nfsmw-nx
NFS_COMMIT=20f8dc1               # nfsmw-nx: parche y scripts de build para Linux
SRC=/work/tools/mesa-switch-src
REF=/work/out/nfsmw-nx
OUT=/work/tools/mesa-switch-sdk

[ -d "$REF/.git" ] || git clone -q https://github.com/StevensND/nfsmw-nx.git "$REF"
git -C "$REF" checkout -q "$NFS_COMMIT"
[ -d "$SRC/.git" ] || git clone -q https://github.com/danfromtico/mesa-switch.git "$SRC"
git -C "$SRC" config --global --add safe.directory "$SRC" || true
if ! git -C "$SRC" apply --check -R "$REF/mesa/mesa-switch-nfsmw.patch" 2>/dev/null; then
  git -C "$SRC" checkout -q -f "$MESA_COMMIT"
  git -C "$SRC" apply "$REF/mesa/mesa-switch-nfsmw.patch"
  echo ">> parche de nfsmw-nx aplicado"
fi
export LLVM_CONFIG=${LLVM_CONFIG:-llvm-config-18}
bash "$REF/mesa/linux/build.sh" "$SRC"
mkdir -p "$OUT"
cp -a "$SRC/mesa-linux-install/opt/devkitpro/portlibs/switch/." "$OUT/"
ls -la "$OUT/lib/libvulkan.a"
