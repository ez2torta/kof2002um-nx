# Referencias: juegos de Xbox 360 recompilados para Switch

Revisión de repositorios públicos el 23-10-2026 para informar el port de KOF 2002 UM. El estado de los proyectos cambia; “jugable” abajo significa que el repositorio lo declara y publica datos de prueba, no una auditoría independiente de cada edición o consola.

## Cómo funcionan los ports de esta familia

ReXGlue recompila estáticamente instrucciones PPC del `default.xex` a C++ y compila ese código a ARM64; no emula la Xbox 360 completa. Eso todavía requiere un runtime Horizon/libnx, memoria guest/kernel, archivos, audio XMA/XAudio, entrada, manejo de fallos, código de juego descubierto incluyendo destinos indirectos, y un renderer. Los proyectos más recientes reemplazan el renderer Xenos/D3D por uno nativo, a menudo NVK/Vulkan o Renut/deko3d. Los ports suelen exigir el ejecutable y los datos exactos del usuario, validar hash/edición, y lanzarse en title takeover/forwarder 39-bit por memoria.

## Proyectos con juego en Switch que el repositorio declara jugable

| Proyecto | Estado publicado / plataforma | Pistas técnicas útiles |
|---|---|---|
| [Mass Effect NX](https://github.com/nebadasSwifty/masseffect-nx) | Prólogo, exploración, diálogo y combate jugables; alrededor de 25 FPS a clocks stock y 960x544; Vulkan/NVK. | Buen ejemplo de juego grande; shaders y pipeline cache creados desde la copia del usuario; dos discos/ediciones requieren manejar orden y archivos. |
| [Need for Speed: Most Wanted NX](https://github.com/StevensND/nfsmw-nx) | Port nativo declarado a 30 FPS en clocks stock, múltiples ediciones regionales. | ReXGlue + renderer Vulkan/NVK; material de referencia para porting y empaquetado/loader. |
| [Sonic Adventure 2 NX](https://github.com/ChanseyIsTheBest/sa2-nx) | 60 FPS stock según documentación; renderer Direct3D-level Renut sobre deko3d en versiones recientes. | Guía de porting detallada: extraer XEX, completar funciones/código no alcanzado por análisis, boot PC, crash triage, renderer y shaders. |
| [Mushihimesama Futari NX](https://github.com/ChanseyIsTheBest/futari-nx) | Juego y DLC, multijugador; ReXGlue + Renut/deko3d. | Caso XBLA con `default.xex` y ocho módulos XEX adicionales; relevante para módulos y contenido adicional. |
| [The Simpsons Game NX](https://github.com/R-drg/the-simpsons-game-nx) | Juego declarado a 30 FPS con clocks de CPU/GPU elevados. | Hereda recompilación PC, capa Switch y renderer de Project 8/NFS; identifica el decoder VP6 como cuello de botella y lo reemplaza por FFmpeg. |
| [Tony Hawk’s Project 8 SW](https://github.com/R-drg/project8-sw) | Free Skate y carrera a ~30 FPS en OLED con GPU a 768 MHz; video de cutscene muestra primer frame, sin audio. | Documenta separación entre archivo/video que sí avanza y reloj de audio que no: indica que un síntoma de audio puede bloquear sincronización aunque el renderer esté bien. |
| [Guitar Hero III NX](https://github.com/guillermedev75/gh3-nx) | Reproduce canciones completas en Switch; el README reporta ~45 game FPS en modo rendimiento/stock en Mariko. | Playbook automatiza arranque, prueba de frames, capturas y comparación. Encontró fallos de ventanas/presentación y medición GPU en hardware; explica que editar código generado se pierde al regenerar. |
| [Need for Speed: Carbon NX](https://github.com/Kaays/nfscarbon-nx) | Menús y carreras jugables; ~28–30 FPS con OC, cae en escenas concurridas. | Diagnóstico de audio exhaustivo: mixer limitado por CPU/esperas/spinlocks; no es un fallo de lectura del parser. Compartido en `docs/measurements.md`. |
| [BattleBlock Theater NX](https://github.com/andresarevalo1/BattleBlockRecomp-NX) | Port y NRO publicados; progreso de pruebas en hardware en documentos. | Tuvo OOM al crear 320 contextos XMA con buffers de paquete de 128 KB inútiles (~40 MB). Fix en runtime; más tarde detectó una fuga de mapeos de texturas durante video que también ocurría con renderer Xenos. Son problemas de recursos, distintos de la lectura invitada de KOF. |
| [Ultra Street Fighter IV NX](https://github.com/Thorhax/usf4-nx) | Menús y peleas jugables a 60 FPS con OC; ReXGlue + deko3d/Renut. | Muestra migración de renderer Vulkan a deko3d y documentación de versiones/historial. |
| [Prototype / Prototype 2 NX](https://github.com/fungalspores/prototype-switch) | Ambos marcados jugables, con sonido/guardado; rendimiento depende de clocks. | Confirma que cargas 3D pesadas y sonido se pueden llevar, con ajustes específicos por juego. |
| [F.E.A.R. NX](https://github.com/infriker/fear-nx) | Campaña jugable; 25–30 FPS según escena/clock de GPU. | Otro juego de acción con XEX retail y renderer Vulkan/NVK. |
| [Metal Gear Rising NX deko3d](https://github.com/tuancapoo/mgr-nx-deko) | Arranca, menús y juego; 40–52 FPS con OC y resolución interna 360p. | ReXGlue + Renut/deko3d; documenta juego basado en módulos/media/CPK y shaders derivados localmente. |

Los proyectos de referencia más antiguos incluyen [Banjo-Kazooie: Nuts & Bolts NX](https://github.com/masterspike52/banjo-nx), [Sonic & All-Stars Racing Transformed NX](https://github.com/masterspike52/sart-nx) y el renderer/herramientas de [Peggle 2 NX](https://github.com/ChanseyIsTheBest/peggle2-nx). ReNut y el renderer nativo de `masseffect-nx`/`nfsmw-nx` son cimientos compartidos; los proyectos actuales acumulan correcciones específicas de Horizon y del SDK.

## Skate 3: distinguir la reimplementación del port del XEX

[Skate 3 Rust Engine](https://github.com/JohnUzoka/skate-3-rust-engine) parte de investigación de reverse engineering y de herramientas de modding, pero el repositorio actual describe una reescritura Rust/Bevy del gameplay, con soporte Switch vía Bevy Horizon. No es evidencia de que el XEX original de Skate 3 arranque con ReXGlue ni de un bug compartido de su parser. El historial menciona una etapa anterior de recompilación/renderizador de Chasm, pero el árbol actual es la implementación Rust. No encontré un reporte público verificable de la lectura KOF `0x4A074CF2` en ese proyecto.

## Relevancia directa para el fallo de KOF

Los repos consultados muestran precedentes de problemas de audio en tres categorías: agotamiento de memoria por buffers/contextos XMA (BattleBlock), falta de tiempo de CPU/esperas en la mezcla (NFS Carbon), y sincronización/reloj de audio en reproducción de video (Project 8). No encontré documentación pública de un fallo equivalente al `guest read 0x4A074CF2` de KOF.

El log de KOF ya demuestra que el NRO, pack, XEX, Vulkan y audio service inicializan. El fallo está en el parser PPC invitado al recibir el reader de `\\media\\sounds`, con puntero/base calculada a `0x4A074CF0`; por semejanza, solo se puede afirmar que cae en la zona de recursos/audio, no que la solución de los otros proyectos aplique. La vía sólida es capturar el contrato/layout del reader y el payload que recibe este título, probar en host y después volver a Switch.

## Fuentes primarias

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)
- [SA2 porting guide](https://github.com/ChanseyIsTheBest/sa2-nx/blob/main/docs/porting-guide.md)
- [GH3 porting playbook](https://github.com/guillermedev75/gh3-nx/blob/main/docs/porting-playbook.md)
- [BattleBlock build status](https://github.com/andresarevalo1/BattleBlockRecomp-NX/blob/main/docs/BUILD_STATUS.md)
- [NFS Carbon measurements](https://github.com/Kaays/nfscarbon-nx/blob/main/docs/measurements.md)
- [Project 8 known issues](https://github.com/R-drg/project8-sw/blob/main/docs/known-issues.md)
- [Skate 3 Rust Engine README](https://github.com/JohnUzoka/skate-3-rust-engine)

