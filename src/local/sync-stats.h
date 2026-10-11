#pragma once
// KYTY_SYNC_STATS=1: where the render thread waits for the GPU (LiveCensus::WaitGpu) and downloads synchronously
// (WaitDownload), by call stack: every 10 s the run log gets the stacks with the most time (exe-relative return
// addresses, symbolized with the release's kyty_emulator.map). Render thread only.
#include <cstdint>

namespace SyncStats {
[[nodiscard]] bool Enabled() noexcept;
// A wait of that kind (0 GPU, 1 download) that took `ns`, from the current call stack.
void Note(uint32_t kind, int64_t ns) noexcept;
// The compute shader of the dispatch being translated (its code address; 0 a draw or none): the waits are listed per
// shader too.
inline thread_local uint64_t g_shader = 0;
} // namespace SyncStats
