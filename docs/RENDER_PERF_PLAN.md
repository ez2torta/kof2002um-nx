# Rendimiento en portátil sin overclock: hallazgos y plan

Documento de trabajo de la sesión del 2026-11-11. Para el camino de fondo (renderer propio) ver `docs/RENDERER_DESIGN.md`. Reúne lo medido, lo descartado, el estado del código y el plan de
cambios para llegar a 60 fps estables en portátil con los relojes de fábrica (CPU 1020 / GPU 460,8 / EMC 1331 MHz)
y, más adelante, a un costo de 6–8 ms por frame con un renderer propio.

Todas las cifras salen de `logs/rex/rex_profile.log` de la consola (reporte cada 10 s), a menos que se diga otra cosa.
Las hipótesis sin confirmar están marcadas como tales.

---

## 1. Resumen

| Métrica (portátil, relojes de fábrica) | Inicio de la sesión | Ahora |
|---|---|---|
| fps en juego normal | 31–34 | **43–48** (35–36 en escenas pesadas) |
| Frames con tiempo ≤ 18 ms | 20–28 % | **65–80 %** (30–34 % en pesadas) |
| GPU ocupada por frame | 27–29,5 ms | **18,3–19,6 ms** (21,8 en pesadas) |
| Transferencias de render targets | 8,5–8,8 ms | **0,13–0,20 ms** |
| Resolves (9 copias por frame) | 3,9–4,2 ms | 3,6–3,9 ms (sin cambio) |

Lo que sigue pesando (por frame): "dibujo" ~14 ms y resolves ~3,7 ms. Para cruzar de forma estable el umbral de
16,7 ms hacen falta unos 3–5 ms menos de GPU en las escenas medias y ~6 ms en las pesadas.

El juego **está limitado por la GPU** (no por la CPU). La cuantización por vsync hace que un frame que tarda un poco
más de 16,7 ms espere al siguiente vblank y dure 33,3 ms, por eso los frames caen en dos grupos (≤18 ms o 25–35 ms) y
casi ninguno entre medio.

---

## 2. Cómo se midió

### 2.1 Reporte en la consola

`tools/rexglue-sdk/src/ui/switch_perf.cpp` escribe cada 10 s en `/switch/kof2002um-nx/logs/rex/rex_profile.log`:

- `frame times`: histograma de tiempo entre presents (≤18, 18–25, 25–35, 35–50, >50 ms).
- `fast (<=18 ms) frames` / `slow (>18 ms) frames`: draws, cargas de textura, resolves, envíos y subidas por frame de cada grupo.
- `GPU per frame`: ms de GPU por pase (`other, draw, rt_xfer, res_clear, dump, resolve, readback, tex_load, upload, swap`),
  de los timestamps de GPU (`gpu_submission_timestamps = true`).
- `CP per frame`: ms del hilo de comandos (ocioso, espera de fence, submit, texturas, `WAIT_REG_MEM`) y conteos por frame.
- `render pass ends per frame by caller`: cuántas veces corta cada llamador un render pass (direcciones `image+0x…`).
- `heap:` línea de heap (ventana, break de sbrk, arena, en uso, libre, fragmentos).
- `stage background: bgXn.bin`: último fondo de etapa que abrió el juego (`data\B\bg\bgXn.bin`), para etiquetar cada ventana.
  Se captura aparte del log `[KOF data I/O]`, que solo cubre las primeras 96 aperturas de archivo.
- `barriers pushed per frame by caller`: quién empuja las barreras pendientes (`i` = imagen, `b` = buffer), por dirección `image+0x…`.
- `texture loads per frame by path`: cuántas cargas de textura van por la carga directa por CPU y cuántas caen al camino de GPU,
  con el motivo (`gpu-written`, `format`, `tiled`, `layout`, `no-source`, `zero-palette`, `no-free-version`, `not-eligible`, `mips`,
  `no-base`) y las texturas que no entran por el camino rápido (formato, tamaño, tiled o lineal).
- `CPU texture copies: N MB per frame`: bytes que copia el CPU a texturas por frame.
- `resolves copied straight into a texture`: resolves por frame, y cuántos usan el blit con intercambio rojo/azul.
- Los logs `[rtt] resolve -> …` (resolve directo a textura) ahora también van al log de archivo.
- Con `switch_profile_stacks = true`: muestreo de pc y pila a 1 kHz por hilo (frena el juego; solo para medir).

