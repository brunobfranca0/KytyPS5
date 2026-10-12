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

#include <algorithm>
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

// Buffers are big (one Vulkan buffer can cover a whole guest heap): their marks are kept per 64 KiB of guest address
// (a direct-mapped table; two addresses on one slot share it, which only adds waits). The buffer's own marks keep its
// last write per lane, for the paths that reuse a buffer without an address (GetBufferIfLive: memos, held V#s).
constexpr uint32_t PageBits  = 16;
constexpr size_t   PageSlots = size_t {1} << 18;
inline Marks       g_pages[PageSlots];

inline void NoteRange(Marks& buffer, uint64_t vaddr, uint64_t size, bool written) noexcept {
	if (!g_on) return;
	const uint64_t first = vaddr >> PageBits;
	const uint64_t last  = (vaddr + (size != 0 ? size : 1) - 1) >> PageBits;
	if (last - first >= 4096) {
		Note(buffer, written);
		return;
	}
	for (uint64_t page = first; page <= last; ++page) Note(g_pages[page & (PageSlots - 1)], written);
	buffer.use[g_lane] = g_cur_tick;
	if (written) buffer.write[g_lane] = g_cur_tick;
}

// The lane whose write of the range is the latest (a copy of the range is recorded there: it waits for nothing else).
inline int WriterLane(uint64_t vaddr, uint64_t size) noexcept {
	if (!g_on) return 0;
	uint64_t       latest[2] {};
	const uint64_t first = vaddr >> PageBits;
	const uint64_t last  = std::min((vaddr + (size != 0 ? size : 1) - 1) >> PageBits, first + 64);
	for (uint64_t page = first; page <= last; ++page)
		for (int lane = 0; lane < 2; ++lane)
			latest[lane] = std::max(latest[lane], g_pages[page & (PageSlots - 1)].write[lane]);
	return latest[1] > latest[0] ? 1 : 0;
}

// A reuse of a buffer obtained before: it waits for the other lane's last write anywhere in the buffer.
inline void NoteReuse(Marks& buffer) noexcept {
	if (!g_on) return;
	const int other = g_lane ^ 1;
	if (buffer.write[other] > g_required[other]) g_required[other] = buffer.write[other];
	buffer.use[g_lane] = g_cur_tick;
}

} // namespace LaneSync
