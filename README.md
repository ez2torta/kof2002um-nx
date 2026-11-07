<h1 align=center>The King of Fighters 2002 Unlimited Match — port para Nintendo Switch</h1>

Port nativo para Nintendo Switch de la versión de Xbox 360 (XBLA) de **The King of Fighters 2002 Unlimited Match**.

No es un emulador. El código PowerPC del `default.xex` del juego se recompila estáticamente a C++ con
[ReXGlue](https://github.com/rexglue/rexglue-sdk), se compila para la Switch con devkitPro/libnx, y el juego se
dibuja con un backend Vulkan que corre sobre NVK (Mesa).

> [!NOTE]
> Este repositorio **no trae datos del juego ni código derivado del juego**. Necesitas tu propia copia del juego de
> Xbox 360 (el paquete XBLA). La [página instaladora](https://tbittech.github.io/kof2002um-nx-installer/) la
> convierte en los archivos que necesita la Switch.

## Estado

En desarrollo. En dock, con los relojes de fábrica de la consola y sin overclock (CPU 1020 MHz, GPU 768 MHz,
memoria 1600 MHz), una pelea completa va a ~52–56 FPS (el objetivo son 60 estables). Por debajo de 60 el juego va
un poco más lento de lo normal, porque su lógica avanza un paso por cada cuadro dibujado.

**Problemas conocidos:** el modo portátil todavía no se midió.

**Arreglado (2026-11-07):** la pantalla de selección de personajes salía negra (sin grilla, fondo ni cursores), y
faltaban el fondo y la interfaz de la pantalla de orden. Ahora se ven bien, sin costo de rendimiento en la pelea.

El registro detallado está en [PORT_STATUS.md](PORT_STATUS.md) (lo más nuevo arriba).

## Instalar en la Switch

La forma más fácil es la **página instaladora: <https://tbittech.github.io/kof2002um-nx-installer/>**. Le das tu
paquete XBLA (o la carpeta extraída de él) y arma `kof2002um-nx.zip` con el NRO, el `.toml` y los archivos del juego,
todo dentro de tu navegador: tu copia nunca se sube a ningún lado. Descomprímelo en `sdmc:/switch/` y queda así:

```text
/switch/kof2002um-nx/
  kof2002um-nx.nro
  kof2002um_nx.toml
  game.nxpack + game.nxidx   (los archivos del juego, hechos a partir de tu copia)
```

Ábrelo en modo aplicación completa (manteniendo **R** al abrir un juego instalado, o con un forwarder). El modo
applet (Álbum) no da memoria suficiente.

## Estructura

| Ruta | Qué es |
|---|---|
| `src/recompiler/` | Manifiesto de ReXGlue, proyecto CMake y el código propio del port (hooks, soporte de autotest/bench) |
| `tools/rexglue-sdk/` | SDK de ReXGlue con el runtime de Switch: capa Horizon, cambios al renderer Vulkan/NVK, contadores de rendimiento |
| `tools/fetch_thirdparty.py` | Descarga las dependencias del SDK en sus commits fijados (`thirdparty_pins.txt`) |
| `tools/kof_stfs_extract.py` | Extrae el paquete STFS de XBLA, incluso con un bloque faltante (ubica los bloques por SHA-1) |
| `tools/xctd/` | Descomprime los archivos XCTD (LZX) del juego |
| `tools/ghidra-scripts/` | Scripts de Ghidra headless usados para la ingeniería inversa |
| `switch/kof2002um_nx.toml` | Configuración del runtime que va junto al NRO (el set actual orientado a 60 FPS) |
| `bench/devkitnx-configs/` | Configuraciones del benchmark automático en la consola (devkitnx) |

## Compilar (resumen)

1. Instala devkitPro (devkitA64 + libnx) y descarga las dependencias del SDK: `python tools/fetch_thirdparty.py`.
2. Extrae tu paquete XBLA: `python -I tools/kof_stfs_extract.py <paquete> game/`, y después descomprime los
   archivos XCTD con `tools/xctd/`.
3. Corre el codegen de ReXGlue sobre tu `game/default.xex` con `src/recompiler/kof2002um_nx_manifest.toml`
   (la salida va a `src/recompiler/generated/`, que nunca se sube al repo).
4. Configura y compila `src/recompiler` con el preset de Switch de `CMakePresets.json`.

## Créditos

- [ReXGlue](https://github.com/rexglue/rexglue-sdk) y el proyecto [Xenia](https://github.com/xenia-project/xenia),
  de cuyo código de GPU y kernel deriva el runtime.
- [nfsmw-nx](https://github.com/StevensND/nfsmw-nx), otro port de ReXGlue a la Switch.
- devkitPro, libnx y Mesa/NVK.

## Apoyo

Si te gusta mi trabajo y quieres apoyarme:

<a href="https://tecito.app/tebit"><img src="https://img.shields.io/badge/Invitame_un-tecito-6d4aff?style=for-the-badge" alt="Invitame un tecito" height="32"></a>
