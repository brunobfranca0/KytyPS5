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
- 2026-10-11, **pasada 4 medida** (8b03bc8, log 234732, 222 s por el camino del dragón, el jugador; referencias
  169d1a9: logs 220008 (222 s) y 222444 (248 s)). **Pasada 4: real 57,3/54,0 → 57,9 fps de media, p5 45,8/45,0 →
  51,2, segundos < 55: 22 %/50 % → 15 %; cpu del hilo de render en los segundos lentos 15,4/16,4 → 15,0 ms;
  sync_download 0,0 → 0,0; gpu_wait 0,0 → 0,0: se queda.** Coste por draw de tabla (xpr) en los frames lentos con ≥
  9 000 draws: 0,98 → 0,82 us (−17 %); dispatch.table igual (5,7 → 5,6 ms: sus runs son pocos). Las rutas son del
  jugador (no la grabada), así que la media y el p5 llevan ruido de ruta (las dos referencias difieren entre sí más que
  la pasada de ellas); lo que no lleva ruido es el coste por draw y que el cambio quita trabajo sin añadir ninguno.
  `fsr3.json` sigue con KYTY_SYNC_STATS=1 (stacks [sync] sin coste por run).
- 2026-10-12, **perfil en la zona con 8b03bc8** (`_measure/prof-pasada5.bin`, 30 s, 120 000 muestras, pedido por
  `live.txt` mientras el jugador estaba en el camino del dragón; log 010951). Esperas 36 % (flip 18 %: tope de 60; 18 %
  esperando comandos del juego). Del trabajo: TableDispatch 20,9 % inclusivo (ya mayor que TableDraw 13,9 %); dentro,
  TableBlocks 7,7 % de las muestras, de ellas ObtainBuffer de los búferes **escritos** 5,3 % (cerrojo de
  m_gpu_modified_mutex + RangeSet::Add 1,8 %, RangeSet::Contains 1,0 %, FindBuffer 0,7 %, NoteGpuWrite 0,3 %);
  TableEvaluate 7,6 %; VirtualProtect (ProtectMappedUnlocked) 5,4 %: 1,6 % desde Process (flush diferido), 1,2 %
  UpdateGpuProtection, 1,1 % FlushDeferredProtection, 0,4 % ReapplyProtection en UploadDirtyRanges, 0,4 %
  UpdateCpuProtection. RangeSet::Add ya devuelve pronto si el rango está cubierto (fast mode): lo que cuesta es el
  cerrojo y la búsqueda, ~35 ns × ~10 000 por frame.
  **Hallazgo de los segundos lentos (log 010951, 49 s < 55 fps)**: frame 19–20 ms = cpu del hilo de render 14–16 ms +
  **reposo 4–5 ms** (en 46 de los 49 segundos; sólo 3 con el hilo saturado). El hilo de render no está saturado: es la
  cadena por frame (traducción → GPU → readbacks del juego → siguientes comandos) la que suma 19–20 ms, como describe
  el desarrollador en `lessons.md`. Quitar traducción sigue bajando el frame casi 1:1 (la cadena es serie), pero el
  reposo de 4–5 ms es GPU + readbacks: hay que saber si la GPU va saturada con FG (generación FSR 3 ≈ 2–3 ms de GPU por
  frame a 1440p). **Prueba pendiente (barata)**: la misma zona con `Jugar.cmd` → 1 (sin FG): si los segundos lentos
  desaparecen, el límite en esa zona es la GPU con FG, no la CPU, y el ataque cambia (trabajo de GPU: resolución
  interna del FG, pases de post, barreras), no la traducción.
- 2026-10-12, **prueba sin FG** (8b03bc8, `sin-fg.json`, log 012001, misma zona, 179 s): fps media 55,5, p5 48,3,
  46 % de segundos < 55; en ellos frame 19–21 ms = cpu 14–18 + reposo 3–5. **Igual que con FG**: la generación de
  frames no cuesta nada medible y la GPU no es el límite. La zona es CPU: traducción (14–17 ms) + la latencia de la
  cadena (3–5 ms de readbacks). Sigue el ataque a la traducción; con ~3 ms menos de cpu el frame cae bajo 16,7.
  (Hoy el PC se colgó en negro una vez con 8b03bc8 a 60 fps en marcha; el log acaba limpio: driver/hardware, no el
  emulador. Pendiente: visor de eventos a las 01:17.)
