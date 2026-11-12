# Renderer propio: diseño básico por etapas

Propuesta para reemplazar, **solo para los draws que el juego usa en pelea**, la emulación de la EDRAM del 360 por un
manejo simple de imágenes del host. Complementa a `docs/RENDER_PERF_PLAN.md` (medidas, hallazgos y plan de optimización
incremental); este documento describe el camino de fondo, el que apunta a 6–8 ms por frame.

Estado: **diseño, sin código todavía**. Las ganancias son estimaciones, no medidas.

---

## 1. Qué es y qué no es

**Es** un camino rápido dentro del procesador de comandos actual:
- Reconoce los draws conocidos por el hash de su pixel shader.
- Los dibuja sobre un **target virtual** (una imagen del host con versiones), sin pasar por el cache de render targets /
  EDRAM, sin `RESOLVE` a textura y sin las copias de "agarrar lo ya dibujado".
- Todo lo desconocido (menús, efectos nuevos, pantallas raras) sigue por el camino genérico de hoy.

**No es**:
- Reescribir los shaders: se reutilizan los ya traducidos a SPIR-V por el port, incluido el filtro de pixel art.
- Reescribir el juego ni el parser de comandos de GPU: la entrada sigue siendo `VulkanCommandProcessor::IssueDraw`.
- Reemplazar el camino genérico: queda como respaldo obligatorio.

## 2. Por qué tiene sentido

Datos medidos (portátil, relojes de fábrica; ver `docs/RENDER_PERF_PLAN.md`):

- ~96 draws por frame con **~8 pixel shaders distintos** y un target lógico de 640x448 (RGBA8, 1x).
- Los sprites juntos cuestan ~4 ms de ~28 ms. El resto era emulación de EDRAM y una cadena de "copiar el target a una
  textura para volver a leerlo": **9 resolves y 7 pasadas** de composición por frame (5 copias teñidas + 2 pasadas del filtro).
- Cada pasada de composición cuesta ~1 ms aunque el cálculo de píxeles es trivial: el costo es fijo por pasada
  (resolve, barreras, corte y reapertura del render pass). El 75–80 % de los cortes de render pass vienen de barreras
  pendientes (`SubmitBarriers`).
- Con las transferencias de render targets ya eliminadas (8,8 → 0,15 ms), lo que queda es esa cadena (~14 ms de "dibujo" +
  ~3,7 ms de resolves a 460 MHz).

## 3. Idea central: target virtual con versiones

El juego usa el target como una pila de "pinturas" con copias intermedias. Un target virtual permite resolver esas copias
**sin tocar píxeles**:

| Lo que hace el juego | Hoy | Con el target virtual |
|---|---|---|
| `RESOLVE` del target a una textura `T` | copia 640x448 (a veces dos: blit con intercambio rojo/azul y luego copia) + barreras + corte del render pass | **alias**: `T` pasa a apuntar a la imagen actual del target (versión *v*); sin copiar |
| Draw que sobreescribe todo el target (opaco, pantalla completa: el filtro) | lee la copia y escribe sobre el mismo target | **renombrar**: el target pasa a una imagen nueva (`loadOp = DONT_CARE`); la anterior queda como la imagen a la que apuntan los alias, sin copiar |
| Copia opaca de pantalla completa de una textura `S` al target | una pasada completa | **adoptar**: el target pasa a ser la imagen de `S`; sin dibujar |
| Draw con blend sobre el target | draw | draw (única operación que sí escribe píxeles); si hay alias vivos sobre la imagen actual, antes se **materializa** (copia) |
| Draw que muestrea `T` | enlaza la textura `T` | enlaza la vista de la imagen aliada, en layout de muestreo (barrera solo si cambia el layout) |

Un alias es **perezoso**: la copia solo ocurre si el target se va a escribir mientras algo aún necesita la versión vieja y la
escritura no es un sobreescrito completo. En la cadena observada (snapshot → filtro opaco → copia opaca → mezcla con alpha)
casi todo se resuelve con alias y renombrados.

### Secuencia observada y cómo quedaría

Por ronda, el juego hace:
`RESOLVE A ; RESOLVE B ; filtro(B)→RT ; RESOLVE B' ; copia(A)→RT ; copia(B') con alpha → RT`.

