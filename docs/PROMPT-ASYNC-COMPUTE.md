# Prompt: cola de cómputo asíncrona (segunda línea de tiempo de GPU) — build experimental

Este documento es el encargo completo para un agente (Claude Code u otro) que trabaje sobre el repo KytyPS5
(rama `integracion`). Léelo entero antes de tocar código. Idioma de respuesta al usuario: español, corto y claro.

## 0. Reglas fijas del usuario (no negociables)

- Las cinemáticas no se tocan (ni velocidad del vídeo, ni físicas, ni audio). No borrar partidas ni `_PipelineCache`.
- Nada a GitHub salvo a la rama `dlss-prueba` de `todojuegos67-bot/KytyPS5`, y solo para compilar con Actions
  (`gh workflow run build.yml -R todojuegos67-bot/KytyPS5 --ref dlss-prueba`; el zip sale como release asset).
- PROHIBIDO hacer trampa: subir `KYTY_HITCH_LOG_MS` para ocultar frames, quitar logs, saltarse frames, bajar la
  resolución, o devolver datos viejos/incorrectos si causan fallos gráficos o de lógica.
- Regla anti-cambio-inútil: estimar el techo antes, sonda de causalidad, y exigir **+3 fps reales o −1,5 ms/frame**
  o revertir. Tabla por pasada en `docs/PERF-ZONE-GOAL.md`.
- Trabajar sin preguntar salvo que algo sea irreversible. Verificar siempre antes de entregar.
- La build experimental va con su **propio lanzador** (`C:\KytyPS5-juego\Launcher-paralelo.cmd/.ps1`, configs
  `config\*-paralelo.json`, ini `launcher-paralelo.ini`) para no mezclarla con la normal (`Launcher.cmd`).
  El lanzador normal **se queda como está**.

## 1. El hallazgo (12-10-2026, Demon's Souls PPSA01341 01.007.000, castillo 1-1, build a1b2b83)

Medido con `tracew` (3,9 s, 226 frames, 17,2 ms/frame, andando) y con volcados de pilas de los hilos del juego
(`KYTY_STALL_DUMP_MS=12`, 14 volcados en tirones de 27–130 ms):

- **Una vez por frame, a ~11,6 ms del flip, ~10 hilos del juego se bloquean 6,1 ms** leyendo memoria que la GPU
  todavía no ha escrito. Siempre la misma instrucción del juego: `eboot.bin+0xad8c20` (12 lecturas/frame,
  93 % de toda la espera de readbacks del juego: 59 ms de hilo-espera por frame, 64 ms contando todo).
- La lectura dispara `GpuResourceManager::HandleFault → BufferCache::ReadMemory → BeginGuestReadback`. El
  comando llega al hilo de render en 0,01 ms, la copia de readback se graba y se envía en 0,01 ms, la copia
  tarda 0,04 ms de GPU… pero el tick **termina 6,0 ms después**: delante hay ~40 ticks (≈6 ms) de trabajo de
  gráficos ya enviados a la única cola Vulkan.
- El dato leído lo escribe un dispatch de la **cola de cómputo del juego q49** (culling), grabado 0,3 ms antes
  de la lectura (escrituras de 4, 2120 y 664 bytes). En la PS5 ese dispatch corre en un pipe de cómputo
  asíncrono y el resultado vuelve en microsegundos; aquí espera a toda la cola de gráficos.
- Dependencias medidas (`BufferUse` × `GpuWrite` en la traza):
  - Lo que lee q49: 11 500 lecturas de datos escritos por q49/q33 (otras colas de cómputo del juego), ~1 000 de
    datos escritos por **q0** (gráficos) hace <5 ms, 338 sin escritura GPU reciente (datos de CPU).
  - Lo que lee q0 de q49: ~3 650 lecturas, casi todas de escrituras de hace <20 ms (frame anterior); 6 de <5 ms.
  - Es decir: las colas de cómputo del juego (q9, q17, q25, q33, q41, q49; ~15 envíos/frame entre todas) forman un
    grupo casi autocontenido; el cruce con gráficos es raro (~4 lecturas/frame en cada sentido).
- GPU ocupada 12,8 ms de 17,2 por frame (no saturada). El hilo de render traduce por delante de la GPU ~6 ms.
- Los tirones al andar son el mismo fenómeno agrandado: `idle` del hilo de render 14–42 ms con `cpu` constante
  (12 ms); en los volcados los hilos del juego están en `ReadMemory` esperando al driver (vkWaitSemaphores).
- Techo estimado: −6 ms por frame de cadena. En el castillo el frame ya va al tope de 60 (16,7) en reposo; la
  ganancia se verá como **desaparición de los picos al moverse y 60 fijos en zonas pesadas**, y en fps reales
  en las zonas < 60 (dragón: 17,3 ms → ~12–13). Criterio de éxito: picos > 25 ms al andar por el castillo
  reducidos ≥ 70 % (contar `over 25 ms` en las líneas `[fps]` de 2 minutos de ruta) y p99 de frame < 25 ms,
  sin fallos gráficos (comparar capturas) ni cuelgues en 15 min de juego.