### 2.2 Log por draw

`diag_draw_log_from/to/step` imprime una línea `[dd]` por draw (shaders, render target, blend, texturas, scissor, viewport)
en frames muestreados. Escribe al **log de archivo** además de stdout. Para los draws con `ps608256F8F99FE9DF` vuelca también
los 3 vértices (`layer vtx …`).

### 2.3 Shaders

`dump_shaders = "/switch/kof2002um-nx/shaders"` vuelca binario y **desensamblado de texto** de cada shader a esa carpeta.

### 2.4 Scripts (en `tools/perf/` y `tools/switch-ftp/`)

| Script | Para qué |
|---|---|
| `tools/switch-ftp/bajar-logs.sh` | baja `/switch/kof2002um-nx/logs` por FTP (puerto 5000) a `tools/switch-ftp/logs` |
| `tools/perf/hot.py` | agrega muestras de pc de un hilo y las agrupa por función (necesita `addr2line` de devkitA64 y el ELF sin stripear) |
| `tools/perf/hotstack.py` | cadenas de llamadas más frecuentes de un hilo (usar `"*"` como filtro) |
| `tools/perf/dd.py` | resume el log `[dd]` por frame: draws por (primitiva, shader, target, texturas) |

El ELF sin stripear (`kof2002um_nx`, en la raíz tras el build) tiene el mismo BuildID que el NRO subido; los offsets
`image+0x…` de los logs se resuelven con `aarch64-none-elf-addr2line -f -C -e kof2002um_nx 0x…`.

---

## 3. Hallazgos

### 3.1 Causas de los crashes y del modo demo (resueltos)

| Síntoma | Causa | Arreglo |
|---|---|---|
| `std::bad_alloc` en `VulkanCommandProcessor` al arrancar, y ~99 errores `XmaContext … Couldn't allocate context` | **probable** falta de heap (no se pudo confirmar: esas corridas no registraban el heap). La corrida con el heap del loader (3115 MB, "loader override") ya no los tiene | sin cambio de código; ahora se registra el heap (`[heap] before command processor`, y línea `heap:` en el reporte) |
| Excepción de CPU: salto a un puntero basura (`0x3900fa8820ce`) tras cargar `data\obj2\p40.bin` | **el pack tenía los 1171 archivos aún en el contenedor XCOMPRESS** (`0F F5 12 ED`); el juego no tiene rutina de descompresión | reconstruir `game.nxpack` desde `x360recomp/extracted/kof2002um_unpacked/data` (idéntico byte a byte a Steam). Pack nuevo: 1 092 687 436 bytes, 1260 archivos |
| Juego en modo demo | `XamContentGetLicenseMask` devuelve el cvar `license_mask` (default 0) | `license_mask = 1` en el toml |
| `abort()` por "Graphics device lost" jugando contra Chin (arcade) | el driver NVK de Switch devolvió `channel submit failed: no queue space (VK_ERROR_OUT_OF_DEVICE_MEMORY)`; antes, decenas de avisos `channel 2 hit native submission pressure … current={sub=3 entries=2294 bytes=94968}` (≈41 bytes por entrada de cola). El port trata cualquier fallo de envío como pérdida de GPU | **sin arreglo directo**. No reapareció en endless (4–5 stages) tras las optimizaciones, que bajan comandos por frame. Ver §5, P12 |

Nota sobre el pack: `docker-compose.yml` monta por defecto `extracted/kof2002um` (comprimido) como `GAME_DIR`. El pack de
la consola se construyó desde un staging (`x360recomp/staging-nxpack/game`) con `data/` tomado del árbol descomprimido.

### 3.2 El cuello de botella

- `Main XThread`: 97 % de un núcleo, pero ~90 % del tiempo **dentro de `D3DDevice_Swap`** (`sub_8265D018`) →
  `sub_82655E28` → `sub_82655B48` → el candado `sub_826571A8`: espera activa a que el hilo de GPU libere el ring buffer.
  No es trabajo de la lógica de pelea.
- Hilo `GPU Commands`: ~47 % en `WAIT_REG_MEM` (espera del vblank), ~7 % ocioso, ~14 % esperando fences de GPU. El trabajo real
  (texturas ~8 %, emulación del dibujo) es el resto.
