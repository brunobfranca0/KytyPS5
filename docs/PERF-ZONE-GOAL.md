# Objetivo: la zona mala de Demon's Souls (camino del dragón y Boletaria) lisa a 60 reales con FSR 3 x2

Este archivo es la única fuente de objetivo y progreso de esta etapa. Cada pasada lee los criterios y las reglas
antes de empezar y actualiza el registro al terminar. La etapa anterior (40 FPS en la escena oficial) está en
`docs/PERF-40FPS-GOAL.md`; su método (medir el techo, probar la causalidad, A/B en la misma ruta) sigue vigente.

Equipo del jugador: RTX 5080 16 GB, Ryzen 9 9950X3D, 2560×1440, monitor 240 Hz con G-Sync. Juego PPSA01341 01.007.000,
modo rendimiento del juego. Carpeta de juego `C:\KytyPS5-juego`, lanzador `Jugar.cmd` opción 2 (FSR 3 x2, `config/fsr3.json`).

## Criterios de la etapa

1. **Rendimiento en la ruta** (dragón + Boletaria, unos 5 minutos jugados, FSR 3 x2): `[fsr3] real` ≥ 59 en el 95 % de los
   segundos y **nunca < 55**; `[fps]` "over 25 ms" = 0 en el 95 % de las ventanas; ningún frame > 50 ms fuera de cargas;
   CPU del hilo de render ≤ 13 ms por frame (columna `cpu` de `[fps]`).
2. **Imagen correcta**: sin texturas rotas ni objetos que desaparezcan (el jugador lo confirma en cada pasada); las
   cinemáticas no se tocan (ni velocidad del vídeo, ni físicas, ni audio); FSR 3 apagado en menús, cargas y películas.
3. **Sin trampas**: no subir `KYTY_HITCH_LOG_MS`, no quitar logs, no saltar frames, no bajar la resolución, no devolver
   datos viejos si causan fallos gráficos o de lógica.
4. **Cada pasada**: una hipótesis; estimar el techo antes de tocar código; A/B en la misma ruta; **se queda solo si da
   +3 fps reales o −1,5 ms/frame en la zona**, si no se revierte. Tabla al final de cada pasada.
5. **Protegido**: nunca borrar partidas (`_SaveData`) ni `_PipelineCache`; el lanzador se queda como está; nada a GitHub
   salvo la rama de compilación `dlss-prueba` (solo para compilar).

## Medidas (herramientas)

- `[fps]` por segundo (`KYTY_FPS_LOG=1`): frames, media y máx ms, `cpu`, `idle`, `gpu_wait`, `download`; "over 17/25 ms".
- `[fps+]`: fallos de página por tipo, `protect_calls`, `sync_downloads`, `upload_wait_us`.
- `[fsr3] real X fps, shown Y fps`: Y debe ser 2 × X con generación; "FSR3 FG: off" en menús/cargas/películas.
- `[sync]` cada 10 s (`KYTY_SYNC_STATS=1`): esperas a la GPU y descargas síncronas por pila de llamadas, con el shader que
  leía (`CS`), la dirección del fallo y **quién escribió** los bytes (`written by CS … via …`).
- `[gpuread]`: lecturas de tablas que esperaron, con las palabras antes/ahora.
- `[memset]`: qué fue de los dispatch del shader de relleno/copia del motor (copias, rellenos en CPU/GPU, dejados al shader
  y por qué).
- `SLOW Frame`: desglose de cada frame > `KYTY_HITCH_LOG_MS` (25).
- Tabla de ronda: `ronda.py <log>` (scratch) → % segundos ≥ 59, mín/media, ventanas over25=0, cpu, download, gpu_wait,
  fallos/s, protect/s, peor frame fuera de cargas, VRAM y RAM.
- Símbolos: `kyty_emulator.map` de la release de cada build (`sym.py <map> <addr…>`).

## Datos de partida

| Log | Build | Segundos ≥ 59 | real mín / media | over25=0 | cpu ms | download ms | gpu_wait ms | fallos/s | protect/s | peor frame | VRAM / RAM |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 181236 (FSR3 x2, 10 min) | dec3c3b | 67 % | 13,0 / 56,2 | 73 % | 10,0 | 3,56 | 2,67 | 48,7 K | 74,4 K | 855 ms | 12,6 / 23,8 GB |
| 185047 (FSR3 x2, 4 min, medición) | b28e99f | 42 % | 36,4 / 52,4 | 47 % | 12,5 | 3,57 | 2,99 | 48,9 K | 82,6 K | 845 ms | 11,4 / 22,3 GB |
| 192138 (FSR3 x2, 3,5 min, pasada 1) | 684e189 | 29 % | 20,2 / 50,3 | 46 % | 12,8 | 4,08 | 3,62 | 46,3 K | 79,7 K | 843 ms | 11,6 / 22,2 GB |
| 195924 (FSR3 x2, 4,5 min, pasada 2) | 318794b | 33 % | 33,9 / 54,3 | 65 % | 13,8 | 0,61 | 0,09 | 67,5 K | 112 K | 850 ms | 12,5 / 23,5 GB |
| 204612 (FSR3 x2, 7 min, castillo, integrada) | 169d1a9 | 66 % | 16,1 / 57,8 | 92 % | 10,8 | 0,01 | 0,00 | 61,2 K | 104 K | 875 ms (creación del contexto FSR 3 al arrancar) | 11,4 / 22,2 GB |