## 2. Qué hay ya en el código (puntos de partida)

- `src/graphics/host_gpu/renderer/commandScheduler.{h,cpp}`: **un** `CommandScheduler` con **una**
  `MasterSemaphore` (timeline), ticks globales, `Submit` sobre `GraphicContext::queue` (familia gráfica, índice 0).
  `present_queue` (índice 1) para presentar; `readback_queue` (KYTY_READBACK_QUEUE, cola de transferencia con su
  propio timeline) para copias de readback cuyo escritor **ya terminó**; FSR3 usa una cola de cómputo asíncrona
  propia (`src/local/fsr3-fg.cpp`, `compute_family`). Precedente útil de dos timelines: `ReadbackQueue::Queue::Copy`
  espera al timeline maestro con el tick del productor.
- `src/graphics/guest_gpu/graphicsRun.cpp`: el hilo de render procesa `Submission` por `queue_id` (0 gráficos,
  1 = ?, 9/17/25/33/41/49 cómputo) **en orden de llegada** sobre el mismo scheduler. `CommandProcessor` por cola.
- `BufferCache` (`cache/bufferCache.{h,cpp}`): rangos GPU-escritos (`m_gpu_modified_ranges`, `m_gpu_writes` con
  sellos de tick `GpuWriteTick`, `InflightWriteTick`), readbacks de invitado (`BeginGuestReadback`, slots,
  `ReadbackQueueReady`), subidas de rangos sucios al prólogo del command buffer actual (`UploadDirtyRanges`),
  `WrittenObtainEpoch`. Todo asume un solo tick actual (`m_scheduler.CurrentTick()`).
- `TextureCache`: estado de layout por subrecurso, "última escritura" por imagen; barreras con `last_work` global.
- `WAIT_REG_MEM` del juego entre colas: el CP espera etiquetas que escribe el EOP de otra cola (ver
  `docs/PARALLEL-TRANSLATION.md` §7: el 8–10 % de las esperas las satisface la cola de cómputo). Hoy esas etiquetas se
  escriben al completar el tick (o al grabarse; revisar `LabelWrite`, `R_RELEASE_MEM`).
- Diagnóstico disponible: `KYTY_LIVE_FILE` + `live.txt` (`tracew N ruta`, `prof`, `proft Kyty.Present`, `measure`),
  `KYTY_STALL_DUMP_MS`, `KYTY_SYNC_STATS`, `KYTY_GPUREAD_PROBE`, `[fps]`/`[fps+]`/`SLOW Frame` por segundo.
  Scripts de análisis de trazas en el scratchpad de la sesión anterior (formato `Record` de `live-trace.h`:
  32 bytes `<QIIQQ` = tsc, tid, type, a, b; tipos en el enum).

## 3. Diseño pedido: segunda línea de tiempo para las colas de cómputo del juego

Objetivo: que los envíos de las colas de cómputo del juego (todas las `queue_id != 0`, o al menos q49/q33) se graben
y envíen en **una segunda `VkQueue`** (misma familia gráfica, índice 2 si existe; si no, familia de cómputo) con **su
propio timeline**, de modo que sus dispatches corran en paralelo con la cola de gráficos y sus resultados estén listos
en ~0,5 ms, no 6.

Interruptor: `KYTY_ASYNC_COMPUTE` (0 = como hoy; 1 = activo). Por defecto 0 en la build normal; 1 en los configs
`*-paralelo.json`. Contador/línea de log por segundo `[acq]` con: envíos por cola, esperas cruzadas (gráficos→cómputo
y cómputo→gráficos) con su tiempo, readbacks servidos por el timeline de cómputo y su espera media.

Pasos (cada uno compila, se mide y se apunta en `docs/PERF-ZONE-GOAL.md` antes del siguiente):

1. **Infraestructura**: segundo `CommandScheduler` ("compute scheduler") con su `MasterSemaphore`, pool de
   comandos y cola. Un `Tick` pasa a ser `(timeline, valor)` donde haga falta: `BufferCache::m_gpu_writes`,
   `InflightWriteTick`, `GpuWriteTick`, sellos de readback (`m_download_ticks`, `producer_tick`), imágenes
   (`last write`), anillos/pools que reciclan por `CurrentTick` (deben reciclar por el timeline que los usó). Con
   `KYTY_ASYNC_COMPUTE=0` todo va al scheduler de gráficos y el comportamiento es idéntico (verificar misma
   velocidad ±0,5 fps en el dragón con `measure 10`).
2. **Enrutado**: `GuestGpu` envía las `Submission` de `queue_id != 0` al compute scheduler. Las subidas de rangos
   sucios que necesita un dispatch de cómputo van al prólogo de **su** command buffer (no al de gráficos).