Con el target virtual:
1. `RESOLVE A`: alias `A := imagen actual (v1)`. `RESOLVE B`: alias `B := v1` (mismo contenido).
2. `filtro(B)→RT`: sobreescritura completa y opaca → **renombrar**: RT := imagen nueva *v2*; `A` y `B` siguen apuntando a *v1*.
3. `RESOLVE B'`: alias `B' := v2`.
4. `copia(A)→RT`: copia opaca de pantalla completa → **adoptar**: RT := imagen de `A` (*v1*); sin dibujar.
5. `copia(B') con alpha → RT`: blend sobre RT (*v1*); `B'` apunta a *v2*, otra imagen, así que no hay conflicto de lectura y escritura.

De 7 pasadas por frame quedan las del filtro (2) y las mezclas con alpha, sin ningún resolve y casi sin barreras.

## 4. Arquitectura

```
Comandos de GPU del juego (PM4)           ya lo parsea el port
        │
IssueDraw(estado del draw)                shaders, blend, depth, texturas, vértices, target, scissor
        │
Clasificador por hash de pixel shader     tabla (datos), no código
        ├── conocido  ──►  Gestor del target virtual ──► pipeline cache existente (SPIR-V ya traducido)
        │                       │                          └─ un render pass por versión del target
        │                       └─ tabla de alias: dirección de textura → (imagen, versión)
        └── desconocido ──► camino genérico (hoy)  ◄── "sincronizar": copiar el target virtual al target genérico
        │
Presentación a 1280x720                   ya existe
```

### 4.1 Clasificador

Tabla `hash de ps → clase`, con los 7 hashes conocidos (`docs/RENDER_PERF_PLAN.md`, §3.4):
`Clear`, `SpriteIndexado` (36D6, 6082, 03B1, 7238), `CopiaTeñida` (7ABD), `Filtro` (D104), `Presentación` (6E57).
Además de la clase, cada entrada define lo que hay que verificar para que el draw sea "conocido": primitiva, formato del
target (RGBA8, 1x), si es sobreescritura completa (rect list / quad a pantalla completa, sin blend, máscara de color completa,
profundidad *always* o desactivada) y las texturas esperadas.

### 4.2 Gestor del target virtual

Estado mínimo:
- `VirtualTarget { imagen_color, imagen_profundidad, versión }`
- `Alias { dirección_guest → (imagen, versión, vista de muestreo) }`
- Un pool pequeño de imágenes 640x448 RGBA8 y profundidad (4–6), con cuenta de referencias (alias vivos + el target).

Operaciones:
- `OnResolve(dst)`: `alias[dst] = (imagen_actual, versión)`. Sin comandos de GPU.
- `OnOverwriteDraw(...)`: pedir imagen libre del pool (`DONT_CARE`), `versión++`, dibujar. Las referencias a la anterior se conservan.
- `OnAdoptCopy(S)`: `imagen_color = alias[S].imagen` (suelta la anterior si nadie la usa). Sin dibujo.
- `OnBlendDraw(...)`: si la imagen actual tiene alias vivos, **materializar** (copiar a imagen libre y dejar los alias en la copia); luego dibujar.
- `OnSample(dir)`: si hay alias, devolver su vista; barrera solo si el layout debe cambiar.
- Expiración: un alias muere cuando se hace otro `RESOLVE` a la misma dirección o al cambiar de frame.

Invariantes: nunca se muestrea la misma imagen que se escribe en el mismo draw; el contenido visible de cada alias es exactamente
el del `RESOLVE` que lo creó.

### 4.3 Depth

Los sprites usan test de profundidad `lequal` con escritura (se ordenan por z). El target virtual lleva su propia imagen de
profundidad 640x448, con las mismas reglas de renombrado (los clears de profundidad del juego son sobreescrituras completas).

### 4.4 Respaldo y entrada/salida del camino rápido

- El camino rápido se decide **por frame** con la firma del frame anterior (los mismos shaders conocidos y el mismo target).
- Si aparece un draw desconocido sobre el target virtual: `sincronizar` (copiar la versión actual al target del camino genérico,
  con el layout correcto) y seguir ese frame por el camino genérico.
- Cvar `renderer_fast_path` (apagado por defecto) y un contador por reporte: draws por el camino rápido, caídas al genérico
  y materializaciones.

## 5. Puntos de integración en el código actual

| Pieza | Dónde | Qué cambia |
|---|---|---|
| Entrada de draws | `VulkanCommandProcessor::IssueDraw` (`graphics/vulkan/command_processor.cpp`) | clasificar; si es conocido, delegar al gestor |
| Enlace de texturas | `UpdateBindings` y el cache de texturas | consultar la tabla de alias antes de buscar la textura por dirección |
| `RESOLVE` | `VulkanRenderTargetCache::Resolve`, lambda `rtt_copy` (`graphics/vulkan/render_target_cache.cpp`), `VulkanTextureCache::ResolveCopyToTextures` (`graphics/vulkan/texture_cache.cpp`) | crear alias en vez de copiar |
| Cambio de target / render pass | `SubmitBarriersAndEnterRenderTargetCacheRenderPass`, `EndRenderPass` | un render pass por versión; las barreras se emiten al cambiar de versión |
| Propiedad de la EDRAM | `RenderTargetCache::Update`, `PerformTransfersAndResolveClears` | el target virtual no participa; `sincronizar` entrega la imagen al camino genérico |
| Presentación | ruta existente | sin cambios |

