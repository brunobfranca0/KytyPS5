#pragma once
// KYTY_ASYNC_COMPUTE: the game's compute queues (queue_id != 0) are translated into command buffers submitted on a
// second Vulkan queue of the graphics family ("lane 1") with its own timeline semaphore; the graphics queue is lane 0.
// Measured 12-10 at Boletaria: once a frame ~10 game threads waited 6 ms for a culling dispatch (q49) whose result
// sat behind ~40 ticks of graphics work on the single queue (docs/PROMPT-ASYNC-COMPUTE.md).
//
// Ordering between the lanes is by data: every buffer and image remembers, per lane, the tick of its last use and of
// its last write (pending ticks count). A use in lane L of something the other lane O wrote (or, for a write, used)
// makes the current command buffer's submission wait on O's timeline for that tick. Ticks stay one global sequence;
// each lane's semaphore is signaled with the ticks of its own submissions (gaps are fine for a timeline).
// Render thread only (the lane and the required waits are the render thread's).

#include <atomic>
#include <cstdint>

namespace LaneSync {

struct Marks {
	uint64_t use[2] {};
	uint64_t write[2] {};
};

inline bool     g_on       = false; // KYTY_ASYNC_COMPUTE=1/2 and the compute queue exists
inline bool     g_safe     = true;  // graphics waits for all earlier compute (KYTY_ASYNC_COMPUTE=1; 2: data marks only)
inline int      g_lane     = 0;     // lane of the open command buffer
inline uint64_t g_cur_tick = 0;     // the tick the open command buffer will be submitted with
inline uint64_t g_required[2] {};   // before the open buffer's submit: wait on lane X's timeline for this tick
inline std::atomic<uint64_t> g_waits[2] {};  // statistics: cross-lane waits added (per lane waited on)
inline std::atomic<uint64_t> g_submits[2] {};

// A use of the resource with `marks` by the open command buffer (written: the GPU writes it).
inline void Note(Marks& marks, bool written) noexcept {
	if (!g_on) return;
	const int      lane  = g_lane;
	const int      other = lane ^ 1;
	const uint64_t need  = written ? marks.use[other] : marks.write[other];
	if (need > g_required[other]) g_required[other] = need;
	marks.use[lane] = g_cur_tick;
	if (written) marks.write[lane] = g_cur_tick;
}

} // namespace LaneSync