- La GPU ocupada (27–29 ms) ≈ el tiempo de frame (29,5 ms). Todas las fases escalan con el reloj de GPU (a 921 MHz da ~14 ms).
  En dock con relojes de fábrica (768 MHz) el port ya daba ~60 fps.

### 3.3 Estructura de un frame de pelea

Reconstruida del log `[dd]` (frame 1200, 78 draws):

```
clear de profundidad 4x (base 0, pitch 640)                      \ el "truco" del clear gigante de EDRAM
clear color+profundidad 4x (base 0x5A0, pitch 320)               /
~17 sprites (ps36D6)  -> RT cb5A0, 640x448, 1x
RESOLVE RT -> 1F52B ; RESOLVE RT -> 186D2          (dos copias idénticas)
DRAW filtro   ps D104  lee 186D2  -> RT   (opaco)
RESOLVE RT -> 186D2                                  (imagen filtrada)
DRAW copia    ps 7ABD  lee 1F52B  -> RT   (opaco)    (restaura la imagen sin filtrar)
DRAW copia    ps 7ABD  lee 186D2  -> RT   (alpha blend 01610701)   (mezcla la filtrada encima)
RESOLVE RT -> 1F52B
sprites del atlas (ps6082) ...                       (personajes y efectos)
RESOLVE -> 186D2 ; filtro ; RESOLVE ; 2 copias       (segunda ronda)
más sprites, HUD
RESOLVE RT -> 185BA ; DRAW copia (ps7ABD) RT <- 185BA ; RESOLVE -> 185BA     (viaje de ida y vuelta)
DRAW ps6E57 -> backbuffer 1280x720 (escena 640x448 escalada)
DRAW ps6E57 -> backbuffer (borde lateral 350x720)
RESOLVE -> 1F6F8
```

Totales por frame: 9 resolves, 7 pasadas de composición (5 copias + 2 filtros), 73–126 draws, 20–26 render passes,
34–40 barreras, 17–19 copias, ~8 envíos, 23–50 cargas de textura.

### 3.4 Shaders

Volcados en `/switch/kof2002um-nx/shaders` (copia local: `tools/switch-ftp/shaders-dump/`, no versionada).

| Pixel shader | Qué hace | Por frame |
|---|---|---|
| `7ABDFEFBCADB4BF9` | `tfetch2D` + `mul oC0, r0, r1`: copia texturizada teñida por el color del vértice (189 B de desensamblado) | ~5,3 |
| `D1048F8A2AB44432` | **filtro de pixel art** tipo Scale2x/EPX: 9 `tfetch2D` en vecindad 3×3 y una cadena de comparaciones de igualdad entre vecinos con predicados (314 líneas) | ~2,1 |
| `6E57A6E305D5D69B` | copia al backbuffer / borde lateral (189 B) | ~2 |
| `36D6E98AD80CB14C` | sprite: índices 512x512 (k_8) + paleta 256x1 | ~31 |
| `608256F8F99FE9DF` | sprite del atlas: índices 640x448 (k_8, **misma dirección `@16F0D` reescrita por el CPU antes de cada draw**) + paleta 256x1; quad `RectList` de 3 vértices | ~21 |
| `03B1CA02E2878307`, `72388BBF02B907C1` | sprites con paletas de 128x256 | ~9,5 y ~8 |
| `A4A965C189287B99` / `0000…` | clear 4x de color+profundidad / solo profundidad | 1 y 1 |

Vértices de los draws del atlas (volcado `layer vtx`, 315 muestras): `(x, y, 0, nan, u, v)` con UV **en texels**, no
normalizados, a veces volteados horizontalmente (ej.: pantalla `x 16..112, y 103..313`, UV `u 48→0, v 0→112`). El área media del
quad es 10 499 px (3,7 % de la pantalla; solo 5 de 315 pasan del 20 %). El scissor sí es de pantalla completa en todos.

### 3.5 Desglose de GPU (línea base, antes de los cambios)

| Familia | ms por frame |
|---|---|
| Pasadas de composición (`ps7ABD` ×5 + `psD104` ×2) | 8,2–9,0 |
| Transferencias de render targets | 8,5–8,8 |
| Resolves | 3,9–4,2 |
| Sprites `ps36D6` | 2,0–3,7 |
| Sprites del atlas `ps6082` | 0,6–1,1 |
| Otros sprites (`ps03B1`, `ps7238`) | 0,0–0,4 |
| Clears + pasada final a 1280x720 | ~2,4 |