Hallazgo útil del código: cuando el resolve trae el flag de intercambio rojo/azul (`copy_dest_swap`), hoy hace **dos** copias
(blit a una imagen B8G8R8A8 intermedia y luego copia a la textura). Con alias el intercambio se resuelve con el *swizzle* de la
vista de muestreo y desaparece la copia intermedia.

## 6. Fases y ganancia estimada (GPU por frame, a 460 MHz)

| Fase | Contenido | Estimación |
|---|---|---|
| 0 | camino genérico + saltos de transferencias (hoy) | ~18–19 ms |
| 1 | target virtual para la cadena 640x448: alias, renombrar, adoptar; sin los 9 resolves y ~5 de las 7 pasadas | **~9–10 ms** (60 fps sin overclock) |
| 2 | menos descriptores y barreras entre sprites; agrupar draws con la misma textura y paleta | ~7–8 ms |
| 3 | pipeline de sprites propio, sin Xenos, para los 4 shaders de sprite | ~5–7 ms |

Orden de trabajo de la fase 1, cada paso con el cvar apagado por defecto y verificable por separado:
1. Instrumentar la cadena (ya hecho en el reporte: resolves con/sin intercambio, cortes de render pass, barreras por llamador).
2. Imagen y pool del target virtual; `sincronizar` con el camino genérico (sin cambiar el resultado).
3. Alias de `RESOLVE` + `OnSample` (primero solo para las direcciones de la cadena observada).
4. Renombrar en la pasada del filtro.
5. Adoptar en la copia opaca de pantalla completa.
6. Materialización en blends con alias vivos.

## 7. Validación

- Cvar apagado = comportamiento actual, bit a bit.
- Con el cvar encendido: capturas de la misma escena (training, etapa y personajes fijos) con el camino genérico y el rápido;
  comparar a ojo y con diferencia de píxeles. La operación debe dar el mismo píxel.
- Contadores en el reporte de 10 s: cortes de render pass por frame, barreras, resolves, ms de GPU por pase.
- Probar escenas distintas: China (fondo con texturas fuera de la carga directa), SDM de Kula (~400 draws), Taiwan normal,
  menús y pantallas de carga (deben caer al camino genérico sin glitches).

## 8. Riesgos

| Riesgo | Mitigación |
|---|---|
| Tocar la propiedad de la EDRAM / imágenes del cache de render targets | el target virtual no usa el cache; la única costura es `sincronizar` |
| Diferencias de píxel en blends o en el orden de capas | comparación de capturas; misma tabla de blend y mismo pipeline cache |
| Escenas no reconocidas | caída al genérico por frame, con contador |
| Fugas o aliasing de imágenes (lectura y escritura de la misma imagen) | invariantes de la sección 4.2 y validación con capas de validación de Vulkan en PC antes de la consola |
| Cambio de hash de shader en otra versión del juego | la tabla es dato: cambia sin recompilar el camino |

## 9. Lo que falta saber antes de escribir código

Resuelto con la corrida del 2026-11-12 (detalle en `docs/RENDER_PERF_PLAN.md`, §3.8):
- El 100 % de los resolves de la cadena usan el blit con intercambio rojo/azul: el *swizzle* de la vista de muestreo sirve.
- Las barreras que cortan los render passes vienen sobre todo de los resolves (`Resolve`, `ResolveCopyToTextures`, `RequestTextures`,
  `PerformTransfersAndResolveClears`).
- Las texturas de los fondos no caen al camino lento de GPU: no hay que ampliar la carga directa para entrar al camino rápido.
- La etapa más lenta (`bgc0`) añade una ronda extra de filtro (+2 resolves, ~2 pasadas): el target virtual la abarata igual que a las demás.

Sigue pendiente:

- Colores de vértice de las copias teñidas (`7ABD`), para saber cuáles son la identidad y se pueden eliminar directamente.
- Disassembly de los shaders D3D9 del exe de PC (`tools/perf/` pendiente) para nombrar los parámetros de los shaders del 360 (ver `docs/RENDER_PERF_PLAN.md`, §3.9).
