#pragma once
// Live probe `skipcs <guest code address hex>`: dispatches of that compute program, or draws of that pixel shader, are not run (toggles; `skipcs 0`
// clears all). For finding which pass draws an artifact; the image keeps its previous contents. Up to 8 programs.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

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

// --skip-shaders <hash,hash,...> and KYTY_SKIP_SHADER_HASHES (hex, from Senaxx's 05bcfc1d in the Wolverine fork):
// the draws and dispatches of these shader programs (ShaderParams::hash) are skipped, the same way as a program whose
// resources fail to materialize. Checked where the program is looked up (PipelineCache::ProgramCache::Get), since the
// hash is only known there. `list` is the configured list, parsed once.
inline const std::vector<uint64_t>& Hashes(std::string_view list) {
	static const std::vector<uint64_t> hashes = [list] {
		std::vector<uint64_t> result;
		const auto parse = [&result](std::string_view text) {
			size_t start = 0;
			while (start < text.size()) {
				const auto end = std::min(text.find(',', start), text.size());
				if (end > start) {
					const auto value = std::strtoull(std::string(text.substr(start, end - start)).c_str(), nullptr, 16);
					if (value != 0) result.push_back(value);
				}
				start = end + 1;
			}
		};
		parse(list);
		if (const char* value = std::getenv("KYTY_SKIP_SHADER_HASHES"); value != nullptr) parse(value);
		return result;
	}();
	return hashes;
}

inline bool HashSkipped(std::string_view list, uint64_t hash) {
	const auto& hashes = Hashes(list);
	return !hashes.empty() && std::find(hashes.begin(), hashes.end(), hash) != hashes.end();
}
} // namespace SkipShader
