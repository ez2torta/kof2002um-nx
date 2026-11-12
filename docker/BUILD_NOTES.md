# Cómo se llegó a este entorno de build (bitácora)

Fecha: 2026-11-11. Objetivo: compilar `kof2002um-nx.nro` en Docker a partir de este repo, con el `default.xex` del XBLA.
Esta bitácora cuenta qué faltaba, cómo se descubrió y por qué cada parche está donde está. Lo que sigue **no** es verdad
de fábrica del repo: son hallazgos de esta sesión.

## Resultado

| Paso | Estado |
|---|---|
| Imagen `toolchain` (devkitA64 + libnx) | Funciona |
| Codegen de `default.xex` con el `rexglue` del SDK del repo | Funciona (349 archivos, ~180 MB de C++) |
| Compilar todo el código recompilado + SDK para Switch | Funciona |
| Link final | Necesita `libvulkan.a` de Mesa/NVK (servicio `mesa`) |
| Probado en consola | **No** |

## Orden de uso

```
docker compose build
docker compose run --rm codegen   # compila rexglue del SDK del repo y genera src/recompiler/generated/
docker compose run --rm mesa      # compila Mesa/NVK -> tools/mesa-switch-sdk
docker compose run --rm nx        # fetch + configure + build -> kof2002um-nx.nro
```

`GAME_DIR` (por defecto `/media/sda1/Games/x360recomp/extracted/kof2002um`) debe contener `default.xex`.

## Cronología de problemas y cómo se resolvieron

1. **No había nada para compilar.** Faltaban toolchain (devkitPro, cmake, clang), datos de Xbox 360 y espacio. Los datos
   estaban en `x360recomp/extracted/kof2002um/default.xex`; el toolchain se resolvió con Docker.
2. **`fetch_thirdparty.py` no alcanza.** El `.gitignore` del repo ignora `tools/rexglue-sdk/thirdparty/*/`, así que
   nunca se subieron 11 carpetas propias del SDK (`disasm`, `ffmpeg-overlay`, `aes_128`, `tiny-aes-c`, `crypto`, `dxbc`,
   `dxc`, `pe`, `stb`, `renderdoc`, `disruptorplus`). Síntoma: `add_subdirectory given source "disasm" which is not an
   existing directory`. Solución: `build.sh fetch` las copia de `rexglue/rexglue-sdk` @ `c94f5eb`, el commit base que
   declara `tools/rexglue-sdk/README.md`. (GitHub no sirve un SHA abreviado con `--depth 1`; hay que hacer fetch completo.)
3. **El `rexglue` precompilado (0.10.0) no sirve.** Genera código que no compila: `label 'loc_82A7B750' used but not
   defined`. El manifiesto usa `parent` y `share_registers`, que solo entiende el codegen del fork. Solución: compilar
   el `rexglue` del SDK del repo.
4. **Compilar ese `rexglue`:**
   - libstdc++ de Debian 12 no tiene `std::chrono::clock_time_conversion` → etapa `codegen` basada en Debian 13 (clang 19).
   - `-march=x86-64-v2`: el SDK usa SSSE3 y clang no lo activa por defecto (igual que el preset `linux-base`).
   - `-DAUTOTEST_DISABLE`: `librexruntime` referencia `at_arg/at_log/at_frame`, que el SDK solo compila para Switch.
   - El binario queda en `tools/rexglue-sdk/out/linux-amd64/rexglue`, no en el directorio de build.
5. **Configure de Switch.** `generated/rexglue.cmake` hace otro `add_subdirectory` del SDK (ya agregado por el proyecto)
   y usa `$<TARGET_FILE:rex::rexglue>`, que no existe en Switch. `build.sh` lo parchea (idempotente).
6. **FFmpeg no tiene config para Switch.** `FFmpeg/config.h` termina en `#error "no config"`. El fork original tiene
   cambios propios para esto (config + tablas FFT/MDCT, ver `tools/rexglue-sdk/README.md`) que **no están publicados**.
   Aproximación: `config_switch_aarch64.h` = linux-aarch64 con `HAVE_MMAP/SYSCTL/GETAUXVAL/POSIX_MEMALIGN...=0` y
   `HAVE_MALLOC_H=1`. Compila; **no está probado en consola** y puede afectar el audio XMA.
7. **Link.** `tools/switch/cmake/switch-devkitA64.cmake` tenía `D:/devkitPro/libnx/switch.ld` escrito a mano. Ahora usa
   `${DEVKITPRO}`. (Cambio en un archivo del repo, no solo en el contenedor.)
8. **Mesa/NVK.** El repo exige `REXGLUE_SWITCH_NVK_SDK/lib/libvulkan.a` y no lo trae. La referencia
   [`nfsmw-nx`](https://github.com/StevensND/nfsmw-nx) (commit `20f8dc1`) publica un parche sobre
   [`danfromtico/mesa-switch`](https://github.com/danfromtico/mesa-switch) (commit `1a8c1a66d6f`, Mesa 26.2.1) y un build
   para Linux (`mesa/linux/build.sh`). El servicio `mesa` lo reproduce: Ubuntu 24.04 (LLVM 18, libclc,
   SPIRV-LLVM-Translator), devkitPro copiado de la imagen oficial, Meson ≥ 1.4, Rust estable con target
   `aarch64-unknown-linux-gnu`, `bindgen-cli 0.72.1` y `cbindgen`.

## Diferencias con el Mesa del autor original

El PORT_STATUS de este repo describe un Mesa 26.2.3 con un parche propio de NAK (XMAD) y otros cambios que **no están en
ningún repositorio público**. El `libvulkan.a` que sale de aquí es el de nfsmw-nx (26.2.1 + su parche): sirve para
linkear y probablemente para correr, pero no es idéntico al que usó el autor para medir rendimiento.

## Recursos

Compilar Mesa y el SDK es pesado: se recomiendan ~16 GB de RAM (esta máquina tiene 7 GB). `JOBS` limita los hilos
(`ninja` está envuelto en la imagen de Mesa para respetarlo). Si el kernel mata procesos por OOM, bajar `JOBS` a 1–2.