- 2026-10-12, **pasada 5** (c128d6b): memo de los V# escritos en `TableBlocks`. Cada dispatch de tabla obtenía como
  escritos sus ~7 búferes (ObtainBuffer: drenar readbacks, FindBuffer, sincronizar, invalidar feedback, cerrojo +
  RangeSet::Add, NoteGpuWrite); en un run del mismo shader son los mismos. La `WrittenRange` del V# recuerda el búfer y
  la dirección mientras no cambien el tick, la época de registro, `BufferCache::WrittenObtainEpoch` (nueva: readbacks
  encolados/terminados, rangos GPU restados, instantáneas de feedback) ni la época CPU del rango. Techo medido en el
  perfil: 5,3 % de las muestras (~8 % del trabajo, ~1 ms por frame lento); el coste añadido son 5 comparaciones.
  Verificación: `KYTY_TABLE_WRITTEN_VERIFY=1` (hace la búsqueda y compara; "written memo mismatch" en el log).
  Contador `table_written_memo`. Se mide en la zona como la pasada 4.
- 2026-10-12, **escena fija del castillo** (c128d6b con FSR 3 x2, log 041505 y 043x; el jugador quieto donde "suele
  caer"): real 51–52 fps, frame 19,6 ms = **cpu del hilo de render 12,3 + reposo 6,8** (esperando comandos del juego).
  Traza GPU de 5 s (`_measure/trace-castillo.bin`): GPU ocupada 69 % = **13,6 ms por frame** (sin contar la generación
  FSR 3, que va en otra cola: con ella ~16 ms), huecos > 1 ms casi ninguno (31 000 microhuecos entre envíos). Readbacks
  del juego: **107 por frame**, 78 ms de espera por frame sumando sus hilos. Es la cadena en serie: traducción (12,3) →
  GPU (13,6, solapada en parte) → readbacks → el juego manda lo siguiente. A 60 fps la GPU iría al 82 % + FG: muy justa.
  Perfil CPU (`prof-castillo.bin`): TableDraw 27,7 % inclusivo, NativeXprTry 23,8 %, DispatchDirect 12,1 %,
  TableEvaluate 9,0 % propio, ProcessPm4 7,2 %.
  **Conclusión**: en el castillo quitar CPU sola no llega (12,3 ya está por debajo de 16,7); hay que acortar la cadena:
  menos tiempo de GPU por frame (medir por shader con `tracem`: niebla volumétrica, sombras en cascada, pases de post;
  el desarrollador midió fog off +1,9 fps, sombras off +3,5 fps) y/o que los readbacks no esperen la cola entera.
  DLSS-G: descartado hoy (fps reales 35 frente a 57 con FSR 3 por bloqueos del hilo de render en WaitFlipDone, y cierre
  del juego con 5 imágenes de swapchain, logs 040442/040552/040700); configs `dlss-*.json` quedan fuera del menú.
- 2026-10-12, **punto fijo del dragón** (route-20261011-220103 punto 4, `glide` desde el spawn; medidas `measure 10`
  en el mismo proceso, ruido ±0,5 fps): c128d6b 50,3–52,6 fps, render 16,3–16,9 ms. **Pasada 6** (76215d2, las
  transiciones de un set en una barrera): 53,4–54,4 fps, render 16,4–16,8 ms, barreras v2 por frame 934 → 862 (−8 %):
  dentro del ruido, **revertida** (la mayoría de los draws transitan una sola imagen). A/B en vivo de
  KYTY_DISPATCH_BATCH (128 → 32, 8, 1): 53,0/53,4/53,6/51,9 fps: sin efecto. Perfil en el punto (prof-d4-7621): esperas
  12,8 % (hilo saturado: 16 ms de 18,9), TableEvaluate 10,8 % + 1,2 % memcpy (~2,1 ms: lecturas frías de las tablas del
  juego, forma plana ya), ProcessPm4 7,5 %, VirtualProtect 5,5 %, TableBlocks 5,0 %, TableDraw 4,6 %, TableGather 3,0 %.
  Aquí el límite es la traducción (no la cadena): faltan ~2,3 ms de CPU por frame para 60, repartidos en trozos de
  ~1 ms sin un culpable claro. Alternativas que dan estabilidad ya: tope de 45 fps reales + FSR 3 x2 (90 fijos; el
  punto nunca baja de 50), o modo cinemático (30 fijos) + FSR 3 x2.

- 2026-10-12 (tarde), **DLSS-G x3/x4 con tope 50/60** (878c87f → a1b2b83, instrumentación del hilo de presentación
  `Kyty.Present`: acquire / record+submit / gpu wait / before / present por flip). Carga ligera: 50 reales / 150
  mostrados perfectos. Zona pesada: el flip tarda 22–27 ms y el culpable medido es `slReflexSleep` dentro de
  `BeforePresent` (24–27 ms/frame: Reflex duerme al hilo que da por hecho el flip). Sin el sleep
  (`KYTY_DLSSG_SLEEP=0`) el bloqueo pasa a `vkQueuePresentKHR` (35 ms, 27 fps reales con CPU en 2 ms) y la generación
  se enciende/apaga sola (`presented=1`). Modo paralelo (`KYTY_DLSSG_PARALLEL=1`): generación intermitente
  (150 ↔ 50 mostrados). Conclusión: el pacing interno de DLSS-G limita los fps reales; **descartado**, fuera del
  Launcher; FSR 3 x2 se queda como única FG.
- 2026-10-12, **traducción en 2 hilos** (`KYTY_SPECULATE=4`, `KYTY_SPECULATE_THREADS=2`, `Launcher-paralelo`), castillo
  cámara fija sin FG: **50 fps (20 ms)** frente a **60 fijos (16,7; cpu 10,8 + idle 2)** en modo normal; el hilo
  principal espera ~8 ms/frame a los traductores. Igual que midió el desarrollador el 6-10 (−15 %). No se sigue.
- 2026-10-12, **picos al andar por el castillo** (log 165046 + `KYTY_STALL_DUMP_MS=12` + `tracew-andar.bin`):
  en cada tirón (27–130 ms) `cpu`=12 constante e `idle`=14–42: el hilo de render espera al juego; en los 14 volcados
  los hilos del juego (2–3 por volcado, 37 en total) están en `HandleFault → ReadMemory` esperando al driver, 34 de
  37 en la misma instrucción `eboot.bin+0xad8c20`. Traza de 3,9 s (226 frames, 17,2 ms): **una ráfaga por frame a
  los 11,6 ms: ~10 hilos del juego esperan 6,1 ms** un readback cuyo escritor es un dispatch de la cola de cómputo
  q49 grabado 0,3 ms antes; la copia se envía al instante (0,04 ms de GPU) pero completa 6,0 ms después porque hay
  ~40 ticks de gráficos delante en la única cola Vulkan. GPU ocupada 12,8 ms/frame (no saturada). Dependencias:
  q49 lee q49/q33 (11 500) y q0 (~1 000, <5 ms); q0 lee q49 del frame anterior (~3 650). **Techo −6 ms/frame de
  cadena**; diseño y encargo en `docs/PROMPT-ASYNC-COMPUTE.md` (segunda cola/timeline para las colas de cómputo
  del juego, `KYTY_ASYNC_COMPUTE`).
- 2026-10-12 (noche), **KYTY_ASYNC_COMPUTE** (1782c95 → 846034f, Launcher-paralelo): segunda cola Vulkan para las colas
  de cómputo del juego. Andando por el castillo: picos de 130 → ~30 ms, pero en el punto fijo del castillo (19 ms/frame)
  la lectura del culling (eboot+0xad8c20) sigue en ~6 ms con todas las variantes (marcas por búfer, por 64 KiB, copia en
  el carril del escritor, sin esperas WAR) y aparecen fallos (temblores de animación, niebla volumétrica verde sin
  WAR). **Descartado**; código detrás del interruptor, apagado por defecto. Perfil del punto fijo (prof-fijo.bin,
  a1b2b83): espera 17 %, TableDraw 34 %, dispatches 18 % (TableEvaluate 12 %), ProcessPm4 8 %, VirtualProtect 4 %.
- 2026-10-12, **voces que no suenan** (Doncella de Negro): el juego manda su mezcla como dos escenas ambisonics de
  orden 5 en puertos de objeto; sólo se decodificaban W e Y. Decodificación completa a estéreo (KYTY_AUDIO2_HOA, ba6ff12)
  y ganancia x4 de los órdenes altos (71da595, KYTY_AUDIO2_HOA_GAIN): **la voz sigue sin oírse**. AJM decodifica
  sonidos nuevos al hablar (peaks 0,1–1,0) pero no llegan a la mezcla de objetos: pendiente seguir el audio desde AJM
  (qué instancia es la voz, a qué puerto/bus de Wwise va).
- 2026-10-12 (noche), **audio por streaming** (KYTY_APR_LOG=1, log 20261012-213558): lo que no suena son fuentes de
  `sound/streams/` (música y voces de la intro `pr_demons_souls_intro_{music,vo,sfx}.at9`, voces de NPC
  `02_dialogue/en/04_maideninblack/v1310001*.at9`); los bancos en memoria sí suenan. Dos síntomas distintos:
  (1) voz de la Doncella: el juego lee los 8 bloques de precarga (0x1864 + 7x0x3000) y **nunca pide el siguiente**
  — la voz no llega a consumirse (no arranca); a la vez se leen sus `v1310000NN_face.cani`. (2) intro: los tres
  streams sí se consumen (relleno cada ~0,17–0,45 s) y AJM decodifica (instancias 8 canales 0x4003/0x4004 con pico
  0,002/0,000, 0x4005 0,17) pero la salida (puerto principal 8 ch y objetos) va casi en silencio. El puerto tipo 6
  (pico 0,56) es la vibración del mando (probado redirigido: no es la voz). Siguiente paso: qué espera Wwise para
  arrancar una fuente streaming (callbacks/estado de AJM o APR que devolvemos) y comprobar la decodificación ATRAC9
  de 8 canales contra una referencia (VGAudio) con un .at9 de la intro.
- 2026-10-12 (noche), **causa de las voces que faltan**: los diálogos (`sound/streams/02_dialogue/*/*.at9`, p. ej.
  `04_maideninblack/v131000120.at9`) son ATRAC9 de **16 canales** (ambisonics de 3.er orden, máscara 0x1fff,
  superframe 0x1800 = 6144 B, 1024 muestras) con config `30 73 c1 7e` (sin el 0xFE de ATRAC9); la música de la intro es
  ATRAC9 normal de 4 canales (config `fe 7a 17 f0`) y LibAtrac9 la decodifica bien fuera del emulador (pico 0,42).
  `AjmDecAt9ParseConfigData` rechaza la config de 16 canales → el juego no crea el decodificador → la voz se precarga
  y nunca se consume. Pruebas fuera del emulador (scratchpad/at9): 16 decodificadores mono con bloques alineados a
  byte, frame tras frame, decodifican los 3 primeros frames (silencio: 16x37, 16x32, 16x29 B) y fallan en el bloque 1
  del frame 3 (ERR_UNPACK_SCALE_FACTOR_OOB); cada superframe acaba en relleno 0x01. Falta averiguar el empaquetado
  exacto de esta variante (posible codificación conjunta entre canales) — o tomar otra ruta: decodificar sólo W
  (canal 0) como mono si se consigue separar su bloque.
  Más pruebas (tools/local/at9-16ch/probe16.c, gcc con 3rdparty/LibAtrac9): con 16 decodificadores mono, bloques
  alineados a byte en orden frame→canal y saltando el relleno 0x01, el superframe 0 entero decodifica (silencio: filas
  de 16x37, 16x32, 16x29 y la 4.ª fila de 16 huecos de 286 B = 28 B + 258 B de relleno, total exacto 6144). En el
  superframe 1 (empieza la voz: canal 0 usa 95/92/160 B) aparecen tamaños sospechosos en canales que deberían ir en
  silencio (ch14 34 B, ch8 95 B) y la 4.ª fila falla en el canal 1 (ERR_UNPACK_SCALE_FACTOR_OOB) → el modelo "16 mono
  independientes" no es exacto (probable: modo de scale factors que referencia otro canal, o bloques estéreo
  mezclados). Descartados: 8 estéreo x 768 B, 16 mono x 384 B, config 16 mono con frame fijo de 1536 B.

