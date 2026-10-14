#ifndef KYTY_GRAPHICS_DRAWDIFF_H_
#define KYTY_GRAPHICS_DRAWDIFF_H_

// KYTY_DRAW_DIFF=1 (live, default 0): finds what flickers or pops. Standing still, consecutive
// frames issue the same draws and dispatches; this records, for 8 frames in a row, every draw
// (vertex-stage hash, pixel hash, index count, instance count, indirect) and dispatch (compute
// hash, group count) that reached the GPU, and prints the keys whose count changes between those
// frames ("Draw diff"), plus the draws the emulator dropped and why ("Draw skips"). A key missing
// in some frames is a candidate for something that appears and disappears.
// Thread_Gpu only (draws, dispatches and flips are recorded there); costs one relaxed load when off.

#include "common/liveSwitches.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace Libs::Graphics::DrawDiff {

inline constexpr int kFrames = 8;

struct Key {
	uint64_t a        = 0; // vertex-stage (or compute) hash
	uint64_t b        = 0; // pixel hash
	uint32_t count    = 0; // index/vertex count or group count
	uint32_t instance = 0;
	uint32_t kind     = 0; // 0 draw, 1 indirect draw, 2 dispatch, 3 indirect dispatch
	bool     operator<(const Key& o) const {
		return std::tie(a, b, count, instance, kind) < std::tie(o.a, o.b, o.count, o.instance, o.kind);
	}
};

struct State {
	std::mutex                                       mutex;
	int                                              frame = -1; // -1 idle, 0..kFrames-1 recording
	int                                              cooldown = 0;
	std::map<Key, std::array<uint32_t, kFrames>>     keys;
	std::map<std::pair<uint64_t, std::string>, uint64_t> skips;
};

inline State& Get() {
	static State s;
	return s;
}

inline bool Enabled() {
	static auto& on = Common::LiveSwitches::Get("KYTY_DRAW_DIFF", 0);
	return on.load(std::memory_order_relaxed) != 0;
}

inline void Note(uint64_t a, uint64_t b, uint32_t count, uint32_t instance, uint32_t kind) {
	if (!Enabled()) {
		return;
	}
	auto&           s = Get();
	std::lock_guard lock(s.mutex);
	if (s.frame < 0) {
		return;
	}
	s.keys[Key {a, b, count, instance, kind}][static_cast<size_t>(s.frame)]++;
}

inline void NoteSkip(uint64_t hash, const char* reason) {
	if (!Enabled()) {
		return;
	}
	auto&           s = Get();
	std::lock_guard lock(s.mutex);
	s.skips[{hash, reason}]++;
}

inline const char* KindName(uint32_t kind) {
	switch (kind) {
		case 0: return "draw";
		case 1: return "draw-indirect";
		case 2: return "dispatch";
		default: return "dispatch-indirect";
	}
}

// Called once per guest flip.
inline void EndFrame() {
	if (!Enabled()) {
		return;
	}
	auto&           s = Get();
	std::lock_guard lock(s.mutex);
	if (s.frame < 0) {
		// Start a capture every ~5 s (about 100 flips at 20 fps, fewer when slower).
		if (s.cooldown-- > 0) {
			return;
		}
		s.keys.clear();
		s.frame = 0;
		return;
	}
	if (++s.frame < kFrames) {
		return;
	}
	s.frame    = -1;
	s.cooldown = 100;
	std::vector<std::pair<Key, std::array<uint32_t, kFrames>>> changing;
	size_t                                                     stable = 0;
	for (const auto& [key, counts]: s.keys) {
		const auto [lo, hi] = std::minmax_element(counts.begin(), counts.end());
		if (*lo == *hi) {
			stable++;
		} else {
			changing.emplace_back(key, counts);
		}
	}
	// Keys absent in some frames first, then by how much they vary.
	std::sort(changing.begin(), changing.end(), [](const auto& x, const auto& y) {
		const auto zx = std::count(x.second.begin(), x.second.end(), 0u);
		const auto zy = std::count(y.second.begin(), y.second.end(), 0u);
		return zx != zy ? zx > zy : x.first < y.first;
	});
	::printf("Draw diff (%d frames): %zu keys stable, %zu change\n", kFrames, stable, changing.size());
	for (size_t i = 0; i < changing.size() && i < 40; i++) {
		const auto& [key, counts] = changing[i];
		::printf("  %-17s a=%016" PRIx64 " b=%016" PRIx64 " n=%u inst=%u :", KindName(key.kind), key.a,
		         key.b, key.count, key.instance);
		for (const auto c: counts) {
			::printf(" %u", c);
		}
		::printf("\n");
	}
	if (!s.skips.empty()) {
		::printf("Draw skips (since last report):\n");
		for (const auto& [k, n]: s.skips) {
			::printf("  %8" PRIu64 "  %016" PRIx64 "  %s\n", n, k.first, k.second.c_str());
		}
		s.skips.clear();
	}
	std::fflush(stdout);
}

} // namespace Libs::Graphics::DrawDiff

#endif