Segundos lentos (~44 fps) frente a los rápidos (60) en 181236: frame 23,0 vs 16,7 ms; `cpu` 15,3 vs 8,8; `download` 7,1
vs 2,6; `gpu_wait` 5,6 vs 1,8. Es decir, **unos 7 ms por frame de esperas síncronas a la GPU** y 6,5 ms más de CPU del
hilo de render.

Reparto de las descargas síncronas en 185047 (40 s en 4 min): **64 % (25,5 s)** son dispatch de tablas (`TableEvaluate`)
que leen descriptores de imagen de 8 palabras que el **shader de relleno del motor (cs_memset32, 0x2088a6600)** había
escrito en la GPU momentos antes; 17 % (6,7 s) lo mismo vía `CopyBuffer`; 10 % (3,9 s) lecturas de la CPU del juego.

## Hitos

- [x] ~~**Z1 Relleno en CPU**~~ (684e189): **revertido**, el juego nunca usa la forma de relleno (0 en 3,5 min).
- [x] **Z1b Copias con alias en CPU** (318794b): **se queda** (+4,0 fps reales, −1,5 ms/frame; descargas síncronas
      4,08 → 0,61 ms, gpu_wait 3,62 → 0,09 ms). Lo que estaba mapeado dos veces era el **origen o los parámetros**
      (solo lectura), no el destino: `[memset] 0 into aliased memory`; el camino de alias del destino existe pero no
      se usó. Nuevo mayor: `cpu` 13,8 ms (sube) y fallos de página 46 K → 67 K/s, protect 80 K → 112 K/s.
- [ ] **Z2 Integrar la rama del desarrollador** (`experiment/perf-40fps-20260926` de chenxiao07/KytyPS5, hasta f2f202c,
      18 commits, 46defa0 incluido). Hecho en la rama `integracion` (merge 169d1a9; lo mío guardado en `mi-fsr3` y la
      etiqueta `antes-de-merge`). Conflictos: `live-counters.h` (lista de nombres: la mía + sus 10 nuevos, comprobada
      contra el enum), `demonsSoulsSceneGuard.cpp` (su mensaje de instalación + mi parche del unlink), `bufferCache.cpp`
      (`NoteGpuWrite`: mi parámetro `caller` + su `LogGpuWriteWatch`; `ReadMemoryOnGpu`: su `KYTY_SYNC_STACK` + mi
      `NoteWriterOf`). Sin choque semántico: su c4a7256 actúa sobre los memmove de libc del juego (CPU), el mío sobre
      los dispatch del shader cs_memset32. Recolectores: el suyo por edad (buffers sin uso 1800 flips, imágenes 1200,
      cada flip) y el mío por presupuesto (al acercarse a KYTY_VRAM_BUDGET_MB) se complementan; se dejan los dos y se
      mide VRAM/fallos/fps en la ruta. La skill `.claude/skills/ps5-emu-optimization` local ya es un superconjunto de la
      rama `docs/ai-optimization-skill` (aaa9e7c): se conserva la local. FSR 3 intacto (sin conflictos en frame-gen,
      fsr3-fg, swapchain, videoOut). **Hecho** (log 204612): FSR 3 activo (`FSR3 FG: on`, shown = 2 × real salvo en
      las 6 transiciones), apagado en menú/carga (2 veces); real 54,3 → 57,8 fps de media, 66 % de segundos ≥ 59,
      over25=0 en el 92 %, descargas síncronas 0,61 → 0,01 ms, VRAM 12,5 → 11,4 GB, fallos 67 → 61 K/s, cpu 13,8 →
      10,8 ms (ruta distinta: castillo de Boletaria; el jugador no vio nada roto). Los dos recolectores se quedan.
- [x] **Z3 Esperas a la GPU restantes**: tras Z1b y la rama del desarrollador quedan 7 ms de descargas por 10 s (de
      2500): resuelto para esta zona.
- [ ] **Z4 CPU del hilo de render**: ~8000 draws indirectos por frame (`table_draws`), `TableEvaluate`, `VirtualProtect`.
- [ ] **Z5 VRAM**: con y sin `KYTY_VRAM_BUDGET_MB=8192`.

## Registro de pasadas

