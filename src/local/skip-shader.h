#pragma once
// Live probe `skipcs <guest code address hex>`: dispatches of that compute program, or draws of that pixel shader, are not run (toggles; `skipcs 0`
// clears all). For finding which pass draws an artifact; the image keeps its previous contents. Up to 8 programs.
#include <atomic>
#include <cstdint>

namespace SkipShader {
inline std::atomic<uint64_t> g_cs[8] {};
inline std::atomic<bool>     g_any {false};

inline bool Skipped(uint64_t address) noexcept {
	if (!g_any.load(std::memory_order_relaxed)) return false;
	for (auto& slot : g_cs)
		if (slot.load(std::memory_order_relaxed) == address) return true;
	return false;
}

inline void Toggle(uint64_t address) noexcept {
	if (address == 0) {
		for (auto& slot : g_cs) slot.store(0);
		g_any.store(false);
		return;
	}
	for (auto& slot : g_cs)
		if (slot.load() == address) {
			slot.store(0);
			return;
		}
	for (auto& slot : g_cs)
		if (slot.load() == 0) {
			slot.store(address);
			g_any.store(true);
			return;
		}
}
} // namespace SkipShader
