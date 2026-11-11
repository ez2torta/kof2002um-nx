#!/usr/bin/env bash
# Uso: kof-build [fetch|codegen|configure|build|all|shell]
# Variables: GAME_DIR (carpeta con default.xex), NVK_SDK (portlibs/switch del Mesa Horizon SDK),
#            REXGLUE_BIN (opcional: binario rexglue ya hecho; si no, se compila el del SDK del repo),
#            JOBS (hilos de compilación).
set -euo pipefail
REPO=/work
SDK=$REPO/tools/rexglue-sdk
REC=$REPO/src/recompiler
GAME_DIR=${GAME_DIR:-/game}
NVK_SDK=${NVK_SDK:-/mesa-sdk/opt/devkitpro/portlibs/switch}
JOBS=${JOBS:-$(nproc)}
OUT=${OUT:-$REPO/out}
HOSTBUILD=$OUT/host-rexglue
SWBUILD=$OUT/switch
cd "$REPO"

step_fetch() {
  python3 -I tools/fetch_thirdparty.py
  # Varias carpetas de thirdparty/ son del propio SDK de ReXGlue (disasm, ffmpeg-overlay, tiny-aes-c, aes_128, ...), pero el
  # .gitignore del port ignora todo tools/rexglue-sdk/thirdparty/*/, así que el repo no las trae. Se completan desde el
  # upstream en el commit base del fork (las que ya existen no se tocan).
  local T=$SDK/thirdparty up=c94f5eb tmp
  tmp=$(mktemp -d)
  git -C "$tmp" init -q && git -C "$tmp" remote add origin https://github.com/rexglue/rexglue-sdk.git
  git -C "$tmp" fetch -q origin   # fetch completo: GitHub no sirve un SHA abreviado con --depth
  git -C "$tmp" checkout -q "$up" -- thirdparty 2>/dev/null || true   # los submódulos quedan vacíos; solo se copian los que faltan
  for d in "$tmp"/thirdparty/*/; do
    n=$(basename "$d")
    [ -e "$T/$n" ] || { echo ">> thirdparty/$n (de upstream)"; cp -r "$d" "$T/$n"; }
  done
  rm -rf "$tmp"
  # El fork de ez2torta cambió FFmpeg para Switch (config_*.h, tablas FFT/MDCT), pero esos cambios no están en el repo.
  # Aproximación: usar la config de linux-aarch64 para __SWITCH__ (newlib/libnx tiene pthreads y libm C99).
  local C=$T/FFmpeg/config.h
  if [ -f "$C" ]; then
    # config_switch_aarch64.h = linux-aarch64 sin lo que Horizon/newlib no tiene (mmap, sysctl, getauxval, ...)
    sed -E 's/^(#define (HAVE_MMAP|HAVE_MPROTECT|HAVE_SYS_MMAN_H|HAVE_SYSCTL|HAVE_GETAUXVAL|HAVE_SYS_AUXV_H|HAVE_SCHED_GETAFFINITY|HAVE_SYS_RESOURCE_H|HAVE_GETRUSAGE|HAVE_ISATTY|HAVE_GETADDRINFO|HAVE_SYS_SELECT_H|HAVE_POLL_H|HAVE_PTHREAD_CANCEL|HAVE_SETRLIMIT|HAVE_ARC4RANDOM|HAVE_SYSCONF|HAVE_POSIX_MEMALIGN)) 1/\1 0/; s/^(#define HAVE_MALLOC_H) 0/\1 1/' "$T/FFmpeg/config_linux_aarch64.h" > "$T/FFmpeg/config_switch_aarch64.h"
  fi
  if [ -f "$C" ] && ! grep -q __SWITCH__ "$C"; then
    sed -i 's|^#elif defined(__ANDROID__)|#elif defined(__SWITCH__)\n#include "config_switch_aarch64.h"\n#elif defined(__ANDROID__)|' "$C"
    echo ">> FFmpeg/config.h: rama __SWITCH__ -> config_linux_aarch64.h"
  fi
}

step_game_link() {
  [ -f "$GAME_DIR/default.xex" ] || { echo "ERROR: no hay $GAME_DIR/default.xex (monta la carpeta extraída del XBLA)"; exit 2; }
  mkdir -p "$REPO/game"
  ln -sfn "$GAME_DIR" "$REPO/game/xbla-extracted"   # el manifiesto usa ../../game/xbla-extracted
}

step_codegen() {
  step_game_link
  local bin=${REXGLUE_BIN:-}
  if [ -z "$bin" ] || [ ! -x "$bin" ]; then
    echo ">> compilando rexglue (codegen) del SDK del repo"
    cmake -S "$SDK" -B "$HOSTBUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=${HOST_CC:-clang} -DCMAKE_CXX_COMPILER=${HOST_CXX:-clang++} \
      -DCMAKE_C_FLAGS="-march=x86-64-v2 -DAUTOTEST_DISABLE" -DCMAKE_CXX_FLAGS="-march=x86-64-v2 -DAUTOTEST_DISABLE" -DCMAKE_CXX_STANDARD=23 \
      -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_BUILD_TESTS=OFF
    cmake --build "$HOSTBUILD" --target rexglue -j"$JOBS"
    bin=$(find "$HOSTBUILD" "$SDK/out" -type f -name rexglue -perm -u+x 2>/dev/null | head -1)
  fi
  echo ">> codegen con $bin"
  ( cd "$REC" && "$bin" codegen kof2002um_nx_manifest.toml )
  [ -f "$REC/generated/rexglue.cmake" ] || { echo "ERROR: el codegen no dejó generated/rexglue.cmake"; exit 3; }
}

# generated/rexglue.cmake vuelve a hacer add_subdirectory del SDK, que CMakeLists.txt ya agregó en Switch
# ("binary directory already used"). Se parchea (idempotente) para que lo salte si el SDK ya está cargado.
patch_generated_cmake() {
  python3 - "$REC/generated/rexglue.cmake" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old='if(REXSDK_DIR)\n    add_subdirectory("${REXSDK_DIR}" rexglue-sdk)'
new='if(TARGET rex::runtime)\n    message(STATUS "ReXGlue SDK already added by the project")\nelseif(REXSDK_DIR)\n    add_subdirectory("${REXSDK_DIR}" rexglue-sdk)'
if old in s and 'already added by the project' not in s:
    s=s.replace(old,new,1); print('patched sdk guard')
a=s.find('add_custom_command(\n    OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp"')
b=s.find('# Include DLL module')
if a>=0 and b>a and 'if(TARGET rex::rexglue)' not in s:
    blk=s[a:b]
    s=s[:a]+'if(TARGET rex::rexglue)\n'+blk+'else()\n    # Switch: no host tools; the codegen runs in its own container step.\n    add_custom_target(kof2002um_nx_codegen)\nendif()\n\n'+s[b:]
    print('patched codegen target')
open(p,'w').write(s)
PY
}

step_configure() {
  patch_generated_cmake
  [ -f "$REC/generated/rexglue.cmake" ] || { echo "Falta el codegen: corre 'kof-build codegen'"; exit 3; }
  [ -f "$NVK_SDK/lib/libvulkan.a" ] || { echo "ERROR: no hay $NVK_SDK/lib/libvulkan.a (Mesa Horizon SDK; ver docker/README.md)"; exit 4; }
  cmake -S "$REC" -B "$SWBUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$REPO/tools/switch/cmake/switch-devkitA64.cmake" \
    -DREXGLUE_SWITCH_NVK_SDK="$NVK_SDK"
}

step_build() {
  cmake --build "$SWBUILD" -j"$JOBS" -- -k 0
  ls -la "$REPO"/kof2002um*.nro 2>/dev/null || echo "(no apareció el .nro en $REPO)"
}

case "${1:-all}" in
  fetch) step_fetch ;;
  codegen) step_fetch; step_codegen ;;
  configure) step_configure ;;
  build) step_build ;;
  all) step_fetch; [ -f "$REC/generated/rexglue.cmake" ] || { echo 'Falta el codegen: corre primero  docker compose run --rm codegen'; exit 3; }; step_configure; step_build ;;
  shell) exec bash ;;
  *) exec "$@" ;;
esac