- 2026-10-11: objetivo fijado. Diagnóstico con `[sync] … written by` (b28e99f): el 64 % de las descargas síncronas
  esperan a rellenos del shader cs_memset32 que el emulador mandaba a la GPU. Implementado Z1 (684e189): rellenos con
  período < cuenta en la CPU (`BufferCache::FillPattern`), período de una palabra como fill de GPU si el destino ya está
  escrito por la GPU. Pendiente de medir en la ruta.
- 2026-10-11, **pasada 1** (684e189, log 192138 vs 185047): real 52,4 → 50,3 fps, ms/frame 19,1 → 19,9,
  sync_download 3,57 → 4,08 ms, gpu_wait 2,99 → 3,62 ms: **se revierte** (ruido/peor; la forma de relleno no existe en
  este juego: `[memset]` 0 rellenos en todo el log). El log sí da la causa: `[memset] … left to the shader: aliased
  memory` pasa de 0 a 650–2050 por 10 s exactamente cuando las descargas síncronas pasan de 0,14 s a 2,5–3,1 s por 10 s
  (`[sync]`), y los escritores de los bytes esperados son ese shader vía `RebindBuffers` (18 s) y `CopyBuffer` (14,6 s)
  de 40 s. Hipótesis Z1b: hacer esas copias en la CPU con los alias coherentes. Techo: ~2,7 s de descargas por 10 s
  (~27 % del tiempo del hilo de render en la zona) más las esperas a la GPU que arrastran (gpu_wait).
- 2026-10-11, **pasada 2** (318794b, log 195924 vs 192138): real 50,3 → 54,3 fps, ms/frame 19,9 → 18,4,
  sync_download 4,08 → 0,61 ms, gpu_wait 3,62 → 0,09 ms: **se queda**. Segundos < 50: 106 → 78; over25=0: 46 → 65 %;
  SLOW frames 1298 → 276. Las esperas restantes (`[sync]` 0,1–0,5 s por 10 s) son dispatch de tablas que leen lo que
  otros shaders de cómputo escribieron (`written by CS 0x3b9dc4600…` vía la preparación de buffers) y lecturas de la CPU
  del juego. Lo que sube: `cpu` 12,8 → 13,8 ms y fallos de página 46 → 67 K/s (las copias ahora en CPU marcan páginas
  CPU-dirty que se vuelven a subir). **Choque**: el primer arranque de la pasada (log 195806) terminó con
  `!is_allocated(id)` en NativeXprTry: `FrameGen::FlipOf` indexaba `m_slot_images` con un id de destino de un draw de
  tabla cuya imagen ya se había borrado (se pregunta antes de comprobar los ids). Corregido (83a9ad4: `TryGetImage`).
  El jugador no vio texturas rotas.
- 2026-10-11, **integración** (169d1a9, log 204612, castillo de Boletaria 7 min): ver tabla. Lo que queda son los
  segundos lentos **de CPU**: en ellos `cpu` 15,9 ms frente a 9,0 en los rápidos, `idle` 3,7 frente a 1,1, esperas a la
  GPU 0. En los frames lentos (40 fuera de cargas): `xpr` 9,8 ms (10–13 K `table_draws` por frame), `dispatch` 5,3 ms,
  `upload_wait` 4,6 ms, `draw_us` 1,5 ms. Siguiente: perfil del hilo de render en la zona lenta con la build integrada
  (`medir.cmd`) y atacar el mayor (los draws de tablas / XPR).
- 2026-10-11, **perfil** (169d1a9, `prof-1791745431`, castillo, 10 s = 472 frames ≈ 47 fps, 40 000 muestras del hilo
  de render): ocupado 87 %. Reparto del tiempo: `TableEvaluate` 16,4 % **de los que 8,6 % son los dos `rdtsc` de mi
  sonda `[gpuread]`** (KYTY_SYNC_STATS=1 en las configs: ~1,8 ms por frame); esperas (CondVar) 12,9 % (9,7 % sin
  trabajo del juego, 3,2 % el flip); `ProcessPm4` 6,4 %; `TableBlocks` 4,4 %; `VirtualProtect` 4,1 %; `TableDraw` 3,8 %;
  `TableGather` 2,4 %; `NativeXprTry` 2,3 %; `FindBuffer`+`ObtainBuffer` 5,6 %. Census por frame: 5471 draws indirectos
  de tabla (5,2 ms, 0,95 µs cada uno) + 2939 draws directos (3,45 ms, 1,18 µs) + 2821 dispatch (4,1 ms; 1603 en cómputo
  asíncrono 3,0 ms; 826 son el memset, 0,41 ms). **Pasada 3**: KYTY_SYNC_STATS=0 en las tres configs (las esperas ya
  están resueltas; la sonda costaba 1,8 ms/frame); mismo ejecutable. Techo: −1,8 ms/frame. Luego: `VirtualProtect`
  (0,9 ms), `TableEvaluate` restante (1,6 ms), `TableBlocks` (0,9 ms).