Los sprites juntos cuestan ~4 ms. El resto es emulación de EDRAM y la cadena de composición. Dato clave: cada pasada de
composición cuesta ~1 ms aunque el trabajo de píxeles y de ALU sea trivial (el filtro de 9 lecturas por píxel sobre 287 K px
no debería pasar de ~1 ms), lo que apunta a un costo fijo por pasada (resolve + barreras + corte de render pass). **Hipótesis,
pendiente de medir** con el contador de cortes de render pass (§4, P1).

### 3.6 Por qué había transferencias (y cómo se eliminaron)

Estadística `[rt-xfer]` del propio port (cada 4 s): 3 pares de transferencias, todas con el mismo origen,
el render target de **profundidad 4x** `D0 b0 p16 m2`:

| Destino | % de los tiles |
|---|---|
| `C0 b0 p16 m0` (color 1x de 1280x720: el buffer de presentación) | 61,6 % |
| `C0 b1440 p8 m0` (color 1x donde se dibuja la escena) | 19,2 % |
| `D0 b1664 p8 m0` (profundidad 1x) | 19,2 % |

El juego "limpia" toda la EDRAM con un clear de profundidad 4x gigante al inicio del frame y luego dibuja en 1x encima; el
port copiaba esa profundidad (reinterpretada como color) hacia cada target 1x que tomaba esa memoria, y el siguiente draw la
pisaba por completo. Dos cvars nuevos:

- `rt_xfer_skip_depth_to_color`: no transferir de un origen de profundidad a un destino de color.
- `rt_xfer_skip_clear_like_depth`: relaja `vulkan_skip_overwritten_depth_transfers` (acepta draws color+profundidad y no se
  desactiva por haber usado stencil antes). La transferencia a profundidad costaba 9 draws (1 + 8 por bit de stencil).

Efecto medido: 8,8 → ~2,1 ms con el primero y → ~0,15 ms con ambos. Validado jugando endless, sin problemas gráficos.
Riesgo conocido: el destino conserva su contenido anterior (el frame previo) en vez de los bits transferidos; si un efecto de
stencil dependiera de que el clear lo dejara en cero, se vería una máscara o sombra mal.

### 3.7 Experimentos descartados (no repetir)

| Experimento | Resultado |
|---|---|
| `render_target_path_vulkan = "fsi"` (interlock de fragmentos) | **11 fps** (89 ms por frame). Transferencias a 0, pero el dibujo pasa de 14,6 a 71 ms |
| `layer_draw_bbox`: recortar el scissor de los draws del atlas al área no nula de la textura | **rompe la imagen**: el área no nula es del atlas (esquina superior izquierda), no de donde se dibuja el quad. Los draws no son capas a pantalla completa. Cvar presente, **debe quedar en `false`** |
| `diag_skip_ps = "7ABD…,D104…"` (no dibujar las 7 pasadas) | imagen **negra salvo el HUD**; GPU 29 → 19,5 ms, ~38 fps. Las pasadas arman la escena: no son un filtro opcional |
| Buscar una opción de filtro en el menú del juego | no existe en los textos de la UI |

---

### 3.8 Medición por etapa (2026-11-12, CPU 1428 / GPU 614,4 / memoria 1331)

**Qué letra es qué etapa.** Los fondos de pelea del 360 están en `data/B/bg/` (24 archivos). El orden alfabético coincide con el
orden de las etapas en `strings.txt` (`StageJapan`, `StageKorea1-3`, `StageChina`, …) y con la tabla de selectores que documentó el
decompilado de PC (`kof02um_decomp/docs/GAME_CODE_MAP.md`, ~l. 99): selector 0 → `BGA0`, 1 → `BGB0/1/2`, 2 → `BGC0`, 3 → `BGD0`,
4 → `BGE0/…`, 5 → `BGF0/1`, 6 → `BGG0/1/2`, 7 → `BGH0`. Inferido y **coincide con las medidas** (la etapa lenta que reporta el usuario
es China = `bgc0`; Taiwan = `bgf0` anda a 60 fps; USA = `bgd0`):

