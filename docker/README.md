# Build en Docker

```
docker compose build
GAME_DIR=/ruta/a/carpeta-con-default.xex docker compose run --rm codegen   # compila el rexglue del SDK y genera el C++ (~10 min + compilación del SDK)
MESA_SDK=/ruta/a/<mesa-...-horizon-sdk>/opt/devkitpro/portlibs/switch docker compose run --rm nx   # fetch + configure + build
```

- **Mesa Horizon SDK (NVK estático)**: tiene que existir `lib/libvulkan.a` en `MESA_SDK` (por defecto `tools/mesa-switch-sdk/`). No está en el repo.
- Pasos sueltos: `docker compose run --rm nx fetch|configure|build|shell`. Ajusta `JOBS` según la RAM.
- `fetch` completa lo que el `.gitignore` del repo deja fuera de `tools/rexglue-sdk/thirdparty/` (carpetas propias del SDK, tomadas de
  rexglue-sdk @ c94f5eb) y agrega a `FFmpeg/config.h` una rama `__SWITCH__` basada en la config de linux-aarch64: **es una aproximación**,
  los cambios reales del fork para FFmpeg/Switch no están publicados.
- `configure` parchea `generated/rexglue.cmake` (SDK ya agregado por el proyecto; sin target `rex::rexglue` en Switch).
