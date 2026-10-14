# Experiments (Wolverine, PPSA03671)

| Date | Build | Change | Measure | Result |
|---|---|---|---|---|
| 2026-10-14 | 4f7b4bf | live A/B, still camera, 15-20 s per segment: KYTY_IMAGE_RECYCLE_MB=0, KYTY_FREE_WRITEBACK=2, KYTY_BINDLESS_SLOT_REUSE=0 (vs base between each) | median mean-abs diff of consecutive distinct frames, 480x230 gray (video 19.09.26) | 0.48 / 0.45 / 0.46 vs base 0.48-0.52: no effect (±7 %), discarded |
| 2026-10-14 | 4f7b4bf | same run, not recorded: ASYNC_READBACK=0, COPY_QUEUE_READBACK=0, REGION_BITMAP=0, LABEL_FLUSH_US=0, ALIAS_TILE_COPY=1 | - | applied (log), no crash; visual not measured |
| 2026-10-14 | 4f7b4bf | brightness jumps 33 -> 78 in the still video | period | every 56.4 s, same pattern: game lightning, not a bug |
| 2026-10-14 | 4f7b4bf | KYTY_DRAW_DIFF=1, still camera | fresh image reads / 5 s | c821afdb reads R8 416x234 @0x1163060000 ~2/frame, its GPU contents dropped 0-2 frames before (evicted by an unwritten RGBA8 832x468 at the same address); 1374f1a5 reads RGBA8 2560x1440 @0x114c6c0000 1/frame, re-created over unwritten B10G11R11 832x468/1664x936 |
| 2026-10-14 | 2141c25 | live A/B KYTY_FREE_WRITEBACK 0/3/0/3, 25 s each, still camera (video 19.37.26), DRAW_DIFF on | fresh reads / 5 s; median frame diff | writebacks happen (~109/5 s at 0x1163060000 and 0x114c6c0000) but stale R8 reads by c821afdb unchanged (209 + 109 per 5 s in both modes); frame diff 1.35/1.51 (m0) vs 1.44/1.53 (m3): no effect. Reverted to 4f7b4bf |
| 2026-10-14 | 4f7b4bf | --rd (RenderDoc 2-frame capture) | fps | 0.6 fps (1.7 s/frame) with RenderDoc injected: unusable in Wolverine, mode removed |
| 2026-10-14 | 4f7b4bf | normal mode, 19:50 run | crash | ErrorDeviceLost at 28 fps after ~70 s (vulkanCommon.cpp:134 submit readback), not reproduced yet |