| Letra | Etapa | Letra | Etapa |
|---|---|---|---|
| `bga0` | Japan | `bgg0/1/2` | Italy-1/2/3 |
| `bgb0/1/2` | Korea-1/2/3 | `bgh0` | Greece |
| **`bgc0`** | **China** | `bgi0/1` | NeoGeo World-1/2 (sin confirmar) |
| `bgd0` | USA | `bgj0/1` | Clone Lab-1/2 (sin confirmar) |
| `bge0/1` | Cambodia-1/2 | `bgk0, bgm0/1, bgn0, bgo0, bgp0` | Rugal, Krizalid, C-Zero, Zero, Igniz (sin confirmar el reparto) |
| `bgf0/1` | Taiwan-1/2 | | |

Primera corrida con el fondo de etapa etiquetado (`stage background`), training, 29 ventanas de 10 s:

| Fondo | Ventanas | fps normal | Notas |
|---|---|---|---|
| `bgg0.bin` | 4 | 57,7–59,1 | picos de SDM de Kula con 142–248 draws: 39,8 y 51,1 fps |
| `bgc0.bin` | 14 | **48,3–50,2 sostenidos** | GPU 18,6–19,8 ms con solo 92–103 draws; 75–80 % de frames ≤18 ms. Es la etapa lenta que reportó el usuario (**China**) |
| `bgf0.bin` | 8 | 58,3–60,5 (66–72 draws) | picos de SDM con 110–218 draws: 36,7–50,2 fps |
| `bgd0.bin` | 1+ | 48,6 | 160 draws |

**Por qué `bgc0` es más lenta** (ventana de `bgc0` contra ventana de `bgf0`):

| | `bgc0` | `bgf0` |
|---|---|---|
| Draws por frame | ~100 | ~71 |
| GPU ocupada | 19,4 ms | 15,8 ms |
| Pasadas de composición (`upload`) | 8,85 ms | 6,69 ms |
| Clears por resolve (`res_clear`) | 1,03 ms | 0,10 ms |
| Resolves por frame | 11 | 9 |
| Render passes / barreras | 27 / 46 | 23 / 38 |

La etapa hace una ronda extra de filtro: dos resolves más, ~2 pasadas de composición más y un clear por resolve. La carga de
texturas va igual en ambas (todo por la carga directa por CPU; la única textura fuera del camino rápido es una `fmt3 128x256`
lineal, no elegible, 1 por frame). **La hipótesis anterior de que las texturas del fondo caían al camino lento de GPU es falsa
para estas etapas**: los 51 dispatches y 43 copias de la ventana de la corrida de 1326 MHz no se repitieron.

**Quién empuja las barreras** (por frame, `bgc0`, ~100 draws), resuelto con `addr2line`:

| Llamador | Pushes por frame | Comentario |
|---|---|---|
| `VulkanRenderTargetCache::Update` (2 sitios) | 99 + 98 | uno por draw por sitio; casi todos se descartan por `skip_if_equal` |
| `VulkanTextureCache::ResolveCopyToTextures` (3 sitios) | 11 + 11 + 11 | barreras del blit y las copias del resolve |
| `VulkanRenderTargetCache::Resolve` | 11 | target → origen de copia |
| `VulkanTextureCache::RequestTextures` | 11 | textura destino → muestreo |
| `PerformTransfersAndResolveClears` (2 sitios) | 8 + 8 | clears por resolve |
| `VulkanTextureCache::FlushTextureLoadBatch` | 1 | |

Las barreras reales que se emiten (46 por frame) vienen sobre todo de los **resolves** (~6–7 pushes por resolve × 11). De los
render passes cortados por frame, ~20 son por `SubmitBarriers` (barreras pendientes), ~5 por `EndSubmission` y ~2 por cambio de
framebuffer. Esto confirma la hipótesis del costo fijo por resolve (§3.5).

**Intercambio rojo/azul:** el 100 % de los resolves directos a textura usan el blit con intercambio (`copy_dest_swap`), o sea
la doble copia (blit a B8G8R8A8 + copia a la textura) en cada uno. Detalle: el contador por frame (45–46) cuenta llamadas con copia
exitosa, que superan a los resolves (9–11), por lo que cada resolve termina en varias llamadas o varias texturas destino;
pendiente de aclarar.

**SDM de Kula** (en Taiwan y en USA): los frames tienen 250–430 draws y la GPU "span" supera a la "ocupada" por 3–8 ms
(ventana de 275 draws: ocupada 20,7 ms, span 28,7 ms, 16 % de frames ≤18 ms). El límite es el **hilo de comandos** (CPU), no la
GPU: ~0,1 ms de CPU por draw. Pendiente: perfil de pila del hilo de comandos durante una SDM.