3. **Dependencias de datos (lo esencial)**:
   - Antes de enviar un command buffer de cómputo: espera (timeline de gráficos, `vkQueueSubmit` wait semaphore)
     al **mayor tick de gráficos que escribió (GPU) o subió (prólogo) cualquier búfer/imagen que este command
     buffer lee**; la información está en `m_gpu_writes`/rangos GPU y en los rangos subidos (añadir sello de tick a
     las subidas). Si ese tick aún no se ha enviado, `Flush` del scheduler de gráficos antes.
   - Antes de enviar un command buffer de gráficos: lo mismo contra el timeline de cómputo para lo que lee de
     escrituras de cómputo (dato: casi todo es del frame anterior → la espera suele estar ya satisfecha).
   - Escritura-escritura y lectura-escritura sobre el mismo rango entre colas (raro): tratar como dependencia.
   - Readbacks del juego (`BeginGuestReadback`): el tick del productor es `(timeline, valor)`; la copia se graba
     en el timeline del productor (si el productor es cómputo, el readback va al compute scheduler o a
     `readback_queue` esperando a ese timeline). Es lo que hace que `eboot+0xad8c20` deje de esperar 6 ms.
   - Barreras: con `VK_SHARING_MODE_CONCURRENT` (ya se usa en `streamBuffer.cpp`; comprobar los demás búferes e
     imágenes) basta semáforo + barrera de memoria global al inicio del command buffer que recibe; sin
     transferencias de propiedad de familia si ambas colas son de la misma familia (preferir eso).
   - Layouts de imagen: una imagen usada en ambas colas necesita que el layout de salida de una sea el de entrada
     de la otra; empezar rechazando (modo 0 para ese envío: si un command buffer de cómputo toca una imagen que la
     cola de gráficos tiene en vuelo, ese envío va a la cola de gráficos como hoy) y medir cuántos son.
4. **Etiquetas y WAIT_REG_MEM entre colas**: las etiquetas (EOP/RELEASE_MEM) de un envío de cómputo se escriben
   cuando **su** timeline llega; el CP de gráficos que espera una etiqueta de cómputo debe esperar en CPU a ese
   timeline (ya hay espera de etiquetas; cambiar la fuente del "hecho"). Nunca escribir una etiqueta antes de que la
   GPU haya hecho el trabajo que representa (regla anti-trampa).
5. **Prioridad**: crear la cola de cómputo con prioridad mayor (`pQueuePriorities` 1.0 frente a 0.5 de gráficos;
   si `VK_EXT_global_priority` está, probar `HIGH`). Medir A/B con `measure`.
6. **Validación**: `KYTY_ASYNC_COMPUTE=2` = modo verificación: todo como en 1 pero además, al terminar cada readback
   del juego servido por cómputo, comparar los bytes con una copia hecha por el camino antiguo (igual que
   `KYTY_READBACK_QUEUE=3` hace con `m_verify_downloads`); "acq mismatch" en el log → bug de dependencias.
   Capas de validación de Vulkan en una sesión corta (VVL THREADING/SYNC) con la cola activa.

## 4. Medición (zona fija: castillo 1-1 "donde suele caer" y camino del dragón)

- Sin FG, `Launcher-paralelo` (configs `sin-fg-paralelo.json` → `KYTY_ASYNC_COMPUTE=1`; quitar de ellos
  `KYTY_SPECULATE=4`/`KYTY_SPECULATE_THREADS`, que medidos hoy dan 50 fps frente a 60 y no se siguen).
- Antes/después, mismo proceso cuando se pueda (`poke32` del interruptor con `switch-addr.txt` del mapa de la build;
  si el interruptor no puede cambiar en caliente, dos sesiones con la misma ruta).
- Métricas: `[fps]` (media, `over 25 ms`), `tracew 10` andando: espera de readbacks del juego por frame (hoy 64 ms
  de hilo-espera, 6,1 ms de cadena una vez por frame), profundidad de cola de gráficos al enviar el readback (hoy
  ~40 ticks), `[acq]`. Capturas comparadas en 3 puntos (castillo, dragón, plaza) para descartar fallos gráficos.
- Si tras el paso 3 la cadena no baja de 6 a < 1,5 ms en el castillo, parar, documentar por qué y no seguir.

## 5. Entregables

- Build en `C:\KytyPS5-juego\kyty_emulator-<hash>.exe`, `Launcher-paralelo.ps1` apuntando a ella (`$exeName`),
  configs `*-paralelo.json` actualizados. El lanzador normal intacto.
- `docs/PERF-ZONE-GOAL.md`: una fila por pasada (hash, qué, techo estimado, medido antes/después, decisión).
- Resumen al usuario en español: 5 líneas máximo, números antes/después, y qué queda.