- 2026-10-11, **pasada 3** (169d1a9, configs con KYTY_SYNC_STATS=0, log 211215): real 56,8/57,8 → 54,0 fps de media,
  cpu 9,4/10,8 → 12,2 ms: **no concluyente**. Cada log es una ruta distinta (el jugador va donde quiere): el ruido
  entre pasadas supera lo que se mide. Antes de seguir, una **ruta reproducible**: `Ruta.cmd` (nuevo) graba los
  puntos que el jugador marca con F6 (`_Build/waypoints/route-*.txt`, del desarrollador: 9fde675) y los recorre solo
  (`glide` a 5 m/s, giro de cámara 3 s en cada punto, por el fichero `live.txt`): misma ruta y velocidad en cada
  versión. La pasada 3 se decide con esa ruta (A: KYTY_SYNC_STATS=1, B: =0, mismo ejecutable).
- 2026-10-11, **pasada 3, con ruta** (169d1a9, `route-20261011-212419.txt`, 8 puntos/137 m; logs `_Build/run-logs/`
  213112, 213836, 214813 = A, `logs/` 212214 = B; 7 vueltas). A (sonda encendida): real 59,3–59,9 de media, 76–85 % de
  segundos ≥ 59, cpu 10,4–11,7 ms. B (apagada): 59,4–59,8, 78–86 %, cpu 10,5–10,7 ms. **Pasada 3: real 59,6 → 59,6 fps,
  cpu 10,9 → 10,6 ms, sync_download 0,6 → 0,6 ms, gpu_wait 0,1 → 0,1 ms: dentro del ruido, se revierte** (las configs
  quedan como estaban: `fsr3.json` con KYTY_SYNC_STATS=1 para diagnosticar, las otras dos a 0). Lección: la ruta
  grabada va al tope de 60 fps casi siempre (1–2,7 ms de reposo por frame), así que no pisa la zona mala y no puede
  medir una mejora de CPU. Siguiente: grabar la ruta con F6 **donde caen los fps** (dragón / Boletaria) y medir
  también `cpu` ms del hilo de render, no solo fps. Lanzador: `run-windows-kyty.ps1` escribía los logs en
  `_Build/run-logs` cuando existía `_Build` (creada por F6); corregido para que siempre use `logs/`. `Medir.cmd`
  (nuevo) hace las vueltas solo: arranca con `fsr3.json`, pulsa Cruz hasta que aparece "FSR3 FG: on", recorre la ruta
  N veces y cierra.
- 2026-10-11, **referencia de la zona mala** (169d1a9, log `logs/20261011-220008-577`, el jugador por el camino del
  dragón; ruta F6 `route-20261011-220103.txt`, 4 puntos, 300 m): 233 s, mediana 60 fps, p5 44,3, 52 s por debajo de
  55. Frame lento típico (26,5 ms): cpu del hilo de render 19,4 ms, reposo 7, **gpu_wait 0, download 0**: puro CPU.
  Reparto: xpr 8,7 ms (indirectos 4,5 + directos 4,1) con 10 810 draws de tabla por frame (3 000–8 000 en el resto
  del castillo), dispatch.table 5,0 ms, linear_copy 0,6, upload_wait 0,4; 20 MB subidos por frame, 1 250 region
  syncs, 103 000 llamadas a protección por segundo (51 600 fallos de ventana). Perfil de la zona (prof 21:05, mismo
  ejecutable): TableEvaluate 16,4 % propio (la sonda [gpuread] ~8,6 %), esperas 12,9 %, ProcessPm4 6,4 %, TableBlocks
  4,4 %, VirtualProtect 4,1 %, TableDraw 3,8 %, TableGather 2,4 %, FindBuffer+ObtainBuffer 5,6 %; inclusivo TableDraw
  30,5 %, DispatchDirect 26,9 %, TableDispatch 21,9 %, TableBlocks 12,3 %.
- 2026-10-11, **pasada 4** (8b03bc8): la sonda por run de TableEvaluate (dos rdtsc y una búsqueda por run; KYTY_SYNC_STATS
  la encendía en `fsr3.json`) pasa a su propio interruptor `KYTY_GPUREAD_PROBE`, apagado. Techo: ~1,5 ms por frame
  lento (8,6 % de 19,4 ms), que en esa zona está en el camino crítico (CPU puro). Se mide en la misma zona: cpu ms del
  hilo de render en los segundos lentos y p5 de fps real. Siguientes: dispatch.table (5 ms: evaluación de tablas de
  ~2 800 dispatches, 1,8 us cada uno), TableBlocks/ObtainBuffer (12 %), VirtualProtect (4 %).