### 3.9 Shaders del exe de PC (identificación)

`kof02um_decomp` desempaqueta el exe de Steam (`tools/steamstub/unpack_steamstub30.py`, vía `scripts/bootstrap.sh`; resultado en
`kof02um_decomp/build/KingOfFighters2002UM_unpacked.exe`). Un escaneo de tokens de versión D3D9 (`0xFFFF0200`, `0xFFFE0200`,
`0xFFFF0300`, `0xFFFE0300`) y token de fin (`0x0000FFFF`) halla **23 shaders compilados**: 12 `ps_2_0`, 7 `vs_2_0`, 2 `ps_3_0` y 2 `vs_3_0`,
con su tabla de constantes (CTAB) y parámetros con nombre:

| Shader PC | Tamaño | Parámetros (CTAB) | Probable equivalente en el 360 |
|---|---|---|---|
| `ps_2_0` @0x3d3f8, @0x3d568, @0x3ef78 | 364–388 B | `IndexTexture`, `PaletteTexture` | sprites indexados + paleta (`ps36D6`, `ps6082`, `ps03B1`, `ps7238`) |
| `ps_2_0` @0x43348 | 456 B | `PictureTex`, `Palette` | variante con paleta |
| **`ps_3_0` @0x403f8** | **9808 B** | `ConstColor` | filtro de pixel art (`psD104`, 314 líneas de desensamblado) |
| `ps_3_0` @0x3f2b8 | 4052 B | `ConstColor` | variante más ligera del filtro |
| `vs_3_0` @0x3f150, @0x40290 | 356 B | `transform` | vértice de los quads |

(Offsets de archivo del exe desempaquetado.) Falta desensamblar el bytecode D3D9 (formato documentado, instrucciones de 1 token +
operandos) para comparar el algoritmo con el desensamblado del 360 y nombrar sus constantes.

Herramientas de extracción del decompilado, útiles para identificar lo que falte: `tools/steamstub/` (desempaquetar el exe),
`tools/xbox360/` (`stfs_extract.py`, `xcompress_extract.py`, `d3d_surface.py`, `xzp_png_to_dds.py`), `tools/bg_bin/` (inspección y
exportación OBJ/GLB de `bg_bin`), `tools/pdata/`, `tools/pe/` (imports), `tools/video/` (WMV → MPEG-1), `tools/audio/` (XSB), y `symbols/*.tsv`.
Ojo: los fondos de pelea del 360 viven en `data/B/bg/` (formato propio, ~4 MB); `data/bg_bin/` es otro conjunto (HUD y barras: `BGT0`).

## 4. Estado del código (working tree, sin commitear salvo este documento)

Todo en `tools/rexglue-sdk/` y `switch/kof2002um_nx.toml`.

| Archivo | Cambio |
|---|---|
| `src/core/threading_switch.{h,cpp}` | `RexSwitchDescribeHeap(out, len, probe_largest)` |
| `src/graphics/graphics_system.cpp` | línea `[heap] before command processor` |
| `src/core/exception_handler_switch.cpp`, `resources/templates/codegen/_indirect_call.inja` | el macro guarda `ctx.last_indirect_target` en cada llamada indirecta; el log de crash lo imprime (requiere regenerar con `docker compose run --rm codegen`) |
| `src/core/autotest.cpp` | acumuladores de intervalo y `rex_autotest_take_interval` (GPU por pase y zonas de CPU) |
| `src/ui/switch_perf.cpp` | histograma de frames, rápidos vs lentos, desglose de GPU/CP, línea de heap, cortes de render pass por llamador |
| `src/graphics/vulkan/command_processor.cpp` | `DdPrint` (log `[dd]` al archivo), `layer_draw_bbox` (descartado), volcado de vértices, `diag_draw_pass_split`, `diag_skip_ps`, contador de cortes de render pass |
| `src/graphics/vulkan/render_target_cache.cpp` | `rt_xfer_skip_depth_to_color`, `rt_xfer_skip_clear_like_depth` y su contador en `[rt-xfer]` |
| `switch/kof2002um_nx.toml` | `license_mask = 1`, `rt_xfer_skip_depth_to_color = true`, `rt_xfer_skip_clear_like_depth = true` |

