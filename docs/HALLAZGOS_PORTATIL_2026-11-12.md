# Hallazgos en portátil (2026-11-12): rama integrada, fusión de la cadena y Kula

Resumen de lo medido en una Switch real (modo portátil, NVK/Vulkan) con la rama `integrar-original` (nuestros cambios +
`tbittech/kof2002um-nx` main). Complementa `RENDER_PERF_PLAN.md` y `RENDERER_DESIGN.md`. Todo son mediciones del log
`rex_profile.log` (ventanas de 10 s); no hay cifras de GPU por frame porque `gpu_submission_timestamps` estaba apagado
(activarlo o el log de draws frena el juego y falsea los fps).

## 1. Build integrado (nuestro SDK + upstream), CPU 1428 / GPU 614,4 / EMC 1331

| Etapa | Frames ≤18 ms | Media | Nota |
|---|---|---|---|
| China (`bgc0`) | 89–92 % | 18,2–18,5 ms (~55 fps) | nuestro build previo: 53–54 fps; la ganancia de upstream sobre nosotros es pequeña aquí |
| Taiwan (`bgf0`) | 97–100 % | 16,7–17,1 ms | 60 fps estables |

Config usada (`switch/kof2002um_nx.toml` + `resolve_texture_store_pages = "186D2,1F52B,185BA"`): cvars de upstream
(`spirv_ps_implicit_lod/static_state`, `spirv_ps_epx_hash=716457010`, `texture_cpu_copy_worker_core=2`,
`vulkan_submit_min_interval_us=8000`, timestamps y `present_painted_profile` apagados) más nuestros
`rt_xfer_skip_depth_to_color/clear_like_depth` y el almacén de texturas de resolve.

Solapes con upstream: ambos atacan la cadena de filtro EPX (ellos por especialización de shader, nosotros con el almacén de
resolve y saltando el blit rojo/azul), los envíos (submit) y las copias de textura. Lo propio nuestro: salto de
transferencias de EDRAM (8,8 → 0,15 ms/frame) y la instrumentación (`[dd]`, cortes de render pass, barreras por llamador).

## 2. Fusión de la ronda del filtro (`chain_fuse`): descartada

Idea: fusionar filtro EPX + 2 copias + resolves de China en menos pasadas (código en `command_processor.cpp`,
`ChainFuseOnDraw/OnResolve`). Resultado medido: 0,00 rondas fusionadas por frame, ~2,0 canceladas, GPU +5 ms, China ~40 fps.
Solo el cambio de blend ya sumaba ~2,4 ms por ronda. Está apagada por defecto (`chain_fuse=false`); no reintentar sin
capturar antes un `[dd]` de China que muestre dónde se desvía el patrón. Los contadores de rendimiento 42–46 pueden chocar
con otros ids: no fiarse de esas cifras.

## 3. Kula: SDM y MAX2 lentas

Síntoma: la SDM (súper desesperación) y la MAX2 de Kula bajan el juego a ~30 fps (frames de 25–35 ms) en cualquier etapa.

Medido (frames lentos vs rápidos, mismos relojes):
- Rápidos ~60 draws; lentos 230–350 draws y 72–91 «pantallas» de scissor por frame (límite superior del área pintada).
- Perfil de pila (`switch_profile_stacks`, solo ventanas lentas): el hilo de comandos pasa ~54 % esperando (`WAIT_REG_MEM`,
  bucle ocioso); el trabajo restante está repartido (IssueDraw, WriteRegister, copias de memoria compartida, mprotect) sin un
  punto caliente. El hilo principal está al ~96 % pero es el spin del D3D esperando a la GPU. Esto **corrige** la nota
  anterior de «limitado por el hilo de comandos»: la CPU no es el cuello dominante.
- Log de draws en frames pesados: aparecen `F975757E` (~156 draws/frame, 3 vértices), `1737D768` (~60) y `E2467BCF` (~22),
  casi todos con blend `01610706`, que los livianos casi no tienen.
- Shaders: `E2467BCF` = color plano (`oC0 = c0*c1`); `1737D768` = 1 lectura de textura + color constante; `F975757E` = ~65 ALU
  sin texturas.
- **Experimento `diag_skip_ps=F975757E5C95CF56`:** ese shader es el **fondo celeste** de la escena (al saltarlo se ve negro) y
  el juego **siguió igual de lento** (300–350 draws, 72–91 pantallas). No es la causa. Tampoco hay excepciones: SEH = 0,
  sin entradas nuevas en `rex_crash.log`/`rex_stderr.log`.

Hipótesis restantes (sin probar): coste por draw de muchos sprites con blend `01610706` (relleno/ROP o cambios de estado en
NVK); efectos que `1737D768`/`72388BBF` dibujan a 1280 px; copias de pantalla extra. Siguiente medición propuesta: timestamps
de GPU solo durante una SDM (sin log de draws) y `diag_skip_ps` con `1737D768` y `72388BBF` uno por uno.

Nota de método: el juego trae un grabador de comandos en el menú de práctica; sirve para repetir la misma SDM/MAX2, pero hay
que dejar pausas entre bloques para separar las ventanas de 10 s.

## 4. Herramientas añadidas

- `tools/switch-ftp/`: subir NRO/toml y **bajar logs** por FTP (`bajar-logs.sh`, `lftp.sh`, `subir-nro.sh`).
- Resolver `image+0x…` de los logs con `aarch64-none-elf-addr2line` (contenedor `kof2002um-nx-build`) y el ELF sin stripear;
  `tools/perf/hot.py` / `hotstack.py` aceptan un log filtrado por ventanas lentas.