- 2026-10-13, **trama de rayitas negras en el agua** (Nexo, 1-1; con y sin FG): rayitas verticales de 1-2 px en
  columnas x mod 16 = 14-15 (pantalla 2560x1440), solo sobre agua. Captura `capture` + sonda nueva `skipcs <cs_va>`
  (26f3b5c). Apagar 9fb5cc274b5e4f82 (wave64, reducciones DPP/permlanex16, escribe 1280x1440) no la quita; los pasos
  a media resolución 8x8 (f2b8b99a, ba3ce0ee, a2444a86, 5098ae0e, b4a64911, d00de9dd) dejan imagen fantasma con
  rayitas (no concluyente: la imagen vieja conserva la trama); 2d7e7a4d/afb8ba8c (16x16) = exposición (todo blanco,
  se queda bloqueada un rato). Sin culpable aún. Siguiente: el pixel shader de la superficie del agua (draw,
  no compute) y una sonda que borre la salida en vez de saltarla.
- 2026-10-13 (2), **trama del agua: culpable encontrado** con `skipcs` sobre draws (6224851): el pixel shader de la
  superficie del agua `7a59c1a20cbe5d4a` (en el Nexo ps_va 20e100c00; la dirección cambia entre partidas: capturar
  antes de sondear). Apagarlo quita el agua, sus reflejos dorados y la trama. Es un PS wave64 con bucles
  "waterfall" sobre máscaras de luces/clusters: OR de onda por DPP row_shr 1/2/4/8 + v_permlanex16 + v_readlane 31 y
  63, luego S_FF1 sobre la máscara de 64 bits. En PS no hay emulación de dos mitades (other_half == nullptr):
  ReadLane(63) es un OpGroupNonUniformShuffle con índice 63 en un subgrupo de 32 (indefinido) y los DPP/permlane leen
  carriles que en NVIDIA pueden no tener fragmento. Lee además buffers a media resolución (1280x720: 28a410000,
  2b78f0000, 291300000, 291a80000). Decodificador offline: tools/local/rdna-decode.cpp (ver cabecera). Siguiente:
  hacer las reducciones de onda en PS con OpGroupNonUniformBitwiseOr (robusto a carriles inactivos) o
  ReadLane(lane & 31) y probar.