Cvars de solo diagnóstico (todos apagados por defecto): `diag_draw_log_from/to/step`, `diag_draw_pass_split`, `diag_skip_ps`,
`dump_shaders`, `switch_profile_stacks`, `layer_draw_bbox`.

---

## 5. Plan de cambios

Orden recomendado por ganancia estimada / riesgo. Las ganancias son estimaciones, no medidas; cada cambio se activa con un
cvar en el toml para poder probarlo sin recompilar.

### P0. Transferencias de render targets: **hecho**
Ver §3.6. Ganancia medida: ~8,5 ms de GPU.

### P1. Medir antes de seguir (en el último compilado subido)
- `render pass ends per frame by caller`: cuántas veces corta cada llamador un render pass. Resolver cada `image+0x…` con
  `addr2line`. Sitios directos: `site1` y `site2` (cambio de framebuffer).
- `diag_draw_pass_split = true`: reparte el "dibujo" en sprites `ps36D6` (→ `dump`), atlas `ps6082` (→ `readback`),
  otros sprites (→ `tex_load`), pasadas de composición (→ `upload`) y el resto (→ `draw`).
- **Hipótesis a confirmar**: la mayor parte de los ~14 ms de "dibujo" es costo fijo por corte de render pass (barreras,
  recarga de color y profundidad), no cómputo. Si es cierta, el orden de P2–P6 se decide por cuántos cortes elimina cada uno.

### P2. Quitar la copia de restauración de cada ronda del filtro
Por ronda, la secuencia del juego es `RESOLVE A; RESOLVE B; filtro(B)→RT; RESOLVE B; copia(A)→RT; copia(B)→RT con alpha`.
Equivale a `RT = mezcla(RT_sin_filtrar, filtrada, alpha_del_filtro)`. Propuesta, reconociendo la secuencia por hash de shader
y estado (sin mirar hacia adelante):
1. Que el draw del filtro (`psD104`) lea `A` en vez de `B` (reemplazar la dirección base del fetch constant 0 antes de enlazar
   la textura) para no necesitar el `RESOLVE B` previo, que duplica `A`.
2. Dibujar el filtro a una imagen auxiliar `S` (640x448 RGBA8) en vez de a `RT`; el `RESOLVE` siguiente toma como origen `S`.
3. Saltar la primera copia (`A → RT`): `RT` aún es igual a `A` porque el filtro no lo tocó.
4. La segunda copia (con alpha) se ejecuta tal cual leyendo la textura resuelta desde `S`.

Ahorro por ronda: 1 resolve + 1 pasada. Estimado: **1,5–3 ms** con dos rondas. Riesgo medio: requiere un framebuffer
auxiliar y una excepción en la ruta de resolve. Verificación: comparar capturas de la misma escena con el cvar apagado y
encendido; el píxel debe ser idéntico (la operación es matemáticamente la misma).

### P3. Viaje de ida y vuelta del final (`185BA`)
`RESOLVE RT→185BA; copia (ps7ABD, opaca) RT←185BA; RESOLVE RT→185BA`. Si el color del vértice es blanco, la copia es la
identidad y los dos resolves son idénticos, así que se pueden saltar la copia y el segundo resolve (~1 ms). Falta verificar
el color de los vértices de esos draws (ampliar el volcado de vértices a `ps7ABD`) y ver si el tinte se usa para fundidos.
Riesgo bajo si se comprueba el color en cada draw.

### P4. Resolves duplicados dentro de una ronda
Los dos primeros resolves de cada ronda copian el mismo contenido a dos texturas. Compartir la imagen entre ambas direcciones
en vez de copiar dos veces. Ahorro estimado 0,4–0,8 ms. Riesgo bajo.

### P5. Barreras de los resolves
Cada resolve exige barreras de ida y vuelta y fuerza `EndRenderPass`. Revisar máscaras de etapa y acceso (que sean las mínimas),
fusionar barreras consecutivas y evitar transiciones de layout innecesarias. Estimado 0,5–1 ms. Depende de P1.

### P6. Subir solo la zona muestreada de las texturas del atlas
El juego reescribe `@16F0D` (640x448, 287 KB) ~30 veces por frame, pero cada draw solo muestrea el recuadro indicado por sus
UV (ej. 48x112). Ahora se vuelcan los vértices, así que se conoce el recuadro. Subir solo esas filas reduciría los ~3–4 ms que
el hilo de comandos gasta en texturas (`cp_tex`) y la presión sobre la cola de GPU. Riesgo medio: cada draw debe subir exactamente
lo que va a leer. Ganancia en CPU/latencia más que en GPU.

