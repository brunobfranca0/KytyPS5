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

Segundos lentos (~44 fps) frente a los rápidos (60) en 181236: frame 23,0 vs 16,7 ms; `cpu` 15,3 vs 8,8; `download` 7,1
vs 2,6; `gpu_wait` 5,6 vs 1,8. Es decir, **unos 7 ms por frame de esperas síncronas a la GPU** y 6,5 ms más de CPU del
hilo de render.

Reparto de las descargas síncronas en 185047 (40 s en 4 min): **64 % (25,5 s)** son dispatch de tablas (`TableEvaluate`)
que leen descriptores de imagen de 8 palabras que el **shader de relleno del motor (cs_memset32, 0x2088a6600)** había
escrito en la GPU momentos antes; 17 % (6,7 s) lo mismo vía `CopyBuffer`; 10 % (3,9 s) lecturas de la CPU del juego.

## Hitos

- [ ] **Z1 Relleno en CPU**: el shader de relleno (período < cuenta) se hace en la CPU cuando ninguno de los rangos
      está escrito por la GPU, así los descriptores que el juego guarda ahí no se descargan. Commit `684e189`. Medir.
- [ ] **Z2 Integrar la rama del desarrollador** (`experiment/perf-40fps-20260926` de chenxiao07/KytyPS5, ≥ 46defa0)
      conservando FSR 3 x2, el relleno en CPU y el recolector de memoria que mida mejor. Comprobar FSR 3 en el log.
- [ ] **Z3 Esperas a la GPU restantes**: lo que quede en `[sync]` tras Z1 (lecturas de la CPU del juego, `CopyBuffer`).
- [ ] **Z4 CPU del hilo de render**: ~8000 draws indirectos por frame (`table_draws`), `TableEvaluate`, `VirtualProtect`.
- [ ] **Z5 VRAM**: con y sin `KYTY_VRAM_BUDGET_MB=8192`.

## Registro de pasadas

- 2026-10-11: objetivo fijado. Diagnóstico con `[sync] … written by` (b28e99f): el 64 % de las descargas síncronas
  esperan a rellenos del shader cs_memset32 que el emulador mandaba a la GPU. Implementado Z1 (684e189): rellenos con
  período < cuenta en la CPU (`BufferCache::FillPattern`), período de una palabra como fill de GPU si el destino ya está
  escrito por la GPU. Pendiente de medir en la ruta.
