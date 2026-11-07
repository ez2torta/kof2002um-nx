# KOF 2002 UM NX — estado del port

## Selección de personajes ARREGLADA: las paletas en cero van por shared memory (2026-11-07, noche)

- **Arreglo** (`vulkan/texture_cache.cpp`, `LoadTextureCpuDirect`, después de `TranslatePhysical`): si la textura tiene alto 1 (paleta 256x1) y sus bytes en la RAM del guest son **todos cero**, `fallback()` (camino de shared memory, como vanilla). Cvar `texture_cpu_direct_zero_row_fallback` (default true). Log `[cpu] zero palette @XXXXX 256x1: shared memory path` (8 como máximo; salieron @1827A, @18252, @1822A…).
- **Run 152121** (`_nl_charsel.json`): cuadrícula, retratos, fondo de la selección y de la pantalla de orden se ven bien; pelea bien. **Rendimiento igual al de antes**: 912 frames, total 21,9 ms, gpu_busy 20,5, n_copy 19,5 (run 151124 sin arreglo: 912 / 21,9 / 20,5 / 19,5).
- **Descartado**: excluir de cpu_direct todas las texturas de alto 1 (run 151743): la selección se ve bien, pero la pelea cae a 853 frames (total 23,5 ms, gpu_busy 22,7, **n_copy 50**): las paletas de la pelea se recargan cada frame y por GPU cortan la pasada.
- Sigue sin saberse **por qué** la RAM del guest tiene ceros y el buffer tiene datos (hipótesis de la sección de abajo). El arreglo imita a vanilla sin resolverlo. Si aparece otra textura negra de alto > 1, retomar el paso 1 de abajo.
- El cambio `IsRangeGpuWritten` (sección de abajo) sigue en el código.
- **Próximo paso**: volver al rendimiento (próximo paso de "CP fijo en el núcleo 1"). Ojo: con `_nl_charsel` el gpu_busy da ~20 ms contra ~13 de `_nl_cpuup`; comparar siempre con la misma config.

## Selección de personajes negra: la causa es `texture_load_cpu_direct` (2026-11-07, tarde)

- **Síntoma completo** (capturas cada ~1 s desde `first_frame`, config `devkitnx/configs/_nl_charsel.json` = pinspin + `on_mark.first_frame` con capturas `selNN`): en la selección el área 4:3 sale **toda negra** (no hay cuadrícula, fondo ni cursores). También faltan el fondo y la UI de la pantalla de orden (sólo se ven los personajes) y el fondo del menú principal. La pelea se ve bien.
- **Bisección** (base = `_nl_charsel_base.json`, sin cvars de rendimiento, pero con los defaults actuales):
  - Vanilla (`_nl_charsel_vanilla.json`: cpu_direct, linear_direct, cpu_poll, resolve_copy_to_texture, direct_host_resolve, partial_reload, skip_unchanged_reload, msaa alias y batch en false): **se ve todo bien** (run 144420).
  - Sólo `--texture_load_cpu_direct=false` (`_nl_charsel_bisB.json`, run 144848): **se ve bien**. **El culpable es cpu_direct.**
  - cpu_direct encendido con partial_reload y skip_unchanged_reload en false (`bisC`): sigue negro. `gpu_msaa_4x_alias_to_1x=false`: sigue negro.
- **Diag nuevo** `--diag_draw_log_from/to/step` (frames de `g_texture_poll_frame`; `vulkan/command_processor.cpp`, struct `DrawDiag` en `IssueDraw`): línea `[dd]` por draw con etapa alcanzada, prim, hashes vs/ps, RT, blend, texturas (formato, tamaño, dirección, checksum de texels o `pnz/or/h` para paletas 256x1 leídos de la RAM del guest), más `[dd] copy` (registros de resolve) y `[dd] cc/aref/vp`. `[dl]` en `LoadTextureCpuDirect` (dirección, filas, bytes no cero) y `[gw]` en `SharedMemory::RangeWrittenByGpu` (rango 0x181–0x183xxxxx). Con el run pinspin la selección cae en frames ~520–610.
- **Lo que se vio**:
  - Ningún draw se descarta (todos llegan a `ok`). En la selección, cada frame: clear 4x→RT 0x5A0, cadena de resolves (1F52B, 186D2, con clear) y composición, **~73–90 sprites `ps…8307`** (índices k_8 256x256 lineales + paleta k_8_8_8_8 256x1 en t1), resolve a 185BA y dibujo al backbuffer 1280.
  - Las paletas de esos sprites (0x18162000–0x1827A000) dan **cero en la RAM del guest en todos los frames, también con cpu_direct apagado (vanilla), que sí dibuja**. Con cpu_direct se recargan ~100 veces en 50 frames y siempre copian ceros (`[dl] … nz0`).
  - `[gw]`: ninguna escritura de GPU en esa zona dentro de la ventana.
- **Descartado**: fetch constants inválidos (segunda opinión del agente; ningún draw falla), alias MSAA, partial/skip reload, **"la GPU escribió las paletas antes de crear la textura"**: se agregó `SharedMemory::IsRangeGpuWritten` (flags `valid_and_gpu_written`) y cpu_direct cae al camino de shared memory si da true. El run 151124 sigue negro y nunca se imprimió `[cpu] texture … was written by the GPU before`. **Ese cambio quedó en el código** (`shared_memory.h/.cpp`, `vulkan/texture_cache.cpp` en `LoadsBaseWithoutSharedMemory` y `LoadTextureCpuDirect`): es inocuo, pero se puede sacar.
- **Hipótesis vigente**: vanilla toma las paletas del buffer de shared memory, que tiene datos que la RAM del guest (vista por `TranslatePhysical`) ya no tiene. Puede ser una copia subida antes, cuando la RAM sí tenía datos, de la que después no se detectó una escritura que la puso en cero. O la RAM física y la vista que escribe el juego no coinciden en esas páginas (vistas perezosas de `guest_memory_switch.cpp`, recommit de chunks, offset de 0xE0000000). Que en vanilla también salgan ceros apunta a lo primero.
- **Próximo paso**:
  1. Confirmar de dónde saca vanilla los datos. En vanilla (`_nl_charsel_dlv.json` + diag), loguear en `SharedMemory` las subidas (`UploadRanges`) de 0x1816xxxx–0x1827xxxx con frame y bytes no cero, y las invalidaciones (`MemoryInvalidationCallback`) de esas páginas desde el arranque (ventana amplia: frames 300–600).
  2. Si la subida ocurrió antes con datos y después la RAM quedó en cero sin invalidación: buscar quién pone ceros (`--diag_write_fault_range=18160000-18280000` desde el frame ~400 con cpu_poll apagado) y decidir. Si es una escritura real del juego, cpu_direct debe imitar a vanilla (no recargar paletas que el watch no marcó). Arreglo posible: excluir de cpu_direct las texturas chicas (por ejemplo, alto 1 / ≤4 KB), que no ahorran nada.
  3. **Arreglo rápido disponible ya**: excluir de cpu_direct las texturas de alto 1 (las paletas). Probarlo con `_nl_charsel.json` y medir fps en el parque con `_nl_pinspin.json`.
- Sobre la Switch: el HOME tiene ahora **KOF primero y Netloader segundo**. Tras cada run, el cursor queda en Netloader y alcanza con `bot … click A`. Si aparece "Se ha cerrado el programa a causa de un error", primero `click A`.
- Rendimiento: esta sesión no se tocó.

## CP fijo en el núcleo 1 (`cp_pin_core=1`): parque 45 → ~53 fps (2026-11-07)

- **Unión de filas por bitmap: no sirve.** Backups `.bak_bitmap` de `vulkan/texture_cache.{h,cpp}`. Cambios: historial de 32 → 128 cargas, unión exacta con un bit por fila y versión libre con el `gen` más nuevo en vez de la primera libre. Las tres corridas con `_nl_cpuup.json` (013841, 014017, 014153) dieron 901, 954 y 876 frames (≈45 fps, igual que antes); cp_tex 3,54–3,61; **134 KB por carga** contra 138. Las filas sucias de cada carga ya son contiguas. Los cambios quedan (son exactos y no cuestan).
- **Cvars nuevos** (`graphics/command_processor.cpp`, InitOnly, default -1; backup `.bak_pin`):
  - `cp_pin_core=N`: el hilo GPU Commands se fija a sí mismo en el núcleo N (`svcSetThreadCoreMask`, log `[pin]`).
  - `main_exclusive_core=K`: llama a `RexSwitchPerfExclusiveCore(K)` (necesita `--perf`).
  - Bug arreglado en `ui/switch_perf.cpp` `ApplyExclusiveCore` (backup `.bak_pin`): buscaba nombres que empezaran con "XThread", pero el hilo principal se llama "Main XThread", así que nunca se fijaba. Ahora usa `strstr`.
- **Antes** (`_nl_cpuup`): Main XThread 97 % de CPU, GPU Commands 80 %, todos con máscara 0x7, compitiendo.
- **Corridas `_nl_pin.json`** (= cpuup + `--main_exclusive_core=0 --cp_pin_core=1`; el main todavía no quedaba fijo por el bug): **1040, 1071 y 1066 frames (≈53 fps)**, total 18,5–19,0 ms, cp_wrm 2,8–3,5. `_nl_pinspin.json` (+ `cp_idle_spin_us=300`): 1084 (una sola corrida; las otras dos fallaron porque el netloader no estaba listo). Imagen correcta (014907 `pelea2.jpg`, parque).
- **Con el main fijo de verdad** (después del arreglo; main con máscara 0x1): `_nl_pin` dio 1056, 1088 y 985 frames; `_nl_pinspin` dio 1052, 1046 y 1125. **Fijar el main no suma nada**; lo que ganó es el CP solo en el núcleo 1. En la mejor corrida (020243): 937 frames rápidos de 16,15 ms en promedio (justo bajo el límite) y 188 lentos de 25 ms, que tienen más draws (115 contra 107). Falta un margen de ~1,5–2 ms en el CP.
- **Perfil** (020619, `_nl_pinspin_prof.json`; profsum con el ELF `kof2002um-nx/kof2002um_nx`, no el `.elf`): ~65 % del Main XThread es la espera de la GPU del juego (`sub_826571A8`, lazo con `db16cyc`, más `sub_82655B48` y `sub_826B0628`). **El camino crítico es el trabajo del CP** (~12,3 ms por frame). CP: memcpy 19 %, RexGmProtect 3 %, memcmp 2,6 %, XXH3 2,6 %, malloc/free ~3 %; el resto está repartido.
- **Prueba del usuario con la build de la SD** (dock): muy jugable, pero saltos y barridos se sienten en **cámara lenta**. Es lo esperable: la lógica del juego avanza un paso por frame dibujado, así que a ~54 fps corre al ~90 % de velocidad, y cada frame que se pasa de 16,7 ms va a 30 fps (medio paso). Solo se arregla con 60 estables. El monitor marcó CPU y GPU casi llenas y memoria al 23 %: la CPU sale inflada por los spins (espera activa).
- **GitHub** (2026-11-07): repo público https://github.com/tbittech/kof2002um-nx (se exporta con `tools/export_github.py`) y pre-release v0.1.0 (NRO, TOML y el NSP forwarder del usuario, guardado en `switch/kof2002um-nx-forwarder.nsp`). Los fps son en dock con los relojes de fábrica (CPU 1020, GPU 768, EMC 1600 MHz), sin overclock. **Bug conocido: en la selección de personajes no se dibuja la cuadrilla.**
- **Build para la SD**: `switch/kof2002um-nx-60fps/` (NRO de las 01:53 + TOML con los cvars de `_nl_pinspin`, sin autotest ni perf). **No se probó que el TOML tome estas claves sueltas.**
- **Próximo paso**: (1) repartir la copia de filas de `LoadTextureCpuDirect` entre el CP y un hilo ayudante en el núcleo 2, en trozos de 16 filas que reclaman los dos con una CAS sobre (seq, siguiente), así el CP nunca espera trozos no tomados (diseño listo, sin código todavía); (2) copia NEON con `stnp`; (3) recortar malloc/free y RexGmProtect en el CP; (4) probar menús y portátil, y pasar `cp_pin_core=1` a default.

## Subidas de memoria compartida por CPU (`shared_memory_cpu_upload`): pasadas 50 → 26, GPU 15,0 → 13,4 ms (2026-11-07)

- **Cvar nuevo `shared_memory_cpu_upload`** (default false, InitOnly; usar con `--vulkan_sparse_shared_memory=false`). Backups `.bak_cpuup` de `vulkan/shared_memory.{h,cpp}`.
  - El buffer de 512 MB se crea en un tipo de memoria host-visible (Tegra es UMA) y se mapea (log `[cpuup] shared memory host-mapped`).
  - `TryCpuUpload` (al principio de `UploadRanges`): escribe las páginas por CPU (memcpy + flush si no es coherente), sin copia, barrera ni corte de pasada. Solo si ningún rango en vuelo (submissions no completadas) lo impide: lecturas pedidas por draws (`OnRangesRequested`) cuyos bytes cambiarían (memcmp guest contra buffer), o escrituras de la GPU (`Use` con written_range, y las copias del camino viejo). Si no, cae al camino de copia.
- **Corrida** (run 012541, `configs/_nl_cpuup.json` = G + `texture_poll_hook_dirty_rows` + cpu upload; parque; imagen correcta en `pelea2.jpg`): **901 frames ≈ 45 fps** (sin esto: 864). n_rp 50 → **26**, n_bar 102 → **38**, n_copy 53 → **18,5**, gpu_busy 15,0 → **13,4** (p99 14,7), gp_draw 9,6 → 8,7. ~41000 subidas por CPU y 1696 que cayeron a copia.
- **Ahora el límite es la cadena juego↔CP**: total 22 ms, cp_wrm 6 (espera al vblank), cp_tex 3,5, cp_idle 3.
- **Perfil por pilas del hilo GPU Commands** (run 012753, `configs/_nl_cpuup_prof.json`, `tools/profsum.py`, 319 muestras): **memcpy 20 %** (sobre todo las filas copiadas a las versiones de la textura de sprites: ~41 × ~140 KB por frame a ~1,6 GB/s, memoria mapeada probablemente sin caché), GetRegisterInfo 4 %, RexGmProtect 2,5 %, XXH3 2,2 % (backstop), el resto repartido.
- **Idea para cp_tex**: la copia solo tiene que terminar antes del submit que usa la versión, no antes de que el CP siga. Pasarla a un hilo worker (el submit espera a que termine) o mapear las versiones con caché + flush. Ojo: con el spin idle del CP los núcleos ya competían (sección "Vblank puntual").
- **Memoria cacheada para las versiones: descartado.** Cvar `texture_cpu_direct_cached` (default false, InitOnly; backup `vulkan/texture_cache.cpp.bak_cached`): `HOST_ACCESS_RANDOM` + flush solo de las filas escritas (el flush de filas quedó también para el caso sin caché). Run 013237 (`_nl_cached.json`): cp_tex 3,54 → **4,01**, 768 frames. La limpieza de caché cuesta más que escribir sin caché. El shared memory mapeado es tipo 0, **no coherente**.
- **Worker para la copia: no sirve.** El juego reescribe la textura apenas ve la cerca (EVENT_WRITE_SHD), así que la copia desde la memoria del guest tiene que terminar antes de que el CP escriba la cerca. Solo se solaparía con los pocos paquetes que hay entre el draw y la cerca.
- **Mejor medida hoy: `configs/_nl_cpuup.json`** (≈45 fps en el parque). Hay mucha variación entre corridas (`_nl_cached` dio 768 frames): para decidir, 3+ corridas por variante.
- **Próximo paso**: (0) cp_tex: unión de filas como **bitmap** de filas (448 bits por carga en `cpu_history`) en vez de min/max, y medir los KB por carga en `[cpu]`; (1) medir la latencia por vuelta juego↔CP (~40 por frame) y recortarla (spin del CP sin `Wait`, afinidad de núcleos, o procesar el ring en el hilo del juego cuando espera la cerca); (2) cp_tex 3,5: menos versiones en vuelo; (3) pasar a default los cvars que ganaron después de probar menús y portátil.

## Cargas solo de filas sucias (`texture_poll_hook_dirty_rows`): cp_tex 4,1 → 3,7, ganancia chica (2026-11-07)

- **Objetivo del usuario: 60 fps.** Cvar nuevo `texture_poll_hook_dirty_rows` (default false). Backups `.bak_dirtyrows` en `pipeline/texture/cache.{h,cpp}` y `vulkan/texture_cache.{h,cpp}`.
  - `TakePollPagesDirty` devuelve el rango de páginas sucias reportadas por el hook. En el camino del hash, si la carga viene del hook, se setean `partial_base_load_row_*` con esas filas.
  - `LoadTextureCpuDirect`: generación por versión (`cpu_gen`, `CpuVersion::gen`) e historial de 32 cargas (`cpu_history`). Una versión reusada recibe la unión de las filas escritas desde su generación; si no alcanza el historial, copia completa. El fallback a GPU cuenta como carga completa.
- **Corrida** (run 011729, `configs/_nl_rows.json` = G + el cvar; parque; build `_logs/rows-build.log`): **864 frames ≈ 43 fps** (G en el parque: 38–44), cp_tex **3,66** (antes 4,1), gpu_busy 15,0, cp_wrm 6,6, total 23,1.
- **Por qué gana poco**: `[cpu]` dice 6144 cargas, **160 en el lugar** y 57 versiones. Con el submit cada 2 ms la GPU casi nunca terminó con la versión actual, así que casi todo va a otra versión, que estuvo vigente hace ~40 cargas: la unión de filas es casi toda la textura (~140 KB por carga en promedio contra 287 KB).
- **Segunda opinión (agente; WebSearch falló, solo leyó código de Mesa y Xenia Canary)**:
  - NVK emite `WAIT_FOR_IDLE` en cada barrera, y la copia va por el copy engine (cambio de subcanal = otro WFI). Cada sprite (barrera + copia + pasada nueva) vacía la GPU unas 2 veces: explicaría los 13–15 ms de GPU con solo ~105 draws. Fuente: mesa `src/nouveau/vulkan/nvk_cmd_buffer.c` (~l.620-670), `nvk_cmd_copy.c`. Tunables de NVK (`NVK_DEBUG`) no ayudan.
  - Xenia upstream tiene lo mismo (`UploadRanges` → `SubmitBarriers(true)` → `EndRenderPass`).
  - Recomienda, en orden: (1) medir la latencia por vuelta del ping-pong juego↔CP; (2) **memoria compartida sin copia** (Tegra es UMA: buffer host-visible que lea el vertex fetch directo, con limpieza de caché del rango sucio), o un ring host-visible con offset de fetch; (3) procesar el ring PM4 en el hilo del juego cuando espera la cerca; (4) HLE del camino de sprites; (5) deko3d al final.
- **Próximo paso**:
  1. Uploads de memoria compartida sin copy engine ni corte de pasada: escribir por CPU en un buffer host-visible (UMA) solo los bytes que cambiaron, cuando ningún draw pendiente lee esos bytes. Esto apunta a la GPU (15 ms, sin margen), que hoy es el límite junto con el CP.
  2. Para cp_tex: menos versiones en vuelo, o que una versión reusada copie solo las filas que leen los draws siguientes (riesgoso).

## Escena del bench FIJA: el rival lo elegía el bit 0 de GetTickCount (2026-11-07)

- **Diagnóstico nuevo `--at-timelog=<desde>-<hasta>`** (frames; `tools/rexglue-sdk/src/core/clock.cpp`, backup `.bak_timelog`): agrupa las lecturas de reloj del guest por tipo, dirección de retorno del host (offset de imagen, para addr2line) e hilo, y las vuelca al pasar el último frame (`[time] <t|s|u|g> ra 0x… th N xCOUNT fA-B`). `t` = mftb/`QueryGuestTickCount`, `s` = hora del sistema, `u` = uptime, `g` = GetTickCount del juego (`sub_826AEA30`, hook en `bench_seed.cpp`, ra = lr del guest).
- **Resultado** (runs 005959 y 010141, config `_nl_timelog.json`): en el hilo del juego las `mftb` son temporizado por frame (`sub_82657098`, `sub_826570C8`, `sub_826636D0`, `sub_826AFA28`), y hay **un solo GetTickCount en f599–601, desde `0x8216A2BC`**.
- **Causa**: `sub_8216A278` hace `GetTickCount() & 1` y elige entre dos equipos de CPU fijos: (30, 31, 32) o (36, 51, 38) (bytes en `[0x84ADB7F0]+4..6`). Por eso salía fábrica o parque, ~50/50, con cualquier config.
- **Arreglo** (`bench_seed.cpp`, hook de `sub_826AEA30`): con `--at-seed=N`, en esa llamada el bit 0 se fuerza a `N & 1` (log `[AT] log seed: team pick GetTickCount X -> bit B`). **Seed impar = parque (espejo K'/Maxima/Whip), seed par = fábrica (Foxy/Angel/Kula)** (el par no se probó todavía: cambiar `--at-seed` también cambia `srand`).
- **Validado**: runs 010516 y 010658 (C) y 010843 (G), las tres en el parque. Frames en la ventana de 20 s: C 670 y 640 (~33 y 32 fps), G 758 (~38 fps). G en el parque da ~38 fps; el 53,6 de G (001108) era en la fábrica.
- `--at-input-latch` (sección de abajo) queda disponible pero no hacía falta.
- **Próximo paso**: con la escena fija, A/B con 3+ repeticiones intercaladas: G contra G + `shared_memory_upload_hoist`, y `vsync_precise_spin_us` solo. Si se quiere medir en la fábrica, probar `--at-seed=2`.

## La escena del bench no depende de rand() ni de los fps: es ~50/50 entre corridas (2026-11-07)

- **randlog** (`configs/_nl_hook_skip_rl.json` ~33 fps, run 003222, y `_nl_spin_novidle_rl.json`, run 003412): la secuencia de `rand()` es **idéntica** hasta la pelea (mismos llamadores, mismas cantidades y los mismos reintentos en `0x8261E4B0`/`0x8261E510`); solo corre el número de frame. Desde la pelea solo consume `0x825C3484` (uno por frame). Aun así: 003222 = fábrica (rival Foxy/Angel/Kula), 003412 = parque (espejo K'/Maxima/Whip). Descartado el "llamador por frame que desfasa rand".
- `0x82636BEC` está en `sub_82636B90`: contador por frame en +212 de la estructura en `[0x84ADB7BC]`, no es la elección del rival.
- **`--at-input-latch`** (nuevo, `tools/rexglue-sdk/include/rex/autotest.h`, backup `.bak_latch`; solo con `--at-clock=vblank`): el replay avanza de a un paso y solo después de que el juego leyó el actual (`in_pub`/`in_seen`). Evita perder toques de 1 vblank a fps bajos. **No cambió nada**: 004236 (C) parque, 004424 (G) fábrica, 004627 (C) parque, 004811 (G) parque. Configs `_nl_*_latch.json` y `_nl_*_latchrl.json`.
- Secuencia de hoy: F P P F P P, sin relación con config ni fps. Los marks (`match_start` vb 1707–1708) y los `[input]` son iguales en todas.
- **Hipótesis vigente**: el azar sale de otra fuente. Candidatos: (a) un contador de ticks del juego en el momento del "confirmar" (el input cae en el tick N o N+1 según la fase del vblank → dos resultados, ~50/50); (b) timebase (`mftb` en `sub_8265C990`, `sub_82657098`, `sub_826570C8`, `sub_8265C878`, `sub_826636D0`, `sub_826AFA28`, `sub_8292E740/E940/F130/F6D0`); (c) carrera entre hilos.
- **fps (frames / 20 s de ventana)**: G fábrica 001108 = 53,6, pero hoy G fábrica (004424) = **38,5**, G parque = 44,4 (003412) y 38,6 (004811). C = 32,6–33,6 en las dos escenas. gpu_busy de G subió a ~16,1–16,7 (antes 15,5). **Hay variación grande en la misma escena y config**: el 53,6 de G puede haber sido una corrida con otra pelea (pelea2 distinta). Antes de pasar cvars a default, repetir G varias veces.
- **Segunda opinión (agente web)**: en los proyectos que revisó (GGPO, Dolphin, BizHawk), la hora y el timebase cuentan como input, y el replay se ata al tick de lógica (la N-ésima lectura del pad), no al vblank. Verificado acá: `mftb` → `REX_QUERY_TIMEBASE()` = `rex::chrono::Clock::QueryGuestTickCount()` (tiempo real; `kof2002um_nx_pch.h:333`), así que la hipótesis (b) es plausible. Propone: (1) en modo bench, tiempo virtual (`QueryGuestTickCount`, QPC, `KeQuerySystemTime`, GetTickCount = frame lógico × constante, con un incremento mínimo por llamada para no colgar esperas activas); (2) replay por lectura de pad; (3) lo más robusto para A/B: forzar rival y escenario en RAM, o una partida guardada fija en la SD. Para medir: 5–10 repeticiones intercaladas A/B y mediana + p99.
- **Próximo paso**: (0) loguear el retorno y el llamador de las lecturas de tiempo (`QueryGuestTickCount`) entre el confirmar y la pelea, en una corrida de cada escena; (1) repetir G 2–3 veces para saber cuánto varía de verdad; (2) si se quiere fijar la escena: buscar con Ghidra dónde se guarda el equipo rival (escritor de la estructura de equipos tras `sub_8261E450`) y vigilarlo con `--diag_write_fault_range`, o forzarlo con un hook bajo `--at-seed`; (3) si no, para A/B emparejar corridas por `pelea2.jpg`.

## Fijar la escena del bench: re-sembrar en las elecciones no alcanza (2026-11-07)

- **Por qué cambia la escena**: algún `rand()` se llama una vez por frame dibujado. Con los fps distintos de cada config (30–54), la secuencia se desfasa y la elección cae en otra escena (fábrica, parque, otra). Cuando todo iba a ~17 fps, `--at-seed=1` alcanzaba (sección "Escena del bench fija…" más abajo).
- **Probado**: en `src/recompiler/src/bench_seed.cpp` (hook de rand `sub_82725520`), con `--at-seed=N --at-seed-picks` re-siembra (`srand`) en la primera llamada de cada frame desde `0x8261E4B0` (elección de escena) y desde `0x82636BEC`.
  - **Hay que hacerlo una vez por frame**: re-sembrar en cada llamada colgó el juego (la elección reintenta con muestreo con rechazo; run 002319, FALLÓ).
  - Con una vez por frame (runs 002521 y 002658, config G): las dos corridas dieron **otra escena distinta** de las anteriores (n_draw 67–68, gpu_busy ~18,2, 51–52 fps). Pero `pelea2.jpg` muestra la **presentación de equipos**, no la pelea: el replay se desfasó del flujo. Y **los rivales difieren** entre las dos (espejo contra Kula/Foxy/Angel). `0x82636BEC` se llama en varios frames: parece un consumidor por frame, no la elección del rival.
  - **Por eso quedó apagado** (solo con `--at-seed-picks`). `--at-seed` sola se comporta como antes.
- **Camino correcto** (pendiente): comparar `--at-randlog=900` de dos corridas con fps distintos (por ejemplo `_nl_hook_skip.json` a ~33 fps contra `_nl_spin_novidle.json` a ~54). Buscar el llamador cuya cantidad de llamadas depende del frame y darle, bajo `--at-seed`, un estado de RNG propio (guardar y restaurar `_holdrand` en ptd+20 alrededor de su llamada). Así el resto de la secuencia no depende de los fps.
- **Mientras tanto**: para comparar A/B, mirar `pelea2.jpg` y emparejar corridas de la misma escena (fábrica ≈ gpu_busy 15,5 con G; parque ≈ 13–15).

## Uploads adelantados al principio del submit (`shared_memory_upload_hoist`): ganancia chica, limitada por la granularidad de página (2026-11-07)

- **Cvar nuevo `shared_memory_upload_hoist`** (default false, InitOnly):
  - `vulkan/shared_memory.cpp` `UploadRanges`: si ningún comando anterior del submit tocó el rango de la subida, la copia no va al stream diferido. Se guarda en `hoisted_uploads_`, y `ExecuteHoistedUploads` (llamado en `vulkan/command_processor.cpp` justo antes de `deferred_command_buffer_.Execute`) la graba con barreras ALL_COMMANDS antes y después.
  - Rangos tocados: los que pide `RequestRanges` (hook virtual nuevo `SharedMemory::OnRangesRequested`), los escritos por la GPU vía `Use(…, written_range)` y las subidas en línea. Hay 256 rangos como máximo por submit; si se pasa, ya no se adelanta nada en ese submit.
- **Corrida** (run 001836, `configs/_nl_hoist.json` = G + hoist + `--at-cmd-trace=1400`; parque, imagen correcta): **39,9 fps**, gpu_busy 15,06 (p99 15,9), n_rp 48, n_bar 85 (antes ~100), n_copy 43.
  - En el parque solo había medida F (solo spin de wrm, 37,5); G (53,8) era en la fábrica. **No se puede comparar bien.**
- **Traza**: el primer submit largo ya va sin cortes (`[1d21d21d…]`, ~40 draws en una pasada). Los siguientes siguen con `b8cb[1d2]` por sprite: el vértice del sprite siguiente cae en la **misma página de 4 KB** que leyó el draw anterior, así que hay solapamiento y la subida va en línea.
  - Adelantarla igual sería incorrecto: el juego puede reescribir esos bytes después de que el CP escribió la cerca, y el draw anterior leería datos nuevos.
- **Para que rinda de verdad**, falta comparar a nivel de byte: guardar por página una copia de lo ya subido en el submit y adelantar solo si los bytes que leyeron los draws anteriores no cambiaron. O subir solo los bytes cambiados a un ring nuevo y redirigir el vfetch.
- **Problema de medición**: la escena alterna entre corridas (fábrica o parque) aunque se use `--at-seed=1`. Para comparar A/B hay que correr las dos variantes en la misma escena: repetir hasta que coincida, o fijar la escena.
- **Próximo paso**:
  1. Fijar la escena del bench (¿la alternancia depende del guardado o de un contador?) para poder comparar A/B de verdad.
  2. Con eso, medir G contra G + hoist en la misma escena. Si gana, pasar los cvars a default.
  3. Seguir con los registros por lote (paso 2 del agente) y ajustar `vsync_precise_spin_us` solo.

## Vblank puntual + spin en WAIT_REG_MEM: dock 30,7 → 53,8 fps en la misma escena (2026-11-07)

- **Cvars nuevos** (los dos con default 0, o sea igual que antes):
  - `vsync_precise_spin_us` (`graphics/graphics_system.cpp`, hilo "GPU VSync", InitOnly): duerme hasta N µs antes del próximo vblank y después cede con yields. Antes sondeaba con `Sleep(1 ms)` y el vblank llegaba hasta ~1 ms tarde.
  - `cp_idle_spin_us` (`graphics/command_processor.cpp` `WorkerThreadMain`, lazo idle, solo Switch): sondea el puntero de escritura N µs antes de dormir en el evento. El `do/while` del lazo pasó a `while`; con 0 se comporta igual.
- **Corridas en dock** (1020/768/1600, base = config C del hook: `_nl_hook_skip.json`; imagen correcta):
  | run | config | escena | fps | % frames > 16,7 | cp_wrm | gpu_busy |
  |---|---|---|---|---|---|---|
  | C (234915) | `_nl_hook_skip.json` | parque | 33,5 | 92 % | 14,0 | 13,2 |
  | E (000653) | `_nl_spin.json`: wrm 18000 + vsync 300 + idle 200 | fábrica | 47,3 | 67 % | 4,9 | 15,9 |
  | F (000907) | `_nl_spin_wrm.json`: solo wrm 18000 | parque | 37,5 | 86 % | 10,9 | 13,4 |
  | **G** (001108) | `_nl_spin_novidle.json`: **wrm 18000 + vsync 300** | **fábrica** | **53,8** | **57 %** | 2,8 | **15,5** |
  - Referencia en la fábrica sin nada de esto: dock-7, 30,7 fps.
  - **El spin idle resta** (E contra G): probablemente compite por los núcleos con el juego (CPU total 212 %). No usarlo.
  - **Lo que más pesa es el vblank puntual.**
- **Límite nuevo**: gpu_busy **15,5 ms** (p99 16,8) contra 16,67. Ahora la GPU es lo que deja afuera al 57 % de los frames, junto con la variación del CP.
- **Próximo paso**:
  1. Probar solo `--vsync_precise_spin_us=300` (sin el spin de wrm, que quema un núcleo) y valores 100 o 500. Después pasar a default lo que gane.
  2. Bajar la GPU en dock (~1–2 ms): la idea (a) del agente, juntar los uploads `8c` por draw para no cortar la pasada (`vulkan/shared_memory.cpp:307-314`, `RequestRange` desde `vulkan/command_processor.cpp:5457`). ~50 pasadas y ~100 barreras por frame. Ver también gp_resolve (~2,9) y gp_rt_xfer (~1,3).
  3. Registros por lote (paso 2 del agente) para bajar la variación del CP.
  4. Pasar a default la config C del hook y probar menús y portátil.

## cp_tex 7,2 → 3,6 ms con un hook al memcpy del juego; dock 30,7 → 34 fps (2026-11-06)

- **Perfil por pilas en dock** (run 232358, `configs/_nl_novb_sub2ms_prof.json`), hilo GPU Commands en la pelea: **XXH3 24 %**, memcpy 15 %, y el resto repartido (WriteRegister, IssueDraw, nvk_*). Script: `tools/profsum.py <log.txt> <elf> "GPU Commands" <t_match_start>` (suma `pc image+0x…` y agrupa con addr2line).
- **Causa**: con `texture_load_cpu_poll`, las texturas cargadas por CPU no tienen protección de páginas y se re-hashean enteras (344 KB) en cada epoch. Son ~40 por frame, de las que ~20 no cambiaron.
- **Quién las escribe**: diagnóstico nuevo `--diag_write_fault_range=<lo>-<hi>` (físico hex; en `system/mmio_handler.cpp` `ExceptionCallback`, desde el frame 1300, 48 líneas `[wfault] phys … pc … lr … r1 … r2 …` como offsets de imagen). Correr con `--texture_load_cpu_poll=false`, config `configs/_nl_wfault.json`. Resultado (run dock10):
  - La textura de sprites (`1745B000`) y la paleta (`179A2000`) las escribe **`sub_8217EE38` → `sub_82726240` (memcpy del CRT: r3 dst, r4 src, r5 n)**.
  - `1FB83xxx` lo escribe `sub_8217EF40` directo y no es textura.
- **Hook** `src/recompiler/src/texture_write_hook.cpp` (redefine `sub_82726240`; agregado a `KOF2002UMNX_SOURCES`): si el destino toca páginas de una textura sondeada (`rex_texture_poll_watched`), compara origen y destino; si cambian, después de copiar llama a `rex_texture_note_cpu_write`.
  - SDK (`pipeline/texture/cache.cpp`): mapa de 1 byte por página física (`g_poll_page_dirty`, `g_poll_page_watched`). En el sondeo, una textura solo se re-chequea si hubo escritura reportada, o cada `texture_poll_hook_backstop_frames` frames. `g_texture_poll_frame` avanza en cada swap (`command_processor.cpp`).
  - **Cvars nuevos**: `texture_poll_write_hook` (default **false**, InitOnly), `texture_poll_hook_skip_hash` (default false: si la carga viene del hook, no hashea), `texture_poll_hook_backstop_frames` (default 1; 0 = solo la primera vez).
- **Corridas en dock** (1020/768/1600, todas con `--vulkan_submit_min_interval_us=2000`, imagen correcta). Ojo: el escenario cambia entre corridas (fábrica o parque) aunque se use `--at-seed=1`.
  | run | config | escena | fps | cp_tex | n_tex/n_skip | gpu_busy |
  |---|---|---|---|---|---|---|
  | dock-7 (231937) | `_nl_novb_skipmem_sub2ms.json` | fábrica | 30,7 | 7,2 | 40/20 | 13,8 |
  | A (234135) | `_nl_hook.json`, sin memcmp, skip hash | parque | 31,6 | 4,8 | 58/0 | 13,0 |
  | B (234521) | `_nl_hook.json`, sin memcmp, con hash | fábrica | 30,1 | 7,7 | 43/20 | 16,2 |
  | **C** (234915) | `_nl_hook_skip.json` (memcmp + skip hash) | parque | **33,5** | **3,8** | 41/0 | 13,2 |
  | **D** (235122) | `_nl_hook_skip_nobs.json` (C + backstop 0) | parque | **34,1** | **3,6** | 41/0 | 14,2 |
  - **Mejor configuración: C** (`--texture_poll_write_hook=true --texture_poll_hook_skip_hash=true`). D casi no gana, así que conviene quedarse con el respaldo.
  - ~40 cargas reales por frame. **Lo que queda de cp_tex son las copias completas de 344 KB** a una versión nueva de la imagen.
- **Al cerrar el juego** (`--at-exit-at-end`) la consola muestra "Se ha cerrado el programa a causa de un error" (visto en varias corridas). Antes del próximo run hay que cerrar el diálogo con A. El runner marca PASÓ igual. Falta investigarlo (¿crash en el shutdown?).
- **Análisis de un agente (2026-11-07), plan recomendado**:
  - **Camino crítico**: el juego y el CP se alternan por sprite. El juego espera la cerca antes de reescribir la textura, y esa cerca la escribe el CP al procesar `EVENT_WRITE_SHD` (`graphics/command_processor.cpp:1426`), no la GPU. Son ~37–40 ciclos por frame.
  - Cuenta: cp_tex 3,7 + ~10 de comandos + 0,75 de submit + 2,7 idle + ~0,5–1 de latencia de despertar ≈ **17,7 ms**. **Hay que recortar ≥1,5 ms (~3 para tener margen en p99).**
  - **(1) Latencias de despertar**: probar `--wait_reg_mem_spin_us=18000` (ya existe; se descartó cuando mandaba la GPU). Además, un spin corto antes del `Wait` del lazo idle del CP (`command_processor.cpp:322-340`) y que el hilo de vblank duerma hasta el plazo exacto (`graphics_system.cpp:171-178`). Para medir el trabajo real sin el salto 30/60: `--vblank_per_swap=true --vblank_per_swap_min_us=16667`.
  - **(2) Registros por lote** (verificado): `ExecutePacketType0` (`command_processor.cpp:859-880`) llama a `WriteRegister` por registro. Si `!write_one_reg`, usar `WriteRegisterRangeFromRing` (`:622` → `VulkanCommandProcessor::WriteRegistersFromMem`), y sacar `GetRegisterInfo` del camino caliente (`:478`). Estimado: 0,5–2 ms.
  - **(3) cp_tex**: histograma del tamaño de cada memcpy (r5) en el hook. Si son chicos, copiar solo las filas sucias a la versión libre. Si no, mover la copia a un worker.
  - Después, para portátil: juntar los uploads por draw (`vulkan/shared_memory.cpp:307-314`). No devolver la cerca antes, porque también libera memoria del D3D.
- **Próximo paso**:
  0. Seguir el plan del agente en orden: (1) spin y vblank, después (2) registros por lote, después (3) cp_tex.
  1. Pasar C a default (`texture_poll_write_hook` y `texture_poll_hook_skip_hash` en true) después de mirar menús, selección de personajes y otra escena, y probar en portátil.
  2. Bajar las copias: el hook sabe el rango exacto escrito; copiar a la versión nueva solo ese rango desde la versión anterior es GPU, así que mejor escribir en el lugar si la versión no está en uso, o copiar por CPU solo el rango sucio en una imagen en anillo.
  3. El resto del hilo del CP (~10 ms fuera de cp_tex y de la espera al vblank): WriteRegister, IssueDraw, descriptores de nvk. Perfil de pilas de nuevo con C.
  4. GPU (portátil): cortes de pasada por el upload `8c` antes de cada draw de sprite (sección de abajo).

## Dock con pelea válida: 30 fps con GPU 13,8 ms; el límite es el hilo del CP, no la GPU (2026-11-06)

- **Por qué caía en el demo en dock**: el replay solo se aplicaba si había un pad conectado en el slot 0 (portátil) o en el 1 (No1). En dock sin mando no hay ningún slot, así que no llegaba ningún input.
  - **Arreglo** (`src/input/switch/switch_input_driver.cpp` `Poll`): (a) el replay va al primer slot conectado; (b) si no hay ninguno y hay replay activo, el slot 1 cuenta como **pad virtual** (`[AT] log [input] slot 1 (npad 0) connected (virtual, replay)`); (c) los cambios de conexión salen por `at_log`.
  - `match_start` es una marca del archivo de replay por vblank (vb 1707): **se dispara aunque el juego esté en el demo**. Validar siempre con `pelea*.jpg` o con n_draw ≈ 100 (demo: gpu_busy ~2,9).
  - Ojo: en dock la pelea es **Maxima vs Foxy en la fábrica**, no la escena del parque del portátil. No comparar números entre los dos modos.
- **Contadores nuevos por frame** (zonas `n_draw n_rp n_bar n_copy n_disp`, contadas en `DeferredCommandBuffer::Execute`; `AT__MAX_ZONES` 24 → 32).
- **Traza de comandos**: `--at-cmd-trace=<frame>` loguea `[cmd] f<frame> n=… <secuencia>` por submit, en ese frame y el siguiente. Letras: d draw, [ ] rendering, b barrera, u buf→img, c buf, i img, l blit, x dispatch, k clear img, a clear attach, 0–9 marcador de categoría (2 rt_xfer, 5 resolve, 8 upload).
- **Corridas en dock** (relojes 1020/768/1600, todas con pelea válida):
  | run | config | fps | gpu_busy | gpu_span | cp_submit | cp_tex | cp_wrm |
  |---|---|---|---|---|---|---|---|
  | dock-4 (230857) | `_nl_novb_skipmem.json` | 30,1 | 16,1 | 31,9 | 1,90 | 7,7 | 11,6 |
  | dock-6 (231519) | `+ --vulkan_submit_on_primary_buffer_end=false` | 30,3 | 14,9 | 14,9 | 0,15 | 7,1 | 15,1 |
  | dock-7 (231937) | `_nl_novb_skipmem_sub2ms.json` (`--vulkan_submit_min_interval_us=2000`) | **30,7** (4 % de frames < 16,7) | **13,8** | 29,6 | 0,84 | 7,2 | 13,1 |
  - Por frame: ~100 draws, **50 render passes, 100 barreras, 53 copias**.
- **Traza (dock-5)**: **~37 submits por frame**. Unos 27 son `2b8cb[1d]`: barrera, **upload de buffer (copia)**, barrera, abrir rendering, **un draw**, cerrar y submit. El juego termina un buffer primario por cada sprite y `vulkan_submit_on_primary_buffer_end` hace un submit por cada uno. Sin esos submits se siguen cortando las pasadas: cada sprite trae su `8cb` (upload de memoria compartida) antes del draw.
- **Cvar nuevo `vulkan_submit_min_interval_us`** (default 0, igual que antes) en `vulkan/command_processor.cpp` `OnPrimaryBufferEnd`: solo hace submit si pasaron N µs desde el último. Con 2000 es lo mejor medido.
- **Conclusión**: en dock la GPU ya entra (13,8 ms). Lo que no entra es el **hilo del CP**: en un frame de 33 ms espera 13 ms al vblank, o sea que trabaja unos 17–20 ms, de los que cp_tex son 7,2 (hash/carga de la textura de sprites, ~40 cargas y ~20 salteadas por hash). Con un submit por frame, CP y GPU van en serie.
- **Próximo paso**:
  1. **Bajar cp_tex (7 ms)**: es el mayor trabajo del CP. Opciones: usar el rango de páginas que invalidó el write-watch para comparar o copiar solo esas páginas, en vez de hashear la textura entera; o el hook al escritor de la textura en el juego (ver la sección de abajo y `texture_partial_reload` en `pipeline/texture/cache.cpp` ~470, que ya se probó y era más lento con shadow completa).
  2. Perfilar el resto del hilo del CP (fuera de cp_tex) con el perfil por pilas (`_nl_novb_skipmem_prof.json`) en dock.
  3. GPU (para portátil): los 50 cortes de pasada vienen del `8c` (upload de memoria compartida) antes de cada draw de sprite. Para evitarlo, el vertex data tendría que leerse de un buffer host-visible sin copia, o agruparse los uploads antes de la pasada.
  4. Decidir el default de `vulkan_submit_min_interval_us` (2000 parece mejor; probar también en portátil).

## En dock (GPU 768 de serie) el bench no llega a la pelea: falta una medición válida (2026-11-06)

- **dock-1** (run 20261106-225824, `_nl_novb_skipmem.json`; relojes **CPU 1020 / GPU 768 / EMC 1600**): **60,3 fps**, FRAME 16,58, gpu_busy 2,87, cp_wrm 14.
  - **No vale**: `pelea.jpg` es la pantalla "New comers in unlimited match" y `pelea2.jpg` la historia de K9999. El replay de inputs no entró a la pelea y el juego cayó en el modo demo (attract), aunque `match_start` se marcó igual.
  - Sospecha: en dock cambia el tipo de mando y los inputs no llegan; o, a 60 fps, el replay se desfasa.
- **dock-2** (run 230016): nxlink "Connection failed". El netloader no se abrió. Antes de HOME + A, mirar la captura de sys-botbase: el cursor a veces queda en NFS, y con `click DLEFT` vuelve al Netloader.
- **Próximo paso**:
  1. Volver a correr en dock (abrir el netloader, verificar con una captura) y mirar las capturas.
  2. Si cae de nuevo en el modo demo: revisar cómo inyecta los inputs el autotest (`--at-input`, `bench_input_vb.txt`) en dock, y si la marca `match_start` se dispara también en el modo demo (hoy se dispara: es una falsa señal).
  3. Con una pelea válida en dock: si da 60, el objetivo en dock está; en portátil sigue faltando bajar gp_draw (sección de abajo).

## Saltear dump + resolve a memoria cuando la copia ya dejó la textura: GPU 29,5 → 21,7 ms; el límite pasó al CP (2026-11-06)

- **A/B resolve → textura** (misma escena, parque, Maxima contra Maxima, `--at-seed=1`):
  - **ab-off** (run 224037, `configs/_nl_novb_rttoff.json`): **28,3 fps**, gpu_busy 33,05, resolve 4,49, tex_load 6,44.
  - **ab-on** (run 224208, `_nl_novb_rtt.json`): **31,0 fps**, gpu_busy 29,48, resolve 8,10, tex_load 0,13.
  - Imagen correcta en las dos.
- **Cvar nuevo `resolve_skip_memory_after_copy`** (default **false**, InitOnly), en `vulkan/render_target_cache.cpp` `Resolve`:
  - El bloque rtt quedó en el lambda `rtt_copy`. Con el cvar activo se llama **antes** del dump.
  - Si copió alguna textura, no se hace `DumpRenderTargets`, ni `RequestRange`, ni el dispatch del compute: solo `MarkRangeAsResolved` + `FinishResolveCopyToTextures`, y se setea `written_*`.
  - Log: `[rtt] memory write skipped -> base (n textures)`, las primeras 8 veces.
  - **Riesgo**: la memoria guest queda vieja. Rompe si la CPU lee el resolve, o si otra textura con otra clave (o una recreada) se recarga desde ahí. Ver COD6-NX en la sección de abajo.
  - Nota: `direct_host_resolve` (default true) **no saltea el dump**; `TryResolveCopyDirectly` termina llamando a `DumpRenderTargets`.
- **skipmem** (run 224704, `configs/_nl_novb_skipmem.json`, build `_logs/skipmem-build.log`):
  - Imagen correcta (misma escena, con HUD).
  - **gpu_busy 29,5 → 21,7**, dump 3,7 → **0**, resolve 8,1 → **3,65**.
  - Pero **30,7 fps** (frame 32,5 ms): **el límite ya no es la GPU**.
  - CP: cp_wrm (sleep en WAIT_REG_MEM) **10,35 ms**, p99 25; cp_tex **7,5 ms** (n_tex 39, n_skip 20); cp_submit 2,0.
  - gpu_span 30,1 contra busy 21,7: la GPU espera al CP.
  - CPU total 180 %.
- **Perfil por pilas** (run 225152, `configs/_nl_novb_skipmem_prof.json`, **relojes de serie: CPU 1020 / GPU 460,8**, 30,2 fps):
  - **Main XThread ~100 %**: 41 % `sub_826571A8`, 16 % `sub_82655B48`, 9 % `sub_826B0628`, 17 % save/restgpr.
    - **Todo es la espera activa del D3D por la cerca de la GPU**: `sub_82655B48` gira mientras `*(10896)` no llega a la cerca; `sub_826571A8` hace `db16cyc`×32 con timeout de 5000 ticks; `sub_826B0628` solo lee un tick del KPCR.
    - **No hay trabajo real del juego que optimizar ahí.**
  - **GPU Commands 33–57 %**: XXH3 16 % (el hash de la textura de sprites), memcpy 10 %.
  - Agrupar por función: script inline con addr2line sobre `kof2002um_nx` (el ELF con símbolos del build actual, en la raíz del port; los de `out/elf-archive` son viejos).
- **Por qué los fps no suben (clave)**:
  - La única espera del CP por frame es WAIT_REG_MEM `mem 0x1FC9B004 == 0`, el flag que limpia el **vblank** (ver `wrm-1` más abajo).
  - El juego va **sincronizado al vblank**: un frame de más de 16,7 ms tarda 2 vblanks, o sea 30 fps. **Los fps saltan de 30 a 60 recién cuando todo entra en 16,7 ms.**
  - cp_wrm de ~10 ms es esperar al vblank, y la espera activa del juego es consecuencia de eso.
- **Lo que falta para 60 en portátil (GPU 460,8)**: gpu_busy **21,7 → < ~15 ms**.
  - **gp_draw 14,9** es el grueso: 73 draws por frame.
  - Para un 2D a 640x448 eso no es fill-rate. Sospecha: un costo fijo por draw o por pasada, porque la CPU reescribe la textura de sprites ~20 veces por frame y cada vez hay carga, barrera y cambio de pasada.
  - El resto: resolve 3,65, rt_xfer 1,75.
  - En dock (GPU 768 de serie) daría ~13 ms: probablemente ya 60. Falta medir.
- **Próximo paso**:
  1. Desglosar gp_draw: render passes y barreras por frame, y cuántos draws hay entre cargas de la textura de sprites. Ver si se pueden juntar las subidas o evitar cortar el render pass, por ejemplo con `vkCmdUpdateBuffer` o un ring sin barrera completa.
  2. Pedirle al usuario una corrida en dock para confirmar si a 768 ya da 60.
  3. Pasar `resolve_skip_memory_after_copy` a default después de probar menús y otras escenas (ahorra 8 ms de GPU; es necesario para 60).
  4. cp_tex 7,5 ms (XXH3 de la de sprites): hook a la función del juego que escribe esa textura, para no hashear.

## Resolve → textura del host funciona con swap R↔B: tex_load 6,5 → 0,18 ms; ya por defecto (2026-11-06)

- **rtt-1** (run 20261106-222806): los 5 resolves se descartaban con `dest format`. `dest info 01000302` es formato 8_8_8_8, endian 8in32 y **`copy_dest_swap = 1`** (el juego guarda el resolve con R y B intercambiados). Con `vkCmdCopyImage` sola no se puede.
- **Arreglo**:
  - `ResolveCopyToTextures(..., swap_red_blue)`: hace un **blit** del RT RGBA8 a una imagen temporal `B8G8R8A8_UNORM` de 1280x720. El blit convierte por color, así que los bytes quedan B,G,R,A.
  - Después hace una **copia cruda** de esa temporal a la textura RGBA8. El blit se hace una vez por resolve, con la primera textura que coincide.
  - Archivos: `vkCmdBlitImage` en `device_1_0.inc`; comando `kVkBlitImage` en `deferred_command_buffer`; `rtt_swap_image_`/`rtt_swap_memory_` en `texture_cache.h`, que se liberan junto al load ring.
- **rtt-2** (run 20261106-223636, build `_logs/rtt2-build.log`):
  - Imagen correcta: colores bien en `pelea2.jpg`, aunque la escena es otra (el parque).
  - **30,5 fps** (alias-2: 27,1, otra escena).
  - gpu_busy 35,1 → **30,7**, tex_load 6,53 → **0,18**, resolve 4,5 → **8,55**, draw 15,0, rt_xfer 1,96, dump 3,76.
  - `[rtt] copied` en 1F6F8000, 185BA000, 186D2000 y 1F52B000. Hay algunos `no matching texture` y `render target key` sueltos, porque se loguean una vez por base y motivo.
- **Default `true`** (build `_logs/rtt3-build.log`).
- **Ideas del agente** (otros ports, 2026-11-06):
  - **sa2-nx "colour handover"**: en vez de copiar, el destino comparte la imagen del RT y se muestrea con una view que aplica el swizzle; las imágenes se intercambian en el siguiente clear completo. Esto sacaría el costo del blit y la copia, que hoy suma ~4 ms dentro de resolve.
  - **COD6-NX**: el mismo esquema con `resolve_destinations` corrompía texturas cuando la memoria se reusaba, porque nada invalidaba la entrada. Nosotros usamos watch + `MakeUpToDateAndWatch`, que en principio lo cubre.
  - **USF4/MK9**: llegan a 60 fps con el renderer Renut/deko3d. Hookean D3DDevice_* sin PM4 ni EDRAM. Con NVK se mide ~3 µs por bind de pipeline y ~2,5 µs por draw en A57, lo que explicaría el hilo CP de ~21 ms. Renut no tiene licencia: hay que pedirle permiso a masterspike52.
  - **Relojes**: esos ports miden con la GPU a 768/844 MHz por sys-clk.
  - Docs: `github.com/ChanseyIsTheBest/sa2-nx/docs/renut-renderer.md` (ítems 36–39), `github.com/Thorhax/usf4-nx/docs/deko3d-renderer-usf4.md`. Descargas en el scratchpad de la sesión (temporal).
- **Próximo paso**:
  1. Correr el build por defecto (`rtt3`) con la config de alias-2 y un par de seeds, para un A/B en la misma escena.
  2. Bajar el resolve de 8,55 ms. Opción a: ver si el resolve compute hacia memoria puede saltearse cuando todos los destinos se copiaron y nadie los lee por CPU (ReGPU no escribe a memoria por defecto; ver los riesgos de COD6). Opción b: el handover de sa2-nx.
  3. Clear D0@0→C0@0 (rt_xfer, 2 ms).

## Resolve → textura del host (cvar `resolve_copy_to_texture`): escrito, falta compilar y medir (2026-11-06)

- **Objetivo**: sacar tex_load (6,5 ms a 460,8): las ~17 cargas/frame de 4 RGBA8 tiled que escribe un resolve (`186D2000`, `185BA000`, `1F52B000` 640x448, `1F6F8000` 1280x720) y se recargan desde memoria por compute.
- **Idea** (sin shaders nuevos): en `VulkanRenderTargetCache::Resolve` (vulkan/render_target_cache.cpp, después del `CmdVkDispatch` del resolve), si el origen es 1x, `k_8_8_8_8`, de un solo RT que cubre todo el rango, y el destino es `k_8_8_8_8` UNORM sin swap/exp_bias/array, pasar el RT a TRANSFER_SRC y hacer `vkCmdCopyImage` RT → la textura que ya existe en `copy_dest_base` (2D, tiled, sin mips, mismo pitch y endian, host `R8G8B8A8_UNORM`). Después de `MarkRangeAsResolved` (que la invalida), `FinishResolveCopyToTextures` la vuelve a marcar al día con `MakeUpToDateAndWatch`. El resolve sigue escribiendo la memoria igual.
- **Archivos tocados**: `vulkan/texture_cache.cpp/.h` (`ResolveCopyToTextures`, `FinishResolveCopyToTextures`, `resolve_copied_textures_`); `pipeline/texture/cache.h` (helpers protegidos `AcquireGlobalLock`, `ForEachTextureAtBasePage`); `vulkan/deferred_command_buffer.h/.cpp` (comando `kVkCopyImage`); `ui/vulkan/functions/device_1_0.inc` (`vkCmdCopyImage`); `vulkan/render_target_cache.cpp` (cvar `resolve_copy_to_texture`, default false, InitOnly; RT 1x de color con `TRANSFER_SRC` si el cvar está; bloque en `Resolve`). Logs `printf("[rtt] ...")`: una línea por destino+motivo (`copied`, `source msaa/format`, `dest format`, `not one render target`, `render target key`, `no matching texture`) y `[rtt] skip` por textura descartada.
- **Ojo**: el canal `gpu` (REXGPU_INFO/WARN) **no llega** al log de nxlink. El aviso del alias se pasó a `printf("[alias] ...")`. El "0 líneas `msaa_alias`" de alias-2 **no probaba nada**; la próxima corrida lo dice de verdad.
- **Estado**: el primer build falló por `DataDimension::k2D` (es `k2DOrStacked`); arreglado y **recompilado bien** (exit 0, `_logs/rtt-build.log`). Config lista: `devkitnx/configs/_nl_novb_rtt.json` (= alias + `--resolve_copy_to_texture=true`, sin BOM).
- **Próximo paso**: 1) (hecho: el build compila) 2) abrir el netloader (sys-botbase HOME + A) y correr `python runner/nxtest.py run configs/_nl_novb_rtt.json --no-build` desde `devkitnx/`; 3) en `log.txt` mirar las líneas `[rtt]` y `[alias]`; comparar tex_load/gpu_busy/fps con alias-2 (27,1 fps, tex_load 6,5) y mirar `pelea2.jpg` (si la escena es la fábrica, K' contra Vanessa, se compara directo con alias-2); 4) si los resolves salen `source msaa/format` porque el RT de la escena es 2x (forzado a 1x con `gpu_force_msaa_1x_from_2x`), aceptar también 2x cuando la clave del RT sea 1x.

## Alias 4x→1x solo para clears, ya por defecto: 26,7 → 27,1 fps en la misma escena (2026-11-06)

- **Las capturas de alias-1 no se podían comparar**: aun con `--at-seed=1`, el replay se desincronizó (alias-1 terminó en Neo Geo World; stock-460 y alias-2 en la fábrica, K' contra Vanessa). Los 28,3 fps de alias-1 son de **otra escena**.
- **Cambio** `pipeline/render_target/cache.cpp` `Update`: el alias solo se aplica a draws **tipo clear**: `VGT_DRAW_INITIATOR.prim_type == kRectangleList`, depth (si se usa) con `zfunc` always, y sin blend en los RT de color usados (one/zero/add, color y alpha). El resto de los draws 4x siguen por 4x y se loguean con `REXGPU_WARN "msaa_alias: 4x draw kept as 4x ..."` (las primeras 16 veces). Un clear a valor constante deja los mismos bytes en EDRAM en 4x o en 1x, así que es correcto por construcción. **Default pasado a `true`** (build `_logs/alias3-build.log`).
- **`alias-2`** (run 20261106-163150, `_nl_novb_alias.json`, build `_logs/alias2-build.log`): misma escena que stock-460, **imagen bien** (`pelea2.jpg` igual a stock), **27,1 fps** (36,84 ms; stock 26,7), gpu_busy 35,1 → **34,4**, rt_xfer 2,9 → **2,2**, draw 16,6, resolve 4,5, dump 3,74, tex_load 6,5. **0 líneas `msaa_alias`** en el log: en esta escena no hubo draws 4x que no fueran clears.
- **Verde de alias-1: NO es bug.** Es Maxima contra Maxima (mismo cruce que la corrida 153219, sin alias) en la intro del round (sin HUD). Ampliado, el panel verde es el escáner del cyborg: grilla, cuadros con lecturas y la silueta de un cuerpo robótico con un corazón rojo, igual en los dos. La barra verde que cruza la pantalla es la baranda de la montaña rusa del escenario (también sale en 153219).
- **Próximo paso**: resolve → textura del host (tex_load 6,5 ms) y el clear D0@0→C0@0 (rt_xfer restante, 2,2 ms).

## Alias EDRAM 4x → 1x (estilo COD6-NX): 26,7 → 28,3 fps sin OC; ¿imagen? falta confirmar (2026-11-06)

- **Qué hace el juego**: color+depth 4x de 320x2048 en base 1440/1664 son solo para el **clear** (rect list de 3 vértices, modo 4, z always+write, dc `00000076`); la escena 640x448 se dibuja y se resuelve en los 1x de 640x4096 en la misma EDRAM. Igual que D0@0 4x (clear depth-only) → C0@0 1x.
- **Cambio** (cvar nuevo **`gpu_msaa_4x_alias_to_1x`**, default **false**, InitOnly): `pipeline/render_target/cache.cpp` `Update`: si el draw es 4x y **todos** los RT usados ya existen como 1x con misma base/pitch/formato, usa el 1x (`last_update_msaa_alias_`, accesor en `include/.../render_target/cache.h`; altura estimada ×2). `vulkan/render_target_cache.cpp` `Update`: render pass key en 1x. `vulkan/command_processor.cpp`: escala ×2 en viewport (`GetHostViewportInfo`), scissor (`UpdateDynamicState`) y flag `kSysFlag_MsaaSamples` en 1x. No toca D0@0→C0@0 (depth→color, distinto tipo).
- **`alias-1`** (run 20261106-162455, `configs/_nl_novb_alias.json` = poll + alias; build `_logs/alias-build.log`): **28,3 fps** (26,7), gpu_busy 35,1 → **32,9**, rt_xfer 2,94 → **2,17**, draw 16,5 → 15,4, dump 3,72, resolve 4,48, tex_load 6,42.
- **OJO imagen**: `pelea2.jpg` muestra **rectángulos verdes en grilla sobre los dos luchadores** (¿roto o es la intro del round?). Comparar con `pelea2.jpg` de stock-460 (run 20261106-155036) antes de dar por bueno; si es del alias, sospechar de un draw 4x que no es clear (sprites/blend) o de la flag MSAA del shader.
- **Próximo paso**: 1) confirmar la imagen (comparar capturas; si está rota, limitar el alias a draws tipo clear: rect list + z always/write sin blend); 2) si está bien, pasar el default a true; 3) siguiente: resolve → textura del host (tex_load 6,4) y el clear D0@0→C0@0 (rt_xfer restante).

## Relojes de serie: 17 → 26,7 fps pidiendo el perfil apm de 460,8 (2026-11-06)

- **El usuario sacó el OC.** `stock-1` (sin pedir perfil): CPU 1020 / **GPU 307,2** / EMC 1331,2, **17,3 fps**, gpu_busy 54,6 (draw 24,6, tex_load 11,2, resolve 7,2, dump 6,0, rt_xfer 4,2).
- **`kof2002um_nx_app.h` `OnPostSetup`**: llama a `RexSwitchApmRequestGpuMhz(460)` + `RexSwitchApmApply()` (la API ya estaba en `switch_apm.cpp`, pero el port nunca la llamaba). En portátil elige 0x92220008 = GPU 460,8 + EMC 1331,2, el perfil de los juegos comerciales (sin OC); en dock no toca nada.
- **`stock-460`** (run 20261106-155036, `_nl_novb_poll.json`; relojes confirmados 1020/460,8/1331,2): **26,7 fps**, gpu_busy **35,1** (draw 16,5, tex_load 6,6, resolve 4,5, dump 3,75, rt_xfer 2,9), cp_tex 3,5. **Manda la GPU.**
- **Agente (2.ª investigación)**, por prioridad: (1) que el resolve escriba directo la textura del host (−~5 ms de tex_load a 460); (2) el truco 4x↔1x (dump + rt_xfer) como clear o draw 1x sobre el alias, ref. COD6-NX `native_msaa_alias.h`; (3) 1x solo en el RT 2x (ya probado: ≤1 ms); (4) saltear resolves repetidos; (5) `vulkan_submit_on_primary_buffer_end=false` (menos envíos); (6) semántica de fences de xenia (`EVENT_WRITE_SHD` sin esperar a la GPU del host); (7) hooks D3D para la textura de sprites. Referencia: COD6-NX reemplazó la ruta Xenos genérica por un renderer nativo. Fuentes en scratchpad `web2`.
- **`nosub-1`** (`_nl_novb_nosub.json` = poll + `--vulkan_submit_on_primary_buffer_end=false`, run 155302): **se cuelga** antes de la pelea (timeout de 1200 s; frames de 66 ms con cp_submit 25). Descartado: el lockstep necesita ese envío.
- **Próximo paso** (manda la GPU, 35 ms a 460,8): 1) el truco 4x↔1x: ver en el log `[rtc]` y en las transferencias qué hace el juego con las superficies 4x de base 1440/1664 (¿clear o relleno?) y reemplazarlo por un clear o un draw 1x (dump + rt_xfer ≈ 6,7 ms a 460); 2) resolve → textura del host para los 4 RGBA8 tiled (tex_load ≈ 6,6); 3) gp_draw 16,5: ver si el muestreo LINEAR de las texturas grandes cuesta (antes de lin-2, draw era 1,5 ms menor con OC).

## Sondeo: epoch y escritura en el lugar; 49,6 fps. OJO: medimos con OC (2026-11-06)

- **Relojes (corrección)**: todos los benchs de hoy son **CPU 1785 / GPU 921,6 / EMC 1600 y en portátil** (`Reverse-NX: console in hand`). Eso es **OC** (de serie: CPU 1020, GPU 768 en dock y 460,8 en portátil). La línea «máximos de serie en dock» de la sección de abajo estaba mal. El objetivo del usuario es 60 fps **sin OC**: falta medir con los relojes de serie (sys-clk).
- **`tl-1`** (`--diag_texload_log=50000`): lo que se recarga desde un resolve son siempre 4 RGBA8 tiled (`186D2000`, `185BA000`, `1F52B000` de 640x448 y `1F6F8000` de 1280x720): intermedios del juego (escena en 640x448 → escala a 720p). Es carga por GPU limitada por el ancho de banda (~185 µs/1,1 MB); evitarla exige que el resolve escriba la textura, y el repo no trae las fuentes de los shaders.
- **`poll-prof`** (48,0 fps): en el hilo CP, **XXH3 18,8 %** + memmove 11,2 %. La textura de sprites `1745C000` (640x448 R8, pitch 768, 343.936 B) no entra en la comparación por filas (pide `pitch × filas ≤ tamaño`), así que hashea 344 KB en cada sondeo.
- **`texture_load_cpu_poll_per_epoch`** (default true; `g_texture_poll_epoch` avanza en cada `ExecutePrimaryBuffer` y en cada `WAIT_REG_MEM`): `epoch-1` 45,5 fps, **sin cambio** (el juego va draw por draw en lockstep: cada draw ya cae en un epoch nuevo).
- **Comparación por filas para la de sprites** (aceptando el pitch sin relleno): `inplace-1` **peor** (30,2 fps, cp_tex 8,0) → revertido, queda en el hash.
- **Escritura en el lugar** (`VulkanTexture::cpu_last_use_submission`, que se marca al tomar la vista; si es < actual y ≤ completada, se escribe la versión actual sin cambiarla, solo la franja si hay filas parciales): correcta pero se usa poco (300 de 47.104). **`inplace-2`** (run 20261106-153219): **49,6 fps**, gpu_busy 15,56, cp_tex 5,26, imagen bien (otro escenario: los runs alternan escenario y rival).
- **Próximo paso**: 1) medir con **relojes de serie** (CPU 1020, GPU 768 en dock) para saber dónde estamos de verdad; 2) cp_tex ~5,3 = XXH3 de la de sprites en cada draw → candidato: hook a la función del juego que escribe esa textura (estilo Unleashed `UnlockTextureRect`; Ghidra) para marcarla sucia sin hashear; 3) GPU: MSAA 4x del truco de alias y EDRAM (resolve+dump+rt_xfer ~5 ms).

## Sondeo de texturas por CPU: 30 → 47,4 fps con la imagen bien; ya por defecto (2026-11-06)

- **`texture_load_cpu_poll`** (base `pipeline/texture/cache.cpp` `RequestTextures` + hook `LoadsBaseWithoutSharedMemory`; `Texture::MarkBaseOutdatedForPoll`): las texturas `cpu_direct` no suben su rango a la memoria compartida (sin proteger/re-proteger páginas). En cada `RequestTextures` se marcan como desactualizadas y la carga las compara con la sombra (`texture_partial_reload`) o el hash, y se salta las iguales. `MakeUpToDateAndWatch` registra el watch solo si no hay uno (así se siguen viendo las escrituras de la GPU). Si la CPU no puede cargar, el fallback pide el rango antes de ir a la GPU.
- **`poll-1`** (run 20261106-151159, `_nl_novb_poll.json` = msaa2x + cpu + poll): **47,4 fps** (71 % de frames > 16,7 ms), imagen correcta (capturas y el usuario lo vio en la consola: «se ve recontra jugable»). cp_tex 13,1 → **5,5**, GPU CP 58,7 %, gpu_busy 17,26, n_tex 52 (n_skip 20), FRAME 21,1 prom / p95 32,9.
- **`texture_load_cpu_direct` y `texture_load_cpu_poll` ahora true por defecto** (build `_logs/poll-default-build.log`). `gpu_force_msaa_1x_from_2x` sigue en false (gana ≤1 ms).
- **Lo que sigue cargándose por GPU** (contadores `[cpu]`): `linear=1 tiled=1 gpu_written=1` **~17 cargas/frame de ~1,4 MB** (24 GB en el run): texturas **tiled que escribe un resolve** y se recargan desde la memoria compartida. Son el grueso de gp_tex_load 3,15. Muy pocas tiled que escribe la CPU (5 en todo el run): no vale la pena destilarlas en CPU.
- **Investigación (agente)**: xenia solo usa watches (agranda las invalidaciones a 256 KB porque fallar sale más caro que subir). UnleashedRecomp/MarathonRecomp enganchan `LockTextureRect`/`UnlockTextureRect` del D3D del juego (`GUEST_FUNCTION_HOOK`) y suben en el Unlock. ReGPU hashea una vez por frame. Alternativa más barata que el sondeo: un hook en la función del juego que escribe o desbloquea la textura de sprites, para marcarla sucia sin memcmp.
- **Próximo paso para 60**: 1) resolves → textura: que el resolve escriba directo en la textura (o que no se recargue lo que el resolve ya dejó en la GPU), para sacar ~17 cargas/frame de 1,4 MB; 2) perfil de pilas de nuevo para ver qué queda en cp_tex 5,5 (¿memcmp del sondeo?); 3) EDRAM: resolve/dump/rt_xfer (~6 ms de GPU).

## Carga de texturas por CPU con versiones de imagen: tex_load 4,9 → 3,4; el hilo CP (~21 ms) también frena los 60 (2026-11-06)

- **Objetivo del usuario: 60 fps sin OC** (relojes actuales: CPU 1785, GPU 921,6, EMC 1600 = máximos de serie en dock).
- **Nuevo cvar `texture_load_cpu_direct`** (default false; `vulkan/texture_cache.cpp` `LoadTextureCpuDirect` + `VulkanTexture::SwitchToFreeCpuVersion`): las texturas LINEAR **sin tiling** con carga de copia plana (8/16/32 bpb + endian) se crean host-mapped (PREINITIALIZED) y la CPU las llena desde la memoria del guest. Cada recarga va a otra **versión** de la imagen (libre si `retired_submission <= completed`), así los draws ya grabados leen la vieja; sin dispatch ni barreras. `GetActiveBindingOrNullImageView` toma la vista de la versión actual. Tope de 128 versiones / 64 MB por textura; si no hay, vuelve a la GPU (copia).
- **`cpu-1`** (run 20261106-143447, `_nl_novb_cpu.json` = msaa2x + cpu): **imagen bien**, 0 fallbacks, 96 versiones. 30,0 fps; gpu_busy 21,73 → **20,39**, tex_load 4,92 → **3,43**; cp_tex 11,5 → **13,1** (la copia, ~8 MB/frame). Solo pasan a CPU las R8 (640x480, 320x240) y una RGBA8 1280x434: las RGBA8 1280x720 y 640x448 siguen en GPU → **tiled** (falta destilar en CPU).
- **El hilo CP también se pasa**: "GPU Commands" 62,4 % de CPU ≈ **20,8 ms/frame** (cp_tex 13,1 es lo grueso). El Main XThread 98 % es el spin de espera al fence (línea "sub_82655C18"), no trabajo. **Para 60 hay que bajar CP (~21 → <16) y GPU (~20 → <16)**.
- **`cpu-prof`** (run 144054, muestreo de pilas; pilas sumadas en 3 intervalos → ÷3): hilo CP = `Protect ← EnableAccessCallbacks ← UploadRanges ← RequestRanges ← RequestTextures` **~3,6 ms/frame** (re-proteger páginas tras subir la memoria compartida), `CommitPreparedTextureLoad` (XXH3/memcmp) ~1,5, `RequestRanges` (memmove) ~1,2, copia CPU ~0,7, kickoff ~0,9. Script de pilas: scratchpad `callers2.py` (las pilas no traen la hoja; el primer frame es el llamador).
- **`cpu-2`** (run 144733, diag: saltear la subida de la memoria compartida para las texturas por CPU): **52,2 fps**, cp_tex 13,1 → 0,65, gpu_busy 17,6, n_tex 52 → 10 → **imagen ROTA** (todos los sprites iguales: sin subida la memoria compartida no protege las páginas y el watch de la textura nunca salta). Quedó como cvar diag `texture_load_cpu_skip_upload_diag` (default false). **Conclusión fuerte**: el cuello es el camino de invalidación/subida/protección de texturas del hilo CP; el juego va en lockstep (escribe un sprite, dibuja, espera el fence: ~33 submits/frame), así que ese costo se serializa con todo.
- **Próximo paso**: para las texturas `cpu_direct`, cambiar el watch por páginas protegidas por **sondeo**: no subir ni proteger; en cada pedido comparar la memoria del guest con la sombra de lo último cargado (memcmp) y recargar en una versión nueva solo si cambió. Medir el costo del memcmp por bind (~30-60 µs por 640x448). Después: destilar en CPU las RGBA8 tiled (`texture_util::GetTiledOffset2D`, tiras de 16 B).

## MSAA 1x global a 921,6 MHz: GPU 23,8 → 18,9 ms, sigue en 30; prueba de 1x solo para 2x (2026-11-06)

- **`msaa1-2`** (run 20261106-135540, `_nl_novb_msaa1.json`, GPU **921,6** confirmado en `[AT] clocks`): 30,6 fps, gpu_busy **18,94** (lin-2 23,8). Bajan draw 9,5 → 7,63, resolve 3,5 → 2,23, dump 2,4 → 1,76, rt_xfer 2,3 → 1,64; tex_load 4,83 igual. Imagen rota igual que en msaa1-1. **Conclusión**: ni con MSAA 1x global se baja de 16 ms; hace falta MSAA **y** tex_load (18,9 − 4,8 ≈ 14,1).
- **Nuevo cvar** `gpu_force_msaa_1x_from_2x` (`command_processor.cpp` `WriteRegister`): solo baja a 1x las escrituras de RB_SURFACE_INFO con 2x y deja las de 4x (truco de alias en EDRAM, la causa probable de la imagen rota). Config `_nl_novb_msaa2x.json` (con capturas). Build `_logs/msaa2x-build.log`.
- **`msaa2x-3`** (run 20261106-141310, GPU 921,6): **imagen bien** (captura `pelea` sin corrupción), pero 29,9 fps, gpu_busy **21,73** (tlx-lin 21,05, lin-2 23,8): draw 9,56, resolve 2,38, dump 1,89, rt_xfer 1,94, tex_load 4,92. Gana ≤1 ms → **casi todo lo que ahorraba msaa1-2 venía de las superficies 4x** (320x2048 en base 1440/1664, las del truco de alias), no del color 2x. (msaa2x-1/-2 fallaron por cortes de red, no cuentan.)
- **Próximo paso**: con tex_load por CPU (−4,9) se queda en ~16,8, justo arriba de 16. Opciones: (a) carga por CPU directo a la imagen LINEAR + MSAA 2x→1x; (b) bajar a 1x las 4x reinterpretando bien el alias 4x↔1x (es lo que más ahorra, ~3 ms, pero hay que entender el truco: Ghidra/draws de esas superficies).

## Carga LINEAR directa funciona (ya por defecto): tex_load 14,4 → 4,9 ms; WAR y MSAA 1x global descartados (2026-11-06)

- **Arreglo**: la condición de `texture_load_linear_direct` excluía MUTABLE_FORMAT, y R8/RGBA8 siempre lo tienen (UNORM+SNORM) → en `lin-1` no se activaba. Ahora exige linear SAMPLED+FILTER en los dos formatos. Logs `[lin]` por `printf` (`REXGPU_INFO` no sale por nxlink). Build `_logs/lin2-build.log`.
- **`lin-2`** (run 20261106-120431, `_nl_novb_lin.json`): `[lin] ... buffer ok` en todas (1280x720/640x448 vkfmt 37/38 pitch = ancho*4; 320x240 vkfmt 9/10 pitch 384). **30,0 fps** (tlx/lin-1 29,5; base 27,8), gpu_busy 31,2 → **23,8**, gp_tex_load 14,4 → **4,9**, gp_draw 8,0 → 9,5 (+1,5: el muestreo LINEAR cuesta, pero se gana neto 7,4 ms de GPU), cp_tex 11,1 → 12,0.
- **`lin-shot`** (run 20261106-120643, `_nl_novb_lin_shot.json` = lin + sys-botbase con capturas `pelea`/`pelea2`, 30,4 fps): escenario, personajes y HUD bien, **sin corrupción**.
- **Por qué sigue en ~30**: FRAME TOTAL 33,3 ms, fijo = 2 vblanks. GPU 23,8 + cp_wrm 8,75 (espera al vblank siguiente) ≈ 33. Mientras la GPU pase de ~16 ms, se pierde un vblank y se queda en 30. GPU por frame ahora: draw 9,5 + tex_load 4,9 + resolve 3,5 + dump 2,4 + rt_xfer 2,3 + upload ~0,7.
- **Seguimiento (mismo día)**: `texture_load_linear_direct` ahora **true por defecto**. `tlx-lin` (`_nl_novb_tlx.json`, 30,6 fps, gpu_busy 21,05): solo quedan ~30 copias/s (R8/R8G8 con mips 9-10, ≤512x512); n_batch 1,9. `nowar-1` (cvar diag `texture_load_linear_nowar_diag`, `_nl_novb_nowar.json`, run 121509): gp_tex_load 4,91, gpu_busy 23,45, 30,0 fps → **descartado: la barrera WAR no es el costo**; los ~30 dispatches/frame cuestan ~160 µs cada uno (probable cambio 3D↔compute del canal NVIDIA). Los runs alternan entre gp_draw ~8,0 y ~9,5 (¿rival/escenario?). Diag nuevo `[rtc]` (printf en `CreateRenderTarget`: tamaño, samples, formato) para ver si hay MSAA, build `_logs/rtc-build.log`.
- **`rtc-1`** (run del `[rtc]`, 30,0 fps, GPU 921,6): RTs creados: color 1280x1024 **2x MSAA** base 0; depth 640x1024 4x base 0; color+depth 320x2048 **4x** en base 1440/1664 y **la misma EDRAM** como 640x4096 1x (truco de 4x↔1x: de ahí gp_rt_xfer ~2,3 ms).
- **`msaa1-1`** (cvar experimental `gpu_force_msaa_1x` en `graphics/command_processor.cpp` `WriteRegister`: borra los bits 16-17 de RB_SURFACE_INFO; config `_nl_novb_msaa1.json`, run 135124): **rompe la imagen** (el escenario solo se ve en el ~25 % de arriba, el resto negro; personajes y HUD bien) → **descartado forzar 1x global**. Ojo: ese run tuvo **GPU a 460,8 MHz** (el resto 921,6): 27,5 fps, gpu_busy 33,5, no comparable. Revisar sys-clk/dock antes de medir de nuevo.
- **Próximo paso**: 1) confirmar GPU a 921,6 (línea `[AT] clocks`); 2) candidatos para bajar ~8 ms de GPU: (a) cargas de textura por CPU directo a la imagen LINEAR (memoria unificada; necesita renombrar imágenes por carga para no pisar draws pendientes) → saca ~4,9 ms de tex_load; (b) MSAA 1x solo para el RT de color 2x de base 0 (no el de 4x/base 1440), medir y mirar capturas; (c) evitar el dump RT→EDRAM en resolves que salen enteros de un solo RT.
- **Próximo paso (anterior)**: 1) dejar `texture_load_linear_direct` en true por defecto (o en los configs `_nl_novb_*`) y tomar `lin-2` como base nueva; 2) para bajar de 16 ms de GPU, atacar lo que queda: gp_draw (9,5), gp_tex_load (4,9: ¿qué sigue copiando? diag `[tlx]` con lin activo), resolve+dump+rt_xfer (8,2 juntos); 3) opcional: a 30 fps fijos, ver si conviene `vblank` cada 2 (presentación estable) mientras tanto.

## Carga en el lugar: imágenes LINEAR con buffer alias, implementado y compilado, falta medir (2026-11-06)

- **Qué se hizo**: cvar `texture_load_linear_direct` (default off) en `graphics/vulkan/texture_cache.cpp` (+ `texture_cache.h`, + `vkGetImageSubresourceLayout` en `ui/vulkan/functions/device_1_0.inc`). Backups `*.bak_tlx`. Build `_logs/lin-build.log` OK (NRO listo).
  - `CreateTexture`: 1D/2D, 1 capa, `mip_max_level==0`, sin scaled_resolve, sin formato mutable, bloque 1x1, no RGBA16F, y `linearTilingFeatures` con SAMPLED+FILTER_LINEAR → `VK_IMAGE_TILING_LINEAR` + `vmaCreateAliasingBuffer` (STORAGE) sobre la misma allocation; exige rowPitch/offset múltiplos de 16; si falla, imagen OPTIMAL o camino de copia. Log `linear direct WxH vkfmt: rowPitch offset size buffer ok/FAILED` (8 primeras).
  - `LoadTextureDataFromResidentMemoryImpl`: si tiene buffer, `level_packed != 0` y 1 mip → `linear_load`: el destino del shader es el buffer alias (range WHOLE), `host_offset` = offset del subrecurso + fila parcial*rowPitch, `host_pitch` = rowPitch; sin copia. Barreras: UNDEFINED→SHADER_READ_ONLY antes de escribir (1ª vez); si venía de TransferDst, flush + TRANSFER→COMPUTE; si no, 1 barrera de ejecución WAR (etapas de shader→compute) por lote (`load_batch_linear_war_done_`). La barrera compute write→sampler read va en `FlushTextureLoadBatch` (`load_batch_linear_`). La imagen queda siempre en SHADER_READ_ONLY_OPTIMAL, usage kGuestShaderSampled.
  - Revisión de Gemini 3.8 Flash: sin bugs reales (vkDestroyBuffer es lo correcto para el alias de VMA; el WAR solo necesita dependencia de ejecución). Riesgo teórico: visibilidad buffer→imagen con barrera de imagen (en NVK da igual).
- **Config**: `devkitnx/configs/_nl_novb_lin.json` (= `_nl_novb_wrm.json` + `--texture_load_linear_direct=true`).
- **`lin-1` (run 20261106-120030)**: 29,5 fps, gp_tex_load 14,37 (= tlx-1: el camino **no se activó**). Causa: R8/RGBA8 tienen UNORM+SNORM → `formats[1]` definido (MUTABLE_FORMAT) y la condición exigía `formats[1]==UNDEFINED`. Corregido: ahora se exige linear SAMPLED+FILTER en ambos formatos. `REXGPU_INFO` no llega al stdout de nxlink → los logs pasaron a `printf("[lin] ...")` (también loguea si falla crear la imagen LINEAR). Build `_logs/lin2-build.log`.
- **Próximo paso**: 1) abrir netloader (sys-botbase HOME, A, A; un clic puede dar timeout, igual anduvo) y correr desde `devkitnx`: `python runner/nxtest.py run configs/_nl_novb_lin.json --no-build *> "D:/Ports Swtch/_logs/lin-1.log"` en segundo plano; 2) Grep `linear direct` (¿buffer ok?, rowPitch), `fps prom` (base novb 27,8; nocopy 30,6; tlx-1 29,5), `gp_tex_load`/`gp_draw` en la cola; mirar la captura `pelea` del run por texturas rotas; 3) si hay corrupción: probar sin el WAR por lote (barrera por textura) o revisar el pitch; si gp_draw sube mucho, el muestreo LINEAR cuesta más de lo que ahorra.

## Carga de texturas sin copia: diagnóstico `[tlx]` para elegir el camino (2026-11-06)

- **Resultado `tlx-1`** (29,5 fps prom, `_logs/tlx-1.log`): en pelea casi todo es `dim=1 levels=1 linear=1`: vkfmt 9 (R8_UNORM) ~600/s 173 Mtexel/s y vkfmt 37 (R8G8B8A8_UNORM) ~230/s 65 Mtexel/s, hasta 640x512; más 1D 256x1 (vkfmt 37, dim=0) ~500/s. Mips (levels=9, R8) ~30/s, poco. → se implementa el camino LINEAR (cvar `texture_load_linear_direct`).

- **Qué se hizo**: medido que sacar las copias buffer→imagen da **30,6 fps con vblank real** (`novb-nocopy-1`, contra 27,8 de la base); con `--vblank_per_swap=true` no se veía porque el CP espera el vblank (WAIT_REG_MEM `0x1FC9B004 == 0`, 1 por frame) y en ese modo el vblank llega recién tras el VdSwap del CPU. Detalle en la sección de abajo ("ÚLTIMO").
- **Medir siempre con vblank real**: configs `devkitnx/configs/_nl_novb_*.json`. Descartado: spin del WAIT_REG_MEM (cvar `wait_reg_mem_spin_us` queda, default 0).
- **Implementación de la carga directa**: en el repo **no están las fuentes de los shaders de carga** (solo SPIR-V compilado en `src/graphics/shaders/bytecode/vulkan_spirv/texture_load_*_cs.h`) ni hay glslang/dxc → hacer shaders nuevos con `imageStore` requiere bajar fuentes de xenia + compilador (pedir permiso). **Camino sin shaders nuevos (hipótesis vigente)**: texturas 2D de 1 mip y formato no comprimido como imagen `VK_IMAGE_TILING_LINEAR` cuya memoria se alías con un buffer; el shader de carga escribe ahí (host pitch = `rowPitch` de `vkGetImageSubresourceLayout`, layout GENERAL) y no hay `CmdCopyBufferToImage`. Riesgos: muestreo linear más lento (mirar gp_draw), alineación de pitch, BC no sirve (ya va por copy engine).
- **Diagnóstico nuevo (en build)**: cvar `texture_load_class_log` en `graphics/vulkan/texture_cache.cpp` (junto a `load_batch_copies_.push_back`): cada 10 s líneas `[tlx] vkfmt dim levels linear | /s, Mtexel/s, max WxH` de las copias en lote; `linear` = `linearTilingFeatures` tiene SAMPLED. Build `_logs/tlx-build.log` (lanzado; el primer intento falló por usar `vkGetPhysicalDeviceImageFormatProperties`, ya corregido). Config `devkitnx/configs/_nl_novb_tlx.json`.
- **Próximo paso**: 1) `tlx-build.log` ya dio exit 0 (NRO listo); 2) abrir netloader (sys-botbase HOME, A, A) y correr `python runner/nxtest.py run configs/_nl_novb_tlx.json --no-build *> "D:/Ports Swtch/_logs/tlx-1.log"` desde `devkitnx`, en segundo plano; 3) Grep `^\[tlx\]` y `fps prom`: si la mayoría de las copias/s son dim=1 (2D), levels=1, linear=1 → implementar el camino LINEAR para esas (cvar nuevo, default off, medir con `_nl_novb_wrm.json`); si no, decidir con el usuario bajar fuentes de xenia + glslang para shaders con `imageStore`.

## Las copias son ~9 ms de GPU, pero el límite es la latencia del WAIT_REG_MEM (Sleep ≥1 ms en el CP); A/B spin (2026-11-06)

**ÚLTIMO (2026-11-06, chat "spin/wrm")**: `novb-nocopy-1` (run `20261106-113749`, nocopy + vblank real) = **30,6 fps** (FRAME 32,64, p99 49,7; gpu_busy 21,38, gpu_span 30,45, cp_wrm 9,38) vs 27,7 de nocopy con vb y 27,8 de la base con vblank real. → **Sacar las copias SÍ da fps, pero solo se ve con vblank real**: con `--vblank_per_swap=true` el vblank llega recién tras el VdSwap del CPU y el CP (que espera ese vblank una vez por frame, ver `wrm-1` abajo) queda serializado con el juego. Ahora el frame ≈ 2 vblanks (30 fps, vsync). Spin descartado. **Próximo paso**: (1) implementar la carga de texturas directo a la imagen (sin `CmdCopyBufferToImage` por meta-compute; ~9–13 ms de GPU) y medirla con `configs/_nl_novb_wrm.json` (vblank real); (2) para el bench determinista, arreglar `WaitSwapForVblank` (`kernel/xboxkrnl/xboxkrnl_video.cpp` ~479) para que no serialice: p. ej. dar el vblank también por reloj cada 16,67 ms cuando el CP está bloqueado en el WAIT_REG_MEM de `0x1FC9B004`, o comparar fps siempre con vblank real. Para 60 fps falta bajar GPU (21 ms sin copias: gp_draw ~7, gp_tex_load ~5,4, resolve+dump+rt_xfer ~8) y el CP (cp_tex 11,3).

Resumen: nowait descartó la barrera; nocopy mostró que las copias cuestan ~9 ms de GPU pero sacarlas no da fps; el profiler (ahora sale por stdout con `[prof]`) muestra que el CP duerme ~11 ms/frame en WAIT_REG_MEM mientras el juego hace spin esperando el fence. **Run `spin-1` YA LANZADO** (build `_logs/spin-build.log` exit 0, netloader abierto con sys-botbase; log `_logs/spin-1.log`; terminó si dice "resultados en"). Detalle (números, rutas, cómo agrupar las muestras) en la sección de abajo; leer también su "Próximo paso".

**`spin-1` TERMINÓ** (run `20261106-111454`, OC confirmado): **27,2 fps, sin cambio**; gpu_busy 34,95 ≈ gpu_span 35,87, cp_idle 2,00, cp_tex 11,53. Con las copias activas la GPU vuelve a ser el límite y el spin no aporta. **Siguiente prueba real: spin + nocopy juntos** (config nueva = `_nl_vb_spin.json` + `--texture_load_split_timing=true --texture_load_nocopy_diag=true --texture_partial_reload=false`). Si así suben los fps, el plan es: implementar la carga directo a la imagen y además el spin. Si tampoco suben, loguear qué memoria espera el WAIT_REG_MEM. **`spin-nocopy-1` TERMINÓ** (run `20261106-112617`, config `configs/_nl_vb_spin_nocopy.json`, OC confirmado): **27,7 fps = igual que nocopy solo**; gpu_busy 21,24, gpu_span 33,51, cp_idle 2,50, cp_tex 11,37; Main XThread 98 %, GPU Commands ~60 %. → **El spin no aporta en ningún caso: descartado.** (el cvar `wait_reg_mem_spin_us` queda, default 0).

**Diagnóstico nuevo `wrm`**: en `graphics/command_processor.cpp` el WAIT_REG_MEM ahora suma zonas nuevas del autotest `cp_wrm` (tiempo en waits que no matchearon al primer poll), `cp_wrm_slp` (la parte en Sleep), `n_wrm` (cantidad; zonas 7–9 en `core/autotest.cpp`, kCpuZones=10). Con `--wait_reg_mem_log=true` imprime cada 10 s líneas `[wrm]` por tupla (reg/mem, dirección, func, ref, mask, wait, por segundo, ms/s, sleep, prom, max, primer valor leído). Config `devkitnx/configs/_nl_vb_wrm.json` (base + log, sin spin ni nocopy). Build `_logs/wrm-build.log`, run `wrm-1` (log `_logs/wrm-1.log`). Con la dirección: si es memoria, buscar en Ghidra/codegen quién la escribe (el juego o un EVENT_WRITE del CP); si es registro, ver qué lo actualiza.

**`wrm-1` TERMINÓ** (run `20261106-113142`, 27,2 fps): **una sola espera por frame, siempre la misma**: `mem 0x1FC9B006` (física 0x1FC9B004, endian 2), func=3 (`== 0`), mask FFFFFFFF, wait=0x100 (1 ms), primer valor 1; **cp_wrm 12,07 ms/frame, todo en Sleep; máx 20,1 ms = `vblank_per_swap_timeout_ms=20`**. → El CP espera un flag que limpia el vblank (callback de interrupción del juego). Con `--vblank_per_swap=true` (modo bench), `WaitSwapForVblank` solo da vblank tras un VdSwap nuevo del CPU (o el timeout): **CPU y GPU quedan serializados por el modo bench**, no por el renderer. Por eso ni spin ni nocopy movían los fps. **Run `novb-wrm-1` LANZADO**: config `configs/_nl_novb_wrm.json` (= wrm con `--vblank_per_swap=false`, vblank real a 60 Hz; puede no ser determinista), log `_logs/novb-wrm-1.log`. Si sube mucho: el bench mide mal; arreglar `WaitSwapForVblank` (p. ej. vblank por reloj cada 16,67 ms pero contado de forma determinista, o vblank también cuando el CP está esperando ese flag) y re-medir nocopy.

**`novb-wrm-1` TERMINÓ** (run `20261106-113500`, vblank real): **27,8 fps (≈ igual)**, gpu_busy 34,87 ≈ gpu_span 35,27, cp_wrm 8,21 (máx 17,2 ≈ un vblank), cp_fence 4,29. Misma única espera. → **Con copias, el modo vb no es la causa: manda la GPU.** Los frames no quedan cuantizados a vblank (peor 42 ms). **Run `novb-nocopy-1` LANZADO** (config `configs/_nl_novb_nocopy.json` = nocopy + vblank real + log wrm; log `_logs/novb-nocopy-1.log`): si con vblank real nocopy sí sube los fps, entonces nocopy-1 estaba frenado por la serialización del modo vb y vale implementar la carga directa a la imagen (y medir el bench sin vb o arreglar `WaitSwapForVblank`). Si sigue en ~27,7: el límite es la cadena juego (spin en fence `sub_82655C18`) ↔ CP, y hay que ver qué fence espera el juego y cuándo lo escribe el CP. Lo que sigue (pasos viejos, por si hacen falta):

**Próximo paso (viejo)**: Grep en `spin-1.log` (`clocks match_start`, `fps prom`, `^gpu_busy`, `^gpu_span`, `^cp_idle`, `^cp_tex`). Referencia sin spin: 27,2–27,8 fps, gpu_busy 34,8, gpu_span ~36. Si suben los fps: barrer `--wait_reg_mem_spin_us` en 1000/2000/4000, dejar el mejor por default en `graphics/command_processor.cpp` y repetir con `--texture_load_nocopy_diag=true` para ver si las copias pasan a ser el límite (entonces hay que implementar que la carga escriba directo a la imagen). Si no suben: loguear `poll_reg_addr`, `ref` y `wait` del WAIT_REG_MEM y buscar quién escribe esa memoria. Si nxlink falla, abrir el netloader con sys-botbase (HOME, A, A) y relanzar desde `devkitnx` con `python runner/nxtest.py run configs/_nl_vb_spin.json --no-build *> "D:/Ports Swtch/_logs/spin-1.log"`.

## nowait: la espera de la barrera NO es el costo (gp_other 10,19 → 8,91); diagnóstico nocopy (2026-11-06)

- **`nowait-1`** (run `20261106-105500`, OC confirmado 1785/921,6/1600): **27,8 fps** (vs 27,2), frame 36,03, gpu_busy 31,39 (vs 34,6), gp_other 8,91 (vs 10,19), gp_draw 7,94 (vs 9,40), gp_tex_load 5,33 (vs 5,64), n_batch 31,8, n_tex 49. Sacar la espera ahorra ~3,2 ms de GPU pero solo 0,7 ms de frame → **descartada la hipótesis "gp_other = esperar a los draws"**; con 8,6 MB/frame el ancho de banda tampoco explica 8,9 ms (~280 µs por lote).
- **Por qué (código NVK, `~/mesa-switch/src/nouveau/vulkan/nvk_cmd_meta.c` ~470)**: `CmdCopyBufferToImage2` usa una copia **meta por compute** (`vk_meta_copy_buffer_to_image`, guarda/restaura estado) para formatos no comprimidos y el copy engine (90B5) para los comprimidos; `NVK_COPY_ENGINE=1` fuerza el CE (medido antes: peor). Hipótesis nueva: el costo es fijo por copia/lote (cambio de clase 3D↔compute en Maxwell = WFI implícito, más el save/restore de meta), no la barrera.
- **Diagnóstico nuevo**: cvar `texture_load_nocopy_diag` (INCORRECTO a propósito: no graba las `CmdCopyBufferToImage` del lote; barreras igual). Config `devkitnx/configs/_nl_vb_nocopy.json` = split + nocopy + `--texture_partial_reload=false`. Build `_logs/nocopy-build.log`. Run: `nocopy-1`.

- **`nocopy-1`** (run `20261106-110001`, OC confirmado): **27,7 fps** (igual), frame 36,10, **gpu_busy 21,27** (−13 ms), gp_other 0,18, gp_draw 7,20, gp_tex_load 5,47, **gpu_span 33,52**, cp_idle 2,51, cp_submit 1,60, **cp_tex 11,33**. → **Las copias buffer→imagen son ~9 ms de GPU** (vale la pena sacarlas), **pero con la GPU libre los fps no cambian: el límite ahora es el thread del command processor** (ocioso solo 2,5 de 36 ms; la GPU espera ~12 ms por frame dentro del span). CP: cp_tex 11,3 (≈230 µs de CPU por carga, 49 cargas) + submit 1,6 + **~20 ms sin atribuir**.
- **Profiler a stdout**: `switch_perf.cpp` `Report()` ahora, si `rex_autotest_owns_stdout()` (weak), copia el reporte de cada 10 s de `rex_profile.log` a stdout con prefijo `[prof] ` → el log del runner trae CPU por thread y pc/lr/pilas muestreadas a 1 kHz. Build `_logs/profstdout-build.log`.

- **`prof-1`** (run `20261106-110509`, `_nl_vb_prof.json`, 27,2 fps, gpu_busy 34,78): reporte `[prof]` en el log del runner OK. **CPU por thread: Main XThread 98,3 % (un core clavado), GPU Commands 53,2 %**, VSync 4,6, resto <5. → el thread del CP NO es el límite; **el límite de CPU es el thread principal del juego**. Ojo: `svcSleepThread yield 154328/s` y "up to 1 ms 1311 (1313 ms)": puede ser espera activa (spin con yield) y no trabajo real. Por frame: 73 draws, 33 submits, 50,6 cargas en 32 lotes, 4,93 MB subidos. Kickoff 926/s (29,6 ms/s), fences consultados 1852/s (39 ms/s).
- **Pilas**: `samples 0` porque el título no tenía el cvar `switch_profile_stacks`. Definido en `kernel/xboxkrnl/xboxkrnl_video.cpp` (junto a `vblank_per_swap`). Build `_logs/profstacks-build.log`. Run `prof-2` con el mismo config.

- **`prof-2`** (pilas activas, 27,8 fps): Main XThread = el **spin del D3D esperando el fence de la GPU** (`sub_82655C18 → sub_82655B48 → sub_826571A8`, 67 % inclusivo; ya visto el 2026-11-05, ver más abajo). **GPU Commands: 51 % en el kernel, y el 32 % de todas sus muestras dormido en `ExecutePacketType3_WAIT_REG_MEM → rex::thread::Sleep(wait/0x100 ms)`** (`graphics/command_processor.cpp` ~1050). Unos ~11 ms por frame: coincide con el hueco de GPU dentro de gpu_span (33,5 − 21,3). `PrepareForWait` no hace nada en Vulkan. **Hipótesis: el límite es la latencia de los Sleep de ≥1 ms del WAIT_REG_MEM** (svcSleepThread "up to 1 ms" 1311/s ≈ 47 por frame).
- Agrupar muestras: sacar el prefijo `[prof] ` a un txt y correr `python -I kof2002um-nx/tools/sa2-nx-reference/tools/switch_profile_report.py <txt> --elf kof2002um-nx/kof2002um_nx --mhz 1785 --thread "Main XThread"` (con `devkitA64/bin` en el PATH).
- **Cambio**: cvar `wait_reg_mem_spin_us` (default 0 = como antes): hace polling con `MaybeYield` hasta N µs antes de los Sleep. `--vsync=false` haría yield siempre, pero también cambia el intervalo de vblank (`graphics_system.cpp:173`): no usarlo. Config `_nl_vb_spin.json` = `_nl_vb` + `--wait_reg_mem_spin_us=4000`. Build `_logs/spin-build.log`. Run `spin-1`.

**Próximo paso**: leer `spin-1.log` (`fps prom`, `gpu_busy`, `gpu_span`, `cp_idle`). Si los fps suben: dejar el spin por default (probar 1000/2000/4000) y volver a medir con nocopy para ver si las copias pasan a ser el límite. Si no: mirar qué memoria espera el WAIT_REG_MEM (loguear `poll_reg_addr`, `ref`, `wait` y quién lo escribe). Más adelante, implementar que el shader de carga escriba directo a la imagen (storage image; BCn con vista sin comprimir `BLOCK_TEXEL_VIEW_COMPATIBLE`) para sacar los ~9 ms de copias: solo sirve cuando el CP baje de ~25 ms.

## Relojes resueltos, referencia nueva con OC; diagnóstico nowait (¿cuánto de las copias es esperar a los draws?) (2026-11-06)

- **Relojes**: ahora cada `[AT] mark` va seguida de `[AT] clocks <mark> cpu= gpu= emc=` (clkrst, `include/rex/autotest.h` `at_mark`). Los perfiles de sys-clk son por title: con el NSP del netloader no se aplicaban; el usuario puso un **perfil global** (1785/921,6/1600). Stock de handheld (1020/307,2/1331,2) = 11,2 fps. **Mirar siempre la línea `clocks match_start` antes de comparar runs.** Las referencias viejas de 17,4 fps eran con relojes intermedios desconocidos: descartadas.
- **Referencia `ref-oc-1`** (run `20261106-105142`, `_nl_vb.json`, OC): **27,2 fps, frame 36,7, gpu_busy 34,6 (limitado por la GPU), gp_tex_load 15,75, gp_draw 9,40, gp_resolve 3,41, cp_fence 0,41**. Con el split (`texsplit-oc-2`, run `20261106-104943`): gp_other (copias + barreras del lote) **10,19** + gp_tex_load (compute) **5,64**; el split no cambia los fps. La carga de texturas = 46 % del tiempo de GPU.
- **Hipótesis**: casi todo gp_other es la espera de la barrera de imagen de `FlushTextureLoadBatch` (~1940): el src stage es el uso viejo (fragment, sampled) → cada lote (~32 por frame) vacía el pipeline de fragment.
- **Diagnóstico nuevo**: cvar `texture_load_nowait_diag` (default false, INCORRECTO a propósito: src stage TOP_OF_PIPE, layout viejo UNDEFINED; backup `texture_cache.cpp.bak_nowait`; build `_logs/nowait-build.log`, exit 0). Config `devkitnx/configs/_nl_vb_nowait.json` = split + nowait + `--texture_partial_reload=false`. **Run `nowait-1` lanzado** desde el chat anterior (netloader abierto con sys-botbase; log `_logs/nowait-1.log`; terminó si dice "resultados en").
- Netloader: el NSP está primero en HOME; abrirlo con `python runner/nxtest.py bot 192.168.0.137 click HOME`, esperar 3 s, `click A`, 2 s, `click A`, 8 s, y lanzar el run. Sin preguntar al usuario. Si nxlink dice "Connection ... failed", repetir los clicks.

**Próximo paso**: leer `nowait-1.log` (`clocks match_start`, `fps prom`, `^gp_other`, `^gp_tex_load`, `^gp_draw`). Si gp_other baja mucho (de 10,2 a ~1-2 ms): la espera es el costo → implementar bien: versiones de imagen (anillo de 2 imágenes por textura que se recarga, escribir en la que no se leyó en este frame, con fence por frame) o, si la textura se reescribe entera, juntar todas las cargas del frame antes del primer draw. Si gp_other no baja: el costo son las copias en sí → que el compute escriba directo a la imagen (storage image) y sacar el paso buffer → imagen.

## Diagnóstico texsplit: separar el compute del load y el lote de copias dentro de gp_tex_load (2026-11-06)

- Cómo se mide: `MarkPass` (deferred_command_buffer.cpp ~318) pone un timestamp BOTTOM_OF_PIPE al cambiar de categoría, y las categorías iguales seguidas se juntan. gp_tex_load cubre el compute del load (MarkPass en texture_cache.cpp ~1591, con `SubmitBarriers` + dispatch) y `FlushTextureLoadBatch` (~1940: barrera del ring, transiciones a TRANSFER_DST, copias). Los lotes ya existen (cvar `texture_load_batch`): 25,7 lotes por frame para 37,9 cargas → ~0,86 ms por lote.
- Nuevo cvar `texture_load_split_timing` (default false; backup `texture_cache.cpp.bak_split`; build `_logs/texsplit-build.log`, exit 0). Con el cvar prendido, el lote de copias y sus barreras se marcan como kOther (**gp_other** = copias, **gp_tex_load** = solo el compute). Config `devkitnx/configs/_nl_vb_texsplit.json` = `_nl_vb` + `--texture_load_split_timing=true`.
- Run `texsplit-1` (run `20261106-101210`; el primer intento `101038` falló en nxlink): **gp_other (copias + barreras) 10,26 ms, gp_tex_load (solo compute) 5,74 ms** → las copias/transiciones son ~64 % del costo de carga. Resto: gp_draw 9,48, gpu_busy 35,0, cp_fence 0,49, n_tex 51, n_batch 32,4. **27,2 fps** (602 frames).
- **OJO, el run no se puede comparar con la referencia** (17,4 fps, gp_tex_load 22,2, gp_draw 13,7, gpu_busy 51): en los frames de arranque, sin texturas, todas las categorías GPU van exactamente 2x más rápido (frame 3: gp_resolve 1314 → 660 µs, gp_rt_xfer 1058 → 540, gpu_busy 7,9 → 4,6 ms). Mismo NRO (65190085 bytes) y "handheld mode" en los dos → **la consola tenía el cargador conectado; el usuario solo tenía configurado el perfil handheld de sys-clk, así que con el cargador corrió con los relojes default**. Paradoja: el default fue más rápido. Solo bajaron a la mitad las categorías que dependen de la memoria (resolve, rt_xfer, dump, tex_load); gp_draw quedó igual → hipótesis: **el perfil handheld tiene la MEM (EMC) más baja que el default** y el bench está limitado por el ancho de banda. El usuario dice que el perfil handheld estaba al máximo (CPU 1785, GPU 921, MEM 1600), así que la MEM no lo explica. Configuró igual los perfiles de carga y corrió **`texsplit-2` (run `20261106-103345`, cargador + OC máximo): 10,8 fps**, gpu_busy 86,8, gp_draw 26,5, gp_other 19,7, gp_tex_load 14,7, cp_fence 53. Frame 3 de arranque: gp_resolve 2827 µs (texsplit-1 1027, refs viejas 1884). → **El OC de sys-clk hace MÁS LENTO el juego**: default 27,2 fps > OC handheld 17,4 > OC cargando 10,8. switch_apm.cpp **descartado**: en kof nadie llama a `RexSwitchApmRequestGpuMhz`/`Apply`, así que el watchdog no hace nada (g_emc_original = 0). Queda el propio sys-clk con OC máximo (¿throttling por potencia/temperatura, o relojes que el PCV no sostiene?). Bench siempre con sys-clk apagado o en default de acá en más.
- **`ref-default-1`** (run `20261106-103843`, `_nl_vb.json` sin split, el usuario dice que apagó sys-clk; el netloader lo abrí con sys-botbase HOME+A y anduvo): **10,9 fps**, gp_tex_load 34,3, gp_draw 26,5, gpu_busy 86,8 → igual que texsplit-2. Entonces el run raro es texsplit-1 (27,2); los relojes reales no se conocen. **Agregado en `include/rex/autotest.h` `at_mark`: línea `[AT] clocks <mark> cpu= gpu= emc=` (clkrst, en MHz) con cada marca** (build `_logs/clocks-build.log`).
- **`texsplit-3`** (run `20261106-104220`, build con el log de relojes, sys-clk apagado): **`[AT] clocks` = cpu 1020, gpu 307,2, emc 1331,2 (stock de handheld) en las 4 marcas → 11,2 fps**, gp_other 17,9, gp_tex_load 14,5, gp_draw 25,3. El log de relojes anda. **`texsplit-oc-1`** (run `20261106-104608`, sys-clk prendido según el usuario): **mismos relojes de stock (1020/307,2/1331,2), 11,2 fps** → sys-clk no aplica el perfil. Hipótesis: los perfiles de sys-clk van por title ID y el juego ahora corre bajo el NSP del netloader (otro title), que no tiene perfil; antes se lanzaba desde hbmenu. Arreglo: perfil global en sys-clk o perfil para el title del NSP. **Confirmado**: con perfil global, **`texsplit-oc-2`** (run `20261106-104943`): clocks cpu 1785 / gpu 921,6 / emc 1600 (en boot todavía 1020/307,2/1600) → **27,2 fps**, gp_other 10,19, gp_tex_load 5,64, gp_draw 9,43, gpu_busy 34,7, cp_fence 0,54 → idéntico a texsplit-1, así que **texsplit-1 era con OC máximo y las referencias viejas de 17,4 fps eran con relojes intermedios desconocidos: no usarlas más**. A OC máximo el bench está limitado por la GPU, y la carga de texturas es 46 % del tiempo de GPU (15,8 de 34,7 ms). **Referencia nueva `ref-oc-1`** (run `20261106-105142`, `_nl_vb.json`, OC 1785/921,6/1600): **27,2 fps, frame 36,7, gpu_busy 34,6, gp_tex_load 15,75 (= 10,19 copias + 5,64 compute del split), gp_draw 9,40, gp_resolve 3,41, cp_fence 0,41**. El split no cambia nada (27,2 en los dos), para ver qué relojes aplica de verdad (¿17,4 = OC handheld, 27,2 = ?).
- Con relojes iguales (texsplit-2 contra ref-default-1): el split no cambia el total (34,3 contra 19,7 + 14,7 = 34,4) → **copias + barreras 57 %, compute 43 %**.
- Netloader: el usuario dejó un NSP primero en HOME; tras cada run abrirlo con `nxtest.py bot 192.168.0.137 click HOME` y `click A`, sin preguntar. El diff de `texture_cache.cpp` contra `.bak_split` solo cambia el MarkPass (diagnóstico). Sí sirve la proporción de adentro del run. **Regla: los runs del bench se comparan solo con el mismo estado del cargador; anotarlo en cada run.**

**Próximo paso**: (1) correr una referencia nueva `_nl_vb.json` (sin split) con el cargador conectado, para tener una línea base de "cargador" comparable con texsplit-1. (2) Como domina gp_other: probar versiones de imagen (no esperar a los draws que leen la imagen vieja) o juntar las transiciones a TRANSFER_DST en una por frame en `FlushTextureLoadBatch` (~1940). Si falla nxlink ("Failed sending chunk size"): reabrir el netloader y reintentar.

## A/B carga parcial con la escena fija: sin diferencia (17,4 vs 17,5 fps); gp_tex_load es costo fijo por carga (2026-11-06)

- `094639` (`_nl_vb`, parcial activada): 17,4 fps, FRAME 57,29 ms, gp_tex_load 22,22, cp_tex 7,82, gp_upload 0,90, gpu_busy 51,16. `094853` (`_nl_vb_nopartial`, flag confirmado en log.txt): 17,5 fps, FRAME 57,25, gp_tex_load 22,12, cp_tex 8,02, gp_upload 0,89, gpu_busy 50,86. Las dos en escena B (n_tex 37,87). Ruido entre runs iguales: ~0,1 fps (17,4-17,5 en 4 runs randlog).
- Conclusión: la carga parcial por filas no cambia nada medible; el "+5 %" de antes era cambio de escena. gp_tex_load ≈ n_tex × 0,59 ms (37,9 × 0,59 = 22,3 ms) → domina el costo fijo por carga, no los bytes (gp_upload < 1 ms).

**Próximo paso**: bajar el costo fijo por carga de textura (~0,6 ms cada una: barreras/flush/transiciones por carga) — p. ej. juntar las cargas del frame en un solo lote con una barrera, o evitar la espera por carga. Medir con `_nl_vb.json` (escena fija).

## Escena del bench fija con `--at-seed=1`: 5 runs seguidos con la misma secuencia de rand() y la misma escena (2026-11-06)

- **randlog** (`_logs/randlog-1..4.log`; runs `092855`, `093113`, `093936`, `094247`): la secuencia de rand() es idéntica en todos (hilo 0; solo cambia el frame en ±4). Todas dan la escena B: n_tex ~37,9 y 17,4-17,5 fps. `092340` también es B. Ganchos: lr 0x825F89E8 = `sub_825F89D8`, wrapper `g_rand8 (0x84ADB7E8) = rand() & 0xFF` llamado desde ~100 lugares (el 68k traducido, registros en globales 0x84ADB7xx). Ahora randlog registra el que llama al wrapper (lee el lr guardado en r1+88; backup `bench_seed.cpp.bak_randlog1`, build `_logs/randlog2-build.log`).
- **Dónde se elige la escena**: justo después de la carga del frame ~575-578, `sub_8261E450` (lr 8261E4B0) saca un g_rand8. Después `sub_8261E500` (lr 8261E510) repite `rand & 0xF` hasta un valor válido (muestreo con rechazo, 325 llamadas en total, mismos reintentos en cada run). Antes hay 3 ráfagas chicas desde 821D0830/0938/0AA8 (f~410-440) y 1 llamada desde 82636BEC (f~617).
- **Sin explicar**: `092202` (con semilla, `srand(9808) -> srand(1)`) dio la escena A (n_tex 25,8, la misma que el run sin semilla `091946`). Fue el primer run con hook después de flashear y no tiene randlog. Si vuelve a salir A, mirar qué consume rand() antes de f578 (o el save en la SD).
- Clasificar runs por escena: el n_tex 3 frames después de la carga de f~575 es 35 en B y 38 en A, y la media de n_tex en los últimos 400 frames es 37,9 en B y 25,8 en A.

**Próximo paso**: A/B de carga parcial (`texture_partial_reload` true/false) con la escena ya fija (configs `_nl_vb*.json`, netloader a mano). Después, atacar el costo fijo por carga (~0,6 ms, barreras).

## Fijar escenario y rival del bench: hook de srand anda, pero no alcanza; diagnóstico de rand() compilado, sin probar (2026-11-06)

- **Origen del azar**: rand() = `sub_82725520` (CRT MSVC, per-thread, `_holdrand` en ptd+20), srand = `sub_827254F0`. Al arrancar, `bl` en 0x82160C90 hace `srand(GetTickCount())` (sub_826AEA30 lee KeTimeStampBundle). time() = sub_82725B78 (solo lo usan un timer de 300 s y otro init, no el RNG). sub_82738080 = __security_init_cookie. Otros srand usan semillas fijas (987, índices).
- **Hook nuevo** `src/recompiler/src/bench_seed.cpp` (en `CMakeLists.txt`, backup `.bak_seed`): las sub_ generadas son alias weak, así que una definición fuerte las pisa (verificado con nm). `--at-seed=N`: si lr == 0x82160C94 cambia la semilla (log `[AT] log seed: srand(9808) -> srand(1)`). `--at-randlog=N`: hasta el frame N, una línea por frame `rnd f=<frame> <hilo>:<lr&0xFFFF>[xK] ...` (hilo = índice por r13).
- **Resultado con semilla fija**: `20261106-092202` 18,5 fps, n_tex 25,2 / `20261106-092340` 17,4 fps, n_tex 37,9 → **siguen siendo escenas distintas**. (Sin hook, `091946`: 18,4 fps, n_tex 25,21.) Hipótesis: algún rand() se llama por frame dibujado (los frames varían: 811/1235 vs 824/1225 con vblanks fijos) y desfasa la secuencia; o la elección usa otro RNG.
- Configs: `--at-seed=1` agregado a `kof2002um_bench_vb*.json` y `_nl_vb*.json`; `_nl_vb_randlog.json` = `_nl_vb` + `--at-randlog=900`. Los `_nl_*` tienen sys-botbase apagado (no respondía en 192.168.0.137:6000): el usuario abre el netloader a mano antes de cada run; nxlink a veces corta la subida ("Failed sending chunk size") → reabrir y reintentar.
- **NRO actual** (`_logs/randlog-build.log`, exit 0) tiene seed + randlog.

**Próximo paso**: 1) Correr dos veces `python runner/nxtest.py run configs/_nl_vb_randlog.json --no-build *> _logs/randlog-N.log` (desde `devkitnx`, en segundo plano, con el netloader abierto) y comparar la secuencia de `rnd` (concatenada por hilo, sin los límites de frame) para encontrar la primera divergencia y su lr. 2) Si diverge un caller llamado por frame dibujado: bajo `--at-seed`, darle a ese caller un estado de RNG propio (así la secuencia del juego no se desfasa). Si no hay divergencia, el azar viene de otro lado (buscar otro LCG/tabla del juego). 3) Recién con escena fija: el A/B de carga parcial y atacar el costo fijo por carga (~0,6 ms, barreras).

## Reloj de vblanks del autotest anda: las marcas caen en la misma escena (±3 vb); A/B carga parcial 16,6 vs 16,3 fps, dentro del ruido (2026-11-06)

- **convert** (`runs/kof2002um-bench-convert/20261106-090702`, PASÓ, 16,4 fps): generó `sdmc:/switch/kof2002um-nx/bench_input_vb.txt` (241 pasos, 1 marca). El usuario vio pasar el cartel ROUND 1.
- **vb** (`runs/kof2002um-bench/20261106-090910`, parcial ON): **16,6 fps**, FRAME 60,3, gpu_busy 58,1, tex_load 25,5, gp_draw 15,5, n_tex 40,7, n_skip 16,1. Marcas match_start/input_end en **vb 1710/2912** (convert: 1707/2910) → el bench ya mide siempre el mismo tramo del juego.
- **vb_nopartial** (`20261106-091058`, parcial OFF): **16,3 fps**, FRAME 61,4, gpu_busy 56,9, tex_load 23,5, gp_draw 16,8, n_tex 32,7, n_skip 8,0; marcas vb 1707/2911.
- **Lectura**: n_tex+n_skip da 56,8 (ON) contra 40,7 (OFF): las escenas no son iguales (escenario y rival salen al azar), así que la diferencia de ~2 % es ruido. Costo por carga: 0,63 ms (ON, que carga ~3 % de las filas) contra 0,72 ms (OFF) → cargar 30× menos datos solo baja ~13 % el costo: **el costo es fijo por carga** (barreras/vaciado del pipeline), lo que refuerza la hipótesis.
- **sys-botbase no respondía** (192.168.0.137:6000, timeout): se corrió con `devkitnx/configs/_nl_{convert,vb,vb_nopartial}.json` (copias con sysbotbase apagado; el usuario abre el netloader a mano y no hay capturas).

**Próximo paso**: 1) Fijar escenario y rival en el bench (si el juego usa RNG, sembrarlo o forzar la selección en el replay) para que el A/B sea limpio. 2) Bajar el costo fijo por carga: juntar paleta + imagen del mismo draw en un lote, o leer la imagen 8bpp del shared memory como texel buffer (sin load/copy/barreras). 3) gp_draw (15,5) y resolves (6,2).

## Carga parcial por filas (`texture_partial_reload`): +5 % (15,8 → 16,6 fps); la pelea real anda a ~16 fps, no a 32 (2026-11-06)

- **Cambio**: `pipeline/texture/cache.cpp` (`CommitPreparedTextureLoad`): para texturas base sin mips, lineales, 2D, bloques 1x1, ≥64 KB, no escritas por GPU, se guarda una sombra de las filas cargadas (`Texture::loaded_base_shadow`, en `cache.h`). En cada recarga se compara fila por fila: si no cambió nada se saltea (cuenta n_skip, reemplaza al hash para esas texturas); si no, se carga solo la banda [primera, última fila distinta] vía `partial_base_load_row_first_/count_`. `vulkan/texture_cache.cpp`: con banda, el host layout usa `count` filas, `guest_offset = first * pitch`, y la copia usa `imageOffset.y = first`. `SupportsPartialBaseLoad()` virtual (solo Vulkan devuelve true). Cvar `texture_partial_reload` (true). Backups `*.bak_partial`.
- **Build** `_logs/partial-build.log`, exit 0. Captura de la pelea bien (sin basura).
- **Bench** `runs/kof2002um-bench/20261106-083550`: **16,3 fps** (FRAME 61,3; gpu 56,7; tex_load 24,6). A/B mismo NRO con `--texture_partial_reload=false` (`configs/kof2002um_bench_nopartial.json`, run `083757`): **15,8 fps** (tex_load 26,8). O sea la carga parcial gana ~3 % / −2,2 ms de tex_load.
- **Pero todo está ~2× más lento que anoche** (`005541`: 31,7 fps) en las dos variantes, también zonas no tocadas (gp_draw 8,7 → 16-17, gp_resolve 3,4 → 6, gp_dump 2,4 → 4,5), y n_tex por frame 27,5 → 41. Costo por carga 0,40 → 0,60 ms: GPU ~1,5× más lenta y ~1,5× más trabajo por frame presentado. Ambos en handheld, mismos args. Sospecha: estado de la consola (clock de GPU/sys-clk, batería sin cargador, temperatura).

- **Corrección (el usuario lo vio en la consola)**: la consola no cambió (overclock igual). Los runs de "31,7 fps" se cerraban **antes del cartel ROUND 1**: el script de entrada cuenta frames pero el juego avanza por tiempo, así que un run más rápido cubre solo la intro (más liviana). Los de hoy pasan el cartel y llegan a los golpes. **La pelea real anda a ~16 fps**; los números de antes de hoy no sirven para comparar con estos.
- **A/B repetido**: `084241` con parcial **16,6 fps** (FRAME 60,1, tex_load 25,3, gp_draw 15,6) contra `084357` sin parcial **15,8 fps** (63,2, 26,6, 17,4). Ganancia estable ~5 %, pero tex_load solo baja 1,3 ms: cada carga sigue costando ~0,6 ms de GPU aunque sea una banda chica → el costo es fijo por carga (barreras/vaciado del pipeline entre draws), no el ancho de banda.

- **Reloj de vblanks en el autotest (compilado, `_logs/vbclock-build.log` exit 0, sin probar)**: el replay avanzaba por swaps pero el juego por vblanks (con `vblank_per_swap_timeout_ms=20` entran más vblanks por swap cuanto más lento va), así que input y marcas caían en otro momento del juego según los fps. `include/rex/autotest.h`: `--at-clock=vblank` (replay, marcas y grabación en vblanks; el paso del input lo mueve el hilo del vblank, marcas y fin los manda el hilo del frame), `--at-convert=<ruta>` (con reloj de swaps, reescribe el script con vblanks), la línea `[AT] mark` ahora trae `vb=`. Hook `rex_autotest_vblank()` en `core/autotest.cpp`, llamado desde `GraphicsSystem::MarkVblank` (`graphics/graphics_system.cpp`). Backups `*.bak_vbclock`. Configs nuevas en `devkitnx/configs`: `kof2002um_bench_convert.json` (genera `sdmc:/switch/kof2002um-nx/bench_input_vb.txt`), `kof2002um_bench_vb.json`, `kof2002um_bench_vb_nopartial.json`. **La conversión falló por sys-botbase (timeout a 192.168.0.137)**; hay que correr convert → vb → vb_nopartial.
- **Investigación (Gemini 3 Flash vía delegar, sin web real; WebSearch caído en esta sesión)**: lo útil: leer la imagen 8bpp + paleta directo del shared memory desde el shader del draw (texel buffer + lookup, filtrado a mano si hace falta), sin imagen ni load/copy/barreras → ataca los ~25 ms de tex_load. Diferir cargas entre draws no sirve (cada draw necesita su sprite recién escrito).

**Próximo paso**: 0) Correr convert → vb → vb_nopartial y confirmar en pantalla que pasa el cartel ROUND 1. 1) Arreglar el bench para que la ventana mida la misma escena siempre (marca al aparecer ROUND 1 o ventana por tiempo, no por frames). 2) Confirmar que el costo es fijo por carga: medir gp_tex_load con la carga limitada a 1 fila (cvar de prueba) — si no baja, el costo son las barreras. 3) Si son las barreras: juntar las cargas (paleta + imagen del mismo draw en un lote; o diferir y agrupar cargas de varios draws), o evitar la copia de la imagen 8bpp leyéndola del shared memory como texel buffer. 4) Resolves de 1-3,7 MB que se recargan; gp_draw.

## Contadores de cargas por frame y diagnóstico [TL]: las cargas son pares paleta + imagen 8bpp que cambian ~3 % (2026-11-06)

- **Contadores nuevos en perf.csv** (`core/autotest.cpp`, `kCpuZones` 4 → 7; cada evento suma 1 ms, así que la columna en ms es la cuenta): `n_tex` (cargas, junto a `RexSwitchPerfCount(14)` en `vulkan/texture_cache.cpp`), `n_batch` (lotes, junto al 33), `n_skip` (saltos por hash, `pipeline/texture/cache.cpp`). Bench `runs/kof2002um-bench/20261106-005541` (31,7 fps, tex_load 10,9): **n_tex 27,5, n_batch 20,7, n_skip 8,4 por frame** → el lote junta solo 1,3 cargas: casi cada draw carga una sola textura. Cada carga cuesta ~0,4 ms de GPU. Más lote no rinde; hay que cargar menos.
- **Diagnóstico** cvar `diag_texload_log=N` (INT32, `pipeline/texture/cache.cpp` en `CommitPreparedTextureLoad`, antes del salto por hash): guarda una copia sombra por textura y escribe por stdout `[TL]` las cargas N..N+300 (dirección, tamaño, formato, chunks de 256 B que cambiaron). Config `configs/kof2002um_bench_tldiag.json` (N=20000 ≈ frame 955). Analizador: copiar `tl.py` del scratchpad de esta sesión o rehacerlo (agrupa `[TL]` por base). Run `20261106-005924` (22,9 fps por las copias sombra; con el cvar en 0 no cuesta nada).
- **Resultado (300 cargas)**: alternan de a pares **`179A3000` (paleta 256x1 k_8_8_8_8, 1 KB)** y **`1745C000` (640x448 k_8 lineal, 336 KB)**: 129 de cada una. La imagen 8bpp cambió en 101 de las 129 (las otras 28 las saltea el hash) y **solo cambia el 2,6 % de los chunks (rango 6,5 %)**: el juego escribe un sprite por draw. La paleta cambia en 79 de 129 (25 %). Además hay 42 cargas (14 %) de texturas tiled 640x448 k_8_8_8_8 (1,1 MB: `186D2000`, `185BA000`, `1F52B000`) y 1280x720 (3,7 MB: `1F6F8000`) con `base_written_by_gpu` (resolves): el hash no aplica y la sombra no ve cambios porque el resolve escribe en el buffer de la GPU, no en la RAM.
- **Build** `_logs/tldiag-build.log`, exit 0 (el NRO actual tiene contadores + diag apagado). Backups `cache/*.bak_counters`.
- **Herramienta**: `run-logged.ps1` no anda en segundo plano; usar `python runner/nxtest.py run <cfg> --no-build *> _logs/x.log` desde `devkitnx`.

**Próximo paso**: 1) **Carga parcial de `1745C000`** (lineal, así que las filas son contiguas): con una sombra por textura (solo para las de base sin mips, lineales, no escritas por GPU), encontrar el rango de filas que cambió y cargar solo ese rango (dispatch con offset en y + región de copia parcial). Baja el ancho de banda y el ALU (~94 %), pero no las barreras. 2) Para las barreras: ver si la paleta y la imagen de un mismo draw ya caen en el mismo lote (n_batch 20,7 contra 27,5 sugiere que sí en parte); si no, juntarlas. 3) Las 42 recargas de resolves de 1-3,7 MB: ver por qué se invalidan si la RAM no cambia (¿el resolve vuelve a escribir lo mismo cada frame?) y si se pueden saltear con un hash del lado GPU o una marca de "resolve sin cambios". 4) Después: gp_draw (8 ms).

## Los tirones de ~120 ms eran el timeout de vblank_per_swap: con 20 ms el bench da 32,4 fps (2026-11-06)

- **Causa**: en el bench, el 27 % de los frames duraba ~124 ms (7 vblanks) con gpu_busy ~33 y ~108 ms fuera de toda zona cp_*. Con `--vblank_per_swap`, `WaitSwapForVblank` (`kernel/xboxkrnl/xboxkrnl_video.cpp` ~476) solo da un vblank si hay un swap nuevo o si pasaron `vblank_per_swap_timeout_ms` (default **100**). Cada 6-8 frames el juego necesita un vblank *antes* del swap, así que se trababa hasta el timeout. Es un artefacto del modo bench (el juego normal tiene vblank real a 60 Hz). Ya pasaba antes del lote (118 frames ≥100 ms en `20261106-002744`).
- **Bench** `runs/kof2002um-bench/20261106-005057` con `--vblank_per_swap_timeout_ms=20` (config `configs/kof2002um_bench_vbto.json`): **FRAME 30,9 ms, 32,4 fps** (p95 38,6), 0 frames ≥100 ms; match_start e input_end en los mismos frames (827/1203), o sea que sigue siendo determinista. gpu_busy 27,6, gpu_span 29,3, tex_load 10,7, draw 8,0, resolve 3,4, rt_xfer 2,5, dump 2,4. cp_tex 5,1, cp_fence 5,0.
- **Config principal** `configs/kof2002um_bench.json`: se le agregó `--vblank_per_swap_timeout_ms=20`. **Las cifras de FRAME de las secciones de abajo están infladas por el timeout de 100 ms**; las de la GPU (gp_*, gpu_busy) valen.
- **Ahora manda la GPU**: 27,6 ms de 30,9. Para 60 fps hay que bajarla a <16,7 ms: sobran ~11 ms. Frames por intervalo de vblank: 234 en 1, 124 en 2.

**Próximo paso**: 1) gp_tex_load (10,7 ms, ~72 cargas/frame): sacar al log el contador 33 (lotes) y el 14 (cargas) por frame para ver cuánto junta cada lote; si junta poco → versiones de imagen o carga parcial de la CLUT `17455000`/`179A3000`. 2) gp_draw (8,0). 3) resolve + dump + rt_xfer (~8,3 juntos).

## Lote de cargas de textura: tex_load 20,8 → 11,8 ms, 14,7 → 19,1 fps (2026-11-06)

- **Bench** `runs/kof2002um-bench/20261106-004313` (PASÓ). Pelea, prom ms (antes → ahora): gp_tex_load **20,8 → 11,8**, gpu_busy 47,6 → 27,4, gp_draw 13,6 → 7,8, cp_fence 14,6 → 2,4, gpu_span 52,8 → 45,6, FRAME **68,0 → 52,3 (19,1 fps)**. Repetición `20261106-004629`: 17,9 fps, tex_load 11,1 (otro escenario: el escenario y el rival salen al azar).
- **Captura**: la de match_start+5 s parecía rota (rectángulos translúcidos, sin ROUND), pero es el zoom del cartel "FIGHT": al ir más rápido, los 5 s caen en otro momento. Con la config `configs/kof2002um_bench_shots.json` (capturas a 3,8 s `pelea_a` y a 5 s `pelea`), ROUND y FIGHT se ven bien. El lote no rompe texturas.
- **Ya no manda solo la GPU**: gpu_busy 27 contra FRAME 52. Los peores frames (840-849, al empezar la pelea) tienen ~130 ms con gpu_busy ~26 y gpu_span ~120-130: la GPU está ociosa entre medio y cp_submit da 12-25 ms. En la pelea estable (frame ~994) sigue habiendo frames de 129 ms con cp_tex 12,7 y tex_load 15,9.
- **Contador de lotes (33)**: "textures loaded (N batches)" no sale en log.txt (solo en el overlay), así que no se sabe cuánto junta cada lote.
- **Herramientas**: `tools/run-logged.ps1` no anda lanzado en segundo plano desde Claude (termina en 0 s, sin log, `$LASTEXITCODE` vacío; `cmd /c` no corre). Para el bench: `python runner/nxtest.py run ... *> _logs/x.log` directo desde PowerShell en `devkitnx`.

**Próximo paso**: 1) Separar los ~25 ms de FRAME que no son GPU: mirar en `perf.csv` cp_submit/cp_tex/cp_idle por frame y la distancia gpu_span − gpu_busy (huecos de la GPU esperando a la CPU). Lo probable es que la CPU (cp_tex 9,2 con el hash, la grabación de draws) sea ahora el cuello. 2) Sacar el contador 33 al log (WARN o línea `[AT]`) para saber si conviene pasar a versiones de imagen. 3) gp_draw (7,8 ms).

## Lote de cargas de textura (anillo + copias juntas) compilado (exit 0, sin warnings nuevos); falta el bench (2026-11-06)

- **Objetivo del usuario: 60 fps** (16,7 ms). Hoy 14,7 fps (FRAME 68). En la pelea estable (frames 980-999) gpu_busy ~70 ms: tex_load 33, draw 19, resolve 6,3, dump 4,8, rt_xfer 4,6. Aunque tex_load fuera 0, la GPU sigue en ~37 ms: después de esto hay que atacar gp_draw.
- **Hipótesis**: el costo de tex_load son las barreras (en NVK/Maxwell cada una es un WFI), no el ALU ni el ancho de banda (30×287 KB ≈ 8,6 MB/frame ≈ 0,3 ms). Por carga había 3: WAR del scratch compartido, compute→transfer + layout de la imagen, transfer→sampled antes del draw. Las cargas pendientes de un draw ya se juntan en el loop de `TextureCache::RequestTextures` (`pipeline/texture/cache.cpp` ~623).
- **Cambio** (`vulkan/texture_cache.cpp`/`.h`, cvar `texture_load_batch`, default true): mientras corre `RequestTextures`, cada carga escribe en su propio rango (alineado a 256) de un anillo device-local de 32 MB (`load_ring_buffer_`), que se reinicia por submission con una sola barrera WAR. Las copias buffer→imagen y las barreras de layout se graban juntas en `FlushTextureLoadBatch()` (una barrera para todo) al final de `RequestTextures`, antes de la transición a sampled. Las cargas con float-convert de dos pasadas, las de más de 8 MB o con el anillo sin crear siguen por el camino viejo (y primero vacían el lote). Contador nuevo 33 = lotes; en el reporte de perf, "textures loaded (N batches)" (`ui/switch_perf.cpp`). Para volver a lo viejo: `--texture_load_batch=false`.
- **Revisión**: Gemini sin cuota (exit 10); la revisé yo y la sincronización está bien. Diff en el scratchpad de la sesión (`loadbatch.diff`).
- **Build**: `_logs/loadbatch-build.log` (lanzado en segundo plano; leer con logfilter). Backups `cache/vulkan_texture_cache.{cpp,h}.bak_loadbatch`, `cache/switch_perf.cpp.bak_loadbatch`.
- **Descartado**: carga parcial "solo las páginas sucias" con el watch actual: el watch se dispara en la primera escritura y se borra, así que no sabe qué páginas cambiaron (haría falta una copia sombra y comparar).

**Próximo paso**: 1) Si el build falló, arreglarlo; si pasó, bench: `cd devkitnx; python runner/nxtest.py run configs/kof2002um_bench.json --no-build` (en segundo plano, consola en WiFi con sys-botbase). 2) Comparar gp_tex_load (20,8) y FRAME (68,0); leer "textures loaded (N batches)" en el reporte: si lotes ≈ cargas, el lote por draw no junta nada y lo siguiente son las versiones de imagen (varias VkImage por textura para subir todas las cargas del frame al principio del command buffer). 3) Después: gp_draw (19 ms).

## Recargas de textura salteadas por hash: tex_load 29,1 → 20,8 ms, 13,8 → 14,7 fps (2026-11-06)

- **Cambio** (`pipeline/texture/cache.cpp`, `CommitPreparedTextureLoad`): cvar `texture_skip_unchanged_reload` (default true). Si una textura se invalida pero el XXH3 de sus datos base en la RAM del guest es igual al de la última carga, no se recarga (solo `MakeUpToDateAndWatch`). Solo cuando se carga la base sin mips, no es `scaled_resolve` y la GPU nunca escribió ese rango (`Texture::base_written_by_gpu`, se marca en `TextureCache::WatchCallback` con `invalidated_by_gpu`, porque ahí la RAM no refleja lo cargado). Campos nuevos en `Texture` (`cache.h`): `loaded_base_hash`, `loaded_base_hash_valid`, `base_written_by_gpu`. Accessor público nuevo `SharedMemory::guest_memory()` (`memory()` es protected). Para volver a lo viejo: `--texture_skip_unchanged_reload=false`.
- **Build**: `_logs/hashskip-build2.log`, exit 0. Backups `cache/texture_cache_base.{cpp,h}.bak_hashskip`.
- **Bench** `runs/kof2002um-bench/20261106-002744` (PASÓ, config normal con sys-botbase). Pelea, prom ms (antes → ahora): gp_tex_load **29,1 → 20,8**, gpu_busy 60,2 → 47,6, cp_fence 25,5 → 14,6, cp_tex 6,4 → 8,6 (lo que cuesta el hash), FRAME **72,4 → 68,0** (14,7 fps). `pelea.jpg` se ve bien (escenario, sprites, ROUND).
- **Pendiente**: el contador `REXGPU_INFO("texture_skip_unchanged_reload: N of the last 4096...")` no aparece en el log del run (¿nivel info filtrado?), así que no se sabe qué fracción se saltea. Ahora la GPU tiene ~20 ms de sobra contra FRAME 68: el frame ya no lo marca solo la GPU (gpu_span 52,8).

**Próximo paso**: 1) Ver el ratio de saltos (pasar el log a WARN o usar un contador de `RexSwitchPerfCount`) y cuántas cargas/frame quedan. 2) Las que quedan: menos barreras (juntar las cargas de un mismo draw: todos los dispatch, una barrera, todas las copias). 3) Averiguar en qué se van los ~15 ms de FRAME que no cubre gpu_span y los picos de `cp_submit` (35-70 ms) al empezar la pelea.

## Bench con zonas cp_*: el vblank sin spin da 13,8 fps; el cuello vuelve a ser la GPU (tex_load) (2026-11-06)

- **Bench** `devkitnx/runs/kof2002um-bench/20261106-001615` (PASÓ). Se corrió con `configs/kof2002um_bench_nobot.json` (copia sin sys-botbase, porque el netloader lo abrió el usuario a mano y las entradas salen de `bench_input.txt`; no hay captura `pelea.jpg`). Log: `_logs/bench-cpuzones.log`.
- **Pelea, prom ms (antes → ahora)**: FRAME **102 → 72,4** (p95 122,8), **9,8 → 13,8 fps**. gpu_busy 59 → 60,2, gpu_span 91 → 65,4. El sleep de 1 ms en el vblank (`vblank_per_swap_sleep`) liberó el núcleo que antes giraba en `yield()`.
- **Zonas de CPU**: `cp_idle` **1,4** (el guest no frena al CP), `cp_fence` **25,5** (p95 56: el CP espera a la GPU), `cp_submit` 3,6 de promedio pero 40-80 ms en los peores frames (840-846, apenas empieza la pelea; ahí gpu_span ≈ 159 con gpu_busy 53), `cp_tex` 6,4.
- **Conclusión**: el cuello es la GPU, no el guest. gp_tex_load **29,1 ms** (p95 41,6) es el pase más caro; le siguen draw 15,6, resolve 5,6, dump 4,3, rt_xfer 4,1.
- **Descartado**: perfilar zonas del guest (cp_idle ≈ 0).
- **Experimento sin dispatch de carga**: cvar nuevo `perf_skip_mask` (INT32, definido en `vulkan/command_processor.cpp` y aplicado en `SetupContext` con `RexSwitchPerfSetFixedSkip` de `ui/switch_perf.cpp`; no puede ir en switch_perf porque `rexglue_switch_startup` no tiene fmt). Bits iguales a los del A/B (8 = dispatch de texture load). Bench `runs/kof2002um-bench-skiptex/20261106-002138` con `configs/kof2002um_bench_skiptex.json` (`--perf_skip_mask=8`, sys-botbase volvió a andar): gp_tex_load **29,1 → 19,2**, FRAME 72,4 → 64,0 (15,6 fps). **El ALU del shader pesa ~10 ms y las barreras más las copias, ~19 ms**: lo que más rinde es hacer menos cargas, no un shader más rápido. Build `_logs/perfskip-build2.log`, backup `cache/command_processor.cpp.bak_perfskip`.

**Próximo paso**: 1) Juntar las ~72 cargas de textura por frame en `vulkan/texture_cache.cpp` (~1536-1780): una sola barrera/submit para todas en lugar de dispatch + `SubmitBarriers` + barrera + `CmdCopyBufferToImage` por cada una, o recargar solo la parte que cambió de la CLUT `17455000`/`179A3000`. 2) Ver qué bloquea `vkQueueSubmit` 40-80 ms en los primeros frames de la pelea (¿compilación de pipelines?).

## tex_load no tiene udiv; zonas de CPU cp_* y vblank sin spin compilados, falta el bench (2026-11-06)

- **Descartado**: `texture_load_8bpb_cs.h` (la que carga la 8bpp `17455000`) no tiene OpUDiv/OpUMod (solo shifts, ands y 7 IMul). Las divisiones están solo en las variantes `_scaled`, en los resolve y en los DXT. Además no hay fuentes `.xesl`: solo los `.h` precompilados. Los 29 ms de gp_tex_load salen de 72 cargas/frame, cada una con dispatch + `SubmitBarriers` + barrera + `CmdCopyBufferToImage` (`vulkan/texture_cache.cpp` ~1536-1780): el costo es probablemente serialización/copias y no ALU.
- **Pista nueva**: en los peores frames del bench anterior FRAME ≈ gpu_span ≈ 157 ms con gpu_busy 53: la GPU espera ~100 ms a la CPU.
- **Zonas nuevas** (perf.csv): `cp_idle` (CP esperando que el guest escriba el ring, `graphics/command_processor.cpp` WorkerThreadMain), `cp_fence` (vkWaitForFences bloqueante en `CheckSubmissionFenceAndDeviceLoss`), `cp_submit` (vkQueueSubmit), `cp_tex` (`VulkanTextureCache::RequestTextures`). Se juntan con `rex_autotest_cpu_add` (atomics en `core/autotest.cpp`). `AT__MAX_ZONES` 16 → 24 en `devkitnx/include/autotest.h` y copiado a `include/rex/autotest.h`.
- **Arreglo posible**: con `vblank_per_swap` el hilo de vblank hacía `yield()` en loop todo el frame mientras esperaba el swap (un núcleo entero ocupado). Ahora duerme 1 ms; cvar `vblank_per_swap_sleep` (default true, `=false` vuelve a lo viejo). Solo afecta al bench (el juego normal no usa vblank_per_swap).
- **Build**: `_logs/cpuzones-build.log` y `cpuzones-build2.log`, exit 0, sin warnings nuevos. NRO 65.157.317 bytes (00:14). Backups `cache/*.bak_cpuzones`, `cache/xboxkrnl_video.cpp.bak_vblanksleep`.
- **Bench no corrido**: `nxtest.py` no pudo conectar a sys-botbase (192.168.0.137:6000, timeout) a las 00:14.

**Próximo paso**: 1) Con la consola prendida, en WiFi y sys-botbase andando: `cd devkitnx; python runner/nxtest.py run configs/kof2002um_bench.json --no-build` (en segundo plano). 2) Comparar FRAME con 102 ms (efecto del vblank sin spin) y leer cp_*: si `cp_idle` es grande → el cuello es la CPU del guest; si es ≈0 → el CP (ver `cp_tex`, `cp_submit`); `cp_fence` = espera de la GPU. 3) Según eso: guest → perfilar zonas del juego; CP/tex → juntar las 72 cargas (una barrera para todas, o carga parcial de la CLUT `17455000`).

## Sin udiv en los shaders de dump/transfer: GPU de la pelea 129 → 59 ms, 7,3 → 9,8 fps (2026-11-06)

- **Cambio** (`tools/rexglue-sdk/src/graphics/vulkan/render_target_cache.cpp`, arriba de todo, fuera del `#if` de Switch): cvar `vulkan_shader_fast_idiv` (default true, kInitOnly) y helpers `FindUDivMagic`, `EmitUDivModConst` (divisor constante: shift por la potencia de 2 y multiplicar por una constante ≤ 0xFFFF; la exactitud se verifica en la CPU para todo el rango) y `EmitUDivModSmall` (divisor variable: `(a+0.5)/d` en float y corrección ±1 entera). Se usan en el dump (tile width/height y tile index / pitch), en el transfer (píxel de destino / tile y tile index / pitch) y en el transfer de host depth. Para volver al código viejo sin recompilar: `--vulkan_shader_fast_idiv=false`. Lo que falta son las mismas divisiones en los shaders precompilados (resolve/tex_load), que no se tocaron.
- **Verificado en Python**: las constantes son 80/40/20 → (3277, 14) tras el shift; 48 → 2731; 240 → 17477. La división en float sale exacta para d < 1024 y a < 8192, aun sin la corrección.
- **Build**: `_logs/fastidiv-build.log`, exit 0, sin warnings nuevos. NRO 65.157.317 bytes (2026-11-06 00:05).
- **Bench** `devkitnx/runs/kof2002um-bench/20261106-000543` (PASÓ). Pelea, prom ms, antes → ahora: gpu_busy **128,8 → 59,0**, gp_dump **45,0 → 4,1**, gp_rt_xfer **28,3 → 3,9**, draw 15,5 → 15,1, resolve 6,3 → 5,4, tex_load 31,5 → 28,9. FRAME 136 → 102 (p95 125), **9,8 fps** (antes 7,3). La captura `pelea.jpg` se ve bien (escenario, sprites, marcos).
- **Descartado**: el plan de la sección anterior (resolve directo leyendo el VkImage) ya no hace falta, porque el dump pasó a costar 4 ms. El stub `TryResolveCopyDirectly` queda como estaba.
- **Lo que queda**: 1) **gp_tex_load 28,9 ms** (p95 42,5) es ahora el pase más caro. Por las notas viejas: la textura 8bpp 640x448 `17455000` (fmt 2) y su paleta 256x1 `179A3000` se recargan ~30 veces por frame porque la CPU las reescribe entre draws (sprites con CLUT). Los shaders de carga son precompilados (`shaders/vulkan_spirv/texture_load_*`) y seguro tienen las mismas divisiones enteras. 2) **FRAME 102 contra gpu_busy 59**, con gpu_span 91: ahora hay ~40 ms por frame que no son trabajo de GPU (espera o CPU de la guest).

**Próximo paso**: 1) Buscar `OpUDiv`/`udiv`/`/ ` y `%` en las fuentes de los shaders de texture load (`tools/rexglue-sdk/src/graphics/shaders/*texture_load*` o el include común de direccionamiento tileado) y aplicarles el mismo truco (o un `#define` que use float). Hay que recompilar los `.h` de SPIR-V: ver cómo se generan (`vulkan_spirv/`). 2) Si tex_load baja, mirar la carga parcial de la textura CLUT que se recarga 30 veces por frame. 3) Perfilar los ~40 ms que no son GPU (zonas de CPU de la guest y espera del present).

## Bench corto con zonas gp_*: en la pelea la EDRAM se come 73 de 129 ms de GPU (2026-11-05)

- **Corrida**: `devkitnx/runs/kof2002um-bench/20261105-235643` (log `_logs/bench_corto.log`, PASÓ, sin timeouts). El replay corto anda y cierra solo: 1203 frames, `match_start` en el frame 827, 376 frames de pelea.
- **Pelea (prom / p95 ms)**: FRAME 136 / 170, 7,3 fps. gpu_busy 128,8 / 141,5. **gp_dump 45,0**, **gp_tex_load 31,5** (41 en p95), **gp_rt_xfer 28,3**, gp_draw 15,5, gp_resolve 6,3, res_clear 1,1, upload 1,1. Lo demás es ~0.
- **Contra el menú** (dump 20 + rt_xfer 16 de 51): en la pelea dump y rt_xfer pasan a 73 ms (57 % de la GPU) y tex_load pasa de 6,9 a 31,5. Los draws siguen baratos. Casi todo el frame es GPU: FRAME 136 con gpu_busy 129, así que los ~65 ms de CPU que había en el menú no aparecen en la pelea.
- **Hallazgo en el código**: `TryResolveCopyDirectly` (`vulkan/render_target_cache.cpp`, ~6613) es un **stub**: pasa los chequeos previos y después llama igual a `DumpRenderTargets`. O sea que cada resolve vuelca el RT del host entero al `edram_buffer_` (gp_dump) y recién después el shader de copia lee de ahí. `direct_host_resolve` no ahorra nada hoy.
- **Sospecha sobre tex_load**: `MarkRangeAsResolved` invalida la textura de destino en cada resolve, así que el juego vuelve a destilear cada frame lo que resolvió. Sin confirmar.

**Próximo paso** (en `render_target_cache.cpp`): 1) Implementar el resolve directo de verdad: un compute que lea el `VkImage` del RT del host (texelFetch, con MSAA) y escriba directo en la memoria compartida con el formato de destino, sin pasar por el `edram_buffer_`. Debería borrar la mayor parte de gp_dump (45 ms). Arrancar por el caso de la pelea (color de 32 bpp, el copy_shader que más aparece en `ResolveStatsBegin`). 2) Para rt_xfer (28 ms): traer el log del juego de la SD y mirar las líneas `[rt-xfer]` (pares fuente→destino ordenados por tiles) para ver qué transferencias hay por frame. Lo más probable es que sean ida y vuelta color↔depth en la misma base, y se podrían saltear como `vulkan_skip_overwritten_depth_transfers`. 3) Confirmar si tex_load sale de texturas resueltas (si el resolve directo deja la textura válida, se puede evitar la recarga).

## GPU por pase en el perf.csv; replay corto grabado; falta el bench con el replay nuevo (2026-11-05)

- **Zonas nuevas** (NRO 65.153.221 bytes, build `_logs/gpupass-build2.log` exit 0): `command_processor.cpp` junta los ticks de cada pase por frame (`frame_pass_ticks`) y llama `rex_autotest_gpu_passes` (en `core/autotest.cpp`). El perf.csv suma `gp_other, gp_draw, gp_rt_xfer, gp_res_clear, gp_dump, gp_resolve, gp_readback, gp_tex_load, gp_upload, gp_swap` (us) y `gp_markers` (cantidad: en la tabla aparece /1000). Hacen falta `--perf --gpu_submission_timestamps=true`. Backup: `cache/command_processor.cpp.bak_passzones`.
- **Fin del replay**: en `autotest.h` (SDK y `devkitnx/include`, idénticos), pasado el último paso sin `END` cuenta como `input_end`, así que `--at-exit-at-end` ahora cierra solo. Ojo: `--at-exit-after=N` cuenta **frames**, no ms.
- **Primer desglose** (`devkitnx/runs/kof2002um-bench/20261105-235040`; solo menú e intro, 960 frames, cortado a mano): gpu_busy 31 ms de promedio, **51 ms** en p95. De eso: **gp_dump (EDRAM dumps) 20 ms + gp_rt_xfer 16 ms**, tex_load 6,9, draw 4,4, resolve 3,1. El costo es la emulación de EDRAM, no los draws. FRAME total p95 118 ms con 51 de GPU: en el menú también hay ~65 ms que no son de GPU.
- **Replay corto**: grabado de nuevo (`runs/kof2002um-grabar/20261105-235253`, ~135 s, inicio rápido, MINUS una sola vez en la pelea). Reemplaza a `sdmc:/switch/kof2002um-nx/bench_input.txt`.

**Próximo paso**: 1) Con el netloader abierto (el usuario ya lo abrió): `cd devkitnx; python runner/nxtest.py run configs/kof2002um_bench_manual.json --no-build`, en segundo plano. Ver el desglose `gp_*` desde `match_start`. 2) Si en la pelea dominan dump y rt_xfer: ver en `render_target_cache.cpp` por qué se vuelca y se transfiere la EDRAM cada frame (¿cambia el layout de RT entre pases? ¿dumps de RTs que nadie lee?) y probar a reducirlo o saltearlo.

## Bench completo por WiFi hasta la pelea: 7,5 fps en la pelea, cuello de botella GPU (2026-11-05)

- **Corrida** `devkitnx/runs/kof2002um-bench/20261105-232546` (Auto-Sleep en Nunca; config `bench_manual`). El primer intento dio timeout con sys-botbase (WiFi lenta, ping de ~100 ms); al reintentar anduvo.
- **match_start** en el frame 2835 (a los 280 s). La captura `pelea.jpg` se ve bien (escenario del parque, HUD correcto, sin errores gráficos).
- **Perf desde match_start** (4994 frames): **7,5 fps**; el 97 % de los frames se pasa del presupuesto. FRAME 132,6 ms prom (p99 222 ms); gpu_busy 121,4 ms (p95 141,7 ms). Antes de la pelea: ~100 ms, de los que ~78 ms son de GPU. Frames 5358–5387 a ~320 ms, con gpu_span ~217 ms y gpu_busy ~115 ms: parece una espera, no trabajo de GPU.
- **Problemas del autotest**: 1) `--at-exit-at-end` no cerró el juego al terminar el replay; lo cerró el usuario a mano y el runner marca FALLÓ. 2) `match_start` se dispara 3 veces (la grabación tiene 3 marcas de MINUS) y saca 3 capturas; hay que quedarse solo con la primera, o grabar con una sola marca.

**Próximo paso**: 1) Arreglar `at-exit-at-end` en `at_init`/autotest y deduplicar las marcas. 2) Atacar el GPU en la pelea (~121 ms): perfilar draws por pase (fondo 3D, sprites, post-proceso), revisar la resolución interna y los blits/resolves.

## Primera grabación y primer bench por WiFi: andan; el bench se corta por el auto-sleep (2026-11-05)

- **Grabación OK** (`runs/kof2002um-grabar/20261105-230658`): con `upload_path` nuevo (`kof2002um-nx-at2.nro`, en grabar y bench) subió en 10 s. `bench_input.txt` quedó con 277 pasos y 3 marcas. El MINUS no manda `[AT]`: solo escribe `@match_start` en el archivo. Al cerrar con HOME no corre `at_shutdown`, así que no hay `END` ni `[AT] done` y el runner marca FALLÓ aunque esté bien.
- **Bench** (`runs/kof2002um-bench/20261105-231512`, config `configs/kof2002um_bench_manual.json` = bench sin before_run ni open_netloader, para usarla con el netloader ya abierto a mano): 2560 frames, **10,5 fps de promedio**, 76 % pasado de presupuesto. Patrón 16/16/117 ms. gpu_busy ~28 ms → ~51 ms desde ~f131 → ~141 ms al final (pelea). Cuello de botella: GPU.
- **Corte a los 278 s**: se apagó la pantalla de la Switch (auto-sleep, porque el replay no cuenta como actividad) y se cayó el socket de nxlink. El juego siguió. No llegó `match_start` antes del corte.

**Próximo paso**: 1) Poner Auto-Sleep en "Nunca" (Configuración → Suspensión) o agregar `appletSetAutoSleepDisabled(true)` en `at_init` (requiere recompilar). Reabrir el netloader y correr `nxtest.py run configs/kof2002um_bench_manual.json --no-build`. 2) Ver si llega `match_start` y comparar perf desde ahí. 3) Atacar el GPU (51→141 ms/frame).

## Autotest: build nuevo OK y conexión WiFi casi resuelta; la grabación todavía no corrió (2026-11-05)

- **Hecho**: header nuevo copiado al SDK y recompilado (`_logs/autotest-build2.log`: 0 errores, 0 warnings; NRO 65.149.125 bytes, SHA `C4411B28…718A`). sys-botbase v2.5 instalado en la SD (carpeta `D:\Ports Swtch\SD_sys-botbase`), responde en **192.168.0.137:6000**. `switch_ip` = 192.168.0.137 en las configs; `sysbotbase.enabled: true` solo en `kof2002um_bench.json`.
- **Arreglado en `devkitnx/configs/*.json`**: 1) `upload_path` sin `switch/` adelante (`kof2002um-nx/kof2002um-nx-at.nro`): hbmenu lo pega detrás de `sdmc:/switch/` y no crea carpetas → `open: err=2` (también en el README). 2) `"nxlink": "D:/Ports Swtch/devkitPro/tools/bin/nxlink.exe"`: el firewall (red Public) solo permite entrar a ese, no al de `C:\devkitPro` (mismo binario). Con el de C: subió el NRO pero no llegó ningún `[AT]` en 240 s.
- **No hacer**: abrir una conexión de prueba al 28280 (Test-NetConnection) antes de nxlink: hbmenu da error 11 y cierra el netloader (memoria `netloader-no-probar`).
- **Último intento** (`runs/kof2002um-grabar/20261105-230403`, ya con el nxlink de D:): "Sending … 65149125 bytes" y a los 19 s "conexión interrumpida por el host remoto / Failed sending chunk size"; en la Switch el netloader quedó **en 0,00 %**. Hipótesis: hbmenu se trabó preparando el archivo, que ya existía de la corrida anterior (`open`+`ftruncate` de 65 MB y `fopen "wb"`), o un corte de la WiFi. La corrida anterior, con el archivo nuevo, subió en 8 s.

**Próximo paso**: 1) Que el usuario salga de esa pantalla (B o HOME → cerrar hbmenu y volver a abrirlo en modo título) y abra el netloader con Y. Reintentar `python runner/nxtest.py run configs/kof2002um_grabar.json --no-build` desde `devkitnx` (en segundo plano + Monitor sobre `runs/kof2002um-grabar/<último>/log.txt`). 2) Si vuelve a quedar en 0 %, cambiar `upload_path` a un nombre nuevo (`kof2002um-nx/kof2002um-nx-at2.nro`, en grabar y en bench) y reintentar. 3) Cuando lleguen `boot`/`first_frame`: el usuario juega con la cruceta hasta una pelea, MINUS al arrancar, ~60 s, HOME → cerrar. Verificar `bench_input.txt` (`@match_start`, `END`). 4) Bench con `kof2002um_bench.json` (sys-botbase reabre el netloader solo).

## autotest integrado al port y compilado (build sin el último header); falta recompilar y la primera corrida real (2026-11-05)

- **devkitnx** (`D:\Ports Swtch\devkitnx`, ver README, sección "Integración en kof2002um-nx"): header nuevo con `at_zone_add` (tiempo medido en otro hilo), `at_record` (devuelve los botones para el juego; stick izq. → cruceta; `--at-record-mark=MINUS:match_start` escribe `@match_start`; al cerrar escribe `END`), marcas `<frame> @nombre` en el replay, `--at-exit-at-end`, `at_input` que se puede llamar desde el hilo del pad (el avance del replay lo hace `at_frame_end`). Runner: `perf_from_mark`, `sysbotbase.before_run` (HOME, R+A, para volver a hbmenu porque el port cierra el título entero), `fps_avg`. Simulacro del runner PASÓ, `perf_from_mark` probado. Configs: `configs/kof2002um_grabar.json` y `configs/kof2002um_bench.json` (sube como `switch/kof2002um-nx/kof2002um-nx-at.nro`, IP null = broadcast, `--vblank_per_swap=true` para que el replay por frames sea determinista).
- **Port** (rexglue-sdk): copia del header en `include/rex/autotest.h` (editar siempre el de devkitnx y copiar); `src/core/autotest.cpp` (en `core/CMakeLists.txt`, solo Switch): `rex_autotest_start` (con `--autotest`: sockets + `nxlinkStdio`) primera línea de `main`, `rex_autotest_stop` en `ExitProcess`; `rex_autotest_swap` al inicio de `VdSwap_entry` (1 frame = swap a swap, marca `first_frame`); `rex_autotest_gpu_frame` en `command_processor.cpp` al cerrar un frame del probe de timestamps (zonas `gpu_busy`/`gpu_span`, calibradas con el tick de `[gpu-pass]`); `at_record`/`at_input` en `SwitchInputDriver::Poll` del jugador 1; `at_should_quit` → `ProcessQuitRequest` en el loop de `windowed_app_context_switch.cpp`; `ConnectLogNetwork` no reinicia sockets ni pisa stdout si autotest lo tiene.
- **Build**: header nuevo copiado (SHA `11470377…`), `_logs/autotest-build2.log` exit 0, 0 errores, 0 warnings. NRO `kof2002um-nx.nro` 65.149.125 bytes, SHA `C4411B28…718A`, magic `NRO0`, con `[AT]`, `at-record-mark`, `at-exit-at-end`. Paso 1 del próximo paso: hecho.
- **sys-botbase** v2.5 (HOS 21+/AMS 1.10+) listo para la SD en `D:\Ports Swtch\SD_sys-botbase` (copiar `atmosphere/` a la raíz, reiniciar). Switch en **192.168.0.137** (`switch_ip` en las dos configs); `sysbotbase.enabled: true` solo en el bench (en grabar está el usuario). sys-botbase instalado y respondiendo en el 6000.
- **Primeras corridas de grabar** (`devkitnx/runs/kof2002um-grabar/`): 1) `upload_path` con `switch/` adelante → hbmenu da `open: err=2` (la ruta es relativa a `sdmc:/switch/`; corregido en las configs y en el README). 2) El NRO subió bien (24,7 MB comprimido, 8 s), pero no llegó ningún `[AT]` en 240 s: el firewall solo permite entrar a `D:\Ports Swtch\devkitPro\tools\bin\nxlink.exe`, y el runner usaba el de `C:\devkitPro`. Las configs ahora fijan `"nxlink"` al de D:. Falta reintentar.
- **Revisión** (`delegar`: Codex sin cuota, respondió Gemini 3.8 Flash, cortado): posible carrera al salir entre `at_shutdown` (fclose del archivo de grabación) y el hilo del pad en `at_record`. Riesgo bajo (en `ExitProcess` los hilos de la guest ya pararon), pero si crashea al cerrar una grabación, ese es el sospechoso. Su punto 2 (`at_zone_add` desde el hilo de la GPU) es falso: lo llama el hilo de la guest.

**Próximo paso**: 1) `Copy-Item devkitnx\include\autotest.h kof2002um-nx\tools\rexglue-sdk\include\rex\autotest.h` y recompilar (memoria `kof-build-powershell`), sin warnings nuevos. 2) Con el usuario: hbmenu en modo título, Y (netloader), `python runner/nxtest.py run configs/kof2002um_grabar.json --no-build` desde `devkitnx`; jugar con cruceta hasta una pelea, MINUS al arrancar la pelea, ~60 s, HOME → cerrar. Verificar que en la SD quedó `bench_input.txt` con `@match_start` y `END`. 3) `run configs/kof2002um_bench.json --no-build` y comparar fps/`gpu_busy` con lo conocido (~10–12 fps con OC). 4) Recién ahí el bucle (falta un comando `compare` de dos `summary.json` en el runner).

## devkitnx creado: pruebas automáticas por WiFi; falta integrarlo al port para el bucle de rendimiento (2026-11-05)

- **Qué es**: `D:\Ports Swtch\devkitnx` (leer su `README.md`). `include/autotest.h` (header único: `at_mark`, zonas `at_zone_begin/end` con `--perf`, replay `--at-input`, `--autotest=escenario`, `--at-exit-after=N`, `at_done`) + `runner/nxtest.py run <config>` (nxlink → sigue líneas `[AT]` → `runs/<juego>/<fecha>/` con `log.txt`, `perf.csv`, `summary.json`; tabla prom/p95/p99/max por zona; exit 0/1/2/3) + `analyze <csv>` + `bot <ip> click A` (sys-botbase). Config del port: `configs/ejemplo_kof2002um.json` (apunta a `kof2002um-nx.nro`, `upload_path` = `switch/kof2002um-nx/kof2002um-nx.nro` para quedar junto al TOML; boot `within` 180 s por los 65 MB por WiFi; sin `build`, se corre con `--no-build` después del build de PowerShell).
- **Verificado**: demo (`examples/demo`) compila a `.nro` sin warnings; header sin warnings en C/C++/DISABLE con devkitA64; runner probado con `tests/fake_nxlink.py` (`--sim=ok|hang|crash|fail`, todos con el exit code correcto). **Nunca corrió en la consola real.** No hay compilador de PC (BuildTools sin C++).
- **Descartado**: medir fps en Ryujinx (HLE: mide la PC, no la Switch). Revisión con `delegar`: Codex sin cuota (429), Gemini devolvió basura cortada.
- **Pedido del usuario**: un bucle autónomo para subir los fps al máximo sin copiar el `.nro` a mano: build → nxlink → escenario fijo de pelea → `perf.csv` → cambio → repetir.
- **Requisitos del lado del usuario**: IP de la Switch en la config; hbmenu en modo título (R + juego); para que sea 100 % solo, **sys-botbase** instalado y `sysbotbase.enabled: true` (el runner aprieta Y para reabrir el netloader en cada corrida). Sin sys-botbase, el usuario aprieta Y antes de cada corrida.
- **Próximo paso**: 1) Integrar `autotest.h` en la capa Horizon de `kof2002um-nx` (Grep puntual, no explorar): `at_init` al arrancar (con `AT_INIT_NO_SOCKETS` si el port ya hace `socketInitialize`/`nxlinkStdio`), `at_frame_begin/end` alrededor del frame de la guest, zonas para CPU de la guest, submit de GPU y present (reusar los timestamps de `[gpu-pass]`/`rex_profile` si ya existen), `at_mark("boot")`/`("menu_ready")`/`("match_start")`. 2) Escenario `--autotest=bench`: entrar sola a una pelea fija (replay de inputs) y `--at-exit-after=N`. 3) Build con PowerShell (memoria `kof-build-powershell`), primera corrida real con el usuario mirando. 4) Recién ahí, el bucle: cada optimización → build → `nxtest.py run --no-build` → comparar `summary.json` con la corrida anterior.

## Parche XMAD de NAK compilado y en staging: falta la corrida del usuario (2026-11-05)

- Revisé el encode: la forma cbuf 0x4e00 de `sm50.rs` coincide con nouveau `emitXMAD` (cmode 50..51, H1(b) 52, H1(a) 53, PSL 55, MRG 56), y `set_src_cb` solo escribe los bits 20..38. El punto 2 de Gemini ("CBuf sobreescribe cmode") es **falso**. Revisé a mano el `imul` de 3 XMAD, el de inmediato con 2 XMAD y el `imul_2x32_64` de 4 XMAD + shr + iadd + prmt: las cuentas dan.
- Build: `nvk26_all.ps1` corrido directo (salida en `_logs/nvk26_all-xmad.out`), **sin errores de Rust**. `libvulkan.a` nuevo (Mesa 26.2.3 git-d4a00ea0ab) tiene el string `Invalid xmad src1`; el `.pre-xmad` no lo tiene, y en el ELF aparece una vez.
- **Staging**: `switch/kof2002um-nx/kof2002um-nx-xmad.nro`, 65.136.837 bytes, SHA256 `4B71E667…0037`, magic NRO0, ELF en `out/elf-archive/kof2002um_nx-4B71E667.elf`.

**Próximo paso**: corrida del usuario con el bench activo. En `[gpu-bench]`, `imul dep` debería bajar de 127 a ~3–6 ranuras, `fetch2x_div` acercarse a `fetch2x` y `store` 2D acercarse a 1D. En `[gpu-bench-asm]` deberían verse `xmad`. Si la imagen sale con basura o se cuelga, volver atrás: copiar `cache/_bak_xmad/libvulkan.a.pre-xmad` a `tools/mesa-switch-sdk/lib/libvulkan.a` y correr `nvk26_all.ps1 -SkipMesa`. Después mirar los fps de la pelea y el reparto de `[gpu-pass]`.

## gpubench2 corrido: culpable IMUL de NAK (A). Parche XMAD en NAK escrito, SIN compilar (2026-11-05)

**Corrida** (logs `kof2002um_nx_001 (14).log`, `rex_profile (17).log` en `%TEMP%`; modo portátil, GPU 768 MHz): pelea 11,3–11,9 fps, GPU 84 ms/frame (dumps 27, texture loads 23,5, transfers 17,5, draws 10), igual que antes.
**`[gpu-bench]`**: `imul dep 127,4 / ind4 130,0` ranuras por warp-paso, `umul_high 127,4`, contra ffma 1,07 e iadd 2,0 → **A confirmada**: el `imul.u32.u32` que emite NAK (con scoreboard) cuesta ~128 ciclos por warp, ~64× una suma. Ocupación plena (`resident_*` = 128/128), así que B queda descartada. udiv 296 contra fdiv 18,7. `fetch2x_fdiv` 1,22 ms (más rápido que fetch2x) contra `fetch2x_div` 14,5. `store` 2D 1,29 ms contra 1D 0,48 ms: la diferencia es 1 IMUL por hilo. nop_128 0,24 ms (el lanzamiento de CTAs no limita). NAK: 24 GPRs y 64 warps/SM en todos los shaders.
**Hecho**: parche en `~/mesa-switch/src/nouveau/compiler/nak` (WSL), originales en `~/mesa-switch/_bak_xmad/` y diff en `cache/_bak_xmad/xmad.diff`; `libvulkan.a` previo en `cache/_bak_xmad/libvulkan.a.pre-xmad`.
- `ir.rs`: `OpXmad` (srcs[3], h1[2], psl, mrg, `XmadCMode` C32/CLo/CHi/CBcc), variante `Op::Xmad`, en las listas de latencia fija, is_virtual y ftz junto a IMadSp. `opt_instr_sched_common.rs`: en la lista de side effects.
- `sm50.rs`: legalize (src0/src2 a GPR; src1 imm ≤ 0xffff sin H1, cbuf sin CBCC, si no a GPR) y encode copiado de nouveau `emitXMAD` (reg 0x5b00, imm 0x3600, cbuf 0x4e00; bits en el diff). En la macro `sm50_op_match`.
- `builder.rs` (solo SM50–69, srcs sin modificadores): `imul` = 3 XMAD (ptxas/nouveau `handleMULMAD`) o 2 si un factor es inmediato ≤ 0xffff; `imul_2x32_64` sin signo (lo usa umul_high) = 4 XMAD + shr + iadd + prmt [0,1,4,5]. Con signo sigue con IMUL.
- Segunda opinión: Codex sin cuota (429); Gemini 3.8 Flash dijo que `src_is_reg` no está en ámbito → **falso** (sm50.rs lo importa de `crate::legalize`). Su punto 2 ("CBuf sobreescribe el bit de cmode", sm50 forma 0x4e00) quedó cortado: revisarlo yo (cmode 50..51, H1 52, H1(a) 53, X 54, PSL 55, MRG 56 según nouveau).
- **Build: NO corrió.** `run-logged.ps1 -Command "powershell -File '...nvk26_all.ps1'"` salió en 0 s sin log; correr el script directo.

**Próximo paso**: 1) Build: `powershell -File "D:\Ports Swtch\kof2002um-nx\tools\mesa-switch-sdk\scripts\nvk26_all.ps1"` en segundo plano (PowerShell; logs `_logs/mesa-build2.log`, `kof-nvk26-*.log`, leer con `logfilter.ps1`); arreglar errores de compilación de Rust (matches exhaustivos en otros archivos, tipos de `set_field`). 2) Verificar el NRO y copiarlo como `switch/kof2002um-nx/kof2002um-nx-xmad.nro` (el script lo deja como `-nvk26.nro`). 3) Corrida del usuario (bench activo): en `[gpu-bench]` `imul dep` debería bajar de 127 a ~3–6 ranuras, `fetch2x_div` acercarse a `fetch2x` y `store` 2D a 1D; en `[gpu-bench-asm]` deben aparecer `xmad` en vez de `imul`. Mirar la imagen: si sale basura o cuelga, el encode de XMAD está mal → volver a `libvulkan.a.pre-xmad` con `nvk26_all.ps1 -SkipMesa`. 4) Después: fps de la pelea y el reparto de `[gpu-pass]`.
**Hipótesis vigentes**: con XMAD bajan mucho los dumps, los transfers y los texture loads (todos usan multiplicaciones y div/mod enteros). Si no alcanza, pasar la div/mod del SDK a float (`render_target_cache.cpp` ~3200, ~3572, ~4577, ~6170, ~6252).
**Descartado**: B (ocupación), límite de lanzamiento de CTAs, lectura de imágenes.

## gpubench2 revisado, compilado y en staging: falta la corrida con OC (2026-11-05)

**Hecho**:
- Segunda opinión: Codex sin cuota (429). Gemini 3.8 Flash high devolvió un razonamiento cortado; su único hallazgo ("`ifn` no declarado") es falso (`command_processor.cpp:637`). Revisé yo el diff: nada bloqueante. Rangos de buffer OK (dirección máx. de fetch2x_div/fdiv 1.843.199 palabras < 8 MiB; ALU y probes sin escrituras fuera de rango). Push constants 72 B = `Pc`. El divisor de udiv viene por push constant, así que no se pliega a mul-high. `pc.never` evita el DCE. Los contadores de `resident_*` se ponen en cero en la ronda 0 (+1/−1 por warp, el máximo se acumula en las 5 rondas). Barrera TRANSFER→HOST antes del readback. Los formatos de log tienen los argumentos justos. Sin la extensión, `load` (vkGetDeviceProcAddr) da null y no hay captura.
- Build OK (`_logs/kof-gpubench2-build.log`, EXIT=0, sin errores; linkeó el `extern "C"` no weak de `RexSwitchPerfGpuClockHz`). NRO **65.132.741 bytes, SHA `0AF20620B97D8BF5…`**, magic `NRO0`, con strings `[gpu-bench]`, `gpu-bench-asm`, `fetch2x_fdiv`, `resident_` y `pipelineExecutableInfo`. ELF `out/elf-archive/kof2002um_nx-0AF20620.elf`.
- **Staging**: `switch/kof2002um-nx/kof2002um-nx-gpubench2.nro` (TOML `kof2002um_nx.toml` igual). Sin probar en consola.

**Hipótesis vigentes**: A = IMUL/IMAD lento en Maxwell (NAK no usa XMAD). B = ocupación baja o límite de lanzamiento de CTAs (QMD, memoria local, CRS de NVK). Criterios en "Cómo leer" de la sección de abajo.
**Descartado**: nada nuevo. La segunda opinión externa no se obtuvo; se puede repetir con Codex cuando vuelva la cuota (opcional).

**Próximo paso**: el usuario copia `kof2002um-nx-gpubench2.nro` a la SD (`/switch/kof2002um-nx/`, skill `switch-nro-deploy`) y corre con OC (dock, 20 s en el título). Después confirmar `clocks:` en `rex_profile`, grep `\[gpu-bench\]` y aparte `\[gpu-bench-asm\]` (nunca el .log entero), y decidir con "Cómo leer" (sección de abajo) y el párrafo "Después" de la sección "Corrida gpubench CON OC". Si cuelga al arrancar: `vulkan_switch_gpu_microbench = false` en el TOML.

## gpubench2 escrito, sin revisar ni compilar: faltan la segunda opinión, el build y el staging (2026-11-05)

**Hecho**: reescrito `RunSwitchGpuMicrobench` (`tools/rexglue-sdk/src/graphics/vulkan/command_processor.cpp`, ~l. 609–1890). Conserva las 18 operaciones de antes y suma (1280x1440 hilos salvo las sondas; 5 rondas, ahora un batch por ronda en un solo `vkQueueSubmit`):
- `fetch2x_fdiv`: `fetch2x_div` con la división y las multiplicaciones en float (exacto < 2^24): el arreglo candidato del dump.
- `store_1d_{64,128,256,1024}` y `nop_{64,128,256,1024}`: 1D (cada warp escribe 128 B contiguos); loguean ns por grupo y por warp.
- ALU (1D, grupos de 128; guardan solo si el resultado == `pc.never`; neto = mediana − `nop_128`; `ns/step` y `slots/warp-step` al reloj medido con 8 schedulers): `imul_dep` 32 IMAD vs `imul_ind4` 4×8; `ffma_dep` 64 vs `ffma_ind4` 4×16; `iadd_dep`/`iadd_ind4` (xor+add, 2 instr/paso); `umul_high` 32; `udiv_dep` 8 (nir_lower_idiv, d = 80) vs `fdiv_dep` 8 (rcp + resto exacto + 1 corrección).
- `resident_{64,128,1024}`: el lane 0 de cada warp cuenta los warps en vuelo con atómicos (binding 5, buffer de contadores → readback); 128 = ocupación plena.
- Estadísticas de NAK por shader (`[gpu-bench] <shader> NAK: …`) y assembly de store, fetch2x_div, fetch2x_fdiv, nop_128, imul_dep, umul_high, udiv_dep y fdiv_dep (`[gpu-bench-asm]`, 40–200 líneas c/u) vía `VK_KHR_pipeline_executable_properties`.
- 2 líneas `[gpu-bench] summary` con los cocientes que separan A de B, y el reloj de GPU antes/después (clkrst).
**Otros cambios**: `ui/vulkan/vulkan_device.cpp` pide `VK_KHR_pipeline_executable_properties` solo en Switch y activa `pipelineExecutableInfo`; `ui/switch_perf.cpp` suma `RexSwitchPerfGpuClockHz()` (extern "C", clkrst); su declaración está arriba en `command_processor.cpp` (~l. 57). Originales y diff (`gpubench2.diff`) en `cache/_bak_gpubench2/`.

**Próximo paso**: 1) Segunda opinión: `delegar -Rol revision -File "D:/Ports Swtch/kof2002um-nx/cache/_bak_gpubench2/gpubench2.diff"` y evaluar cada hallazgo. 2) Build con el comando de la memoria `kof-build-powershell` (PowerShell, TEMP/TMP de Windows, cwd `D:\Ports Swtch`, en segundo plano, log `_logs/kof-gpubench2-build.log`, leerlo con `logfilter.ps1`); sale `kof2002um-nx/kof2002um-nx.nro`. 3) Verificar el NRO (tamaño, SHA, `NRO0`, strings `[gpu-bench]`, `gpu-bench-asm`, `fetch2x_fdiv`, `resident_`) y copiarlo como `switch/kof2002um-nx/kof2002um-nx-gpubench2.nro` (TOML igual). 4) Corrida del usuario con OC (dock, 20 s de título); confirmar `clocks:` en `rex_profile`; grep `\[gpu-bench\]` y aparte `\[gpu-bench-asm\]` (nunca el .log entero); decidir con el párrafo "Después" de la sección de abajo.

**Cómo leer**: `imul ind4 ≈ dep` y muy por encima de ffma → A (rendimiento de IMUL). `imul ind4 ≈ dep/4`, `ffma dep/ind4 ≥ 2` o `resident_*` muy por debajo de 128 → B (mirar QMD, memoria local y CRS de NVK). `fetch2x_fdiv ≈ fetch2x` → aplicar la división en float en `render_target_cache.cpp`. `nop_128` cerca de 1 ms → límite de lanzamiento de CTAs. `store_1d_128` muy por debajo de `store` (2D) → pesa el patrón de 4×32 B por warp de los grupos 8x16. Las líneas NAK dan GPRs e instrucciones; el asm dice si NAK emite IMUL o XMAD. Peor caso estimado ~1,3 s de GPU al arrancar; si cuelga, `vulkan_switch_gpu_microbench = false` en el TOML.

## El usuario aprobó el gpubench2: falta escribirlo, compilarlo y ponerlo en staging (2026-11-05)

Datos y conclusiones en la sección de abajo ("Corrida gpubench CON OC"). Hipótesis vigentes: **(A)** IMUL/IMAD lentas en Maxwell (NAK usa IMUL/IMAD, no XMAD) o **(B)** poca ocupación (pocos warps residentes por SM, todo queda limitado por latencia). Descartado: el driver o el hardware leyendo imágenes (fetch2x 9,8 GB/s, blit 20), el patrón de memoria del dump (es el mismo en `fetch2x` y `fetch2x_div`) y el reloj (768 MHz confirmado).

**Qué agregar** a `RunSwitchGpuMicrobench` (`tools/rexglue-sdk/src/graphics/vulkan/command_processor.cpp`): shaders GLSL ~l. 853–921, `PushConstants` ~l. 790 (width, height, pitch_tiles=16, tile_w=80, tile_h=16), lambda `dispatch` ~l. 1072, casos de ops ~l. 1139–1159, nombres/etiquetas ~l. 976. Mismo esquema de timestamps, init-only, cvar `vulkan_switch_gpu_microbench`. Todos los tests sobre 1280x1440 hilos, escriben el resultado al SSBO (para que no los elimine el compilador) y usan operandos de push constant (para que no se plieguen como constantes):
1. **Ocupación**: `store` con grupos de 64 (8x8), 128 (8x16, el actual), 256 (16x16) y 1024 (32x32) hilos. Si el tiempo baja mucho con grupos grandes → B (límite de CTAs residentes o de lanzamiento).
2. **Rendimiento contra latencia**: 32 IMUL dependientes (`x = x * pc.k + p.x`) contra 4 cadenas independientes de 8; lo mismo con FFMA (control) y con IADD/shift (control). Si las IMUL independientes tardan lo mismo que las dependientes y mucho más que FFMA → A; si las independientes van mucho más rápido → B.
3. **División**: 8 udiv dependientes por un valor de push constant contra la versión en float (`(float(n)+0.5) * (1/float(d))`, más una corrección hecha en float/selects, sin IMUL), y una cadena de `umulExtended` (mul-high).
4. Opcional, si es barato: `VK_KHR_pipeline_executable_properties` para loguear GPRs, cantidad de instrucciones y SLM de `fetch2x` y `fetch2x_div`.
Loguear también ns por hilo y por operación, o ranuras por warp (a 768 MHz: 2 SM × 4 schedulers). Segunda opinión con el skill `delegar` antes de compilar. Build con PowerShell (memoria `kof-build-powershell`) vía `tools/run-logged.ps1`; verificar el NRO (tamaño, SHA, `NRO0`, strings `[gpu-bench]`) y ponerlo en staging como `switch/kof2002um-nx/kof2002um-nx-gpubench2.nro` (TOML igual).

**Corrida (usuario)**: con OC (en el dock: la vez sin OC la GPU quedó en 460,8 MHz) bastan 20 s de título (el bench corre al arrancar); traer los 3 logs. Confirmar `clocks:` en `rex_profile` y hacer grep de `[gpu-bench]`.

**Después**: si sale A → bajar imul/umul_high a XMAD en NAK para SM50 (`~/mesa-switch/src/nouveau/compiler/nak/sm50.rs`; rebuild de Mesa con `nvk26_all.ps1`), o en el SDK evitar div y multiplicaciones en el dump y la transferencia (`render_target_cache.cpp` ~3200, ~3572, ~4577, ~6170, ~6252). Si sale B → mirar en NVK el QMD de Maxwell y el tamaño de memoria local/CRS (`SET_SHADER_LOCAL_MEMORY_*`), que limitan los warps residentes. Si la división en float va mucho más rápido → aplicarla en esas líneas del SDK.

## Corrida gpubench CON OC: 11,8 fps; el bench culpa a la aritmética entera de los shaders (div/mod), no a la lectura de imágenes (2026-11-05)

**Corrida** (NRO gpubench `6F2FF697`; logs `kof2002um_nx_001 (13).log`, `rex_profile (16).log`, `rex_stderr (16).log` en `%TEMP%`): sin crash. **GPU a 768 MHz toda la corrida** (`clocks:` de `rex_profile`; CPU 1785, memoria 1600). Título 28 fps, menús 13–17 (el usuario los nota más fluidos), **pelea 11,7–11,8 fps**, GPU **84 ms/frame** (lo que se esperaba), GPU ocupada 98 %. Reparto: EDRAM dumps 27 | texture loads 23 | RT transfers 17,5 | draws 10 | resolve copies 3,9 | uploads 1,2.

**`[gpu-bench]` a 768 MHz** (mediana): fill 22,3 GB/s, copy_buffer 21, clears 21–53, blit 20, resolve_2x 21, copy_image 20 (los caminos fijos del driver van casi al pico de 25,6); buffer↔imagen RGBA8 5,8–5,9; buffer→R8 640x448 0,40 ms (1,4 GB/s); **fetch1x 0,77 ms (9,7 GB/s), fetch2x 1,52 ms (9,8 GB/s)**, depth4x 1,28 ms (5,8), store 1,29 ms (5,7), copy32 1,39 ms (10,6), copy128 1,26 ms (11,9); **`fetch2x_div` 14,49 ms (1,02 GB/s): 9,5× más lento que `fetch2x`** y al mismo ritmo que el dump del juego.
- `fetch2x_div` solo agrega 2 pares div/mod por valores de push constant (tile_w=80, tile_h=16) y 3 multiplicaciones; el patrón de escritura es el mismo que `fetch2x` (4 tramos de 32 B por warp). O sea, la diferencia es **ALU**: ~1.400 ranuras de emisión extra por warp (~700 por par div/mod).
- **Decisión según la regla**: el driver y el hardware leen bien las imágenes (fetch2x, blit, image_to_buffer van rápido) → **el culpable es el shader**: la división/módulo enteros (y las multiplicaciones) como las compila NAK para Maxwell. NAK usa `nir_opt_idiv_const` (divisor constante → mul-high + shift) y `nir_lower_idiv` (divisor en tiempo de ejecución: 2 imul + 2 umul_high + rcp + conversiones por división), y en SM50 emite las **IMUL/IMAD viejas** (`OpIMul`/`OpIMad` en `nak/sm50.rs`; no hay XMAD en NAK; ptxas y nouveau codegen usan XMAD en Maxwell porque IMUL/IMAD son lentas).
- En el SDK (`vulkan/render_target_cache.cpp`): div/mod por el **pitch en tiempo de ejecución** en la transferencia (~l. 3572), en el depth de la transferencia (~4577) y en el dump (~6252); div/mod por constantes (ancho/alto de tile) en ~3200 y ~6170. Los `texture_load_*` sin escala no tienen div (sí los `_scaled` y los DXT/CTX1, por /3 y /7 constantes).
- Ojo: hasta un shader trivial (`store`, 1,29 ms) va 4× más lento que un fill (0,33 ms) y copy32/copy128 llegan a la mitad del copy engine, con 4× menos hilos en copy128 y el mismo tiempo. Dos hipótesis, sin separar todavía: **(A)** IMUL/IMAD lentas en Maxwell (arreglo en NAK: bajar imul/umul_high a XMAD, o en el SDK: evitar div y multiplicaciones), **(B)** poca ocupación (pocos CTAs/warps residentes por SM, todo queda limitado por latencia; mirar el QMD/memoria local/CRS de NVK para Maxwell). La B explicaría que todos los compute shaders vayan lentos.

**Próximo paso (propuesto)**: build **gpubench2**, solo bench al arrancar, para separar A de B: `store` con grupos de 64/128/256/1024 hilos; cadenas de 32 IMUL dependientes contra 4 cadenas independientes; la misma cuenta con FFMA (control); cadena de udiv por push constant contra división en float (rcp + corrección). Con eso se decide entre reescribir la div/mod del dump y la transferencia en el SDK (división en float, exacta con corrección; sin IMUL) y arreglar NAK (XMAD) o NVK (ocupación), que también aceleraría los texture loads.

## Corrida nvk26 SIN OC (7 fps) + build gpubench (micro-benchmark de GPU) en curso (2026-11-05)

**Corrida** (NRO nvk26 `B36BB879`; logs `kof2002um_nx_001 (12).log`, `rex_profile (15).log`, `rex_stderr (15).log` en `%TEMP%`): sin crash, NVK nuevo confirmado (`[nvk] c186 … NVK_SWITCH_DRAW`, ZCULL). **La GPU estuvo a 460,8 MHz toda la corrida (sin OC)**; la corrida 14 iba a 768. Por eso "se siente igual" (el usuario estimó ~9 fps).
- Pelea **7,0 fps**, GPU **140 ms/frame** (calibrado; el `[gpu-ts]` crudo dice 86 ms porque toma 1 ns/tick: hay que ×1,6276), GPU ocupada 98,6 % → sigue mandando la GPU. Reparto: EDRAM dumps 45,3 | texture loads 40,0 | RT transfers 28,7 | draws 17,5 | resolve copies 6,3 | uploads 1,6.
- Mismo reloj con el NVK viejo (corrida depthclear sin OC): 208 ms (4,6 fps) → **el NVK nuevo rinde 1,5× por MHz**; con OC se esperan ~84 ms ≈ 12 fps. Por MHz mejoraron draws (×1,7) y resolve copies (×2,7); dumps igual; texture loads algo peor.
- Título: 20 fps, 46 ms/frame: 1 transfer D0→C0 16,3 ms, 1 dump 1280x720 2xMSAA 20,1 ms, resolve copy 3,1, texture load 4,4.
- Ritmos: todo lo que lee un RT en un shader (dumps ~0,35 GB/s ≈ 92 M hilos/s sea 1x o 2xMSAA; transfers ~0,43 GB/s) o carga texturas (~0,3 Gtexel/s) va 10–30× por debajo de lo esperable; resolve copies (buffer→buffer) ~3,4 GB/s.
- Main XThread 100 % (probable espera activa); GPU Commands espera fences 781 ms/s (35 submits/frame, una espera por submit); `channel 2 hit native submission pressure` ×351 (una submission de 1216 entradas GPFIFO / 132 KB).

**Descartado (leyendo `~/mesa-switch` en WSL)**: memoria GPU-uncached (solo query pools y event heap; el resto mapea con `IsCacheable`); páginas chicas (usa big pages); sector promotion (PROMOTE_TO_2_V en block-linear); copy engine (las copias van por vk_meta, `NVK_COPY_ENGINE` off). Opciones del parche ME: `NVK_SWITCH_DRAW` y `NVK_SWITCH_DYN_UBO_DELTA` on por defecto; **`NVK_SWITCH_FRAG_BARRIER`** (PIXEL_SHADER_BARRIER en vez de WFI entre pasadas de fragment) está **off**: candidato barato a probar; `NVK_SWITCH_STAGE_CACHE` es solo CPU. Mass Effect NX usa un renderer nativo propio (`masseffect_native_*`), no el camino EDRAM genérico.

**Build gpubench** (`command_processor.cpp`, solo Switch, cvar `vulkan_switch_gpu_microbench` default true, init-only): `RunSwitchGpuMicrobench` al final de `SetupContext`, 18 operaciones × 5 rondas con timestamps y barrera global antes/después: fill/copy de buffer 7 MB, clears 1x/2x/depth 4x, buffer↔imagen RGBA8 1280x720, buffer→R8 640x448, blit, resolve 2x→1x, copy image, y shaders GLSL (glslang del SDK, grupos 8x16 como el dump, `SAMPLED_IMAGE` + `GL_EXT_samplerless_texture_functions` como el RT cache): `fetch1x`, `fetch2x`, `fetch2x_div` (dirección estilo EDRAM con div/mod), `depth4x`, `store`, `copy32`, `copy128`. Log: `[gpu-bench] <op> min/mediana ms, GB/s`. Revisión: Gemini 3.8 Flash (Codex sin cuota) → descriptores a SAMPLED_IMAGE; además arreglé `vkGetQueryPoolResults` con WAIT sobre queries nunca escritas (colgaba si faltaba una operación): ahora lee solo las grabadas.
- 1.ª build (`_logs/kof-gpubench-build.log`) sin esos arreglos. **2.ª build OK** (`_logs/kof-gpubench-build2.log`, 0 errores, solo warnings viejos): NRO **65.075.397 bytes, SHA `6F2FF69729D2DB6C…`**, magic `NRO0`, ELF con `[gpu-bench]`, `GL_EXT_samplerless_texture_functions` y "Mesa 26.2.3". ELF `out/elf-archive/kof2002um_nx-6F2FF697.elf`; **staging `switch/kof2002um-nx/kof2002um-nx-gpubench.nro`** (TOML igual). Sin probar en consola.

**Cómo leer el bench**: si `fetch2x_div` da ~1 ms (≈15 GB/s) → el culpable es el dump del SDK (20 ms para lo mismo): su shader o estado → comparar/reescribir el dump en GLSL. Si `fetch2x`, `image_to_buffer` y `blit` también van a ~0,4 GB/s → es el driver/hardware leyendo imágenes (mirar PTE kinds, `NVK_DEBUG=no_compression`, reloj real).

**Próximo paso (usuario)**: **con OC (GPU 768 MHz; revisar que sys-clk lo aplique)**, copiar `kof2002um-nx-gpubench.nro` como de costumbre, borrar `logs/`, 20 s de título + 1–2 min de pelea, traer logs. Después: grep `[gpu-bench]` en el log principal (sale al arrancar) y leerlo con la regla de arriba; fps/`[gpu-pass]` con OC (esperado ~12 fps). Si arranca colgado o crashea al inicio → el bench (apagar con `vulkan_switch_gpu_microbench = false` en el TOML).

## Wrapper de rustc parcheado (crates del host sin `--target`); 3.ª build lanzada (2026-11-05)

- El arreglo de abajo (solo `--crate-type proc-macro`) **no alcanzaba**: en `builddir-switch/build.ninja` la regla del host es `rust_COMPILER_FOR_BUILD` = `rustc -C linker=cc …` (el `rustc` del PATH = el wrapper, porque PATH empieza con `/usr/local/libexec`), y la usan 7 crates: los proc-macros `nak_ir_proc` y `paste` **y sus rlib** `syn`, `quote`, `proc-macro2`, `unicode-ident`, `compiler_proc`. Si estos últimos salen AArch64, el proc-macro x86-64 no los puede usar.
- **Arreglo** (en el paso python de `build_mesa.sh`, idempotente, backup `rustc-switch-wrapper.sh.orig`): en el bucle de args del wrapper, `linker=*|-Clinker=*` sin `aarch64-none-elf` → `RUST_TARGET=` (cae al fallback nativo con `cc`). La regla cross (`rust_COMPILER`) pasa `-C linker=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc` y sigue con `--target`.
- **Probado** en la imagen `devkitpro-mesa-rust` con un proc-macro mínimo + dep rlib + staticlib (script en el scratchpad de la sesión, `wrap_test.sh`): `libpm.so` x86-64, `libt.a` 431 miembros AArch64 / 0 x86-64, `kof_answer` presente. `~/mesa-switch/rustc-switch-wrapper.sh` ya está parcheado.
- 3.ª corrida de `nvk26_all.ps1` lanzada en segundo plano, directo (`*> _logs/nvk26_all.log`; detalle en `_logs/mesa-build2.log`, `kof-nvk26-config.log`, `kof-nvk26-build.log`). Por `run-logged.ps1` no arrancó: `cmd /c` con las comillas anidadas de un `powershell -File "…"` no corrió nada (`exit  en 0s`, sin log).
- **Mesa OK** (~4 min, 13:05–13:09 UTC): `tools/mesa-switch-sdk/lib/libvulkan.a` 121.106.000 bytes, **1998 miembros AArch64, 0 x86-64**, "Mesa 26.2.3 (git-d4a00ea0ab)", `.sha256` y `SOURCE.txt` escritos. Pero `build_mesa.sh` salió con **141**: el `strings | grep -m1 "Mesa 26"` final da SIGPIPE y con `pipefail` cuenta como falla → el ps1 cortó antes de KOF. Arreglado (`{ strings … || true; } | grep -m1`).
- `nvk26_all.ps1` tiene ahora `-SkipMesa` (solo reconfigura config5 + relinkea KOF + staging).
- **KOF linkeado contra el NVK nuevo**: OK, sin warnings ni undefined en `_logs/kof-nvk26-build.log` (el std de Rust `aarch64-unknown-linux-gnu` no pidió nada de glibc). NRO **65.046.725 bytes, SHA256 `B36BB879B75C5DEAD1BF39DEA61F792C1E04C393B2E8180CE80AF06F4353D8B1`**, magic `NRO0`, ELF con "Mesa 26.2.3 (git-d4a00ea0ab)". Staging: `switch/kof2002um-nx/kof2002um-nx-nvk26.nro`; ELF en `out/elf-archive/kof2002um_nx-B36BB879.elf`. `out/switch-config5` queda apuntando a `tools/mesa-switch-sdk` (para volver al viejo: comando en el encabezado de `nvk26_all.ps1`).

**Próximo paso (usuario, en consola con OC)**: copiar `kof2002um-nx-nvk26.nro` como de costumbre, borrar `logs/`, 20 s de título + 1–2 min de pelea, traer `logs/` y comparar con **9,4 fps / GPU ~100 ms**. Si arranca mal (crash/negro) → `x360-crash-triage` sobre los logs; sospechar primero del WSI nuevo (`vkCreateViSurfaceNN`) o de los stubs de `rust_switch_stubs.c`. Pendiente aparte: el TOML de Mass Effect NX usa `NVK_SWITCH_STAGE_CACHE=1` (`masseffect_nvk_cache_per_stage`); probarlo en KOF después de medir sin él.

## 2.ª build de Mesa: falla el proc-macro de NAK por el `--target` (2026-11-05 ~04:30)

- `nvk26_all.ps1` (corrido por mí) llegó a ninja `[889/1234]` y paró: `FAILED: src/nouveau/compiler/libnak_ir_proc.so` → `rust-lld: error: --fix-cortex-a53-843419 is only supported on AArch64` (`linking with cc failed`). Log: `_logs/mesa-build2.log`. KOF no se tocó: config5 sigue en el SDK viejo.
- **Causa**: con `MESA_SWITCH_RUST_TARGET` puesto, el wrapper agrega `--target=aarch64-unknown-linux-gnu` a **todo** rustc, también a `nak_ir_proc`, que es un **proc-macro** (se ejecuta dentro del compilador, tiene que ser del host x86-64 y se linkea con el `cc` del host).
- **Arreglo propuesto** (sin escribir): en el paso python de `build_mesa.sh`, parchear `~/mesa-switch/rustc-switch-wrapper.sh` para que, en el bucle que ya existe sobre los args (`sanity_check_for_rust.rs`), también deje `RUST_TARGET=` vacío si el crate es proc-macro (`--crate-type` seguido de `proc-macro`, o `--crate-type=proc-macro`). Lo copia al contenedor el primer `run` de `build-switch.sh`, así que con editar el archivo de `~/mesa-switch` alcanza. Revisar también que no haya otros crates del host (build scripts) con el mismo problema.

**Próximo paso**: escribir ese parche del wrapper, volver a correr `powershell -File "D:\Ports Swtch\kof2002um-nx\tools\mesa-switch-sdk\scripts\nvk26_all.ps1"` en segundo plano y seguir con lo de la sección de abajo (staging, medir en consola contra 9,4 fps).

## 2.ª build del NVK nuevo lanzada (`nvk26_all.ps1`, en segundo plano desde el chat "Compilar NVK nuevo") (2026-11-05 ~04:00)

- La 1.ª corrida del usuario de `nvk26_all.ps1` murió enseguida: `sed: unterminated 's' command` (en `build_mesa.sh`, la edición de `Docker.rust` tenía `\\/` → sed veía `\/`). Corregido: las dos ediciones (Rust target en los bloques cross de `build-switch.sh` y `rustup target add aarch64-unknown-linux-gnu` en `Docker.rust`) ahora son reemplazos de texto exacto en python dentro de `build_mesa.sh`. `bash -n` OK.
- El usuario pidió que lo corra yo: `nvk26_all.ps1` corriendo (salida en `_logs/nvk26_all.log`; detalle en `_logs/mesa-build2.log`, `kof-nvk26-config.log`, `kof-nvk26-build.log`). Tarda ~20–30 min.

**Próximo paso**: mirar el final de `_logs/nvk26_all.log` (si no terminó, `mesa-build2.log` muestra por dónde va). Si dice `OK: NRO …` → el NRO está en `switch/kof2002um-nx/kof2002um-nx-nvk26.nro` y config5 apunta al SDK nuevo: anotar SHA/tamaño acá y pedir la corrida en consola con OC (título 20 s + 1–2 min de pelea, traer `logs/`, comparar con 9,4 fps). Si falla: en Mesa, ver el conteo de arquitecturas (`== member architectures`) y el error; en el link de KOF, `logfilter` del build log (riesgo: símbolos de glibc del std `aarch64-unknown-linux-gnu`; ver cómo lo resuelve `build-unified.sh`). Si queda roto, volver config5 al SDK viejo (comando en el encabezado de `nvk26_all.ps1`).

## NVK nuevo: la 1.ª build salió con NAK en x86-64; arreglo escrito, la 2.ª build la tiene que lanzar el usuario (clasificador) (2026-11-05 03:40)

- **1.ª build** (`_logs/mesa-build.log`, ~20 min): OK, "Mesa 26.2.3 (git-d4a00ea0ab)", 118 MB, devkitA64 del contenedor GCC 15.2.0, rustc `1.100.0-nightly (5ceaf6608 2026-09-25)`. **El link de KOF falla** (`_logs/kof-nvk26-build.log`): `libvulkan.a(compiler_builtins-…rcgu.o): Relocations in generic ELF (EM: 62)` / "file in wrong format". `readelf`: `libnak_rs.a` 356 de 357 miembros x86-64; `libvulkan.a` 697 x86-64 + 656 AArch64.
- **Causa**: `rustc-switch-wrapper.sh` (upstream, no el parche) solo agrega `--target` si existe `MESA_SWITCH_RUST_TARGET`; lo exporta `build-unified.sh` (MSYS2: `aarch64-unknown-linux-gnu`), **`build-switch.sh` no** → en Docker cae al "native sanity fallback" y compila para el host. A Mass Effect le anduvo porque compiló en un Mac arm64 (host = aarch64).
- **Arreglo** (en `tools/mesa-switch-sdk/scripts/build_mesa.sh`, reescrito): `export MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu` en los dos bloques cross de `build-switch.sh` (configure y ninja, no el nativo); `rustup target add aarch64-unknown-linux-gnu` en la imagen (`docker commit`) y en `Docker.rust`; tras el merge, **falla si queda algún miembro x86-64** (`readelf -h`). Bindgen ya usaba `--target=aarch64-none-elf`.
- **Bloqueado**: el clasificador de permisos de auto mode rechazó correr la 2.ª build ("Code from External", por la descarga de rustup); no se esquivó. Pendiente del usuario.
- Estado: `out/switch-config5` **volvió al SDK viejo** (`tools/nvk-switch-sdk`, se puede compilar como antes). El lib roto quedó como `tools/mesa-switch-sdk/lib/libvulkan.a.broken-x86` (borrable).

**Próximo paso (usuario)**: correr `powershell -File "D:\Ports Swtch\kof2002um-nx\tools\mesa-switch-sdk\scripts\nvk26_all.ps1"`. Hace la build de Mesa (~20–30 min), reconfigura config5 al SDK nuevo, relinkea KOF, chequea `NRO0` + "Mesa 26" en el ELF, archiva el ELF y deja `switch/kof2002um-nx/kof2002um-nx-nvk26.nro` en staging. Después, en consola **con OC**: copiar ese NRO, borrar `logs/`, 20 s de título y 1–2 min de pelea, traer `logs/`, comparar con 9,4 fps / GPU ~100 ms. Si falla el link por símbolos de glibc del std de Rust (`aarch64-unknown-linux-gnu`), mirar cómo lo resuelve el SDK de MSYS2 de mesa-switch (`build-unified.sh`).

## Compilando el NVK nuevo (mesa-switch 26.2.3 + parche Mass Effect NX) en WSL con podman (2026-11-05 03:10)

- Podman 5.7.0 instalado en WSL (Ubuntu 26.04) como root; git/make ya estaban. Shim `~/.local/bin/docker` → podman y `~/.config/containers/registries.conf` (docker.io, short-name permissive). Aviso "/ is not a shared mount" de podman rootless: por ahora inofensivo.
- `~/mesa-switch` = danfromtico/mesa-switch `d4a00ea0` + `git apply ~/me-nx/mesa/mesa-switch-masseffect.patch` (18 archivos M, OK). Parche y `build_mesa_docker.sh` bajados por blob sha (`18e2ab64`, `4a2fe6c6`). El clasificador bloqueó correr `build_mesa_docker.sh` de ME → sus pasos se hacen a mano con `tools/mesa-switch-sdk/scripts/build_mesa.sh` (también `clone_patch.sh`, `setup_shim.sh`, `install_podman.sh`; correr con `wsl -- bash /mnt/d/...`): `build-switch.sh` con `-Doptimization=2`, `Docker.rust` con `nightly-2026-09-26` (la de la build de referencia de ME), merge `libvulkan.a + libnak_rs.a` con `ar -M`, salida en `kof2002um-nx/tools/mesa-switch-sdk/lib/libvulkan.a` + `SOURCE.txt`. Log: `_logs/mesa-build.log`.
- Verificado: el WSI nuevo implementa `vkCreateViSurfaceNN` con `NWindow*` (lo que pasa `vulkan_presenter.cpp:1194`). El SDK de KOF solo usa `<sdk>/lib/libvulkan.a` (los headers vienen de portlibs), y ningún código del runtime usa `nvk_switch_draw`/`set4` (solo el comentario en `rexglue_switch.cmake`).
- devkitA64 local: GCC 16.1.0.
- La carpeta de Mass Effect NX del usuario (`D:\Descargas\roms switch\Mass Effect ...\switch\masseffect-nx`) solo trae NRO/TOML/shaders; su TOML pone `masseffect_nvk_cache_per_stage = true` (= `NVK_SWITCH_STAGE_CACHE=1`).

**Próximo paso**: al terminar la build, reconfigurar `out/switch-config5` con `-DREXGLUE_SWITCH_NVK_SDK=D:/Ports Swtch/kof2002um-nx/tools/mesa-switch-sdk` (solo relinkea; para volver, el valor viejo es `.../tools/nvk-switch-sdk`), compilar, `strings | grep "Mesa 26"` en el ELF, staging y medir en consola contra los 9,4 fps.

## Corrida resolvestats + causa de fondo: el NVK que linkeamos es viejo (Mesa 25.0.7, jul-2026); plan: pasar a mesa-switch 26.2.3 (2026-11-05)

**Corrida** (build resolvestats `6AE71E6B`; logs `kof2002um_nx_001 (11).log`, `rex_profile (14).log`, `rex_stderr (14).log` en `%TEMP%`): sin crash ni errores gráficos. Pelea **9,4 fps con OC** (CPU 1785 / GPU 768 / mem 1600), GPU ~100 ms/frame: draws 17,7 | RT transfers 22,9 | EDRAM dumps 27,3 | resolve copies 10,3 | texture loads 20,4 | uploads 1,2.
- `[resolve]` en pelea: 9/frame = 1× `C0 b0 p16 m1` 1280x720 (2xMSAA) → `1F6F8000` (sh 6, la imagen final) + 8× `C0 b1440 p8 m0` 640x448 → `186D2000` ×4, `185BA000` ×2, `1F52B000` ×2. Dumps 15,8 MB/frame, copied out 12,3 MB/frame. **repeated 0 %** → un cache de volcados no sirve (descartado).
- Texturas: 72 cargas/frame (12,2 Mtexel), de resolves solo 9 (3,2 Mtexel). **La 8bpp 640x448 `17455000` (fmt 2) y su paleta 256x1 `179A3000` se recargan 30,6 veces/frame** (8,8 Mtexel, el grueso de texture loads): la CPU la reescribe entre draws (sprites con CLUT). Idea para después: carga parcial (solo las páginas sucias).
- Ritmo efectivo: dumps ~0,6 GB/s, copias ~1,2 GB/s (la X1 da ~25 GB/s). No es el algoritmo, es el driver.

**Causa de fondo**: `tools/nvk-switch-sdk/lib/libvulkan.a` (el `REXGLUE_SWITCH_NVK_SDK` de todos los `out/switch-config*`) es el prebuilt de jhuz0/nvk-switch del 25-07-2026, **Mesa 25.0.7** (sin las strings `gpu-cached`/`zcull`). Después vino todo el trabajo de rendimiento de NVK en Tegra:
- nxvk (PalindromicBreadLoaf, Mesa 26.2.2, sep-2026): "split CPU and GPU cacheability" (en nxvk, memoria `HOST_COHERENT` ⇒ `NVKMD_MEM_GPU_UNCACHED` ⇒ `MapBufferEx` sin `IsCacheable` = GPU sin L2), zcull, compresión, UBOs como constant buffers en pre-Turing, copias en copy engine, sector promotion block-linear.
- **danfromtico/mesa-switch `main` (Mesa 26.2.3)**: base de Mass Effect NX (25 fps stock) y NFS MW NX (30 fps stock). Ya trae del parche de NFS (StevensND) ZCULL, fix SM50 `FADD32I.SAT`, UBO por cbuf, memoria CPU-write uncached.
- Mass Effect NX (`nebadasSwifty/masseffect-nx`): `mesa/mesa-switch-masseffect.patch` sobre mesa-switch `d4a00ea0ab3f59afb967cc5d779e4263d237bd77` (NAK: latencias 200 y `peephole_select` 8; dead color outputs; faster draws `nvk_switch_draw`; set 4 por diferencias; submit tail reservation = no más device lost; WSI recovery). Build probado: `mesa/build_mesa_docker.sh` (Docker, -O2 = +5 %). MSYS2 sin probar. Docs útiles: `mesa/README.md`, `docs/mesa.md`, `docs/nvk-per-draw.md`.
- El SDK de ReXGlue ya conoce esos contratos (`cmake/rexglue_switch.cmake`: `patch_nvk_set4`, `nvk_switch_draw`; linkear por ruta completa, no el libvulkan.a de portlibs). El runtime usa `VK_NN_vi_surface`.

**Autorizado por el usuario (2026-11-05)**: a la pregunta "¿Instalo podman y git en tu Ubuntu de WSL como root para compilar el NVK nuevo?" respondió "Sí, instalalo". Comando previsto: `wsl -u root -- apt-get install -y podman git make` (nada instalado todavía). Fuentes ya bajadas (pueden perderse) en el scratchpad del chat anterior: `nxvk/`, `me/` (docs de Mass Effect), `nfs/`.

**Próximo paso**: instalar lo de arriba; clonar mesa-switch en WSL (`~/mesa-switch`, checkout `d4a00ea0`), `git apply` del parche de Mass Effect y correr su `mesa/build_mesa_docker.sh` (con podman como docker) en (Ubuntu 26.04, 12 núcleos: **no hay podman/docker/git/make**; el usuario `tebi` no tiene sudo sin contraseña → instalar con `wsl -u root` o que lo haga el usuario), armar `tools/mesa-switch-sdk/` (lib + include, con libnak_rs fusionado como hace `build_mesa_docker.sh`), build dir nuevo con `REXGLUE_SWITCH_NVK_SDK` ahí, compilar, verificar `Mesa 26` en el ELF y medir en consola.

## Build resolvestats (`[resolve]`: qué copia cada resolve, cuánto vuelca, si el volcado se repite): compilado y en staging; falta la corrida (2026-11-05 02:23)

**Por qué**: con OC la pelea va a ~10 fps y la cadena resolve parece ser más de la mitad del frame: EDRAM dumps ~27 ms + resolve copies ~10 ms + parte de texture loads ~16 ms. `rex_profile` (pelea): **~9 resolves/frame (3,5 pantallas de área), 72 texturas cargadas/frame, 33 submits/frame, 5,8 MB de shared memory subida/frame**.
**Dato**: `direct_host_resolve` (default true) es un stub: `TryResolveCopyDirectly` termina llamando a `DumpRenderTargets`, así que siempre se vuelca.

**Cambio** (solo Switch, solo log): `render_target_cache.cpp` + `texture_cache.cpp`.
- `ResolveStatsBegin` (en `Resolve`): firma = RT origen (C/D fmt b p m, `64bpp`), rect WxH @offset, `copy_dest_info` (fmt), dest pitch x alto y base, `RB_COPY_CONTROL`; además, el índice del shader de copia.
- `DumpRenderTargets`: por rect, tiles (5120 B c/u), dispatches, RT leído y **"repeated"**: el mismo RT con el mismo span ya se había volcado, sin nada dibujado/transferido/limpiado en él desde entonces (`ResolveStatsRtChanged` en `Update` para los 5 RTs acumulados y en los clears de `Resolve`) y sin que otro volcado pisara esa parte del EDRAM buffer. El host depth store (transfers) borra todo el seguimiento (`clobbered`).
- `RexResolveStatsTextureLoad` (en `LoadTextureDataFromResidentMemoryImpl`, solo base): cuenta las cargas y marca las que leen un rango que escribió alguno de los últimos 32 resolves.
- Cada 4 s: `[resolve] … resolves/frame, dumps MB/frame, repeated %, copied out MB/frame, texture loads/frame, from resolve dests/frame`; hasta 12 líneas por firma (ordenadas por tiles) y `[resolve-tex]`, las 10 texturas con más texels cargados (`from a resolve` si corresponde).

**Build OK** (log `_logs/kof-resolvestats-build-20261105.log`, 0 errores, solo warnings viejos). NRO `kof2002um-nx.nro` 61.978.821 bytes, SHA `6AE71E6B0801…`, magic `NRO0`, strings `[resolve-tex]` y `EDRAM buffer clobbered` presentes. ELF `out/elf-archive/kof2002um_nx-6AE71E6B.elf`. Staging `switch/kof2002um-nx/kof2002um-nx-resolvestats.nro` (TOML igual). Sin revisión de otro modelo (se cortó por contexto; opcional: `delegar -Rol revision` sobre los hunks `[resolve]`).

**Pendiente (usuario, MTP)**: **con OC (768 MHz)**, copiar `kof2002um-nx-resolvestats.nro` a `sdmc:/switch/kof2002um-nx/`, borrar `logs/`, 20 s de título y 1–2 min de pelea **sin L+R+ZL**, traer `logs/`.

**Qué mirar**
- `repeated %` alto → un cache de volcados (saltear el dump si el RT no cambió) es una ganancia directa sobre los ~27 ms.
- Firmas de resolve con el mismo origen y rect varias veces por frame, o dumps que leen RTs que no son el origen (ownership).
- `from resolve dests/frame` contra `texture loads/frame`: si casi todas las cargas vienen de resolves, el siguiente paso es resolver directo a la textura (o no invalidarla); si vienen de la CPU (sprites subidos cada frame), apuntar a las cargas o a las subidas.
- `sh` 0/1 son las copias rápidas de 32bpp (1x/2x y 4x); 4–8 son las completas (más caras).

## Corrida depthclear: el salto del clear de depth anda (transfers −77 %); pelea ~10 fps con OC; ahora pesan los resolves (2026-11-05)

**Corrida** (build depthclear `0E928D2A`; logs `kof2002um_nx_001 (10).log`, `rex_profile (13).log`, `rex_stderr (13).log` en `%TEMP%`). ~3 min, sin crash, sin errores nuevos.
- **Título 20 fps** (24 ms/frame GPU). **Pelea con OC: 9–10 fps**, GPU 92–110 ms/frame, ~95 % ocupada (es la GPU, no la CPU). Reparto con OC: EDRAM dumps ~27, RT transfers ~22, draws ~20, texture loads ~16, resolve copies ~10.
- **Sin OC** (desde 03:35:25): todo ×2,1 → 4,6 fps, ~208 ms/frame.
- **`overwritten depth: skipped 54 transfers`** cada 4 s: la C0@0→D0@0 desapareció. RT transfers sin OC bajó de ~201 a ~45 ms/frame. Sin artefactos reportados en modo normal.
- **El "bug gráfico" fue la rotación A/B** (L+R+ZL a las 03:36:14; menú shortcut). Medidas sin OC: no draws 161 ms (5,9 fps), no transfers 161 ms, **no resolves 125 ms (7,3 fps; dumps+copias ≈ 83 ms)**, después "no texture loads". El log termina en medio de la rotación.
- Transfers que quedan: D0@0→C0@0 (63 % de los tiles, la que lleva el clear al color) y las cuatro chicas de @1440/@1664 (9 % c/u).

**Próximo paso (a decidir)**: (a) reemplazar D0@0→C0@0 por un clear del color (la "siguiente idea" de abajo; ~−13 ms con OC), o (b) medir los resolves por frame (cuántos, rect, RT origen, si el dump cubre todo el RT) para ver por qué los dumps cuestan 27 ms con OC.

## Corrida fiberstack: sin crash; ~3 fps porque la GPU iba a 460 MHz; la transfer C0→D0 es un clear que sobra → build depthclear (2026-11-05)

**Corrida** (build fiberstack `8DDED8CF`; logs `kof2002um_nx_001 (9).log`, `rex_profile (12).log`, `rex_stderr (12).log` en `%TEMP%`). ~6 min, **sin crash**: `CreateFiber … stack=0x40000` confirmado (fix de la pila OK).
- **2–3 fps**: el clock de la GPU fue **460,8 MHz** toda la corrida (antes, con OC, 768). Con eso la GPU tarda ~300 ms/frame (`[gpu-pass]`): RT transfers ~201 ms (65–68 %), EDRAM dumps ~45 ms (15 %), draws ~20–30 ms. Mismo reparto que antes, solo más lento.
- **Los glitches y el freeze son la rotación A/B** (L+R+ZL a las 03:24:44): "no draws" congela la imagen, "no transfers" deja estelas de sprites (screenshot: el color ya no se limpia porque el clear llega por la transfer D0→C0). En "no transfers" sube a **8,6 fps** contra 3,2.
- **Salto de stencil inútil**: `guest enabled stencil (RB_DEPTHCONTROL 00008777)` en el primer segundo, así que nunca se salteó nada. Ese draw es el propio clear.

**Qué disparan las transfers grandes** (`[rt-xfer-draw]`):
- `→ D0 b0 p16 m2` (la de C0@0→D0@0): rect list de 3 vértices, `mode 5` (kDepthOnly, sin pixel shader), z always + write, stencil always/REPLACE (dc `00008777`), sin blend. **Es el clear de EDRAM del juego**: limpia el color 2x en C0@0 dibujando sobre un depth 4x en la misma base. Copiar C0 a D0 antes (depth + clear de stencil + 8 pasadas de bit) es trabajo tirado.
- `→ C0 b0 p16 m1` (D0@0→C0@0): quad list normal, sin z. **Hace falta**: lleva el valor del clear al color.
- @1440/@1664: clears modo 4 de 3 vértices con stencil off (dc `00000076`), y draws 640x448 con blend. Son chicos (224 tiles c/u).

**Build OK** (log `_logs/kof-depthclear-build-20261105.log`): NRO 61.950.149 bytes, SHA `0E928D2A636F…`, magic `NRO0`, strings nuevos presentes. ELF `out/elf-archive/kof2002um_nx-0E928D2A.elf`. Staging `switch/kof2002um-nx/kof2002um-nx-depthclear.nro` (TOML igual).

**Cambio**: `render_target_cache.cpp`, solo Switch, cvar `vulkan_skip_overwritten_depth_transfers` (default true). `RtXferIsDepthOverwriteDraw` (en `Update`, con `normalized_depth_control`): kDepthOnly + rect list + z always/write + (stencil always/REPLACE con write mask 0xFF, también BF si backface; o stencil off y el guest nunca usó stencil). Si da sí, `PerformTransfersAndResolveClears` descarta las transfers del índice 0 (depth). El resumen `[rt-xfer]` suma `overwritten depth: skipped N transfers, T tiles (total)`. Riesgo: si el rect del clear no cubre todo el rango del que toma ownership, queda depth viejo del host en esa parte, y la transfer D0→C0 lo pasaría al color (se vería como basura en bordes). Si pasa: `vulkan_skip_overwritten_depth_transfers = false` arriba de `[log]`.

**Siguiente idea**: la D0→C0 también se puede reemplazar por un clear del color con el valor del clear reinterpretado (D24S8 → 2x color), o directamente reconocer el patrón y hacer `vkCmdClearAttachments`.

**Pendiente (usuario)**: ¡poner el OC de la GPU (768 MHz)! Copiar `kof2002um-nx-depthclear.nro`, borrar `logs/`, 20 s de título + pelea 1–2 min, **sin L+R+ZL**, traer `logs/`.

## Build fiberstack (pila de fibras 256 KB + salto de stencil + `[rt-xfer-draw]`): compilado y en staging; falta la corrida (2026-11-05 01:46)

**Build OK** desde PowerShell con la TEMP de Windows (log `_logs/kof-fiberstack-build2-20261105.log`). Ojo: el cmake de msys2 **no acepta `--build "D:\..."` absoluto** (lo pega a `/d/Ports Swtch/`): usar la ruta relativa desde `D:\Ports Swtch` (`kof2002um-nx/out/switch-config5`). Solo se recompiló `threading.cpp`; `render_target_cache.cpp` y `command_processor.cpp` ya habían quedado compilados en el intento fallido (se relinkeó `librexgpu-xenos.a`).
NRO 61.946.053 bytes, SHA `8DDED8CF40F4…`, magic `NRO0`, strings `[rt-xfer-draw]`, `vulkan_skip_unused_stencil_transfers` y `guest enabled stencil` presentes. ELF archivado `out/elf-archive/kof2002um_nx-8DDED8CF.elf`. Staging: `switch/kof2002um-nx/kof2002um-nx-fiberstack.nro` (TOML igual; `level = "debug"`, así que sale `CreateFiber: … stack=`).

**Pendiente (usuario, MTP)**: copiar `kof2002um-nx-fiberstack.nro` a `sdmc:/switch/kof2002um-nx/`, borrar `logs/`, 20 s en el título, después pelea con OC 1–2 min (L+R+ZL ya se puede usar), traer `logs/`.

**Qué mirar en la corrida**
- Sin crash y `CreateFiber: … stack=0x40000` en las fibras.
- Título: `[gpu-pass]` RT transfers por debajo de 102,7 ms/frame si `[rt-xfer]` dice `guest stencil never`; `stencil bits skipped in N transfers`.
- `[rt-xfer-draw]` del destino `D0 b0 p16 m2`: si es rect list, z always + write, sin blend, la transfer C0@0→D0@0 se puede saltear.
- Artefactos: con el salto, el stencil del host queda viejo. Se vería si (a) una transfer depth→color (D0@0→C0@0) o un resolve de depth lleva ese stencil a datos que el juego usa sin redibujar, o (b) el guest activa stencil sobre un depth RT que ya era dueño del rango (`guest enabled stencil` marca cuándo). Si pasa: `vulkan_skip_unused_stencil_transfers = false` arriba de `[log]` en el TOML.

**Revisión** (Gemini 3.8 Flash high; los dos Codex dieron 429): sin bugs ni UB en los hunks; `CreateFiber` correcto (alineación, heap, pila del host); los riesgos (a) y (b) de arriba. Descartado: que el `std::map` de `[rt-xfer-draw]` cause tirones (pocas firmas, se vacía cada 4 s).

**Próximo paso**: con los `logs/`, triage si crashea (`x360-crash-triage`); si no, decidir el salto de C0@0→D0@0 con `[rt-xfer-draw]` y medir con `[gpu-pass]`.

## Corrida rtxfer: el crash es la pila de las fibras (64 KB en vez de 256 KB); fix + salto de stencil escritos, falta compilar (2026-11-05)

**Corrida** (build rtxfer `EF2CE4D1`; logs `kof2002um_nx_002 (2).log`, `rex_crash (7).log`, `rex_stderr (11).log`, `rex_profile (11).log` en `%TEMP%`). Marcador `[rt-xfer]` presente. **No hubo L+R+ZL** (no aparece `menu shortcut`) y aun así crasheó a las 02:44:57, ~21 s de pelea: mismo desborde de pila de guest (escritura en 0x704CFFD0).

**Causa raíz del crash (confirmada)**
- Es código 68000 traducido (el juego de Neo Geo): los globals 0x84ADB7A8… son registros; `sub_82591DD0` = `move.b -(An)` + salto por la tabla 0x82022190 (índice = byte de estado, ya ×4); `sub_82592058` = `dbra` sobre la palabra baja de 0x84ADB7C8 (si ≠ 0xFFFF vuelve a llamar a `sub_82591DD0`). Cada salto 68000 es una llamada anidada: 3 frames de 0x60 = 288 B de pila por vuelta. Handler en este crash `sub_82592B00` (estado 20); en el anterior `sub_82593390` (estado 40).
- Corre en una fibra (r23 = 0x402E47D0 = parámetro de `CreateFiber`, fibras 0x30087000/0x30088000). El juego llama `CreateFiber(0, …)` (`sub_821652E0`, `li r3,0`; starts 0x821676A0 y 0x82981AC8).
- El `CreateFiber` original (0x826AEB30, Ghidra) con 0 lee `KTHREAD+0x84 → KPROCESS+0x1C` (`kernel_stack_size`, que `kernel_state.cpp` llena con `XEX_HEADER_DEFAULT_STACK_SIZE` = **0x40000**), redondea a 4 KB, mínimo 16 KB, y llama a `MmCreateKernelStack`. El hook de ReXGlue (`kernel/crt/threading.cpp`, `CreateFiber_entry`) ponía **0x10000 fijo**: 64 KB alcanzan para ~220 vueltas; en la 360 son 256 KB.
- **Descartado**: que el modo "no draws" de la prueba A/B causara el crash (fue coincidencia). Con el fix, la rotación L+R+ZL vuelve a servir.

**RT transfers en la pelea** (por frame, ~4,6 fps): 8 transfers, 4224 tiles. Hay solo 6 RT: C0 2x @0 (1280 de ancho), D0 4x @0 (640), C0/D0 4x @1440/@1664 (320), C0/D0 1x @1440/@1664 (640).

| par | tiles/frame |
|---|---|
| C0 b0 p16 2x → D0 b0 p16 4x | 1440 |
| D0 b0 p16 4x → C0 b0 p16 2x | 1440 |
| 6 pares en @1440/@1664 (4x→1x, y desde/hacia D0@0) | 224 c/u |

- En el título solo están los dos de 1440: **102,7 ms/frame** de transfers, muy estable entre reportes (sirve de referencia para medir).
- Hipótesis: D0 4x @0 cubre 0..1888 y es un "clear de EDRAM con un quad de depth 4x"; si es así, la transfer C0@0→D0@0 sobra.
- NVK (Mesa 25.0.7) **no expone** `VK_EXT_shader_stencil_export` ni `VK_EXT_fragment_shader_interlock` (solo 5 extensiones habilitadas; ReXGlue sí las pide). No hay camino FSI, y cada transfer a depth hace depth + clear de stencil + **8 pasadas, una por bit** (`need_stencil_bit_draws` en `render_target_cache.cpp`).

**Cambios (escritos, sin compilar)**
- `kernel/crt/threading.cpp`, `CreateFiber_entry`: con `dwStackSize == 0` usa `GetExecutableModule()->stack_size()` (256 KB); si no hay, 64 KB. Agregado `#include <rex/system/user_module.h>`: el primer build falló por tipo incompleto `UserModule` (`_logs/kof-fiberstack-build-20261105.log`).
- `graphics/vulkan/render_target_cache.cpp` (solo Switch): cvar `vulkan_skip_unused_stencil_transfers` (default true). Mientras ningún draw haya activado stencil, las transfers a depth no hacen el clear ni las 8 pasadas de stencil. `RtXferNoteGuestStencil` (llamada desde `IssueDraw` en `command_processor.cpp` con `normalized_depth_control`) las vuelve a activar para siempre en el primer draw con stencil y loguea `[rt-xfer] guest enabled stencil (…)`. El resumen `[rt-xfer]` agrega `stencil bits skipped in N transfers (T total), guest stencil never|USED`.
- Log nuevo `[rt-xfer-draw]` cada 4 s: por destino, el estado del draw que dispara la transfer (prim, `src`, cantidad de índices, z E/W + zfunc, stencil, color mask, blend0, modo EDRAM, scissor; o `RESOLVE-CLEAR`). Sirve para decidir si la transfer C0@0→D0@0 se puede saltear (draw tipo clear: rect list, z always + write, sin blend).

**Próximo paso**: compilar desde PowerShell en segundo plano: `$env:TEMP = "C:\Users\Casita\AppData\Local\Temp"; $env:TMP = $env:TEMP; Set-Location "D:\Ports Swtch\kof2002um-nx"; & "c:\devkitPro\msys2\usr\bin\cmake.exe" --build out/switch-config5 --target kof2002um_nx --parallel 8 *> "D:\Ports Swtch\_logs\kof-fiberstack-build2-20261105.log"`. Opcional: segunda opinión con `delegar -Rol revision` sobre esos hunks (no se llegó a pedir; el SDK es un repo git pero tiene cambios viejos sin commitear, armar el diff a mano). Verificar el NRO (`NRO0`, strings `[rt-xfer-draw]` y `vulkan_skip_unused_stencil_transfers`, SHA ≠ `EF2CE4D1`), archivar el ELF en `out/elf-archive/`, staging como `switch/kof2002um-nx/kof2002um-nx-fiberstack.nro`. Pedir al usuario: copiar por MTP, borrar `logs/`, 20 s de título + pelea con OC 1–2 min (L+R+ZL ya se puede usar), traer `logs/`. Esperado: sin crash, `CreateFiber … stack=0x40000`, transfers del título por debajo de 102,7 ms si el stencil nunca se usa, y las líneas `[rt-xfer-draw]` del destino D0 b0 p16 m2.

## Build rtxfer (qué pares de RT causan las transfers): compilado y en staging; falta la corrida (2026-11-05 01:06)

**Build OK** desde PowerShell con `$env:TEMP`/`$env:TMP` = `C:\Users\Casita\AppData\Local\Temp` (desde Bash falló dos veces por TEMP aun con la ruta de Windows: **compilar desde PowerShell**). NRO 61.929.669 bytes, SHA `EF2CE4D12D80…`, magic `NRO0`, string `[rt-xfer]` presente. ELF archivado `out/elf-archive/kof2002um_nx-EF2CE4D1.elf`. Staging: `switch/kof2002um-nx/kof2002um-nx-rtxfer.nro` (TOML igual).
**Pendiente (usuario, MTP)**: copiar `kof2002um-nx-rtxfer.nro` a `sdmc:/switch/kof2002um-nx/`, borrar `logs/`, pelea con OC 1 min, **sin L+R+ZL**, traer `logs/`.


**Hecho**
- `render_target_cache.cpp` (Vulkan), solo en Switch: al principio de `PerformTransfersAndResolveClears`, por cada transfer suma `(key de RT origen, key de RT destino, +host depth)` → cantidad y tiles de EDRAM (`end_tiles - start_tiles`). Cada 4 s, una línea `[rt-xfer] … calls, transfers, tiles, distinct pairs` y el top 12 por tiles: `% tiles n | C<fmt> b<base> p<pitch> m<msaa> -> …`. Funciones libres `RtTransferStatsAdd/Call` en un namespace anónimo (los tipos de la base son protected, por eso las keys se decodifican a mano: base 11 bits, pitch 8, msaa 2, is_depth 1, formato 4).
- Primer build: falló por `Cannot create temporary file in C:\WINDOWS\` (pasé TEMP como `/c/...`). **Hay que usar TEMP/TMP con ruta de Windows** (`C:/Users/Casita/AppData/Local/Temp`). Relanzado así; log `_logs/kof-rtxfer-build-20261105.log`.

**Vigente**: RT transfers ~63–66 % y EDRAM dumps ~14 % de la GPU en la pelea (sección de abajo). El crash con L+R+ZL lo causa el modo "no draws" de la prueba A/B; no usarla.

**Próximo paso**: revisar el log del build (si falló, corregir y recompilar con el comando de arriba y la TEMP de Windows). Si pasó: verificar el NRO (magic `NRO0`, string `[rt-xfer]`, SHA nuevo distinto de `9B6AE7D3`), archivar el ELF en `out/elf-archive/`, dejarlo en staging como `switch/kof2002um-nx/kof2002um-nx-rtxfer.nro` y pedir al usuario: copiar por MTP, borrar `logs/`, llegar a una pelea con OC, 1 min, traer `logs/`. Con el top de pares, decidir qué transfers evitar (p. ej. depth↔color que el juego después limpia, o RT con el mismo base y distinto formato).

## Corrida gpupass: RT transfers se lleva ~2/3 de la GPU; el crash con L+R+ZL lo provoca el modo "no draws" (2026-11-05)

**Corrida** (build gpupass `9B6AE7D3`, con OC; logs `kof2002um_nx_001 (8).log`, `rex_profile (10).log`, `rex_crash (6).log` en `%TEMP%`). `tick` calibrado a 1,6276 ns (se confirma el período de la sección de abajo).

| fase | ms GPU/frame | RT transfers | EDRAM dumps | draws | resolve copies | texture loads |
|---|---|---|---|---|---|---|
| título | ~132 | 77 % | 9 % | 5 % | 3 % | 5 % |
| pelea | ~191–200 | 63–66 % | 14 % | 6–10 % | 5 % | 8 % |

- **El blanco es RT transfers** (~126 ms/frame en la pelea): copias de EDRAM entre render targets del host. Sumado a EDRAM dumps, ~80 % de la GPU es emulación de EDRAM, no el trabajo del juego. Los draws reales son ~10 ms.
- En la selección de personajes hay frames con 256 marcas y `dropped` > 0; la pelea no pierde marcas.

**Crash**: L+R+ZL a las 02:15:44.9 (`menu shortcut: GPU A/B tests`). En el siguiente reporte de `switch_perf` la rotación pasó a "no draws" y antes del reporte siguiente el Main XThread desbordó su pila de guest (escritura en 0x704CFFD0, la guarda de abajo). La recursión es `sub_82591DD0 → (bctrl) sub_82593390 → sub_82592058 → sub_82591DD0…`: un escáner del juego que lee un buffer byte por byte hacia atrás (puntero en 0x84ADB7CC) y por cada byte vuelve a llamarse, hasta encontrar un byte igual al esperado (`sub_82593390`, 0x82593450). Con "no draws" el dato que tenía que dejar la GPU no aparece y el escáner no para. **No es un bug del juego en una partida normal**: es el modo de diagnóstico. "no draws" también saltea los draws con memexport (`command_processor.cpp:4223/4258`), y los resolves copian un EDRAM vacío; no está confirmado cuál de los dos lee la CPU.
- La rotación A/B ya no hace falta para encontrar el blanco. Si se vuelve a usar, evitar "no draws" y "nothing", o no saltear draws con `memexport_ranges_` no vacío.

**Próximo paso**: ver por qué hay tantas transfers. Loguear por frame qué pares de RT (formato/base EDRAM/pitch) disparan transfers en `render_target_cache.cpp` (cerca de 4690 y 5316, donde está el skip bit 2) y probar las opciones de Xenia que las reducen (p. ej. no transferir depth↔color cuando el juego limpia después, o `render_target_path` distinto si NVK lo permite).

## Build gpupass (timestamps por pase) compilado y en staging; falta la corrida (2026-11-05 00:41)

**Hecho**
- `deferred_command_buffer.h/.cpp`: `MarkPass(PassCategory)` mete un comando `kPassMarker` solo cuando cambia el tipo de trabajo. En `Execute`, si hay un `PassTimer` puesto, escribe un timestamp BOTTOM_OF_PIPE. Categorías: other, draws, RT transfers, resolve clears, EDRAM dumps, resolve copies, readback, texture loads, uploads, swap.
- Marcas puestas en: los dos draws de `IssueDraw`, `IssueSwap`, el dispatch de `IssueCopy_ReadbackResolvePath`, `PerformTransfersAndResolveClears` (clear o transfer según `resolve_clear_needed`), `DumpRenderTargets`, el dispatch de copia y los dos de clear en `Resolve`, `LoadTextureDataFromResidentMemoryImpl` y `UploadRanges`.
- `command_processor.cpp`: segundo query pool de 512×128 (se activa junto con `gpu_submission_timestamps`, no hace falta tocar el TOML). Lo que pasa entre dos marcas va a la categoría de la primera. Cada 2 s sale una línea `[gpu-pass]` con ms por frame y % por categoría, marcas por frame, dropped y unread. El período del tick se calibra contra `steady_clock` desde un ancla tomada con la GPU ocupada.
- Build OK, solo warnings viejos. Log `_logs/kof-gpupass-build-20261105.log`. NRO 61.925.573 bytes, SHA-256 `9B6AE7D30B7E…`. ELF `04ED1E42…` archivado en `out/elf-archive/kof2002um_nx-9B6AE7D3.elf`. Revisado a mano (no se encontró `verify_nro.py`): magic `NRO0`, strings `[gpu-pass]` y `(calibrated)` presentes, keys del TOML arriba de `[log]`.
- En staging: `switch/kof2002um-nx/kof2002um-nx-gpupass.nro`. El TOML es el mismo.

**Pendiente (usuario, por MTP)**: copiar `kof2002um-nx-gpupass.nro` a `sdmc:/switch/kof2002um-nx/`, borrar `logs/`, llegar a una pelea **con OC y sin cambiarlo**, dejarla correr 1 o 2 minutos y traer `logs/`. Opcional, al final: L+R+ZL en la pelea arranca la rotación A/B de `switch_perf` (sin draws, sin transfers, sin resolves, sin texturas…, 10 s cada uno, 80 s en total). Sale en `rex_profile` como `mode:`.

**Cómo leer**
- `tick` debería dar ~1,628 ns. Si da ~1,0, NVK sí cuenta en ns y lo de la sección de abajo hay que revisarlo.
- La categoría con más % es el blanco. draws: los shaders traducidos y NVK (ver la codegen de NVK o un renderer propio). transfers, dumps o clears: copias de EDRAM de la emulación de RT (hay que evitarlas o fusionarlas). texture loads: los compute de untile.
- Los timestamps BOTTOM serializan algo. Si los fps bajan mucho contra 4,7, aumentar el espaciado de las marcas.
- `dropped` > 0 quiere decir que se llenaron las 128 marcas de una submission, y el resto de esa submission quedó en la última categoría.

## Corrida gputs: el límite es la GPU real, que escala con el clock de la GPU (2026-11-05)

**Corrida** (NRO `517DC32A`, logs `rex_profile (9).log`, `kof2002um_nx_002 (1).log` en `%TEMP%`). A mitad de la pelea el usuario sacó el overclock, y quedó como un A/B gratis:

| | CPU / GPU / RAM MHz | fps | `[gpu-ts]` por frame | `[gpu-ts]` ocupada |
|---|---|---|---|---|
| con OC | 1785 / 768 / 1600 | 4,7 | 127 ms | ~600 ms/s |
| sin OC | 1020 / 307,2 / 1331 | 2,1 | 285 ms | ~605 ms/s |

- Los frames tardan 2,25× más con una GPU 2,5× más lenta. La RAM bajó solo 17 %, así que **casi todo es tiempo de shader/ROP en la GPU**, no ancho de banda. La CPU (~118 %) no cambió y no limita.
- **El `timestampPeriod = 1 ns` está mal**: en los dos casos lo medido da ~0,61 del tiempo real (127 ms × 4,7 fps ≈ 600 ms/s). El contador de la GPU de la Switch corre a 614,4 MHz (ns × 384/625), así que el período es ~1,628 ns. Corregido: **la GPU está ~98 % ocupada**, ~207 ms por frame con OC y ~464 ms sin OC. Idle entre submissions casi cero: no hay una espera mal encadenada. El comentario de `command_processor.cpp:92` es falso.
- Presenter: `vkQueuePresentKHR` promedia 245 ms, que es solo la espera a la cola. El resto da ~0.
- Costo fijo alto: en el título, con 4 draws/frame, ya son ~120 ms reales (scissor 8,9 pantallas, 1,6 resolves, 12,7 draws de transfer, 4,2 texturas cargadas). Path de RT = host render targets (`fbo`, no FSI).
- Descartado: espera mal encadenada, presenter, CPU. Muy debilitado: memory type 0 de la shared memory como causa principal (el costo no sigue a la RAM).

**Próximo paso**: un build con timestamps por pase dentro del deferred command buffer (draws del juego / transfers de RT / resolves / carga de texturas por compute), con el período real (leer `timestampPeriod`, o calibrar contra el reloj de la CPU, o fijar 1,6276). Así se ve qué pase se come los ~120 ms fijos. Si están repartidos, el camino es el renderer a nivel D3D (Renut/deko3d) que se anotó abajo.

## Build gputs compilado y listo para la SD; falta la corrida en consola (2026-11-05 00:18)

- Recompilado con TMP/TEMP = `C:/Users/Casita/AppData/Local/Temp`, cmake directo desde Bash (no por `run-logged.ps1`: su `cmd /c` rompe las comillas de `"C:\Program Files\CMake\bin\cmake.exe"` y no corre nada). Log `_logs/kof-gputs-build2-20261105.log`, solo warnings.
- NRO `kof2002um-nx.nro` 61.921.477 bytes, SHA-256 `517DC32A4B25…`; ELF `8AB1112D…` archivado como `out/elf-archive/kof2002um_nx-517DC32A.elf`. `verify_nro.py`: magic OK, keys del TOML arriba de `[log]`, marker `[gpu-ts]` presente.
- Preparado `switch/kof2002um-nx/kof2002um-nx-gputs.nro` (el TOML se busca por nombre de app, `kof2002um_nx.toml`, así que el NRO renombrado lo encuentra).
- **Pendiente (usuario, por MTP; no hay letra de SD)**: copiar `kof2002um-nx-gputs.nro` y `kof2002um_nx.toml` a `sdmc:/switch/kof2002um-nx/`, borrar `logs/` viejo, abrir con title override, llegar a una pelea, traer `logs/`. Éxito = líneas `[gpu-ts]` cada 2 s en el log principal. Copia a la SD sin verificar.
- Próximo paso: leer `[gpu-ts]` según "Cómo leer la corrida" de la sección de abajo.

## Build de diagnóstico con timestamps de GPU: código listo, el build falló por TEMP (2026-11-05)

**Hecho** (para medir los ~170 ms por frame de la sección de abajo)
- `tools/rexglue-sdk/include/rex/ui/vulkan/functions/device_1_0.inc`: agregadas `vkCmdWriteTimestamp` y `vkGetQueryPoolResults`.
- `tools/rexglue-sdk/src/graphics/vulkan/command_processor.cpp`:
  - cvar nuevo `gpu_submission_timestamps` (init-only, apagado por defecto) y struct `GpuTimestampProbe g_gpu_ts` (pool de 512×2 timestamps, se crea lazy).
  - En `EndSubmission` escribe un timestamp TOP antes de `deferred_command_buffer_.Execute` y uno BOTTOM después, en el slot `GetCurrentSubmission() % 512`. Marca si es la submission que cierra el frame.
  - En `CheckSubmissionFenceAndDeviceLoss`, cuando avanza `submission_completed_`, lee cada slot. Cada 2 s loguea `[gpu-ts]`: GPU ocupada ms/s, idle entre submissions ms/s, la submission más larga y, por frame, GPU ocupada promedio/máx y span primero→último.
  - Supone `timestampPeriod = 1 ns` (NVK). El pool no se destruye en Shutdown (es solo diagnóstico).
- `switch/kof2002um-nx/kof2002um_nx.toml`: agregados `gpu_submission_timestamps = true` y `present_painted_profile = true` arriba de `[log]`.

**Build FALLÓ**: `Cannot create temporary file in C:\WINDOWS\: Permission denied` (g++ sin TEMP/TMP en el entorno del Bash). Log `_logs/kof-gputs-build-20261105.log`. El NRO sigue siendo `E3D5CCCA`.

**Cómo leer la corrida**
- GPU ocupada ≈ 850+ ms/s y ~170 ms por frame: el trabajo de la GPU emulada es real. Siguiente: partir por pase (resolves, transfers, draws) o probar el shared memory en otro memory type.
- GPU ocupada baja con mucho idle: los 170 ms están en el present/presenter o en una espera mal encadenada. Mirar el dump de `present_painted_profile`.

**Próximo paso**: recompilar con TEMP definido, por ejemplo `TMP=/c/Users/Casita/AppData/Local/Temp TEMP=$TMP /c/devkitPro/msys2/usr/bin/cmake.exe --build out/switch-config5 --target kof2002um_nx --parallel 8`. Verificar que el SHA del NRO cambió, archivar el ELF en `out/elf-archive/` y copiar a la SD con otro nombre (`kof2002um-nx-gputs.nro`) junto con el TOML nuevo. Correr hasta una pelea y traer `logs/`.

## Llega al juego y los botones andan; 5 fps porque la GPU emulada tarda ~170 ms por frame (2026-11-05)

**Corrida** (NRO `E3D5CCCA`, logs `rex_profile (7).log`, `kof2002um_nx_001 (7).log` en `%TEMP%`; muestreo de pila activo)
- 4,8–5,0 fps estables, del título a la pelea. CPU total ~118 % de 400 %.
- `Main XThread` 100 %, pero el 98 % de las muestras está en un **spin de espera a la GPU** del D3D del juego: `sub_82655C18 → sub_82655B48`, un bucle `while (dev[10908] - r30 < dev[10908] - *dev[10896]) sub_826571A8()`, que lee el fence que escribe la GPU. No es lógica del juego.
- `GPU Commands`: el 86 % de las muestras está bloqueado en `RefreshGuestOutput → PaintAndPresentImpl → wsi_switch_swapchain_queue_present → vk_common_WaitForFences → nvFenceWait`. Son 5 esperas por segundo de ~170 ms (850–930 ms/s). Entre medio, `WAIT_REG_MEM` duerme de a 1 ms.
- El costo es **constante**: con 7 draws/frame (arranque) también tarda ~150 ms. En la pelea hay 75–90 draws, 30 submits, 9 resolves, 48 draws de transfer de RT y 66 texturas/frame. Esto no se explica por la cantidad de trabajo.
- Sospechosos: (1) shared memory de 512 MB en el **memory type 0, no device-local** (el fallback del 2026-10-22, máscara `0x1`). Todas las lecturas de texturas y vértices y todos los resolves pasan por ahí. (2) RT de color 2xMSAA 1280x1024 + depth 4xMSAA, con transfers por frame.
- Codegen sin optimizar: `non_volatile=false non_argument=false skip_lr=false`. Hoy no es el límite, pero lo va a ser cuando la GPU baje.

**Referencia** (`tools/sa2-nx-reference/docs/porting-guide.md` §5.1, §8–11): de la GPU emulada del SDK dice "On the Switch it is several times too slow". SA2 llega a 60 fps con un renderer propio, primero el nativo Vulkan (`app/src/me`) y desde la 1.0.3 Renut/plume sobre deko3d (`deko3d-renderer-guide.md`; los parches están en `patches/renut-*`, `plume-*`). Otras guías útiles: §9.5 (la espera del juego a la GPU duerme en vez de hacer spin), §9.1–9.2 (codegen y LTO/PGO), `gpu-optimisation-candidates.md`.

**Próximo paso**: medir antes de elegir. Hacer un build de diagnóstico con timestamps de GPU por submission en el command processor xenos y `present_painted_profile = true`, para saber si los 170 ms son GPU real (y de qué pase) o una espera mal encadenada. Probar también shared memory en otro memory type. Si es GPU real y repartida, ir al renderer a nivel D3D (Renut/deko3d).

## Los botones andan; al apretar en el título, thunk sin registrar `0x826B8470` (2026-11-04 23:59)

**Corrida en consola** (NRO de keyrepeat `5CA9161C`; logs `rex_crash (5).log`, `rex_profile (6).log`, `rex_stderr (6).log`, `kof2002um_nx_001 (6).log` en `%TEMP%`)
- El arreglo de GetKeystroke funciona: `XamInputGetKeystrokeEx` da `4 ok` de 10 y para (`buttons seen 0x5810`), sin quedar pegado. El juego toma el botón y entra al código del menú.
- Abort en `Main XThread`: `[FATAL] Call to invalid or unregistered function at guest address 0x826B8470 (lr=0x826BF1E4, r3=0x11C42, r5=0x40565E90)`.
- FPS sigue en ~5. `Stack sampling: disabled` otra vez.

**Causa** (bytes de Ghidra, `scratch_g/thunks_826B8.txt`)
- `0x826B8398..0x826B84A8` es una tabla de thunks de 8 bytes `mr r3,r5; b <func>`. Hay dos sin registrar: `0x826B83C8` (`b 0x826D6250`) y `0x826B8470` (`b 0x826D9CF0`). El puntero se arma en código, así que ScanCodePtrs no los vio.
- Script nuevo `tools/ghidra-scripts/ScanThunkPairs.java`: busca en todo `.text` pares `<insn>; b X` justo después de un `blr`/`bctr`/`b`. Hay 2.860 candidatos y 12 sin registrar ni label. Solo esos dos son thunks reales; los otros 10 saltan a labels internos (código muerto dentro de funciones).

**Arreglo**: en el manifest, `[entrypoint.functions."0x826B83C8"]` (`end = 0x826B83D0`) y `"0x826B8470"` (`end = 0x826B8478`). Codegen: 6 archivos escritos, `init.cpp` pasó de 44.252 a 44.254 funciones. Logs `_logs/kof-codegen-thunks826B-20261104.log`, `_logs/kof-thunks826B-build-20261104.log`.
- Build: NRO SHA-256 `E3D5CCCA1428AECB98C99F0EA2605205702CAE4EB6E619BCA83344431D9FCF36` (61.917.381 B, `NRO0`). ELF `out/elf-archive/kof2002um_nx-E3D5CCCA.elf`, con `sub_826B8470` y `sub_826B83C8` adentro. Incluye el arreglo de keyrepeat. **Todavía no está en la SD.**

**Próximo paso**: copiar a la SD con otro nombre (p. ej. `kof2002um-nx-t826.nro`), llegar al título, apretar botones y traer los 4 logs. Si sale otro `unregistered function`, correr primero `ScanThunkPairs` y `ScanCodePtrs`. Si la dirección no aparece en ninguno, es una función entera que el discovery no vio (como `0x8270EEF0`), no un thunk.

## El input llega; una tecla quedaba pegada en GetKeystroke → arreglado (2026-11-04 23:45)

**Corrida del NRO de diagnóstico `6838CBDF`** (logs: `rex_profile (5).log`, `rex_stderr (5).log`, `kof2002um_nx_001 (5).log` en `%TEMP%`)
- `XamInputGetState` funciona: user 0 da `ok` y los botones llegan (A/B/X/Y `0xF000`, START `0x10`, BACK `0x20`, cruceta). Users 1–3 dan `0x48F`, como debe ser. El mapeo HID → XInput está bien.
- **Bug**: desde 01:02:02 `XamInputGetKeystrokeEx` da `ok` en el 100 % de las llamadas (VK `0x5815` = BACK) aunque `GetState` muestre `0x0000`. Causa en `switch_input_driver.cpp` `GetDeviceKeystroke`: el auto-repeat (100 ms) se atendía antes que los cambios; a ~5 fps el juego pregunta cada ~200 ms, así que cada llamada era un repeat y el KEYUP no salía nunca. BACK repitiéndose para siempre → el menú no avanza.
- **Arreglo**: los cambios (UP/DOWN) van primero; el repeat solo si no hay cambios y la tecla sigue apretada.
- FPS: igual que antes, 4,5–5,4 fps, `Main XThread` ~100 %. En el título 271 draws/frame. El hilo de GPU espera fences ~920 ms por segundo (5 esperas de ~185 ms): falta ver si el hilo principal espera a la GPU o al revés.
- **El muestreo de pila no se activó** (`Stack sampling: disabled`) aunque se copió `logs/rex/stacks_profile.flag`. La ruta del código es `<carpeta del NRO>/logs/rex/stacks_profile.flag`, que coincide. Hipótesis: el `CopyHere` por MTP no copia archivos de 0 bytes. Probar con un flag que tenga texto adentro, o confirmar en la consola que existe.

**Build** NRO SHA-256 `5CA9161C2E8B69AD9CFEDEBB1FC9C179A4B502028ACCBC3C855BE3BE55793DA5` (61.917.381 B, `NRO0`), ELF `out/elf-archive/kof2002um_nx-5CA9161C.elf`. Log `_logs/kof-keyrepeat-build-20261104.log`. `run-logged.ps1` volvió a salir en 0 s sin log; usar `c:\devkitPro\msys2\usr\bin\cmake.exe --build out/switch-config5 --target kof2002um_nx --parallel 8` directo. **Todavía no está en la SD.**

**Próximo paso**: copiar a la SD con otro nombre (p. ej. `kof2002um-nx-key.nro`) y un `stacks_profile.flag` que no esté vacío. Probar si el menú del título responde a A/START y traer `logs/` para ver las funciones calientes de `Main XThread`.

## Llega al título; ~5 fps y no toma botones → build de diagnóstico (2026-11-04 23:40)

**Corrida en consola** (NRO `DC9B0B2C…`; logs copiados en `_logs/kof-console-20261104-title/logs/`)
- Llega a la intro y al título, sin crash. El audio va a velocidad correcta.
- `rex_profile.log`: 4,6–5,7 fps estables. `Main XThread` 98–101 % de CPU; el resto está casi ocioso (CPU total ~112 % de 400 %). El cuello es la CPU del hilo principal, no la GPU (2–4 draws por frame). El muestreo de pila estaba apagado.
- Input: ninguna línea en el log (los TRACE de `xam_input.cpp` no salen con `level=debug`). El juego importa `XamInputGetState`, `GetCapabilities`, `GetKeystrokeEx` y `SetState`. `XamUserGetSigninState` siempre da 1. Con esto no se sabe si el juego pregunta ni qué recibe.
- El 99 % del log (215 mil líneas, ~1100/s, varios hilos) era `[apu] XMA: Write to unknown register (0601)`, el registro de lock.

**Build de diagnóstico** (NRO SHA-256 `6838CBDF20302A590F13F8EE4637B443733A76CEC31669ACB59E266AB3F88454`, 61.913.285 B; ELF `out/elf-archive/kof2002um_nx-6838CBDF.elf`, SHA `C1C96D00…`; log `_logs/kof-inputdiag-build-20261104.log`)
- `xam_input.cpp`: `[input-diag]` registra la primera llamada a cada export y después un resumen cada 2 s (llamadas, cuántas ok, último user/flags/resultado, OR de botones vistos).
- `switch_input_driver.cpp`: `[input-diag] HID slot N … connected/disconnected` y los botones HID crudos en cada cambio (primeros 300).
- `xma_decoder.cpp`: ya no registra la escritura a `0601`.
- En la SD (verificado por SHA leyendo de vuelta): `switch/kof2002um-nx/kof2002um-nx-diag.nro`, más `logs/rex/stacks_profile.flag` para activar el muestreo de pila de 1 ms. **Sobrescribir un archivo por MTP desde script no funciona** (`CopyHere` deja el viejo sin avisar), así que se copia con otro nombre. El `kof2002um-nx.nro` de la SD sigue siendo `DC9B0B2C`.

**Cómo leer la próxima corrida**
- Sin `first XamInputGetState`: el juego no pregunta por input; buscar en Ghidra quién llama a `__imp__XamInputGetState`.
- Llamadas con `-> 0x48F` (DEVICE_NOT_CONNECTED): mirar las líneas `HID slot`. Si no hay slot conectado, falla la enumeración; si hay, falla la asignación de slot.
- `ok` con `buttons seen 0x0000` aunque el HID muestre botones: falla el mapeo o `is_active`.
- FPS: con el muestreo de pila, `rex_profile.log` muestra las funciones calientes de `Main XThread`.

**Próximo paso**: abrir `kof2002um-nx-diag.nro` con title override, llegar al título, tocar A/START/cruceta unos segundos y traer `logs/`.

## Llega a una animación del menú; 61 thunks de vtable sin registrar (2026-11-04 23:10)

**Corrida en consola** (NRO `E23178C5…`; simbolizado con `out/elf-archive/kof2002um_nx-E23178C5.elf`)
- `0x8270EEF0` ya pasa. El juego casi hace una animación del menú inicial y aborta: `[FATAL] Call to invalid or unregistered function at guest address 0x8291E5C8 (lr=0x82913010, r3=0x4008BBE0)`. Cadena: `FiberEntryPoint` → `sub_8271F398` → `sub_82982140` → `sub_82981C68` → `sub_82986D90` → `sub_82983DD0` → `sub_82924DE8` → `sub_82912F48`.
- Justo antes, `NtCreateFile game:\_pass_._code_` da `0xC000000F` (archivo opcional, no parece la causa).

**Causa** (bytes de Ghidra)
- `sub_8291E5B0` son 6 instrucciones que terminan en `bctr` (`0x8291E5C4`). `0x8291E5C8..0x8291E5DC` es su gemelo (`lwz r11,0x48(r3); addi r3,r11,0x40; lwz r11,0x40(r11); lwz r11,0x14(r11); mtctr; bctr`), un thunk de herencia múltiple que solo aparece en una vtable (`.rdata 0x82099864`). Quedó como hueco sin compilar.
- Script nuevo `tools/ghidra-scripts/ScanCodePtrs.java`: busca palabras en bloques de datos que apunten a `.text` justo después de `blr`/`bctr`/`b`, o a un `mflr r12`. Encontró 39.352; 97 no estaban registradas. Las crucé contra el fin de cada función generada (inicio + 4·comentarios de instrucción en `recomp.*.cpp`):
  - **61 huecos reales**: todos son thunks (`addi r3,r3,-4; b`, `lwz r11,0(r3)…bctr`, `lis r11,0x82C0…bctr`) pegados al final de la función anterior. Entre ellos están `0x8291DCE0..0x8291DFF0`, `0x82777630..`, `0x8298E9E0..0x8298EB00` y `0x821676B0..`.
  - 36 caen dentro de funciones ya compiladas y se referencian desde `0x82108E10..0x82109350`, que parece una tabla de rangos de excepciones. No los registré.

**Arreglo**: agregué al manifest 61 entradas `[entrypoint.functions."0x…"]`. El `end` es el siguiente thunk o el inicio de la función siguiente. `register.cpp` pasó de 44.191 a 44.252 `SetFunction`, con las 61 adentro.
- Build: NRO SHA-256 `DC9B0B2C928E598D43668B619AA001CFFCC40D88FAD8F8F139972FA7049A3291` (61.913.285 B, `NRO0`). ELF `out/elf-archive/kof2002um_nx-DC9B0B2C.elf`, con `sub_8291E5C8` adentro. Ya copiado a `switch/kof2002um-nx-xctd/` (SHA verificado). Logs: `_logs/kof-codegen-vtthunks-20261104.log`, `_logs/kof-vtthunks-build-20261104.log`, `_logs/ghidra_scanptrs.log`.

**Próximo paso**: probar `switch/kof2002um-nx-xctd/` (NRO `DC9B0B2C…`) y traer los 4 logs. Si vuelve un `unregistered function`, buscar primero la dirección en la salida de ScanCodePtrs: si no está, el puntero se arma en código (`lis/addi`) y no en datos, y hay que extender el escaneo a pares `lis/addi`.

## libjpeg ya pasa; nueva función sin descubrir `0x8270EEF0` (2026-11-04 22:50)

**Corrida en consola** (NRO `E49CA367…`; simbolizado con `out/elf-archive/kof2002um_nx-E49CA367.elf`)
- Ya no aparecen el error de libjpeg ni `0xC000009A`, y no sale el log de `MmLockAndMapSegmentArray` (no se llamó en esta corrida).
- Abort en un fiber: `[FATAL] Call to invalid or unregistered function at guest address 0x8270EEF0 (lr=0x8270D624, r3=0x702AEDC0, r4=0x702AECE0, r5=0x18)`. Cadena: `FiberEntryPoint` → `sub_8271E8A8` → … → `sub_826C2838` (recursivo ×5) → `sub_826BF0C8` → `sub_826D70F8` → … → `sub_82709D28` → `sub_82708FB8` → `sub_8270D858` → `sub_8270D578`.

**Causa** (confirmada con bytes de Ghidra, script nuevo `tools/ghidra-scripts/ListRange.java`)
- `sub_8270D578` @`0x8270D620` hace `bctrl` a `*(r28+0)`. `sub_8270F0A0` llena esa struct: `+0 = 0x8270EEF0`, `+4 = 0x8270EF40`, `+8/+12 = 0x8288A6D0`. Los dos últimos están registrados.
- `sub_8270E9A0` termina en `0x8270EEEC` (`li r3,-3; b`). `0x8270EEF0..0x8270EF3C` es una función hoja sin stack frame que termina en `blr`, justo antes de `sub_8270EF40`. El discovery no la vio porque solo se la referencia como puntero (`lis/addi`).

**Arreglo**: en el manifest, `[entrypoint.functions."0x8270EEF0"]` con `end = 0x8270EF40`. WSL había quedado en `Wsl/Service/E_UNEXPECTED` y se arregló con `wsl --shutdown`. El codegen reescribió 5 archivos y `register.cpp` ahora tiene `SetFunction(0x8270EEF0, sub_8270EEF0)`. El aviso `0x82B95400 exceeds max_file_size_bytes` no es nuevo.
- Build: NRO SHA-256 `E23178C5A357EADE12ECC91441288244AEDE819B4777FEB157281346EE731E0B` (61.900.997 B, `NRO0`). Pesa lo mismo que el anterior por la alineación, pero el hash cambia. ELF `out/elf-archive/kof2002um_nx-E23178C5.elf`, con el símbolo `sub_8270EEF0` adentro. Ya copiado a `switch/kof2002um-nx-xctd/` (SHA verificado). Logs: `_logs/kof-codegen-8270EEF0-20261104.log`, `_logs/kof-8270EEF0-build-20261104.log`.

**Próximo paso**: probar `switch/kof2002um-nx-xctd/` (NRO `E23178C5…`) en la consola y traer los 4 logs. Si aparece otro `unregistered function` en este bloque (`0x8270C000..0x8270F400`, que parece un descompresor), revisar los punteros que guarda `sub_8270F0A0` y sus hermanos.

## MmLockAndMapSegmentArray real (2026-11-04 21:06, NRO `E49CA367…`)

- Busqué en GitHub (rea-atlas + code search). No hay nada específico de KOF. Lo útil: XeniOS (`xenios-jp/XeniOS`, BSD-3) es el único que implementa `MmLockAndMapSegmentArray`. ReXGlue, Xenia y 360-rcomp lo dejan como stub o fatal.
- **Portado** a `tools/rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_memory.cpp`: `MmLockAndMapSegmentArray(unk0, segment_array, length, read_only)` y `MmUnlockAndUnmapSegmentArray(base)`. Corrige la nota de abajo: **r5 es la longitud en bytes**, no n. Si las páginas son contiguas, devuelve la primera. Si están dispersas, copia al heap del sistema y en el unlock escribe de vuelta solo los bytes que cambiaron. Loguea `MmLockAndMapSegmentArray: length=… contiguous|scattered`.
- Supuesto (igual que en XeniOS, "sketchy"): el array tiene direcciones virtuales de página, no PFN.
- Build: NRO SHA-256 `E49CA36786F3EEB800E2F5C67CBC4D875A3CCB3F75A1394A6DE895F24F3C507A` (61.900.997 B, `NRO0`), ELF `out/elf-archive/kof2002um_nx-E49CA367.elf`. Ya copiado a `switch/kof2002um-nx-xctd/` (SHA verificado). Incluye también el arreglo de setjmp/longjmp de abajo. Log: `_logs/kof-mmsegarray-build-20261104.log`. Nota: `run-logged.ps1` salió en 0 s sin log; lo corrí con `cmake.exe` directo.
- Referencias para más adelante: ports de ReXGlue a Switch `infriker/mini-ninjas-nx` (`patches/rexglue-nx.patch`, `docs/porting.md`), `VibePorter/Lazarus` (base SA2, como la nuestra), `ChanseyIsTheBest/kameo-nx`, `GreenieSparkles/COD6-NX`. Todos GPL-3.0: tomarlos como referencia, no copiar código.

**Próximo paso**: probar `switch/kof2002um-nx-xctd/` (NRO `E49CA367…`) en la consola. En el log, buscar `output_message` de libjpeg, `MmLockAndMapSegmentArray`, `0xC000009A` y cualquier FATAL nuevo.

## setjmp/longjmp sin registrar → crash en libjpeg (2026-11-04 16:10, NRO `28F3921D…`)

**Corrida en consola** (NRO `0D3DFC71…`, se simbolizó bien con `out/elf-archive/kof2002um_nx-0D3DFC71.elf`)
- Los fibers ya andan: `ConvertThreadToFiber` + 10 `CreateFiber`, y no aparecen `SwitchToFiber: no valid fiber` ni `fiber stack overflow`.
- El juego carga `fix2.bin` y `DEF.PAL` y abre `game:\_pass_._code_`, que no existe (`0xC000000F`). Eso es probablemente normal; está sin confirmar.
- Abort: `[FATAL] Call to invalid or unregistered function at guest address 0x00000000 (lr=0x82681FB8, r3=0, r5=0x8266D58C)` en un fiber. Cadena: `sub_8266D538` (loader JPEG del juego) → `sub_8267FF10` jpeg_read_header → `sub_8267FC78` → `sub_82688098` consume_markers → `sub_82681D38` read_markers.

**Causa** (confirmada en el codegen)
- `sub_82681D38` es el `read_markers` de libjpeg: `unread_marker` está en +420 y `marker` en +444. El bctrl de `0x82681FB4` es `ERREXIT1` (`err->msg_code` +20, `msg_parm` +24, `err->error_exit` +0).
- `sub_8266D538` hace `jpeg_std_error(&jerr @r1+576)` (`sub_826825D0`), pone `error_exit = 0x8266B718` y llama a **setjmp `sub_82729940`** (`&jerr+144`, retorno `0x8266D58C`). `0x8266B718` llama a `output_message` y después a **longjmp `sub_82729400`** (`jmp_buf, 1`).
- En el manifest no había `setjmp_address`/`longjmp_address`, así que se recompilaron como funciones normales. El longjmp "volvía" a read_markers con los registros del setjmp (`r31=0`). El `r5=0x8266D58C` es el LR que guarda el jmp_buf. Con eso, el ERREXIT siguiente llamaba a `*(*(0))` = 0.

**Arreglo**
- Manifest `[entrypoint]`: `setjmp_address = 0x82729940`, `longjmp_address = 0x82729400`. Codegen OK: `ppc_setjmp` en los 7 sitios de setjmp y `ppc_longjmp` en 17 (el pch tiene un `jmp_buf` map `thread_local` por dirección guest, que sirve con fibers).
- **Build (16:14)**: NRO SHA-256 `28F3921DBCE0EF1A57CB36CE7EB862D8DC6190B433E1C0A35E3F394D48014578` (61.888.709 B, magic `NRO0`), ELF `out/elf-archive/kof2002um_nx-28F3921D.elf`. Ya copiado a `switch/kof2002um-nx-xctd/` (SHA verificado). Log: `_logs/kof-setjmp-build-20261104.log`.

**Hipótesis vigente**: con el longjmp real, el juego se recupera del error JPEG (lo ignora o usa un fallback) y sigue. Pero el error JPEG en sí sigue sin explicarse. Si el juego no avanza o la imagen sale mal, buscar en el log el mensaje de `output_message` (libjpeg imprime "Unsupported marker type 0x..", JERR 68) y ver qué buffer decodifica `sub_8266D538` (¿datos de `_pass_._code_`? ¿un XCTD sin descomprimir?).

**Descartado**: los fibers (`SaveNonVolatiles` cubre r14–r31/f/v/CR/FPSCR y `X_FIBER_CONTEXT` tiene static_assert), y que los fibers cambien de hilo (todos se crean y corren en el hilo `t…10439724128`).

**Próximo paso**: probar `switch/kof2002um-nx-xctd/` (NRO `28F3921D…`) en la consola. En el log, buscar el texto de `output_message` y si aparece otro FATAL.

## Tanda nocturna: ObReferenceObject real (2026-11-04 00:06, NRO `0D3DFC71…`)

- Revisión estática: ya no queda ningún `REX_FATAL` de llamada sin resolver ni `REX_UNIMPLEMENTED` en el codegen. Los 242 imports del juego linkean todos.
- Imports que el juego usa y que en el SDK son `REX_EXPORT_STUB`. Ojo: `REX_STUB` **no toca r3**, así que "devuelve" su primer argumento. Son: `ObReferenceObject`, `MmLockAndMapSegmentArray`/`Unlock…`, `XeCryptRotSumSha`, `XeKeysGetKey`, `XeCryptBnQwBeSigVerify`, NetDll_XNet*, XamShow*UI, XUsbcam*, `XamVoiceSubmitPacket`, `XamUserCreateStatsEnumerator`.
- **Arreglado**: `ObReferenceObject` (`xboxkrnl_ob.cpp`) ahora hace `RetainHandle`, el espejo de `ObDereferenceObject`. El juego lo usa en pares ref→deref (`0x82AB1E38`→`0x82AB1F2C/F58`, `0x82AB7BDC`→`0x82AB7CBC/CE8`). Con el stub, el deref soltaba una referencia que nadie había tomado y el objeto se liberaba antes de tiempo.
- Sin tocar: la cadena crypt de `0x82708374..83CC` (RotSumSha → GetKey → SigVerify). Con el stub, SigVerify devuelve r3 = puntero ≠ 0, o sea TRUE, y la verificación "pasa".
- Build: NRO SHA-256 `0D3DFC71DAC29BEAC146FE59E1276340D9D092320AAB4A4631C38E98D9D6F2C4` (61.884.613 B), ELF `out/elf-archive/kof2002um_nx-0D3DFC71.elf`. Ya copiado a `switch/kof2002um-nx-xctd/`. Incluye el canario de fibers de abajo.
- Pendiente: `MmLockAndMapSegmentArray` (llamado en `sub_8290B8E8` @`0x8290B9FC` con `r3=0, r4=segs, r5=n, r6=0`). Con el stub devuelve 0 y el juego sale con `0xC000009A`. Solo pasa por ahí cuando el arg de stack `[r1+372]` ≠ 0 y n ≠ 0 (lectura scatter). Implementarlo si aparece `0xC000009A` en la consola, o antes si se confirma la semántica. Las 2 búsquedas con /gemini dieron timeout (exit 4): reintentar con `-Pages 0`.
- `request_keep_awake` lo denegó el clasificador de auto mode.

**Próximo paso**: probar `switch/kof2002um-nx-xctd/` en la consola y traer los logs (buscar `fiber stack overflow`, `SwitchToFiber: no valid fiber`, `0xC000009A`, `STUB`). Si no hay consola: semántica de `MmLockAndMapSegmentArray` con /gemini `-Pages 0` e implementarlo.

## Fibers: canario de stack + 1 MiB de host (2026-11-04 00:02, NRO `33BDF6E0…`)

- Revisé los hooks de fibers en Switch: `fiber_switch.cpp` (swap a mano AAPCS64) y `kernel/crt/threading.cpp` se ven bien; `fiber_ptr` en KTHREAD+0x164 (=356) coincide con lo que lee el juego.
- Riesgo que encontré: los hilos guest tienen 16 MiB de stack de host, pero los fibers solo 256 KiB, en un `std::vector` **sin página de guarda**: si se desborda, corrompe el heap sin avisar. El juego crea 2 fibers por tarea (`sub_821652E0`, 8 llamadores, stack guest 0 → 64 KiB).
- Arreglo: mínimo de 1 MiB de host en Switch (`threading.cpp`, ~16 MiB en total) + canario de 1 KiB al fondo del stack (`fiber_switch.cpp`), que se chequea en cada `SwitchTo`. Si salta: `[FATAL] fiber stack overflow` en `rex_stderr.log` y abort.
- Build: NRO 61.884.613 bytes, SHA-256 `33BDF6E09E8D3252A6E4EC2379074AA22344666BF953EC892C471B95E556922F`, ELF `out/elf-archive/kof2002um_nx-33BDF6E0.elf`. Ya copiado a `switch/kof2002um-nx-xctd/` (mismo pack `e6827a3a…`).
- Nota: `run-logged.ps1` pasa el comando a `cmd /c`; para cambiar de carpeta usá `-WorkDir`, no `cd ...;`.

**Para probar en Switch**: `switch/kof2002um-nx-xctd/` → ¿pasa el loop de tareas (`0x82165900`)? En el log, buscar `SwitchToFiber: no valid fiber`, `FiberEntryPoint` y `fiber stack overflow`.

## XCTD OK; abort en SwitchToFiber → hooks rexcrt de fibers (2026-10-31)

**Resultado de la corrida con el pack XCTD** (NRO diag 4 + pack `e6827a3a…`)
- `P_DATAA.BIN` ahora llega descomprimido (`size=0xdd00`, `raw = 40000000 c04a0000 c06a0000`) y pasa el parseo. Después carga `BAR`, `MAX2BG`, `CSLST`, `OFFSET`, `FIX2`, `DEF.PAL` sin errores.
- Abort nuevo: `[FATAL] Unresolved call from 0x826AEC70 to 0x826B4410` (el aviso ya salía en codegen desde el 22/10).

**Causa**
- `0x826AEC70` es el thunk de `SwitchToFiber` (`b 0x826B4410`); `0x826B4410` guarda r14–r31/f14–f31/v64–v127/CR/LR en `*(KTHREAD+356)` y restaura los del fiber `r3`. No se puede ejecutar recompilado. El manifest lo tenía como chunk de `0x826B4400`, por eso no tenía CallTarget.
- Bloque XTL de fibers (estático): `0x826AEA40` GetCurrentFiber, `0x826AEA50` ConvertThreadToFiber, `0x826AEAE0` ConvertFiberToThread, `0x826AEB30` CreateFiber, `0x826AEC10` DeleteFiber, `0x826AEC70` SwitchToFiber. Contexto de `0xA50` bytes, igual a `X_FIBER_CONTEXT` del SDK.

**Arreglo** (persistente, en el manifest)
- `[entrypoint.rexcrt]` con los 5 hooks del SDK (`kernel/crt/threading.cpp`). Van juntos: el SwitchToFiber del host busca el fiber en su registro. **Ojo**: tiene que ser `[entrypoint.rexcrt]`; `[rexcrt]` en la raíz se ignora sin aviso.
- Codegen corrido (`cache/codegen.sh` en WSL): `register.cpp` tiene `SetFunction(0x826AEC70, rexcrt_SwitchToFiber)` etc. y ya no está el FATAL. El codegen borró los diags `recomp.12/.18/.25` (ya no hacen falta).

**Build**: NRO 61.884.613 bytes, SHA-256 `E844EF8243352A853221B584C6C40D59680CCA75280086E33583572AA113131B`, ELF `out/elf-archive/kof2002um_nx-E844EF82.elf`. Ya copiado a `switch/kof2002um-nx-xctd/` (mismo pack `e6827a3a…`). Compila y linkea; sin probar en consola.

**Hipótesis vigente**: con los hooks, el loop de tareas del hilo principal (`0x82165900`, llama a SwitchToFiber por cada tarea) debería avanzar hasta el logo/título.

**Descartado**: problema de datos en pdata (resuelto por XCTD); la vtable de voz (`E86867E4` es un campo normal).

**Próximo paso**: probar `switch/kof2002um-nx-xctd/` en consola y traer los 4 logs. Si un fiber no arranca, buscar `SwitchToFiber: no valid fiber` / `FiberEntryPoint` en el log principal.

## Pack con XCTD descomprimidos (2026-10-31, NRO diag 4 `D35E0CBB…`, pack `e6827a3a4bc256ae`)

**Formato XCTD completo** (`tools/xctd/xctd.c`, documentado en la cabecera)
- `n = (flags & 0xFFFF) >> 6` bloques; ventana `15 + ((flags>>4)&3)`. Desde `+10`, tabla de n tamaños descomprimidos: 20 bits MSB-first, o u32 BE si `flags & 0x00400000` (algún bloque ≥ 1 MB); padding a u32.
- Cada bloque es un stream LZX independiente (lzxd nuevo por bloque) y arranca en un múltiplo de `0x20000` del archivo (relleno con ceros). Frames XMem de 0x8000 salvo el último del bloque. lzxd necesita ~64 bytes de ceros al final del input.
- `n = 0` (solo `data/vic/vic0_00.bin`, 24 bytes): lo tomo como 1129 bytes en cero (**supuesto**).
- Hay que usar `game/xbla-extracted-fixed/`; `game/xbla-extracted/` tiene archivos corridos (falla la mitad).
- Resultado: 1175/1175 descomprimen; `bgj1.bin` deja el bloque 4 en ceros (el bloque faltante del paquete). `P_DATAA.BIN` → tabla `0x40, 0x4AC0, 0x6AC0`.

**Cómo regenerar** (WSL): `gcc -O2 -Itools/xctd/ms tools/xctd/xctd.c tools/xctd/ms/lzxd.c tools/xctd/ms/system.c -o tools/xctd/xctd`; `python3 tools/xctd/unpack_all.py game/xbla-extracted-fixed /tmp/kofpack/game`; `python3 tools/sa2-nx-reference/tools/build_asset_pack.py /tmp/kofpack/game --out-dir switch/kof2002um-nx-xctd --verify`. (`xctdtest.c`, `prep.py`, `batch.py` quedan obsoletos.)

**Listo para probar**: `switch/kof2002um-nx-xctd/` (NRO diag 4 + toml + `game.nxpack` 1.092.493.628 bytes + `game.nxidx`, ver `LEEME.txt`). Sin probar en consola.

**Próximo paso**
1. Probar en consola: el log `KOF pdata` debería mostrar `raw = 40000000 c04a0000 c06a0000 …` y no crashear en `sub_82718478`.
2. Si pasa, sacar los diags `recomp.12/.18/.25` y reconstruir el NRO.

## Causa: los datos son XCTD (descompresión transparente del kernel) (2026-10-31)

**Qué se encontró**
- `LDI*` no se importan (no hay `__imp__LDI*` en `kof2002um_nx_init.cpp`): hipótesis 1 de abajo descartada.
- `sub_8217A3B8` = `sprintf("game:\data\%s")` + `fopen("rb")`/`fseek`/`ftell`/`fread`/`fclose`; `sub_82160E20` lo reintenta. `sub_82221168` carga `FIGHT\P_DATAA.BIN` (o `P_DATAN`, según el byte `0x837B7B00`) en `0x83FC3040` y llama a `sub_82219C58` **sin descomprimir**. El juego asume que `fread` devuelve los datos ya descomprimidos.
- 1175 de 1259 archivos extraídos empiezan con `0FF512ED`. Es la firma xcompress LZX **TD**: en la 360 el kernel descomprime al leer (XCTD; el SDK tiene la clase `XFileXctdCompressionInformation`=27 y responde "no comprimido" en `xboxkrnl_io_info.cpp:151`). El juego nunca consulta esa clase (la única llamada a `NtQueryInformationFile` usa la 14).
- Formato simple (745 archivos): `+0` magia BE, `+4` `01000000`, `+8` hash, `+0C` flags BE (ventana = `15 + ((flags>>4)&3)` bits, lo mismo que hace `sub_8290C430`), `+10` u32 BE con **tamaño descomprimido = valor >> 12**, desde `+14` frames XMem (`[u16 BE comp]` o `[FF][u16 uncomp][u16 comp]`). Concatenando los frames y pasándolos a libmspack `lzxd` (reset 0) salen enteros. `p_dataa` → `0xDD00` bytes con tabla `0x40, 0x4AC0, 0x6AC0` (LE): es lo que espera el parser.
- Formato extendido (415 archivos, los grandes, p. ej. `data/art/*.grc`): bits altos de `+0C` y bajos de `+10` distintos de cero; la cabecera sigue con una tabla bit-packed (probablemente tamaños por bloque para acceso aleatorio) antes del primer frame. Falta descifrarlo. 15 archivos más fallan con el formato simple (ver `batch.py`).
- Herramientas: `tools/xctd/` (`xctdtest.c` = libmspack lzxd; `prep.py` = quita los frames; `batch.py` = valida todos). Se compilan en WSL: `gcc -Ims xctdtest.c ms/lzxd.c ms/system.c` (con los .c/.h de `tools/rexglue-sdk/thirdparty/libmspack/libmspack/mspack` copiados a `ms/`). Scripts Ghidra: `tools/ghidra-scripts/` (`DecompOne`, `DumpStr`, `RefsTo`). Ghidra corta las funciones porque marcó noreturn los `__savegprlr` (`0x827252xx`); para leer funciones es mejor el desensamblado de ReXGlue.

**Próximo paso** (el chat se cortó por contexto antes de empezar el punto 1; arrancar acá)
1. Descifrar la cabecera extendida (ejemplo: `data/art/art_i00.grc`, flags `0x162`, `+10` = `3646 3351 8d37 c523 e287 2458 b000 0000`, en `+20` aparece `47ac` que parece el tamaño del primer frame): seguir `sub_8290C430` → `sub_8290D640`/`sub_8290E0F0` (el lector de cabecera del xcompress estático) o comparar varias cabeceras `.grc`.
2. Descomprimir al armar el pack (herramienta en PC, el VFS ya sirve bytes planos), o hacerlo en el VFS del SDK al abrir un archivo con `0FF512ED`. Preferible al armar el pack: costo cero en la consola.
3. Reconstruir el pack, probar en consola y sacar los diags `recomp.12/.18/.25`.

## Resultado diag 4: el archivo se parsea SIN descomprimir (2026-10-31, NRO `D35E0CBB…`)

**Qué se vio** (`rex_stderr.log`, líneas mezcladas entre hilos; separé las palabras de la voz #45 de las de pdata)
- Mismo crash que en la diag 3 (swap16 `sub_82718478` llega a `0x86600001`), en la única llamada (`KOF pdata #1`, `buf=83fc3040`, `lr=822213e0`).
- `raw` en la entrada de `sub_82219C58` = `0ff512ed 01000000 be05a11d 00000062 0dd00000 0e6c0820 07007113 00b23300 35330040 60006f6f 7777bb7b ddbbefee …`, que son **los bytes del `p_dataa.bin` comprimido tal cual** (cabecera XMemCompress `0FF512ED`). El parser espera los datos ya descomprimidos y lee la cabecera comprimida como tabla de offsets; de ahí sale `n = 0x0651FD2F`.
- Causa: la carga `sub_82160E20` → `sub_8217A3B8`/espera `sub_82716320` devuelve el archivo sin pasar por el descompresor (o el descompresor no hace nada).

**Hipótesis vigente**
1. En el SDK `LDICreateDecompression`, `LDIDecompress`, `LDIDestroyDecompression` y `LDIResetDecompression` son `REX_EXPORT_STUB` (`tools/rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_modules.cpp:261-264`). `XMemDecompress` del XDK usa esas exportaciones de xboxkrnl (ordinales `0xB6`/`0xB7`…). Si el juego las importa, la descompresión no hace nada y el buffer queda con los datos crudos. **Sin confirmar**: un grep de `__imp__LDI` en `generated/*.cpp` no dio resultados; falta ver con qué nombre aparecen los imports (tabla de imports del XEX / Ghidra) o si el descompresor está enlazado estáticamente.
2. Si no importa LDI*: el cargador elige descomprimir según algo (flag, extensión, tamaño) que acá da distinto; habría que seguir `sub_8217A3B8`.

**Descartado**: corrupción del pack (los bytes coinciden con el archivo extraído), doble parseo (una sola llamada), la vtable de voz (`E86867E4` es un campo normal).

**Próximo paso**
- Confirmar si el XEX importa `LDICreateDecompression`/`LDIDecompress` (Ghidra o la tabla de imports de ReXGlue); buscar la llamada desde el código del cargador.
- Si los importa: implementarlos en el SDK sobre `rex/system/lzx.cpp` (libmspack `lzxd`, como hace Xenia en `xboxkrnl_xmem`/`LDI*`) y reconstruir. Eso arregla el parseo de todos los archivos `0FF512ED`.
- Diags que siguen en el código generado: `recomp.12`, `recomp.25` (voces) y `recomp.18` (pdata). Se pueden sacar cuando se arregle.

## Prueba diag 3 y build diag 4 (2026-10-31, NRO `D35E0CBB…`)

- La diag 3 (`CF031961`) **pasó el audio**: se listaron 64 voces y no se repitió el EC 0x22. En todas las voces sanas `obj+112 = E86867E4`, así que ese valor es un campo normal y no una vtable rota (la hipótesis 1 del audio pierde fuerza; ese crash puede depender del orden de los hilos).
- Crash nuevo, en el hilo principal del juego (`xstart` → `sub_82181C78` → `sub_827134B8` → `sub_821817D0` → `sub_82163E70` → `sub_82160C38` → `sub_82221168` → `sub_82219C58` → `sub_82718478`). Lectura de guest `0x86600001`, que no está mapeada (fin de la memoria del guest).
- `sub_82718478(buf, n)` hace swap16 in-place de `n` halfwords. `sub_82219C58` parsea un archivo de datos ya cargado: `F = *0x84AC2388` (buffer `0x83FC3040`), swap32 de la cabecera (3 words), `sub = F+F[0]`, tabla `T = F+F[1]` (13 words, swap32), guarda `sub+T[i]` en globales `0x83FBA5xx` y después hace swap32/swap16 de rangos entre esos punteros. El swap16 es de `[g(-23152), g(-23128))`, con `n = (fin-inicio)/2`: si el fin queda antes que el inicio, `n` da la vuelta y recorre toda la memoria.
- El llamador `sub_82221168` carga el archivo con `sub_82160E20(path 0x82BD2FDC, dst 0x83FC3040)` (si devuelve > `0x60800`, escribe en la dirección 1 = assert). El último archivo leído es `data\fight\p_dataa.bin` (`0x207A`, valor que también está en el stack). Viene con cabecera XMemCompress `0FF512ED` (igual que P40–P45, que se cargaron bien), así que la descompresión la hace código guest. No es el formato LZX estándar documentado, así que no lo descomprimí en la PC.
- Diag 4: `recomp.18` (`sub_82219C58`, codegen la borra) registra en stderr `KOF pdata #N in buf lr raw:` (16 words crudos al entrar) y `KOF pdata #N swap16 start n | F hdr | g:` (los 14 punteros globales) antes del swap16, con fflush (máx 8 llamadas). Las voces siguen logueando como en la diag 3. NRO `switch/kof2002um-nx-diag/kof2002um-nx.nro`, 61.880.517 bytes, SHA-256 `D35E0CBBAFCB20F9A9B48DB2C466E2699516F20D3DF5C03DDE5EBC348B75D9F2`, ELF `out/elf-archive/kof2002um_nx-D35E0CBB.elf`. Compila y linkea; sin probar.
- **Qué tiene que mostrar la corrida**: si `raw` está en ceros o es basura con alta entropía → la descompresión o la carga no corrieron (o se parsea antes de que termine). Si es una tabla coherente y aun así los punteros no son crecientes → el buffer se parsea dos veces (doble swap) o el archivo es otra versión. Siguiente paso: según el caso, `sub_82160E20`/descompresor o quién llama dos veces a `sub_82219C58`.

## Prueba diag 2 y build diag 3 (2026-10-31, NRO `CF031961…`)

- La diag 2 (`5AB431A8`) se cerraba antes del logo **por culpa del propio log**: en `sub_8292F6D0` leía `*(obj+112)+8` y `obj+112 = 0xE86867E4` (fault EC 0x24 en guest `0xE86867EC`, `x22 = 0x8292F788`). Se imprimió `KOF voice list self=4009eb40 len=1`, pero no la línea de la voz.
- Dato nuevo: la única voz de la lista (probablemente `0x400A7140`, `x3`) tiene en `+112` una vtable basura (`E86867E4`, distinta del valor de la corrida anterior). Es memoria **sin inicializar**: el subobjeto no se construyó, o la voz está liberada o reutilizada. Refuerza la hipótesis 1.
- Diag 3: solo desreferencia valores dentro de la imagen (`82000000–850F0000`) y vuelca 160 bytes de cada voz (`KOF voice #N dump:`). NRO `switch/kof2002um-nx-diag/`, 61.880.517 bytes, SHA-256 `CF0319615B0F1A63F90471BE3EEB817DE00CA297F478E2334CF1CCA4728B6F0C`, ELF `out/elf-archive/kof2002um_nx-CF031961.elf`. Sin probar.
- **Próximo paso**: correr en consola, traer `rex_stderr.log`; con la vtable principal de la voz (dump `+0`), buscar su clase y su constructor en Ghidra y ver quién debería escribir `+112`.

## Build diag 2: voces de audio (2026-10-31, NRO `5AB431A8…`)

- Mecanismo del EC 0x22: `REX_CALL_INDIRECT_FUNC` solo chequea el rango; si el destino está en rango pero no alineado a 4, `REX_LOOKUP_FUNC` lee media entrada de la tabla y devuelve basura (`0x2900878480d7`). O sea, el valor de `(*(self+112))[2]` es un destino no alineado.
- Ojo: en `sub_829134C0` `self+112` es un **objeto embebido** (`r3 = self+112`, vtable = `*(self+112)`), no un puntero a objeto. Lo corrupto es la vtable de ese subobjeto (o `self` entero).
- Unidades generadas editadas (codegen las borra): `recomp.12` (`sub_829134C0`) registra `KOF bad vcall 829134EC …` con self, `self+112`, `vt[0..3]`, destino, LR, volcado `self+96..+224`, `+556`, `+564`, solo si el destino está desalineado, fuera de rango o sin registrar (máx 8). `recomp.25` (`sub_8292F6D0`) registra `KOF voice list self len` y `KOF voice #N obj vtbl vt0 obj+112 sub.vt2` para cada elemento nuevo de la lista `+556` (máx 64). Se mantiene el log `KOF audio vcall` anterior.
- NRO `switch/kof2002um-nx-diag/kof2002um-nx.nro`, 61.880.517 bytes, SHA-256 `5AB431A8540464059AF776794D6110CB5935011E78E3FEE1A75AA48074ABE7AD`; ELF `out/elf-archive/kof2002um_nx-5AB431A8.elf`. Mismo `game.nxpack`. Compila y linkea limpio; sin probar en consola.
- **Próximo paso**: correr en Switch (title override) y traer `rex_stderr.log`. Comparar la voz rota contra las sanas: si `sub.vt2` difiere, ver si la vtable cae en `.rdata` (objeto sin construir) o en heap (liberado/reutilizado), y buscar en Ghidra quién construye ese objeto.

## Resumen para retomar (2026-10-31, NRO diag `C28C6FC1…`)

**Qué se hizo hoy**
- Causa raíz de los crashes viejos: al paquete LIVE le falta un bloque de 4 KB y los datos estaban mal extraídos. `tools/kof_stfs_extract.py` reextrae ubicando cada bloque por su SHA-1 → `game/xbla-extracted-fixed/`; el pack se reconstruyó (está en `switch/kof2002um-nx-candidato/`). Se quitaron los guards de Codex.
- Con los datos corregidos: no aparece el diálogo “data corrupted”, se ve el logo XBLA, el audio carga, la GPU presenta frames y el juego empieza a cargar datos de pelea.
- La build diag (`switch/kof2002um-nx-diag/`, ELF en `out/elf-archive/kof2002um_nx-C28C6FC1.elf`) copia el reporte fatal a `rex_stderr.log`, y eso funcionó.

**Crash actual (confirmado en `rex_stderr.log`)**
- `CPU exception` EC 0x22 (PC desalineado): `blr x2` hacia `0x2900878480d7`, que es basura, desde `__imp__sub_829134C0+0x2a0` (`image+0x736560`). Cadena: `sub_826B6410` (hilo) → `sub_8292E640` → `sub_8292F848` → `sub_8292F6D0` (bucle sobre la lista `self+556`, llamada `obj->vtbl[0]`) → `sub_829134C0` → primera llamada virtual `(*(self+112))->vtbl[2]` (PPC `0x829134EC`, LR guest `0x829134F0`).
- El destino guest pasa el chequeo de rango de código, pero la entrada de la tabla de funciones leída es basura. Eso indica un destino no alineado a 4, o sea un objeto/vtable en `self+112` corrupto o sin inicializar.
- `sub_8292F6D0` toma un lock con `vtbl[2] = sub_82912CF0` = `RtlEnterCriticalSection(obj+4)` (objeto `0x4009EB80`, vtable `0x82099150`). Parece el hilo del motor de audio recorriendo sus voces.

**Hipótesis vigentes**
1. Una voz/objeto de la lista `+556` se usa ya liberado o a medio construir (carrera entre el hilo de audio y el hilo del juego, o falla en la creación de una voz XMA).
2. El dato que llena `self+112` viene de un camino del runtime que devuelve algo distinto a la consola (XAudio/XMA).

**Descartado**: datos/pack (verificados por SHA-1), NVK, SaltyNX (solo avisos), el `rex_crash.log` en español (es de otro programa).

**Próximo paso**: en `sub_829134C0`, antes de la llamada en `0x829134EC`, registrar en stderr `self`, `*(self+112)`, la vtable y el destino, solo si el destino no está alineado (`&3`) o cae fuera de una función registrada. En `sub_8292F6D0`, registrar el tamaño de la lista `+556` y cada elemento la primera vez que se ve. Con eso se sabe qué objeto está roto y se puede rastrear quién lo crea (skill `ghidra-x360-ppc`).

## Causa raíz: paquete LIVE con un bloque faltante (2026-10-31)

- Al paquete `584109AD/000D0000/308C93…` le falta un bloque físico de 4 KB (bloque de datos 19917). Todo lo que sigue está una posición antes de lo que dice la aritmética STFS estándar, y los dos extractores usados antes leían el bloque equivocado. Por eso 925 archivos tenían la cabecera XMemCompress (`0FF512ED`) del archivo siguiente en el último bloque, los `.xsb`/`.xwb` no tenían firma y `voice.xgs` aparecía vacío.
- `tools/kof_stfs_extract.py` ubica cada bloque por su SHA-1 en las tablas de hash. Salida: `game/xbla-extracted-fixed/` (1.175 archivos con `0FF512ED` al inicio, audio `KBDS`/`DNBW`/`FSGX`, `default.xex` idéntico). Único archivo dañado: `data/B/bg/bgj1.bin` (4 KB en ceros).
- Primera prueba en Switch del candidato `8D7E9643…`: desapareció el diálogo “data corrupted”, se ve el logo, el audio carga sin el crash del parser y la GPU presenta frames. El juego llega a abrir `COMMAND.BIN`, `OBJ2\P40–P45`, `SEQ.BIN` y `BASE\PAL_A.BIN` (el log `[KOF data I/O]` se corta ahí porque tiene un tope de 96 entradas; no marca el punto del cierre).
- El cierre es el fatal del handler (`2345-0102`, reporte Atmosphère `01769234710_01e5129861770000.log`). Simbolizado contra el ELF de esta build: hilo guest `sub_826B6410` → `sub_8292E640` (usa `KeSetEvent`) → `sub_8292F848` → `sub_8292F6D0`. La dirección de retorno `image+0x9e864c` está justo después de la primera llamada virtual de `sub_8292F6D0` (PPC `0x8292F6F4`: `(*(self+64))->vtbl[2]`), así que el fallo ocurre dentro de ese método virtual. Parece el hilo de procesamiento del motor de audio (zona `0x829xxxxx`, mismo bloque que el parser XACT), pero falta confirmarlo. El usuario no encontró `logs/rex/rex_crash.log`; sin él no tenemos `far`/`esr` ni las líneas de log que no se volcaron.
- El `rex_crash.log` que llegó después no es de KOF (texto en español, imagen de ~44 MB; el NRO de KOF escribe “memory access fault”); probablemente salió de `sdmc:/switch/rex_crash.log`, la ruta compartida por otros ports ReXGlue.
- NRO diagnóstico `switch/kof2002um-nx-diag/kof2002um-nx.nro`, SHA-256 `C28C6FC151094C8AFA92CCE5025E5EC4D8469F982BD11EDCD160CDF02C013ED3`, ELF guardado en `out/elf-archive/kof2002um_nx-C28C6FC1.elf`. Cambios: (1) SDK `exception_handler_switch.cpp` copia el reporte fatal y las líneas pendientes del log a stderr (`rex_stderr.log`) y registra `[crash] could not translate/open/write` si no puede escribir `rex_crash.log`; (2) unidad generada `recomp.25` (codegen la borra) registra en stderr `KOF audio vcall #N self obj vtbl target` en la primera llamada virtual de `sub_8292F6D0`. Usa el mismo `game.nxpack`.
- Se quitaron los guards/diagnósticos de `recomp.18/61/157/165` (siguen solo los logs `[KOF data I/O]` del SDK). Pack reconstruido y NRO candidato en `switch/kof2002um-nx-candidato/`, SHA-256 `8D7E9643EA8DCE3F9091D4F56060FEE26CEA5C64F79C9AC40F058BB3AA66F256`. Sin probar en hardware ni en host.

## Revisión con la Switch conectada (2026-10-23)

- La SD se ve en Explorador de archivos por MTP, en `SD Card/switch/kof2002um-nx/`. Allí están el NRO (aprox. 59 MiB), `game.nxpack` (554 MiB), `game.nxidx`, la carpeta `game/` y los logs. No es una unidad con letra; cualquier actualización del NRO tendrá que hacerse por Explorador/MTP.
- Releí el último `kof2002um_nx_001.log` y `rex_crash.log`: el cierre sigue en `sub_82219C58` al procesar `data/fight/p_dataa.bin`. Después de invertir endian del encabezado, el parser suma el segundo DWORD a la base del buffer (`0x83FC3040`), obtiene `0xFC7B159E` y trata de invertir 13 DWORD ahí. Esa dirección no está dentro del recurso de 0x207A bytes; el siguiente puntero/cantidad resultan basura (`0x2630BCD0` / `0x0652AF83`) y se dispara el acceso inválido. No es el aviso de Vulkan/NVK ni el parser de audio.
- Corregí la revisión anterior de integridad STFS: el SHA-1 sí coincide. Los tres bloques lógicos `0x7E65..0x7E67` de `P_DATAA.BIN` tienen SHA-1 `b621ba476e1d371a61f043befbf0f7253c90a745`, `aacb2f959b646eb59b760fc7fcacb805d060214c` y `d2c87ecc855a1924378ae2184644e4c0791e65d8`; esos digest se encuentran en sus entradas hash del paquete y se calculan sobre los mismos bloques extraídos. El mismatch anterior era una fórmula equivocada para ubicar entradas del hash STFS. El archivo es auténtico; el problema es cómo el runtime/juego interpreta esa estructura o un paso de preparación que falta.
- El NRO visible en la SD tiene el tamaño esperado de la build diagnóstica de las 14:02; no he copiado todavía ninguna build nueva ni iniciado el juego desde la UI. Próxima tarea: rastrear el flujo que entrega `P_DATAA.BIN` a `sub_82219C58` y validar su encabezado contra otro archivo de lucha del mismo formato antes de tocar la transformación.

## Build de diagnóstico acotado (2026-10-23)

- Añadí una salida segura en `sub_82219C58`: si el segundo DWORD tras el byte-swap excede 64 MiB, registra el encabezado y retorna antes de calcular/desreferenciar la tabla imposible. **No convierte ni corrige el formato**; sirve para ver si el loader puede manejar el rechazo o cuál es el siguiente punto de fallo.
- Compilé el target Switch. El artefacto tiene firma `NRO0`, mide 61.884.613 bytes e incluye la cadena de diagnóstico. SHA-256: `4B378C782AF56E8D0857B7F29D4083E39FBA3BD66337675C0F056DF17CD4DFEE`. Copia local lista en `switch/kof2002um-nx/kof2002um-nx.nro`.
- Intenté transferirlo por la ventana de Explorador/MTP, pero Computer Use se interrumpió con Escape antes de reemplazar el archivo de la SD. La SD sigue con la build anterior; no se lanzó ningún NRO nuevo.

## Última prueba en Switch (2026-10-23)

- La prueba adjunta sí ejecuta el NRO: inicializa Vulkan/NVK, crea swapchain 1280×720, monta `game.nxpack` (1.259 archivos, 554,1 MB), carga `default.xex` y prepara el módulo.
- A los ~3 s se cierra en el mismo acceso invitado inválido: lectura de `0x4A074CF2` en `sub_82965280`, mientras procesa `\\media\\sounds`. `rex_crash` confirma que la dirección está dentro del rango virtual invitado pero la página no tiene permisos/mapeo. El contexto vuelve a mostrar `r30=0x4A074CF0`, `r3=0x4009FCF0` y LR `0x82965314`.
- El aviso de memoria de NVK aparece antes, pero no es la causa inmediata: la ejecución sigue hasta el parser de audio y el crash report apunta a esa lectura PPC recompilada. `rex_stderr` solo contiene avisos de conformidad/WSI.
- Estos logs no contienen las líneas de instrumentación `KOF stream`/dump de payload descritas abajo; por tanto el cierre está confirmado, pero esta prueba no añadió datos nuevos sobre el contenido ni el constructor del reader. No hay todavía una corrección validada.
- Próximo paso: instrumentar el límite de `sub_82960FB0` justo antes de llamar al parser y verificar la compilación/Copia NRO, o rastrear estáticamente el reader. No pedir otra prueba hasta que haya una build con un dato diagnóstico nuevo.

## Build candidata con salida segura del parser de audio (2026-10-23)

- La instrucción PPC calcula el cursor como `base + lwz(input + 65)`. Con el dump previo, ese campo produce `0x09FD5000` aunque el tamaño declarado por el parser es `0x43C`; el resultado cruza el espacio guest y causa el crash. Un fallo de bounds en el recurso es una hipótesis más consistente que NVK o falta de memoria.
- Añadí un guard en el código recompilado antes de desreferenciar el cursor: si el cursor cae en dirección invitada alta/claramente fuera del buffer, registra parser, cursor, base, longitud y entrada, devuelve `E_INVALIDARG` por la salida de limpieza del parser y evita la lectura fatal. El caller ya trata HRESULT negativo como error. Es una protección acotada para comprobar si el juego puede omitir el bank/seguir el arranque; todavía no corrige la interpretación del formato ni demuestra por qué el cursor está mal.
- Build Switch completado (`kof2002um_nx`), se validó la firma NRO `NRO0`, y se copió el artefacto al directorio de SD del proyecto. SHA-256 en ambas rutas: `84F5FAF411A10F34F181C387E4F3FFA007D68B7EB6606137F527B055C8187BD6`.
- La compilación editó una unidad generada por codegen; si se vuelve a ejecutar codegen se sobrescribe el guard. Prueba requerida: sustituir el NRO y arrancar una vez. Éxito observable: no hay crash en `0x4A074CF2`; el log debe mostrar `KOF audio parser invalid cursor`. Si el arranque continúa, el siguiente trabajo será identificar/corregir el dato de origen y restaurar parsing correcto, no dejar el audio omitido.

## Resultado de la build guard y salida antes del finalizador (2026-10-23)

- El log recibido incluye el marcador nuevo `KOF audio parser invalid cursor`, así que sí ejecutó la build guard. Confirma cursor `0x4A074CF0`, base `0x4009FCF0`; el tamaño/cuenta que yo había rotulado desde los offsets `+152/+108` salen cero y **no son una longitud fiable**.
- El primer guard evitó la lectura original, pero después cayó en `0x4A072CF8`, por una lectura de byte desde `sub_829650C8` invocada dentro de `sub_82965D80` (finalización del parser). La ruta de finalización también presupone que el banco se parseó correctamente; devolver error por la salida normal no es suficiente.
- La siguiente build candidata devuelve `E_INVALIDARG` directamente desde `sub_82965280`, restaurando su frame sin llamar a `sub_82965D80`. Así permite al caller entrar en su ruta de error sin ejecutar el finalizador sobre estado parcial. Sigue siendo una mitigación/prueba de control de flujo, no una corrección del reader ni del formato XSB.
- Build Switch completa y copia al folder `switch/kof2002um-nx/kof2002um-nx.nro`; firma `NRO0`, tamaño 61.876.421, SHA-256 `FCC52362683A138FFCE82D0E0609448989DF3DA11BB3635FCB2A5E641A8A767D`. Requiere una prueba adicional en hardware para saber si el error se propaga de forma limpia o el init de audio tiene otra ruta dependiente.

## Reanudación: callback faltante y validación host (2026-10-23)

- El build host había llegado a una llamada indirecta a `0x8283DB38` desde `sub_82727C08` (`bctrl`, LR `0x82727CC8`). Ese destino cae en el hueco entre funciones descubiertas, y el registro confirma que se estaba usando como callback. Lo agregué al manifiesto como punto de entrada hasta `0x8283DBAC` y generé su código; el registro generado ahora incluye `SetFunction(0x8283DB38, sub_8283DB38)`.
- La ejecución Linux del juego pasó ese callback. Durante 45 segundos emitió continuamente presentaciones GPU de 1280×720 y no volvió a reportar función indirecta no registrada ni fatal. Se cerró al acabar el límite de ejecución; esto valida avance estable del host y la ruta de renderizado, pero no confirma visualmente el menú ni jugabilidad porque la prueba fue offscreen.
- Recompilé el target Switch con el nuevo codegen y el runtime actualizado. El build concluyó y empaquetó el NRO en `switch/kof2002um-nx/kof2002um-nx.nro` (73.656.336 bytes; SHA-256 `527AE5C982BE1E22F6754B2F9519E0D03EE5D6C9F408511D1ECD2D6B75E01AE8`). Verifiqué que la copia es idéntica al artefacto recién generado.
- Es la primera versión que incorpora la entrada del callback detectado en PC; aún hace falta una prueba en Switch para saber si el camino Horizon llega al mismo punto y qué ocurre después. La dirección observada está sustentada como destino de callback en ejecución, pero su límite final se tomó del comienzo de la siguiente función detectada, por lo que conviene vigilar el siguiente log.

## Corrección de paquete NRO en la SD (2026-10-23)

- El reporte enviado tras lanzar desde hbloader bajo title override de Prince of Persia sí correspondía al intento de KOF: el `Program ID` es el título que aporta el title override, no el NRO ejecutado. `Result 2347-0005`, con PC dentro de `hbl`, es el rechazo de hbloader por `NroHeader.magic != NROHEADER_MAGIC`.
- La causa fue un error mío al copiar el artefacto: copié el ejecutable ELF sin empaquetar (`kof2002um_nx`, 73.656.336 bytes, encabezado ELF) y le di extensión `.nro`. El build también había producido el NRO empaquetado correcto (`kof2002um-nx.nro`, 61.876.421 bytes, firma `NRO0`).
- Reemplacé el archivo de la SD en `switch/kof2002um-nx/kof2002um-nx.nro` con ese NRO válido y verifiqué la copia de vuelta: firma `NRO0`, 61.876.421 bytes, SHA-256 `2AC27C5140DBF82FCE33C2F18146DFDCBB2ABC3EAB5EC2597BC580FA0F56F310`. Ahora corresponde repetir el lanzamiento con el mismo title override; el rechazo del encabezado ya está corregido, pero el arranque del juego aún no está validado.

## Objetivo

Primera compilación ejecutable en Switch de la versión Xbox 360 XBLA, con el usuario generando el NSP y este proyecto generando `kof2002um-nx.nro`.

## Estado confirmado

- El paquete de entrada era un contenedor Xbox 360 LIVE/STFS.
- Sus archivos están extraídos en `game/xbla-extracted/`; el ejecutable principal es `default.xex` (4,591,616 bytes).
- Los recursos incluyen 898 `.bin`, 234 `.grc`, vídeos WMV, texturas XPR y bancos de audio XWB/XSB. Hará falta conservar el formato y reproducir las APIs de Xbox 360 que el juego usa.
- Está disponible el SDK de ReXGlue 0.10.0 en `tools/rexglue-sdk/`. Es un recompiler estático de PowerPC Xbox 360, no un conversor automático de XEX a NRO; necesita generar el código del juego y adaptar kernel, gráficos, audio, entrada y archivos para Switch.
- El SDK de Switch/devkitPro (libnx y devkitA64) está instalado en el equipo.

## Bloqueo actual

No se ha generado código recompilado del juego ni un runtime compatible con Switch, y no existe `kof2002um-nx.nro`. La compilación local de la herramienta de ReXGlue no quedó lista: el intento con Clang/MSYS se detuvo en incompatibilidades del runtime POSIX/FFmpeg; el intento alternativo con GCC quedó mal configurado por el manejo de rutas del shell. El paquete binario precompilado fue detectado por Defender y no se ejecutó.

Sin esas piezas, un NRO ahora sería solo un stub de pantalla/arranque: no lanzaría el juego ni serviría como primera prueba jugable. No se empaquetó ni se copió código o datos comerciales dentro del NRO.

## Próxima ruta técnica

1. Compilar ReXGlue en un entorno host coherente y ejecutar análisis/codegen sobre `default.xex`.
2. Medir imports, funciones no soportadas y APIs usadas; decidir si se puede adaptar ReXGlue o si hace falta otro runtime.
3. Implementar/adaptar las capas de kernel, gráficos, audio, controles y filesystem para libnx.
4. Compilar el NRO, leer los datos desde `sdmc:/switch/kof2002um-nx/` y preparar un primer arranque para depuración en consola.

La salida prevista es `kof2002um-nx.nro` en la raíz de este proyecto, cuando exista una compilación que realmente ejecute el título.

## Auditoría de los ports Xbox 360 a Switch (2026-10-22)

- La comparación con `PsiOps` confirma que ya hay una cadena devkitPro/libnx funcional en la máquina: allí se generó un NRO nativo de prueba. Ese NRO carga y dibuja un modelo estático; sirve como referencia de empaquetado, entrada homebrew, EGL y distribución de RomFS, pero no incluye un runtime Xbox 360 ni recompila el juego completo.
- Revisé la guía de SA2 NX, la capa Switch de ReXGlue y los ports de Simpsons/Futari. La barrera `rex/platform.h`/`ucontext.h` ya tiene solución pública: ReXGlue necesita su capa Horizon (`guest_memory_switch`, `threading_switch`, `fiber_switch`, input, audio, archivos y el arranque de libnx). La guía también indica que el juego necesita memoria de aplicación y debe arrancar desde un override/título de 39-bit, no desde el modo applet del Álbum.
- Cloné en `tools/sa2-nx-reference/` la documentación, scripts de compilación, toolchain devkitA64, y parches publicados por SA2 NX. El parche del SDK v0.10.0 pasó `git apply --check` contra el SDK local y ya está aplicado a `tools/rexglue-sdk/`; incluye la capa Horizon para libnx. Conservé los cambios KOF previos del SDK.
- El proyecto KOF generado todavía usa la plantilla de aplicación host (`rexglue_setup_target` + SDL); para Switch debe cambiar a `rexglue_switch_configure_target(... GPU_PLUGINS xenos)` y empaquetar con `rexglue_switch_add_nro`. El plugin Xenos existente requiere Vulkan/NVK en Horizon para la GPU. El `devkitPro` local sí tiene devkitA64/libnx pero no tiene `libvulkan.a` ni la SDK de NVK/Mesa Switch que esperan estos ports.
- El ejecutable generado tiene 171 unidades C++ más registro/arranque y el juego extraído ocupa aproximadamente 0.54 GiB. El NRO debe leer esos datos desde SD; el runtime ofrece `OnConfigurePaths` y un pack de datos NX para evitar miles de aperturas en SD.

### Estado y punto de preparación

- En esta reanudación no he compilado ni empaquetado ningún NRO. La capa Horizon de referencia ya está integrada en el SDK local, pero **todavía no estoy listo para pedir el OK de compilación final**: falta instalar/compilar una NVK SDK compatible (o escoger e implementar una ruta gráfica alternativa), agregar el preset/toolchain Switch a este proyecto KOF, configurar la carga del juego desde SD y conseguir que el target Switch enlace el runtime y las 171 unidades. Después de eso corresponde compilar, inspeccionar el ELF/NRO y dejar una carpeta de primera prueba que no finja ser jugable si el arranque del título falla.
- Referencias primarias: [guía de porting SA2 NX](https://github.com/ChanseyIsTheBest/sa2-nx/blob/main/docs/porting-guide.md), [guía de compilación SA2 NX](https://github.com/ChanseyIsTheBest/sa2-nx/blob/main/docs/building.md), [port Simpsons NX](https://github.com/R-drg/the-simpsons-game-nx), [port Futari NX](https://github.com/ChanseyIsTheBest/futari-nx).

## Preparación para el primer NRO (2026-10-22, estado actualizado)

- La nota anterior de “todavía no estoy listo” quedó superada por este trabajo.
- Apliqué al SDK local el parche Horizon de SA2 NX y el parche complementario para FFmpeg/Horizon. El `rexruntime` compiló completo para devkitA64. También compiló completo el plugin Vulkan/Xenos `rexgpu-xenos`.
- Conseguí la biblioteca NVK de Switch publicada en el release `jhuz0/nvk-switch` Mesa 25.0.7. Descargué el archivo de 3.09 MB, comprobé su SHA-256 (`4AB244F982DC078AAFF8C58640D7CF8A0F3AB43BBF2DAADFD00C02FBF352AF86`) y el `libvulkan.a` de 27.2 MB (`FF65A74B46FE749F6BE73EAC3C8968D70E312C7646F815F26B69F4AA35737D26`). Está en `tools/nvk-switch-sdk/`, con `NOTICE.md`; el release documenta prueba del driver en una Switch V1. Es una dependencia experimental anterior al Mesa 26 usado por algunos ports recientes, de modo que el enlace y el comportamiento de KOF siguen por comprobar.
- Añadí el toolchain devkitA64, integración CMake para la aplicación Switch y enlace estático de `rexgpu-xenos`. Corregí el target de fuentes recompiladas para que el build Switch realmente compile las unidades generadas; compilar `kof2002um_nx_recomp` completó para ARM64 (incluye 171 particiones más init y tabla de registro). La función de registro de 60 mil direcciones usa `-O0` porque es de inicio y en `-O3` el compilador tardó muchos minutos sin aportar rendimiento al juego. El runtime apuntará a `sdmc:/switch/kof2002um-nx/game`; la carpeta de esa ruta debe contener la copia propia extraída del juego. El build dejará el nombre solicitado `kof2002um-nx.nro`.
- La configuración CMake Switch se completó. **No ejecuté el target `kof2002um_nx` ni se generó ningún NRO**; ese target hace el enlace final y empaqueta `kof2002um-nx.nro`. Queda pedir la autorización del usuario para intentarlo. Un enlace exitoso demostraría que el artefacto está construido, pero no que KOF ya arranque o sea jugable en consola; esa validación viene con la primera prueba.
- Referencia NVK y SHA-256 publicados: [release NVK for Switch — Mesa 25.0.7](https://github.com/jhuz0/nvk-switch/releases/tag/nvk-switch-mesa-25.0.7-r1). El tarball verificado es `4AB244F982DC078AAFF8C58640D7CF8A0F3ABBBF2DAADFD00C02FBF352AF86`; la biblioteca es `FF65A74B46FE749F6BE73EAC3C8968D70E312C7646F815F26B69F4AA35737D26`.

## Revalidación de toolchain (2026-10-22)

- Creé junctions locales `D:\KofBuild`, `D:\KofGcc` y `D:\KofClang` para evitar espacios en rutas durante CMake.
- Reintenté construir `rexcodegen` desde el target de CMake. Aunque compilar directamente un `.c` simple funciona, Ninja falla al ejecutar los comandos de compilación con código 127 dentro de este entorno MSYS/Windows.
- La copia de Clang disponible apunta a `x86_64-pc-windows-cygnus`; al compilar usa cabeceras incompatibles/faltantes con FFmpeg, y el SDK también arrastra cientos de objetivos FFmpeg aunque se pida solo `rexcodegen`.
- Hay un instalador LLVM 23.1.4 en caché; todavía no se logró extraer una toolchain host Windows funcional. El tarball precompilado de ReXGlue sigue sin ejecutarse.
- No se alteró el código de ReXGlue para ocultar el fallo ni se generó un NRO vacío. La carpeta `src/` está preparada, pero el port real aún no tiene runtime ni código recompilado.

## Reanudación y diagnóstico de compilación (2026-10-22)

- Reanudé el entorno correcto `Ubuntu-26.04` de WSL; el nombre `Ubuntu` no existe en esta máquina. El build host ahora compila el código recompilado hasta la unidad `kof2002um_nx_recomp.62.cpp`, tras procesar las 349 salidas del codegen.
- El primer error no era una API Xbox faltante, sino ocho `goto` a etiquetas que el codegen dejó en otra función (`sub_82A7B188`, dentro de `sub_82A7B038`). Añadí la relación de chunk al manifiesto (`0x82A7B188` dentro de `0x82A7B038`, fin exclusivo `0x82A7B754`) y corregí en `function_scanner.cpp` la prioridad del chunk frente a la detección de callable/prologue.
- Recompilé `rexglue` y forcé el codegen leyendo el manifiesto actualizado, pero el resultado todavía mantiene las funciones divididas. El header de `FunctionScanner` tiene soporte para chunks, pero la búsqueda en el SDK no encontró llamadas a `setChunks`; el codegen sigue emitiendo etiquetas cruzadas. Falta integrar correctamente esa configuración en el flujo de análisis o encontrar una alternativa de manifiesto que la versión 0.10 sí consuma.
- La configuración CMake generada también se reescribe con cada codegen y pierde el parche local que permite consumir el SDK como subdirectorio; por eso hubo que reponerlo para cada nuevo ciclo de build. Hay que hacer persistente esa integración antes de seguir iterando.
- Aún no existe `.nro`. Aunque se logre completar el build host, eso no constituye un port: el objetivo sigue requiriendo un runtime/puente libnx y APIs de Xbox 360 compatibles con la GPU Xenos.

## Recursos auxiliares añadidos (2026-10-22)

- Añadido el playbook Agent Skill de reverse engineering a `.agents/skills/reverse-engineering/` para flujos de triage y análisis estático. Su cobertura es general y no específica de Xbox 360.
- Clonado `sp00nznet/360tools` en `tools/360tools-reference/` como guía de flujo y comparación ReXGlue para XEX; está orientado a ejecutables PC, no a Switch.
- Se conserva el clon fuente de la skill en `tools/reverse-engineering-skill-reference/` para rastrear licencia, referencias y cambios.
- No se instaló ni ejecutó un servidor MCP de Ghidra. La skill `ghidra-re-skill` revisada está enfocada en Apple Mach-O; se descartó por no ser una buena correspondencia para PowerPC/XEX.
- El SDK ReXGlue y devkitPro/libnx ya estaban disponibles en este equipo. Estos recursos agregados no resuelven automáticamente el codegen bloqueado ni implementan las APIs Xbox 360 para Switch.

## Reanudación con límite de una hora (2026-10-22)

- El codegen de ReXGlue ahora pasa las seis validaciones que fallaban. La causa era que los chunks configurados se guardaban en el grafo, pero la rutina real `discoverBlocks()` solo aceptaba un límite contiguo derivado de PDATA. `phase_discover.cpp` extiende el límite del padre hasta el final del chunk configurado, manteniendo los saltos dentro de la misma CFG. ReXGlue generó 171 unidades C++ y registró 0 errores de análisis.
- Hice persistente el uso de targets padre de ReXGlue en la plantilla `resources/templates/init/rexglue_cmake.inja`; corregí el enlace de `rex::ui` al host app y habilité en `src/kernel/CMakeLists.txt` el archivo de compatibilidad USB camera que el SDK ya tenía pero excluía del build. Instalé en WSL las cabeceras X11/XCB/Wayland que el enlace host necesitaba.
- El ejecutable host recompilado se creó en `tools/rexglue-sdk/out/linux-amd64/kof2002um_nx` (84,075,872 bytes; SHA-256 `8A0C4ECCF6EA5BC12D1F2E0F376AFD2D6B110933279E983DABF62CB207885080`). Compilación verificada; todavía no he validado el comportamiento jugable del título.
- No existe `kof2002um-nx.nro`. La primera prueba de compilación directa de una unidad recompilada con devkitA64 falla dentro del runtime antes de llegar al juego: `rex/platform.h` rechaza el target de Switch y `rex/thread/fiber.h` incluye `ucontext.h`, que no está disponible en devkitA64. El runtime de ReXGlue sigue escrito para Windows/Linux/macOS; además los backends de interfaz/gráficos/audio no están adaptados a libnx/deko3d. No es correcto copiar el ejecutable host ni fabricar un NRO stub como si fuera el juego.
- Para completar el NRO hacen falta una capa de plataforma Horizon/libnx (memoria, hilos/fibras, reloj, entrada y archivos), una ruta de presentación Xenos compatible con Switch (probablemente renderer propio sobre deko3d), audio XMA/Xbox, sustituciones de imports que faltan, compilación cruzada de las 171 unidades y prueba en consola/emulador. Ese bloque excede el límite de una hora y continúa pendiente.

## NRO ARM64 generado (2026-10-22)

- Continué el build autorizado. Corregí la ruta de `switch.ld` para que no se corte por el espacio de `D:\Ports Swtch` y resolví los aliases débiles de las funciones recompiladas que GCC devkitA64 no generaba con la macro por defecto.
- El linker del NVK incluido en esta versión exporta `vkGetInstanceProcAddr`, pero no un símbolo global `vkDestroyInstance`. Añadí al runtime Switch el wrapper que resuelve esa función mediante `vkGetInstanceProcAddr`.
- Pacman tenía su base local incompleta y no podía instalar los portlibs. Extraje los tres paquetes oficiales que ya estaban descargados en su caché (`switch-libexpat`, `switch-libzstd`, `switch-zlib`) directamente al prefijo devkitPro; las bibliotecas requeridas quedaron disponibles para el enlace.
- `cmake --build out/switch-config5 --target kof2002um_nx --parallel 8` terminó con éxito. El NRO solicitado existe en la raíz como `kof2002um-nx.nro` (61,868,229 bytes), su encabezado contiene `NRO0`, y su SHA-256 es `1DAE7138A3483CF45FBD71DF2D2176B6A8D651CD8CEFEF961AACF5697190804C`. El archivo interno `kof2002um_nx.nro` tiene el mismo tamaño y hash.
- El juego todavía no está validado en hardware. El NRO requiere los datos extraídos bajo `sdmc:/switch/kof2002um-nx/game`; que compile y tenga cabecera NRO válida no demuestra todavía que pase el arranque, renderice correctamente o sea jugable. Además, el backend Vulkan/Xenos y el audio Xbox requieren prueba real en Switch.

## Paquete para primera prueba y diagnóstico (2026-10-22)

- Preparé `switch/kof2002um-nx/` para copiarlo directamente a la raíz de la microSD. Contiene el NRO, `kof2002um_nx.toml` con logging a archivo en nivel debug y la carpeta `game/` con los datos extraídos.
- Copié los 1.259 archivos de `game/xbla-extracted/` directamente a `switch/kof2002um-nx/game/` (580.989.832 bytes). Verifiqué que el `default.xex` fuente y la copia tienen el mismo SHA-256 (`088E8D0F2D3DFC1EB1CFDD06213C5BDDE01A40A1EEBC16A5A44928FC507AA18D`). La ruta SD en el runtime es `/switch/kof2002um-nx/game` y se resuelve en `sdmc:/switch/kof2002um-nx/game`, así el runtime encuentra `game:/default.xex`.
- El SDK ya proporciona tres salidas útiles, todas junto al NRO: `logs/kof2002um_nx_NNN.log` (registro de ReXGlue en debug), `logs/rex/rex_stderr.log` (stderr, incluyendo mensajes del runtime/Mesa) y `logs/rex/rex_crash.log` (abort/exit con stack y PC). El TOML activa `log_to_file`, así que stderr también se redirige.
- Añadí `switch/Collect-KofLogs.ps1`: en Windows, con la tarjeta montada, `powershell -ExecutionPolicy Bypass -File .\Collect-KofLogs.ps1 -SdRoot E:\` crea en el Escritorio un ZIP con todos los logs y la configuración usada. El script pasó el análisis sintáctico de PowerShell.
- `switch/kof2002um-nx/LEEME.txt` explica la ruta exacta, la copia a SD, el lanzamiento mediante title override para disponer de memoria completa y cómo enviarme los logs. Aún debe confirmarse el arranque en la consola.

## Diagnóstico de `rex_crash.log` y corrección (2026-10-22)

- Los dos logs principales avanzaban hasta inicializar audio; `Too few processor cores` era una advertencia de afinidad, no el fallo.
- `rex_crash.log` dio la causa: `std::filesystem::filesystem_error: status: Invalid argument [sdmc:/switch/kof2002um-nx/sdmc:/switch/kof2002um-nx/game]`. En Horizon libstdc++ interpreta `sdmc:/...` como ruta relativa y `Runtime::SetupVfs` le antepone el directorio del NRO; el resultado duplica el prefijo de SD y `status()` lanza una excepción.
- Cambié la ruta de datos en `src/recompiler/src/kof2002um_nx_app.h` a `/switch/kof2002um-nx/game`, la forma absoluta que el port Switch de ReXGlue espera y que libnx resuelve en la SD. El rebuild completó y el NRO empaquetado tiene cabecera `NRO0`; lo actualicé en `switch/kof2002um-nx/kof2002um-nx.nro` (61.868.229 bytes, SHA-256 `52C421A01314820B72BC3A5FA6408ADABFCDD8C2AD2C63120E9FD704C9628AB2`). Falta repetir la prueba en consola; pueden aparecer errores posteriores una vez superado este cierre.

## Diagnóstico de pantalla negra y asset pack preconstruido (2026-10-22)

- Los logs 001/002 adjuntos son de la versión anterior y repiten el fallo de ruta ya corregido. El log 003 usa `/switch/kof2002um-nx/game`, supera la inicialización del audio y entra en `[pack]` para empaquetar recursos por primera vez.
- El último progreso reportado fue `packing 100 %`, `verifying 20 %`. `rex_profile.log` registra `game 0.0 fps`, guest con solo 26 MB backing / 7 MB mapeados y cero submits GPU. No hay un nuevo error fatal en la información adjunta; el título todavía no había llegado a renderizar.
- Para que el primer arranque no tenga que crear en la Switch un pack de ~581 MB, generé `game.nxpack` y `game.nxidx` en PC desde los datos del juego. `build_asset_pack.py --if-stale --verify` comprobó correctamente los 1.259 archivos y el checksum integral del pack.
- Copiar los dos archivos de pack al lado del NRO permite que `Runtime::SetupVfs` monte el pack preconstruido y omita la etapa de creación/verificación local. En la SD se puede retirar la carpeta raw `game/` después de copiar ambos packs; el XEX y los recursos están dentro del pack. La copia raw se conserva en el directorio fuente del proyecto.
- Siguiente prueba: copiar `game.nxpack` y `game.nxidx` a `sdmc:/switch/kof2002um-nx/`, ejecutar el NRO y capturar el siguiente log principal. Si se mantiene en negro, los nuevos logs deberían mostrar si montó el pack y qué ocurrió al cargar `default.xex`.

## Siguiente prueba tras el cierre en audio (2026-10-22)

- Los registros nuevos, tras borrar `game/` de la SD, son idénticos en la causa: `game.nxpack` monta correctamente y el fallo ocurre tras `XFileXctdCompressionInformation`, con acceso inválido del invitado `0x4A074CF2`. Por tanto la carpeta raw no era la causa.
- Revisé upstream ReXGlue y encontré la corrección específica: cuando el VFS sirve bytes planos debe responder en `X_FILE_XCTD_COMPRESSION_INFORMATION` con el campo `unknown = 0`, éxito y longitud de 4 bytes. El `INVALID_PARAMETER` anterior hacía que algunos juegos no tomaran la ruta comprimida ni la no comprimida.
- Apliqué esa corrección en `tools/rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_io_info.cpp`. También configuré el cvar superior `gpu_plugin = "xenos"`; el SDK lo convierte a esa clave desde TOML y Switch tiene el plugin enlazado estáticamente.
- Rebuild incremental con `cmake --build out/switch-config5 --target kof2002um_nx -j 2`: éxito; recompiló runtime, enlazó el NRO y ejecutó el paso de empaquetado. Copié el nuevo artefacto a `switch/kof2002um-nx/kof2002um-nx.nro`; SHA-256 idéntico a la salida de build: `C3B00CFB9BBB2C67EF6D84F8BA9DF99EAC136A77333A3098D9224FFCFFC8F4E4`.
- La siguiente validación necesita hardware: reemplazar NRO y TOML en SD, limpiar logs viejos, ejecutar y recopilar nuevos logs. La compilación no garantiza que el juego arranque ni que el render Xenos funcione correctamente.

## Prueba posterior al fix XCTD (2026-10-22)

- Los intentos 001 y 002 muestran que el fix XCTD sí llegó al NRO: ya no aparece el error `XFileXctdCompressionInformation unimplemented`. Sin embargo, el acceso inválido `0x4A074CF2` continúa, así que la consulta XCTD no era la única causa.
- Los logs también demuestran que no se activó la GPU: `gpu_plugin not set` aparece tres veces y el runtime inicia en native rendering mode. Detecté un error mío en el TOML: agregué `gpu_plugin` después de `[log]`, así que TOML lo interpretó como `log_gpu_plugin` en vez de la opción global.
- Corregí `switch/kof2002um-nx/kof2002um_nx.toml`, colocando `gpu_plugin = "xenos"` antes de `[log]`. No requiere recompilar el NRO; reemplazar el TOML de la SD por el nuevo es suficiente para verificar que la GPU se cargue. El log correcto deberá decir que cargó xenos y no mostrar `gpu_plugin not set`.
- Una vez habilitada la GPU, si persiste el acceso inválido, la nueva traza sirve para separar si el plugin llega a inicializarse antes del crash. Las direcciones de retorno mapean a funciones del juego alrededor de `0x8296107C`, `0x827236F0`, `0x82718060` y `0x826B644C`; el fallo ocurre inmediatamente tras registrar el cliente de audio y resolver `\\media\\sounds`.

## Cierre de GPU al inicializar Vulkan shared memory (2026-10-22)

- El log nuevo confirma que `gpu_plugin = "xenos"` ya se leyó correctamente: el plugin está enlazado, creó una swapchain 1280x720 y la GPU llega a crear el hilo de comandos.
- El cierre ya no es el anterior fallo de audio. NVK/Tegra no presenta ningún tipo de memoria `DEVICE_LOCAL` compatible con el gran buffer de shared memory Xenos de 512 MB. `VulkanSharedMemory::Initialize()` lo trataba como requisito obligatorio y retorna fallo; `CommandProcessor::WorkerThreadMain()` luego llama `FatalError("Unable to setup command processor internal state")`, que coincide exactamente con `rex_stderr.log`.
- Ajusté `src/graphics/vulkan/shared_memory.cpp`: primero prefiere memoria device-local, pero si el driver no la ofrece toma un tipo compatible de `VkMemoryRequirements.memoryTypeBits`, permitido por Vulkan, y deja que la asignación confirme su viabilidad. La compilación incremental recompiló el plugin Xenos, enlazó y empaquetó el NRO correctamente.
- NRO actualizado en la carpeta SD: 61.872.325 bytes, SHA-256 `34D5B1066A395AF46793E1B6BA039BF7D0676DA304514BD2F85CE4F34A8A9134`. TOML conserva `gpu_plugin = "xenos"` a nivel superior.
- Próxima comprobación en Switch: reemplazar NRO (y mantener el TOML actual), borrar logs, ejecutar. El nuevo log debe mostrar `no device-local Vulkan memory type... trying compatible memory type mask ...`; luego veremos si asigna los 512 MB y configura el command processor. La compilación no sustituye esta comprobación en hardware.

## Inicio corto: Xenos activo, crash al recorrer datos de audio (2026-10-22)

- El log nuevo confirma que está usando el NRO con fallback de memoria y el TOML correcto. `xenos` enlaza, crea una swapchain 1280x720, inicia el hilo GPU y ya no falla al crear el buffer Xenos; aparece `trying compatible memory type mask 00000001` y no aparece el fatal de command processor.
- El XEX y el audio llegan a arrancar. A los ~3 s la ruta `\\media\\sounds` desencadena el mismo guest access violation al leer `0x4A074CF2`.
- Resolví el host PC con los símbolos del ELF no stripped: `image+0x24CF41C` pertenece a `sub_82965280`, en el bucle que recorre registros de una estructura de datos del juego. El host instruye una carga `lbz` desde `r30 + 2`; `r30` tiene `0x4A074CF0`, dentro del espacio virtual invitado pero en una página sin mapear. El call stack lo enlaza a `sub_82960FB0` y sus llamadores; la ruta activa es la inicialización tras resolver `media\\sounds`.
- `rex_stderr.log` ahora solo avisa que NVK no es Vulkan conformant; el log principal añade un error no fatal de NVK al consultar memoria de sistema. No son el cierre: después de ellos el título carga el XEX, registra el driver de audio y el crash handler apunta al fault invitado.
- Siguiente trabajo: rastrear de dónde sale el buffer `r30` (objeto de lectura XACT/XSB o datos de enumeración) y si la ruta `media/sounds`/el contenido del paquete produce un offset inválido. No preparo un NRO de tanteo hasta identificar una corrección defendible.

## NRO diagnóstico para seguir el puntero de audio (2026-10-22)

- Instrumenté temporalmente los dos puntos identificados en el código recompilado: parámetros que `sub_82960FB0` entrega a `sub_82965280`, y el puntero de datos/offset que el lector calcula antes del primer registro. Los avisos llevan el prefijo `KOF audio/resource wrapper` y `KOF stream parser`.
- Rebuild del NRO completo correcto; hash del artefacto y de la copia para SD: `0297BBB842574FE9E3521762D9058BDEFEBF6100FE4B50C1D1C1D8E24355C17D`.
- Es una compilación diagnóstica con el fix de memoria Vulkan previo, no un fix del crash de audio todavía. Reemplazar solo el NRO; mantener TOML `gpu_plugin = "xenos"`. Tras un único arranque y cierre, enviar el log principal nuevo y rex_crash. El log debe incluir tres avisos KOF, con punteros, offset y conteo de registros.

## Logs del 22-10-2026: cierre reproducible en el parser de recursos de audio

- Los dos intentos incluidos en `kof2002um_nx_002 (4).log` y `rex_crash (8).log` repiten el mismo fault invitado al leer `0x4A074CF2`, desde `sub_82965280` (`lbz r11,2(r30)`). El reproductor ya montó el pack de 554,1 MB y la ruta que precede al fallo es `\\media\\sounds`; NVK solo deja un aviso de conformidad y no es la causa inmediata.
- El diagnóstico previo demuestra que `sub_82965280` recibe `self=0x400A8CE0`, `input=0x4009FCF0`; su tercer método virtual devuelve un campo `0x09FD5000`, que sumado al input produce la dirección sin mapear `0x4A074CF0`. Aún no se puede atribuir con certeza a una API XACT concreta ni afirmar que el offset sea erróneo; hay que identificar las implementaciones de los tres métodos virtuales/las estructuras del objeto.
- Añadí instrumentación tras cada llamada virtual para registrar vtable, target, retorno, contador y campo 65. Recompilé el target Switch correctamente y actualicé `switch/kof2002um-nx/kof2002um-nx.nro` (61.876.421 bytes; SHA-256 `2287D21B7F49604426001E607D0AAA3629B2F79042524F8419DDD6BAA3FB94E8`). El TOML mantiene `gpu_plugin = "xenos"` en el nivel superior.
- Esta compilación sigue siendo diagnóstica: aún no corrige el fallo. Reemplazar el NRO de la SD por esta versión, borrar los logs previos, hacer un solo arranque y compartir el log nuevo junto a `rex_crash.log`; los registros `KOF stream vcall1/2/3` deben revelar qué getter está devolviendo el offset.

## Seguimiento de logs con vtable (22-10-2026)

- Los nuevos logs confirman que las tres llamadas virtuales usan la misma función `0x82964D70`; esta función solo devuelve el campo `+144` de `self`, por lo que retorna intacto el puntero `0x4009FCF0`. La dispatch de vtable funciona; el cierre sucede después al sumar al puntero el valor leído en `input+65` (`0x09FD5000`) y leer `data+2`.
- Esto acota el problema al valor/semántica del objeto de entrada `0x4009FCF0` o a cómo el parser interpreta su layout. Todavía no hay base para parchear ese offset ni culpar a NVK o al montaje del pack.
- Añadí otro diagnóstico que registra el descriptor completo en `sub_82960FB0` y las palabras del objeto de entrada entre offsets +48 y +92 para ver cómo se forma el buffer/offset. Rebuild Switch correcto; `switch/kof2002um-nx/kof2002um-nx.nro` actualizado (diagnóstico, no corrección). Debe reemplazarse en la SD antes de la siguiente prueba.

## Descriptor y memoria del reader en logs 22:01 (22-10-2026)

- `kof2002um_nx_001 (7).log` usó la compilación diagnóstica nueva. Monta correctamente los 1.259 archivos del pack y cae en el mismo parser de audio.
- El descriptor que llama al parser está en `0x702AFEC0`; el campo +4 contiene el objeto de entrada `0x4009FCF0`. Las tres llamadas virtuales retornan ese mismo objeto mediante `0x82964D70`.
- El volcado del reader muestra +64=`0x4009FD50`, +68=`0x0000043C`, +72=`2`; el `lwz` del juego desde +65 produce `0x09FD5000`, y el parser suma eso al reader dando `0x4A074CF0`, sin página mapeada. El dato confirma una disposición de campos/offset empaquetada o una interpretación del stream que no encaja con lo que el parser espera. Todavía no prueba cuál componente crea el estado incorrecto; no se parcheó el offset a ciegas.
- NVK stderr no informa un error fatal y el fault sigue siendo la lectura guest `0x4A074CF2` en `sub_82965280`. Próxima investigación: rastrear la procedencia del objeto `0x4009FCF0`, su buffer `0x4009FD50` y el campo de posición/offset hasta las funciones que crean/llenan el reader, y contrastar la estructura del resource container de audio.

## Lectura del dump de reader y siguiente dato a capturar (22-10-2026)

- El campo `+64` apunta justo a `input+96` (`0x4009FD50`), y `+68` declara tamaño `0x43C` (1084 bytes), `+72` vale 2. El valor de `+65` que el parser lee corresponde a los bytes desalineados que cruzan el campo `+64` y el siguiente byte; por ello aún falta distinguir si es un campo empaquetado intencional o si el lector entrega un objeto distinto al que espera el parser. No modificar la instrucción original con la evidencia actual.
- Preparé otro diagnóstico que vuelca los primeros 32 bytes del payload señalado por `+64`, además de descriptor y palabras de cabecera existentes. Build Switch completado; NRO actualizado para una prueba. Ese payload empieza dentro del mismo bloque huésped según el puntero informado; si el log mostrara fallo al instrumentarlo, quitar la lectura adicional.

## Revisión de assets XACT y dump de payload (22-10-2026)

- Revisé cabeceras de los archivos de `media/sounds`: `BGM_soundbank.xsb` (4.746 B), `se_soundbank.xsb` (91.742 B), `BGM_wavebank.xwb` (69.029.888 B), `se_wavebank.xwb` (31.838.208 B) y `voice.xgs` (vacío). Las cuatro cabeceras no comienzan por las firmas habituales de XACT (`SDBK`, `WBND`, `XGSF`); esto puede significar formato específico del título, datos sin cabecera o que el volcado no corresponde al formato estándar. Falta confirmar el formato antes de concluir que sean banks XACT normales.
- La próxima revisión de hardware requiere leer los primeros 32 bytes del payload que el reader apunta con `+64`; añadí ese dump (ptr, tamaño, primeros 8 dwords) al NRO. El build terminó, se actualizó la copia en `switch/kof2002um-nx/`; SHA-256 `DBF27390C377869E9E12CBDA3209D1DF6E895DFB3F963ED0CB237ED3066C098C`. No constituye una corrección del crash.

## Diagnóstico del mensaje de carga de audio (2026-10-23)

- La captura muestra el diálogo XBL Arcade: `Load failed. The storage device has been removed or the data is corrupted.` Los logs llegan a resolver `\\media\\sounds`; no es un fallo de inicialización de la GPU.
- El parser recibe `size=0`. En los recursos fuente de `game/media/sounds`, `voice.xgs` mide exactamente 0 bytes; los otros cuatro recursos de audio tienen datos. La hipótesis principal es que el juego intenta parsear ese placeholder vacío como configuración global XACT/XGS y aborta la carga.
- El diagnóstico anterior imprimía `ctx.r29` antes de copiar el resultado de `r3`, por lo que el status mostrado allí no era fiable. Corregí el registro para guardar parámetros antes de llamar al parser y reportar el retorno real de `r3`.
- Añadí la opción configurable `asset_pack_ignore_empty_voice_xgs` (por defecto apagada). El TOML de KOF la activa, así el VFS omite únicamente el `media/sounds/voice.xgs` vacío y registra esa decisión. No se altera el archivo original ni el pack.
- Build Switch completado. NRO actualizado en `switch/kof2002um-nx/kof2002um-nx.nro`, 61.880.517 bytes, SHA-256 `6321BCDDEF421B30DC87045482E73117DE01B9DB0D4FDD3E4AF4BDB2E175371B`; magic `NRO0`. El TOML en la carpeta está actualizado. Aún requiere prueba en hardware: si el archivo vacío era opcional, debería pasar el diálogo; si el juego exige ese XGS, el log nuevo incluirá la decisión de omitirlo y el retorno real del parser para afinar el siguiente cambio.

## Resultado de omitir `voice.xgs` y siguiente corrección (2026-10-23)

- La última prueba confirma que el juego abre `game:\\media\\sounds\\voice.xgs` y, al ocultarlo, falla en `NtCreateFile` con `0xC000000F` (`STATUS_NO_SUCH_FILE`). Por eso no se debe quitar del VFS: el archivo vacío tiene que seguir existiendo.
- Cambié la corrección: el archivo continúa montado y visible; si el tamaño recibido por el inicializador es cero, el código salta el parser de bytes vacíos y sigue por la ruta común de inicialización. El parser ya no debe recorrer memoria ajena cuando no tiene datos.
- Corregí el log del parser para que use el retorno real de `r3` y los argumentos capturados antes de la llamada. En el siguiente arranque deberá aparecer `KOF audio parser skipping empty resource` si ese es el archivo que dispara el caso.
- Rebuild Switch completado. NRO actualizado en `switch/kof2002um-nx/kof2002um-nx.nro`, 61.876.421 bytes, SHA-256 `44BEA6D42D54866940536602B27E36CCB129F725A18B53C743E4098E492973A4`; magic `NRO0`. Requiere una nueva comprobación en hardware.

## Crash tras saltar la configuración XGS vacía (2026-10-23)

- La última prueba sí ejecutó `KOF audio parser skipping empty resource`, así que la condición de tamaño cero funcionó y el juego avanzó más allá del fallo de audio. Después ocurrió otro fallo: `rex_crash` y el log identifican una lectura invitada inválida en `0x2630BCD3` dentro de `sub_827184A0`, invocada durante `sub_82219C58` al inicializar/decodificar tablas de recursos del juego. La pantalla de Switch es el aviso genérico de cierre por error; no es el diálogo de almacenamiento de XBL.
- El log muestra que justo antes se resuelven `\\data\\OBJ2`, `\\data\\SEQ`, `\\data\\BASE` y `\\data\\FIGHT`. Las advertencias de lecturas directas del pack indican que Horizon rechazó esos destinos para IPC, pero el runtime completó cada lectura por un buffer de copia; no son el error fatal demostrado.
- Añadí registro acotado de aperturas y lecturas de `game:\\data\\` (ruta, tamaño, offset, bytes y estado) para determinar qué recurso alimenta el puntero/longitud inválidos. Los logs actuales no registran esos datos.
- Build Switch completado. NRO diagnóstico en `switch/kof2002um-nx/kof2002um-nx.nro`, 61.880.517 bytes, SHA-256 `538FF9DAB0ADBDBE4A0346DCE2561591D86D8C7CB6B8CF971A02981DBE5B6B04`; magic `NRO0`. Requiere otra captura en hardware para registrar la ruta del recurso antes del cierre.

## Crash actual tras saltar el XGS vacío (2026-10-23)

- Los adjuntos `kof2002um_nx_001 (14).log` y `rex_crash (16).log` confirman que el mensaje visible sigue siendo genérico, pero la falla técnica ya es distinta al parser de audio: ese parser saltó `voice.xgs` vacío y el juego llegó a cargar recursos de pelea. El cierre es una lectura PPC inválida en `0x2630BCD3` dentro de `sub_827184A0`, invocada con LR `0x82219ED8` desde la decodificación de la tabla asociada al `P_DATAA.BIN` abierto justo antes.
- El rastro de `NtReadFile` estaba filtrando `file->path()` solo como `\\data\\`, aunque el VFS entrega rutas relativas como `data\\fight\\p_dataa.bin`. Corregí el filtro para aceptar ambas formas; así la próxima ejecución registrará offset, solicitado, bytes leídos y estado para los assets de `data`.
- Recompilé el target Switch. NRO diagnóstico actualizado en `switch/kof2002um-nx/kof2002um-nx.nro`, SHA-256 `1376C3FDCAB49EB57A6BA70BA2B11FB769D1032F56742D4279F36A001D2B63E9`. No es todavía un arreglo del crash; es una compilación para capturar la lectura que lleva al parser a datos incoherentes.

## Lecturas confirmadas y diagnóstico PPC puntual (2026-10-23)

- `kof2002um_nx_001 (15).log` demuestra que la corrección del filtro funcionó: se registran todas las lecturas previas al cierre. `P_DATAA.BIN` se leyó completo: offset 0, 0x2000 bytes; offset 0x2000, 0x7a bytes; ambos retornaron `status=0`. La entrada del `.nxpack` es byte por byte idéntica a `game/xbla-extracted/data/fight/p_dataa.bin` (tamaño 0x207a). No hay evidencia de truncamiento o corrupción introducida por el pack o la lectura de SD.
- El cierre se repite en `sub_827184A0` durante la tabla de `P_DATAA.BIN`: lectura PPC inválida en 0x2630BCD3; el contexto registra `ptr=0x2630BCD0`, `count=0x0652AF83` y LR 0x82219ED8 (el logger de contexto aclara que algunos registros PPC pueden ir retrasados). El valor de count es incompatible con la tabla de tamaño 0x207a; apunta a un puntero/offset mal formado antes del helper, no a una lectura corta.
- Añadí un log condicional al helper de byte swap para capturar sus argumentos al entrar cuando el puntero o el conteo son anómalos. Recompilé y copié el NRO diagnóstico a `switch/kof2002um-nx/kof2002um-nx.nro`. SHA-256: `34EF08D103ED1B671F5919D59E8B93CEEB7380545C713EB83E3CFCA6DD784ECF`. La compilación solo añade diagnóstico; no oculta ni corrige el crash.

## Reproducción con datos instalados y trazado del offset (2026-10-23)

- `kof2002um_nx_001 (17).log` confirma que la SD ya montó `game.nxpack` y avanzó hasta leer `P_DATAA.BIN` completo (0x207a, dos lecturas exitosas). El aviso anterior de `game_data_root` desapareció.
- La instrumentación nueva captura al entrar al helper: caller `0x82219ED8`, puntero `0x2630BCD0`, count `0x0652AF83`. El crash PPC en `0x2630BCD3` sigue siendo el mismo. El log no apunta a un fallo de Vulkan ni de SD.
- Añadí un volcado de los trece offsets del encabezado de pelea ya convertidos a endian nativo, inmediatamente antes de que el parser calcule los límites inválidos. Recompilé y copié el NRO diagnóstico en `switch/kof2002um-nx/kof2002um-nx.nro`; SHA-256 `F0CE6BD2C7D2ED6BA5F5B0AFA8CB15A473CB32AADC0428C635B69B4E7D97956E`, magic `NRO0` en offset 0x10. Es solo diagnóstico; no cambia los datos ni evita el fallo.

## Revisión de los ports de referencia y NRO diagnóstico de cabecera (2026-10-23)

- Revisé la matriz y notas locales de los ports Xbox 360 para Switch (SA2, Futari, The Simpsons, Project 8, NFS Carbon, BattleBlock y otros), incluida la documentación de fallos. Hay casos de OOM de XMA, bloqueos de mezcla y sincronización de video/audio, pero no aparece documentado el diálogo Xbox LIVE “storage device removed / data is corrupted” ni una lectura PPC equivalente al puntero imposible de KOF. Esto coincide con la búsqueda previa en issues públicos: no encontré un caso equivalente; la ausencia en las referencias no demuestra que nunca haya ocurrido.
- Los adjuntos que motivan esta build reproducen el error al interpretar `P_DATAA.BIN`: las dos lecturas del archivo llegan completas (tamaño `0x207a`, estado 0), pero la tabla relativa deriva en puntero/conteo fuera de rango. Esto separa fallo de interpretación/estado del juego de truncamiento de lectura; aún no demuestra si el formato de entrada está dañado o si la ruta PPC/recompiler lo procesa mal.
- Añadí a `sub_82219C58` dos registros diagnósticos: palabras de entrada antes del primer byte-swap y las mismas tres palabras después. Compilé el NRO Switch y lo copié a `switch/kof2002um-nx/kof2002um-nx.nro`; tamaño 61.880.517 bytes, magic `NRO0`, SHA-256 `6C53F8D60CD9836BCADD79FD66A68E638DB5FCE139C74F264949966D82F1F754`; la copia coincide con el artefacto de build.
- Es una build diagnóstica, no una corrección del mensaje de datos. En la siguiente ejecución, el log principal debe incluir `KOF fight resource entry` y `KOF fight resource header swapped`; esos valores permiten distinguir si los bytes ya llegan mal al parser o si el problema aparece en el byte-swap/offset.

## Resultado del nuevo registro previo al byte-swap (2026-10-23)

- La Switch ejecutó la build diagnóstica: aparecen `KOF fight resource entry` y `KOF fight resource header swapped`. El fallo sigue en `0x2630BCD3`, caller `0x82219ED8`, con conteo imposible `0x0652AF83`; no avanzó a otra etapa.
- El buffer entra al parser con palabras `A91727C9 5EE57E78 EA27C064`, idénticas a los primeros bytes del `P_DATAA.BIN` del paquete LIVE local (tamaño 8314; SHA-256 `210c742c3f0809dbefe326beb88b090b84531f9005691ad116b45b0bb4f186b8`). Tras el helper se ven exactamente invertidas por palabra (`C92717A9 787EE55E 64C027EA`), que es lo que hace el byte-swap. Las palabras resultantes no forman un offset relativo válido para un archivo de 0x207a bytes.
- No aparece un caso igual en la documentación local de los ports comparados. Los precedentes de audio/XMA, reloj y memoria de BattleBlock/NFS/Project 8 tienen causas distintas.
- Hipótesis de trabajo: el problema precede al byte-swap, en el origen/formato del buffer o en una transformación que debía ocurrir antes del parser. La lectura desde STFS/pack está completa y su payload coincide con el archivo extraído, pero esto por sí solo no demuestra que el contenido STFS tenga hashes válidos ni que sea el formato que espera el parser. No producir otra build que repita únicamente el mismo guard; seguir estáticamente el flujo que prepara `0x83FC3040` antes de `sub_82219C58`.
- Verifiqué además el payload con dos extractores STFS distintos: el extractor local XBLA y el `stfs_extract.py` de la referencia SA2 obtienen del paquete LIVE el mismo `P_DATAA.BIN` de 8314 bytes y el mismo SHA-256 `210c742c3f0809dbefe326beb88b090b84531f9005691ad116b45b0bb4f186b8`. Esto reduce la probabilidad de un error de nuestro extractor, pero no valida los hashes internos STFS ni explica qué decodificación espera el título.

## Repetición de la prueba diagnóstica (logs 21/10? 2026-10-23)

- Los nuevos adjuntos vuelven a ejecutar la build identificada por los marcadores de cabecera (`entry` y `header swapped`). El recurso `P_DATAA.BIN` se abre y lee completo, los primeros bytes son idénticos a las ejecuciones previas, y el mismo conteo `0x0652AF83` termina en lectura invitada inválida `0x2630BCD3` desde caller `0x82219ED8`.
- No hay regresión ni señal adicional respecto de la última captura: el NRO sigue siendo diagnóstico y no funcional. No pedir otra prueba con esta compilación; continuar el análisis estático del flujo de carga/preparación del buffer.

## Análisis adicional de P_DATAA.BIN y P_DATAN.BIN (2026-10-23)

- Consulta externa sobre el dump: la lectura como tabla directa no encaja; en los primeros 64 valores (anchos 2/3/4 bytes, ambos endianess) la monotonía observada queda entre 0.476 y 0.540, y ningún span cabe en 0x207A con escalas probadas. Los valores `ptr=0x2630BCD0` y `count=0x0652AF83` no aparecen literalmente en ninguno de los órdenes. Esto no identifica todavía la transformación ni valida el contenedor STFS.
- Cálculo local de entropía por bloques de P_DATAA: `[0,0x1000)` H=7.9344, 26 ceros; `[0x1000,0x2000)` H=0.3800, 3976 ceros; `[0x2000,0x207A)` H=6.0523, 11 ceros. Por lo tanto, no corresponde describir todo el archivo como un único flujo uniforme cifrado/comprimido.
- Hay un magic `0F F5 12 ED` (big-endian `0x0FF512ED`) en P_DATAA al offset 0x2000; P_DATAN (tamaño 0x155A) lo tiene en 0x1000. Las colas miden 0x7A y 0x55A. En ambas, el dword +4 es `01 00 00 00`; el layout y la interpretación de los demás campos siguen sin confirmarse, así que el magic por sí solo no demuestra que esa cola sea exactamente el dato que consume el parser.
- En P_DATAA, `[0x1000,0x107A)` contiene 120 bytes no nulos repartidos en tres runs (`[0x1000,0x101D)`, `[0x101E,0x106B)`, `[0x106C,0x107A)`), luego hay ceros hasta 0x2000; la cola `[0x2000,0x207A)` también mide 0x7A, pero no es idéntica al rango anterior (1 byte coincide en la misma posición, 100 valores XOR distintos). La igualdad de longitudes no prueba que sean réplicas.
- P_DATAN tiene solo 8 runs no nulos en `[0,0x1000)` y entropía 3.5175 (2733 ceros); difiere de P_DATAA. Próxima evidencia útil: validar SHA-1 de bloques físicos STFS contra entradas L0, comparar prefijos/colas de más `P_DATA*`, y trazar en el XEX quién pasa el buffer a `sub_82219C58` y si se aplica decodificación antes de construir offsets. No generar otro NRO solo con estos hallazgos; la transformación esperada no está identificada.

## Confirmación del puntero de tabla fuera del archivo (log local 2026-01-16, build diagnóstica)

- El log disponible `kof2002um_nx_001 (20).log` contiene el dato adicional `KOF fight offset table`: tras swap, el segundo dword `0x787EE55E` produce `base + offset = 0xFC7B159E` (aritmética guest de 32 bits). Esa es la dirección que registra como `ptr`; los 13 dwords que leyó allí son `29B5A732 8BC99924 4300653F B1560FA1 1E0AF448 2C8F4724 1E0311C0 1F81EBC6 904976EC 60EF292D 42078120 E4B04304 FF2FC23B`.
- `0xFC7B159E` queda muy fuera del buffer `0x83FC3040..0x83FC50BA`; por tanto, esos 13 valores no son una tabla del archivo, sino datos leídos de otra región guest después de seguir un offset inválido. Esto confirma un offset malformado en el encabezado crudo; aún no identifica por qué el archivo no presenta el encabezado esperado (transformación omitida, versión/archivo incorrecto o datos inválidos).
- El `ptr=0x2630BCD0,count=0x0652AF83` posterior se deriva de usar esas fronteras/valores fuera de archivo; no se debe tratar como una estructura válida. Próxima instrumentación útil: registrar y validar cada uno de los 3 dwords post-swap y calcular la tabla objetivo antes de leerla, abortando el diagnóstico si queda fuera del buffer. No alcanza por sí sola para decidir qué formato debería cargarse.

## Crash tras rechazar P_DATAA (logs adjuntos rex_crash 22 / KOF 001 21)

- La ejecución llega a `P_DATAA.BIN`, que se abre con tamaño `0x207a` y se lee completo. La build registra otra vez `KOF fight parser rejected header` para base `0x83fc3040`, offset post-swap `0x787ee55e`, primer dword `0xc92717a9` y tercero `0x64c027ea`.
- El fault nuevo es una **escritura** a `0x5bb8361c`. El contexto/stack la asocia a la llamada de `sub_822C1D80` desde `0x822214f4`, que vuelve a invocar el byte-swap helper (`sub_827184A0`) con count 529. El buffer/tabla apuntado ya es inválido.
- Este fallo posterior no aporta evidencia de que `P_DATAA` sea válido ni es una solución distinta: el guard diagnóstico de `sub_82219C58` retorna solo de esa rutina, sin comunicar error al llamador. El juego sigue cargando y consume sus globals de tabla sin inicializar; eso explica el puntero inválido que aparece después. Evitar interpretar `0x5bb8361c` como un segundo bug raíz.
- La lectura completa, el contenido y el encabezado siguen coincidiendo con los logs anteriores. Próximo paso útil: rastrear por qué el parser espera una tabla de offsets en el encabezado de `P_DATAA` cuando los bytes iniciales post-swap no pueden producir un offset dentro de 8314 bytes; quitar el guard como supuesto fix o pedir que se pruebe esta misma build no sirve. No solicitar una prueba idéntica.

## Ghidra instalado y XEX importado (2026-10-31)

- Instalé JDK 21.0.13 (Temurin) y Ghidra 12.1.3 en `D:\Tools`, más la extensión XEXLoaderWV 12.1.3. Los tres SHA-256 coinciden con los publicados. Ghidra usa el JDK 21 vía `JAVA_HOME_OVERRIDE`; el Java 8 del PATH no cambió.
- Proyecto `ghidra/kof2002um.gpr` con `default.xex` analizado: `PowerPC:BE:64:A2ALT-32addr`, base `0x82000000`, entrada `0x826AFED8`, 38.944 funciones. El loader avisa `out of input bytes (LzxDecompression)`; comprobé que `.text` está completo (`sub_82965280`, `sub_82219C58` empiezan en `mflr r12`); solo queda en ceros la cola de `.reloc`.
- Ghidra tampoco detecta como función el callback `0x8283DB38` (coincide con el hueco que encontró ReXGlue). No se ha modificado nada en el análisis; sirve para rastrear estáticamente quién prepara el buffer `0x83FC3040` antes de `sub_82219C58`.