### P7. Filtro `psD104` más barato
Después de P2, medir el filtro aislado. Si pesa más de ~1 ms, probar menos lecturas (usar `textureGather`) o ejecutar la
comparación en espacio de índice. Riesgo visual: se debe verificar a ojo en bordes de pixel art.

### P8. Overhead por sprite
~31 draws `ps36D6` cuestan 2–3,7 ms (~0,1 ms cada uno para quads de ~20 K px). Revisar cambios de descriptores, pipelines y
barreras entre ellos; evaluar agrupar los que comparten textura y paleta. Prioridad baja hasta tener P1.

### P9. Renderer propio (etapa grande)
Un camino rápido dentro del procesador de comandos que, para los draws conocidos (por hash de shader), dibuje directo sobre imágenes
del host con el blend de la GPU, sin EDRAM ni resolves, dejando el camino genérico como respaldo para menús y efectos no
reconocidos. Por qué tiene sentido: ~96 draws por frame, ~8 shaders distintos, un target de 640x448, y los sprites solo cuestan
~4 ms de ~28. Estimación: **5–8 ms** por frame. Requisitos: decodificar formatos de vértice y blends de cada familia, replicar
paletas (índice + paleta 256x1) y el orden de capas, y reproducir el filtro de P7. El conocimiento de formatos del decompilado
(`kof02um_decomp`: P_DATA, OBJ, paletas, GRC) y los puntos de hook de la capa D3D (`sub_82651D10`, `sub_82650CE8`, `sub_82655340`,
`sub_826504C8`, `sub_8265D018`) del censo de D3D sirven de base. Varias sesiones; se hace por etapas, midiendo cada una.

### P10. Presión de la cola de comandos (crash con Chin)
Opciones, de menor a mayor costo: (a) seguir reduciendo comandos por frame (P2–P6 ya lo hacen); (b) registrar el estado de la cola
(entradas pendientes, tamaño) al fallar para saber qué límite se alcanza; (c) cortar el envío cuando las entradas pendientes
superan un umbral antes de llegar a `GPFIFO_QUEUE_SIZE`; (d) parche en `nouveau_horizon_channel.c`
(`tools/mesa-switch-src`), que exige recompilar Mesa (`docker compose run --rm mesa`; la receta pide ~16 GB de RAM y esta máquina
tiene 7 GB).

### P11. Observaciones menores
- Heap: en uso 1,4–1,7 GB de 3115 MB, ~24 000 fragmentos; vigilar en sesiones largas.
- GPU a 460,8 MHz es el máximo de fábrica en portátil; no se plantea subirla.
- Modo de título completo (no solo applet): pendiente de revisar.
- Intro WMV: no revisada.

### Criterios de aceptación
- Escena media (60–90 draws): GPU ocupada ≤ 14 ms y ≥ 80 % de frames ≤ 18 ms.
- Escena pesada (100–130 draws): ≥ 60 % de frames ≤ 18 ms.
- Sin diferencias visuales frente al camino genérico en capturas comparadas.
- Sin `Failed to submit a Vulkan command buffer` en sesiones de 30 min.

---

## 6. Cómo repetir una medición

1. Subir NRO y toml: `tools/switch-ftp/subir-nro.sh` (copia primero el NRO a `switch-sd/kof2002um-nx/`).
2. Jugar un par de minutos en portátil, **sin overclock**; cerrar el juego y abrir el servidor FTP de la consola.
3. `tools/switch-ftp/bajar-logs.sh`.
4. Leer `logs/rex/rex_profile.log` (líneas descritas en §2.1) y, para direcciones `image+0x…`,
   `aarch64-none-elf-addr2line -f -C -e kof2002um_nx 0x…`.
5. Para un desglose por función: `python3 -I tools/perf/hot.py logs/rex/rex_profile.log kof2002um_nx <addr2line> "Main XThread"`.

Notas operativas: el build (`docker compose run --rm nx build`) tarda más de 10 minutos; correrlo en segundo plano con el log
en el disco del proyecto, no en `/tmp` (tmpfs de 3,7 GB). Si el paso `strip` falla con "invalid operation", mover
`kof2002um_nx.stripped.elf` antes de recompilar. El FTP de la consola debe estar abierto (con el juego corriendo da
`Connection refused`).