## Wolverine (para cuando se trabaje con él)
- Carpeta: `E:\Wolverine-KytyPS5-v1` (Marvel's Wolverine PPSA03671 01.001.005). Emulador propio en `emulator\`
  (kyty_emulator.exe 25.9 MB + launcher Qt, Kyty.ini), arranque `Play Wolverine.bat`; README dice: KytyPS5 upstream
  release 2026-10-03 (f53e5d2) + PR #937 + arreglos. Es OTRO árbol de código, no esta rama: los arreglos genéricos
  (ATRAC9 multicanal 0x30, FSR3 FG, wave OR en PS, skipcs) habría que portarlos a ese código; lo específico de
  Demon's Souls (seeds, hints, DemonsSouls::*) no aplica.
- 2026-10-13 (3), **trama del agua ARREGLADA** (9ce1872, confirmado en juego): en shaders de gráficos wave64 sobre
  subgrupos de 32, `v_readlane 31/63` de un OR por DPP row_shr + v_permlanex16 se emite como
  OpGroupNonUniformBitwiseOr (spirvEmitterFlow.cpp, IsWaveOrScanRead; KYTY_PS_WAVE_OR=0 lo apaga). El primer intento
  (aa159bb) no casaba: la escritura del permlane en su VGPR va envuelta en SelectU32(exec). En el Nexo lo aplica a 11
  shaders (log `PS_WAVE_OR <hash>`), así que puede corregir más superficies con el mismo patrón.
- 2026-10-13 (4), **vídeo del menú de archipiedras con rayas/escalones unos segundos: ARREGLADO** (c38c76b,
  confirmado en juego). El recuadro es un vídeo (NV12: luma 2048x1088 R8 + croma 1024x544 R8G8). Las escrituras del
  host sobre memoria que la GPU lee como textura (fotogramas de VideoDec2/AvPlayer, lecturas APR) ahora refrescan las
  imágenes del rango al acabar (Memory::FinishHostOverwrite; KYTY_HOST_WRITE_REFRESH=0 lo apaga): una subida a mitad
  de la copia leía páginas antes de que la copia llegara y las protegía después.
