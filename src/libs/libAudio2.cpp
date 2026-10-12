#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "kernel/pthread.h"
#include "libs/audio.h"
#include "libs/audio_internal.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace Libs::Audio {

namespace {

constexpr int AUDIO_OUT_PORT_TYPE_MAIN      = 0;
constexpr int AUDIO_OUT_PORT_TYPE_BGM       = 1;
constexpr int AUDIO_OUT_PORT_TYPE_VOICE     = 2;
constexpr int AUDIO_OUT_PORT_TYPE_PERSONAL  = 3;
constexpr int AUDIO_OUT_PORT_TYPE_PADSPK    = 4;
constexpr int AUDIO_OUT_PORT_TYPE_VIBRATION = 10;
constexpr int AUDIO_OUT_PORT_TYPE_AUX       = 127;

} // namespace

namespace AudioOut2 {

LIB_NAME("AudioOut2", "AudioOut");

struct AudioOut2ContextParam {
	uint32_t max_ports;
	uint32_t max_object_ports;
	uint32_t guarantee_object_ports;
	uint32_t queue_depth;
	uint32_t num_grains;
	uint32_t flags;
	uint32_t reserved[10];
};

struct AudioOut2PortParam {
	uint16_t            port_type;
	uint16_t            pad;
	uint32_t            data_format;
	uint32_t            sampling_freq;
	uint32_t            flags;
	AudioOut2UserHandle user_handle;
	uint32_t            reserved[10];
};

struct AudioOut2Attribute {
	uint32_t    attribute_id;
	int32_t     reserved;
	const void* value;
	size_t      value_size;
};

struct AudioOut2Pcm {
	const void* data;
};

struct AudioOut2Position {
	float x;
	float y;
	float z;
};

struct AudioOut2PortState {
	uint16_t output;
	uint8_t  num_channels;
	uint8_t  pad1;
	int16_t  volume;
	uint16_t reroute_counter;
	uint32_t flags;
	uint32_t pad2;
	uint64_t reserved[6];
};

struct AudioOut2SystemState {
	float    loudness;
	uint32_t pad;
	uint64_t reserved[7];
};

struct AudioOut2SpeakerAngle {
	int16_t azimuth;
	int16_t elevation;
};

struct AudioOut2SpeakerInfo {
	uint8_t               type;
	uint8_t               pad1;
	int16_t               pad2;
	uint32_t              available_bits;
	uint32_t              flags;
	uint32_t              pad3;
	AudioOut2SpeakerAngle speaker_angle[16];
};

struct AudioOut2SystemDebugStateParam {
	uint32_t debug_state_id;
	int32_t  reserved;
	void*    param;
	size_t   param_size;
};

struct AudioOut2MasteringParamsHeader {
	uint32_t params_id;
};

struct AudioOut2MasteringStatesHeader {
	uint32_t states_id;
};

struct AudioOut2MasteringStatesDescriptor {
	uint32_t id;
	uint32_t size;
};

static constexpr uint32_t AUDIO_OUT2_MASTERING_MAX_SYSTEM_CHANNELS = 8;
static constexpr uint32_t AUDIO_OUT2_MASTERING_COMPRESSOR_BANDS    = 3;
static constexpr uint32_t AUDIO_OUT2_MASTERING_COMPRESSOR_BANDS_V2 = 4;

struct AudioOut2MasteringCompressorStates {
	AudioOut2MasteringStatesDescriptor descriptor;
	uint32_t                           reserved[2];
	float                              input_rms[AUDIO_OUT2_MASTERING_COMPRESSOR_BANDS]
	                                            [AUDIO_OUT2_MASTERING_MAX_SYSTEM_CHANNELS];
	float                              compression_coeff[AUDIO_OUT2_MASTERING_COMPRESSOR_BANDS]
	                                                    [AUDIO_OUT2_MASTERING_MAX_SYSTEM_CHANNELS];
};

struct AudioOut2MasteringCompressorStatesV2 {
	AudioOut2MasteringStatesDescriptor descriptor;
	uint32_t                           reserved[2];
	float                              input_rms[AUDIO_OUT2_MASTERING_COMPRESSOR_BANDS_V2]
	                                            [AUDIO_OUT2_MASTERING_MAX_SYSTEM_CHANNELS];
	float                              compression_coeff[AUDIO_OUT2_MASTERING_COMPRESSOR_BANDS_V2]
	                                                    [AUDIO_OUT2_MASTERING_MAX_SYSTEM_CHANNELS];
};

struct AudioOut2MasteringLimiterStates {
	AudioOut2MasteringStatesDescriptor descriptor;
	uint32_t                           reserved[2];
	float                              input_peak[AUDIO_OUT2_MASTERING_MAX_SYSTEM_CHANNELS];
	float                              output_peak[AUDIO_OUT2_MASTERING_MAX_SYSTEM_CHANNELS];
	float                              gain_peak[AUDIO_OUT2_MASTERING_MAX_SYSTEM_CHANNELS];
};

struct AudioOut2MasteringStates {
	AudioOut2MasteringStatesHeader     states_header;
	uint32_t                           reserved[3];
	AudioOut2MasteringCompressorStates compressor_states;
	AudioOut2MasteringLimiterStates    limiter_states;
};

struct AudioOut2MasteringStatesV2 {
	AudioOut2MasteringStatesHeader       states_header;
	uint32_t                             reserved[3];
	AudioOut2MasteringCompressorStatesV2 compressor_states;
	AudioOut2MasteringLimiterStates      limiter_states;
};

static std::atomic_uint64_t g_audioout2_next_context {1};
static std::atomic_uint64_t g_audioout2_next_port {1};
static std::atomic_uint64_t g_audioout2_next_user {1};

struct AudioOut2ContextState {
	bool                   used        = false;
	AudioOut2ContextHandle handle      = 0;
	uint32_t               queue_depth = 4;
	uint32_t               queued      = 0;
	uint32_t               num_grains  = 512;
	uint64_t               last_update = 0;
};

struct AudioOut2PortStateEntry {
	bool                   used          = false;
	AudioOut2PortHandle    handle        = 0;
	AudioOut2ContextHandle context       = 0;
	uint16_t               port_type     = 0;
	uint32_t               data_format   = 0;
	uint32_t               sampling_freq = 48000;
	uint32_t               samples_num   = 512;
	AudioInternal::Format  audio_format  = AudioInternal::Format::Unknown;
	int                    audio_handle  = 0;
	const void*            pcm_data      = nullptr;
	float                  gain          = 1.0f; // a mono (object) port's gain attribute
	uint32_t               ambisonics    = 0;    // an object port's attribute 8: 0x40 | ACN when it carries one
	// KYTY_AUDIO_STATS: the pushes that carried this port's PCM in the period, and its peak.
	uint32_t               stat_pushes   = 0;
	float                  stat_peak     = 0.0f;
};

struct AudioOut2SpeakerArrayState {
	bool                        used          = false;
	uint32_t                    num_speakers  = 0;
	uint8_t                     is_3d         = 0;
	uint8_t                     is_ambisonics = 0;
	AudioOut2SpeakerArrayHandle handle        = nullptr;
};

struct AudioOut2LatencyState {
	bool     used       = false;
	uint32_t user_id    = 0;
	uint32_t output     = 0;
	uint32_t latency_us = 0;
};

static Common::Mutex                              g_audioout2_context_mutex;
static std::array<AudioOut2ContextState, 16>      g_audioout2_contexts;
static Common::Mutex                              g_audioout2_port_mutex;
static std::array<AudioOut2PortStateEntry, 256>   g_audioout2_ports;
static Common::Mutex                              g_audioout2_speaker_array_mutex;
static std::array<AudioOut2SpeakerArrayState, 32> g_audioout2_speaker_arrays;
static Common::Mutex                              g_audioout2_latency_mutex;
static std::array<AudioOut2LatencyState, 16>      g_audioout2_latencies;

static constexpr int AUDIO_OUT2_ERROR_NOT_READY                   = -2144960504; /* 0x80268008 */
static constexpr int AUDIO_OUT2_ERROR_PORT_FULL                   = -2144960494; /* 0x80268012 */
static constexpr int AUDIO_OUT2_ERROR_INVALID_PARAM               = -2144960511; /* 0x80268001 */
static constexpr int AUDIO_OUT2_ERROR_MASTERING_INVALID_API_PARAM = -2144959999; /* 0x80268201 */
static constexpr int AUDIO_OUT2_ERROR_MASTERING_INVALID_STATES_ID = -2144959996; /* 0x80268204 */
static constexpr uint32_t AUDIO_OUT2_PORT_ATTRIBUTE_ID_PCM        = 0;
static constexpr uint32_t AUDIO_OUT2_PORT_ATTRIBUTE_ID_GAIN       = 1;
static constexpr uint32_t AUDIO_OUT2_PORT_ATTRIBUTE_ID_AMBISONICS = 8;
static constexpr uint32_t AUDIO_OUT2_AMBISONICS_CHANNEL           = 0x40;
static constexpr uint32_t AUDIO_OUT2_MASTERING_OUTPUT_RECORDING   = 2;
static constexpr uint32_t AUDIO_OUT2_MASTERING_STATES_ID_DEFAULT  = 1;
static constexpr uint32_t AUDIO_OUT2_MASTERING_STATES_ID_V2       = 2;
static constexpr uint32_t AUDIO_OUT2_MASTERING_STATES_STRUCT_ID_COMPRESSOR_DEFAULT = 0x01010001;
static constexpr uint32_t AUDIO_OUT2_MASTERING_STATES_STRUCT_ID_COMPRESSOR_V2      = 0x01020001;
static constexpr uint32_t AUDIO_OUT2_MASTERING_STATES_STRUCT_ID_LIMITER_DEFAULT    = 0x01010003;

static AudioOut2ContextState* audioout2_find_context_locked(AudioOut2ContextHandle ctx) {
	for (auto& state: g_audioout2_contexts) {
		if (state.used && state.handle == ctx) {
			return &state;
		}
	}

	return nullptr;
}

static uint32_t audioout2_grain_micros(uint32_t grains) {
	const auto sample_count = (grains == 0 ? 512u : grains);
	return std::max<uint32_t>((sample_count * 1000000u) / 48000u, 1000u);
}

static uint8_t audioout2_data_format_channels(uint32_t data_format) {
	const auto channels = (data_format >> 8u) & 0xffu;
	return static_cast<uint8_t>(channels == 0 ? 2u : std::min(channels, 16u));
}

static AudioInternal::Format audioout2_data_format_to_audio_format(uint32_t data_format) {
	const auto channels  = audioout2_data_format_channels(data_format);
	const auto data_type = data_format & 0x7fu;
	const auto is_std    = (data_format & 0x80u) != 0;

	switch (data_type) {
		case 0:
			switch (channels) {
				case 1: return AudioInternal::Format::FloatMono;
				case 2: return AudioInternal::Format::FloatStereo;
				case 8:
					return is_std ? AudioInternal::Format::Float8ChStd
					              : AudioInternal::Format::Float8Ch;
				case 12:
					if (!is_std) {
						return AudioInternal::Format::Float12Ch;
					}
					break;
				default: break;
			}
			break;
		case 1:
			switch (channels) {
				case 1: return AudioInternal::Format::Signed16bitMono;
				case 2: return AudioInternal::Format::Signed16bitStereo;
				case 8:
					return is_std ? AudioInternal::Format::Signed16bit8ChStd
					              : AudioInternal::Format::Signed16bit8Ch;
				default: break;
			}
			break;
		default: break;
	}

	return AudioInternal::Format::Unknown;
}

static int audioout2_port_type_to_audio_out_type(uint16_t port_type) {
	switch (port_type & 0xffu) {
		case 0: return AUDIO_OUT_PORT_TYPE_MAIN;
		case 1: return AUDIO_OUT_PORT_TYPE_BGM;
		case 2: return AUDIO_OUT_PORT_TYPE_VOICE;
		case 3: return AUDIO_OUT_PORT_TYPE_PADSPK;
		case 4: return AUDIO_OUT_PORT_TYPE_PERSONAL;
		case 5: return AUDIO_OUT_PORT_TYPE_AUX;
		case 6: {
			// KYTY_AUDIO2_PORT6_MAIN=1 (experiment): type 6 plays on the speakers instead of the vibration device (it
			// carried the loud signal while the Maiden in Black spoke and the rest was silent, log 20261012-213558).
			static const bool to_main = [] {
				const char* value = std::getenv("KYTY_AUDIO2_PORT6_MAIN");
				return value != nullptr && *value == '1';
			}();
			return to_main ? AUDIO_OUT_PORT_TYPE_MAIN : AUDIO_OUT_PORT_TYPE_VIBRATION;
		}
		default: return AUDIO_OUT_PORT_TYPE_MAIN;
	}
}

static bool audioout2_port_type_is_object(uint16_t port_type) {
	return (port_type & 0xff00u) == 0x0100u;
}

static void audioout2_update_context_locked(AudioOut2ContextState* state) {
	if (state == nullptr) {
		return;
	}

	const auto now = LibKernel::KernelGetProcessTime();
	if (state->last_update == 0 || state->queued == 0) {
		state->last_update = now;
		return;
	}

	const auto grain_micros = static_cast<uint64_t>(audioout2_grain_micros(state->num_grains));
	if (now <= state->last_update || grain_micros == 0) {
		return;
	}

	const auto elapsed = now - state->last_update;
	const auto drained = std::min<uint64_t>(state->queued, elapsed / grain_micros);
	if (drained == 0) {
		return;
	}

	state->queued -= static_cast<uint32_t>(drained);
	state->last_update += drained * grain_micros;
	if (state->queued == 0) {
		state->last_update = now;
	}
}

static AudioOut2PortStateEntry* audioout2_find_port_locked(AudioOut2PortHandle port) {
	for (auto& state: g_audioout2_ports) {
		if (state.used && state.handle == port) {
			return &state;
		}
	}

	return nullptr;
}

static bool audioout2_context_has_queueable_device(AudioOut2ContextHandle ctx) {
	Common::LockGuard lock(g_audioout2_port_mutex);
	for (const auto& state: g_audioout2_ports) {
		if (state.used && state.context == ctx && state.audio_handle > 0 &&
		    state.pcm_data != nullptr && AudioInternal::AudioOutHasDevice(state.audio_handle)) {
			return true;
		}
	}
	return false;
}

// KYTY_AUDIO_STATS: what the game hands AudioOut2, every 5 s per context: its pushes, each speaker port (type,
// format, peak) and its object ports grouped by what the mix below does with them (mixed, or dropped and why), with
// the ambisonics attribute values of the sounding ones; the port attributes set and the speaker-array queries.
struct AudioOut2ContextStats {
	AudioOut2ContextHandle ctx        = 0;
	uint32_t               pushes     = 0;
	bool                   float_main = false;
};
static std::array<AudioOut2ContextStats, 16>       g_audioout2_ctx_stats;  // (g_audioout2_port_mutex)
static std::array<std::atomic<uint32_t>, 33>       g_audioout2_attr_counts; // attribute id (32: larger ids)
static std::atomic<uint32_t>                       g_audioout2_speaker_coeff_calls {0};
static std::atomic<uint32_t>                       g_audioout2_ambi_coeff_calls {0};
static std::atomic<uint32_t>                       g_audioout2_speaker_info_calls {0};

static AudioOut2ContextStats* audioout2_ctx_stats_locked(AudioOut2ContextHandle ctx) {
	AudioOut2ContextStats* free_entry = nullptr;
	for (auto& entry: g_audioout2_ctx_stats) {
		if (entry.ctx == ctx) return &entry;
		if (entry.ctx == 0 && free_entry == nullptr) free_entry = &entry;
	}
	if (free_entry != nullptr) free_entry->ctx = ctx;
	return free_entry;
}

static float audioout2_port_peak(const AudioOut2PortStateEntry& port) {
	const auto channels = audioout2_data_format_channels(port.data_format);
	const auto count    = static_cast<size_t>(port.samples_num) * channels;
	switch (port.audio_format) {
		case AudioInternal::Format::FloatMono:
		case AudioInternal::Format::FloatStereo:
		case AudioInternal::Format::Float8Ch:
		case AudioInternal::Format::Float8ChStd:
		case AudioInternal::Format::Float12Ch: return AudioInternal::PcmPeak(port.pcm_data, count, 2, 3);
		case AudioInternal::Format::Unknown: return 0.0f;
		default: return AudioInternal::PcmPeak(port.pcm_data, count, 0, 3);
	}
}

static std::string audioout2_ranges(const std::vector<uint32_t>& values) {
	std::string out;
	for (size_t i = 0; i < values.size();) {
		size_t j = i;
		while (j + 1 < values.size() && values[j + 1] == values[j] + 1) j++;
		char part[40];
		if (j == i) std::snprintf(part, sizeof(part), "%s0x%x", out.empty() ? "" : ",", values[i]);
		else std::snprintf(part, sizeof(part), "%s0x%x-0x%x", out.empty() ? "" : ",", values[i], values[j]);
		out += part;
		i = j + 1;
	}
	return out.empty() ? "-" : out;
}

// (g_audioout2_port_mutex held.)
static void audioout2_report_locked() {
	static double last = 0.0;
	const double  now  = AudioInternal::StatsSeconds();
	if (last == 0.0) last = now;
	if (now - last < 5.0) return;
	last = now;
	for (auto& ctx: g_audioout2_ctx_stats) {
		if (ctx.ctx == 0 || ctx.pushes == 0) continue;
		std::printf("[audio2] ctx %llu: %u pushes, %s\n", static_cast<unsigned long long>(ctx.ctx), ctx.pushes,
		            ctx.float_main ? "objects mixed into its first float main port" : "no float main port: its objects are not heard");
		struct Group {
			uint32_t              ports = 0, with_pcm = 0, sounding = 0;
			float                 peak  = 0.0f;
			std::vector<uint32_t> values; // the sounding ones' ambisonics attribute
		};
		std::map<std::string, Group> groups;
		for (auto& port: g_audioout2_ports) {
			if (!port.used || port.context != ctx.ctx) continue;
			const bool sounding = port.stat_pushes != 0 && port.stat_peak > 0.001f;
			if (!audioout2_port_type_is_object(port.port_type)) {
				std::printf("[audio2]  speaker port %llu: type 0x%x, format 0x%x (%u ch), %s, %u pushes, peak %.3f\n",
				            static_cast<unsigned long long>(port.handle), port.port_type, port.data_format,
				            audioout2_data_format_channels(port.data_format), port.audio_handle > 0 ? "device" : "no device",
				            port.stat_pushes, static_cast<double>(port.stat_peak));
			} else {
				const char* fate = nullptr;
				if (!ctx.float_main) fate = "dropped: no float main port";
				else if (port.audio_format != AudioInternal::Format::FloatMono) fate = "dropped: not float mono";
				else if ((port.ambisonics & AUDIO_OUT2_AMBISONICS_CHANNEL) != 0 &&
				         (port.ambisonics & ~AUDIO_OUT2_AMBISONICS_CHANNEL) > 1 && std::getenv("KYTY_AUDIO2_HOA") != nullptr &&
				         *std::getenv("KYTY_AUDIO2_HOA") == '0')
					fate = "dropped: ambisonics above first order";
				else fate = "mixed";
				char key[160];
				std::snprintf(key, sizeof(key), "type 0x%x format 0x%x %s, %s", port.port_type, port.data_format,
				              (port.ambisonics & AUDIO_OUT2_AMBISONICS_CHANNEL) != 0 ? "ambisonics" : "point", fate);
				auto& group = groups[key];
				group.ports++;
				if (port.stat_pushes != 0) group.with_pcm++;
				if (sounding) {
					group.sounding++;
					group.values.push_back(port.ambisonics);
				}
				group.peak = std::max(group.peak, port.stat_peak);
			}
			port.stat_pushes = 0;
			port.stat_peak   = 0.0f;
		}
		for (auto& [key, group]: groups) {
			std::sort(group.values.begin(), group.values.end());
			group.values.erase(std::unique(group.values.begin(), group.values.end()), group.values.end());
			std::printf("[audio2]  objects %s: %u ports, %u with PCM, %u sounding, peak %.3f, attribute 8 of the sounding: %s\n",
			            key.c_str(), group.ports, group.with_pcm, group.sounding, static_cast<double>(group.peak),
			            audioout2_ranges(group.values).c_str());
		}
		ctx.pushes = 0;
	}
	std::string attrs;
	for (uint32_t id = 0; id < g_audioout2_attr_counts.size(); id++) {
		const auto n = g_audioout2_attr_counts[id].exchange(0, std::memory_order_relaxed);
		if (n == 0) continue;
		char part[32];
		std::snprintf(part, sizeof(part), " %s%u:%u", id == 32 ? ">" : "", id == 32 ? 31 : id, n);
		attrs += part;
	}
	std::printf("[audio2] port attributes set (id:count)%s; speaker-array queries: coefficients %u, ambisonics %u, speaker info %u\n",
	            attrs.empty() ? " none" : attrs.c_str(), g_audioout2_speaker_coeff_calls.exchange(0),
	            g_audioout2_ambi_coeff_calls.exchange(0), g_audioout2_speaker_info_calls.exchange(0));
}

// The object ports (mono float) are for the system's 3D audio to render. Demon's Souls puts its whole
// game mix there as ambisonics: two fifth-order scenes, one channel per port (attribute 8 = 0x40 | ACN,
// SN3D, its beds silent). Decoded here for stereo with two virtual cardioids facing left and right
// (W and Y, left positive), which only the first order feeds; an object that is no ambisonics channel
// plays in the centre. Each port's gain applies, and the result goes to the front left and right of
// the context's first float main port. Adding the 36 channels up instead cancelled most directions.
static const void* audioout2_mix_objects_locked(AudioOut2ContextHandle ctx, const AudioOut2PortStateEntry& target) {
	constexpr float                 HALF_POWER = 0.70710677f;
	static constexpr float HoaLeft[36]  = {0.265712f, 0.123879f, 0.000000f, 0.214565f, 0.160253f, 0.000000f, -0.106836f, 0.000000f, 0.092522f, 0.131933f, 0.000000f, -0.051098f, 0.000000f, -0.088504f, 0.000000f, 0.000000f, 0.071768f, 0.000000f, -0.054251f, 0.000000f, 0.042023f, 0.000000f, -0.031322f, 0.000000f, -0.041435f, 0.019143f, 0.000000f, -0.028537f, 0.000000f, 0.013210f, 0.000000f, 0.022881f, 0.000000f, 0.000000f, 0.000000f, -0.033157f};
	static constexpr float HoaRight[36] = {0.265712f, -0.123879f, 0.000000f, 0.214565f, -0.160253f, 0.000000f, -0.106836f, 0.000000f, 0.092522f, -0.131933f, 0.000000f, 0.051098f, 0.000000f, -0.088504f, 0.000000f, 0.000000f, -0.071768f, 0.000000f, 0.054251f, 0.000000f, 0.042023f, 0.000000f, -0.031322f, 0.000000f, -0.041435f, -0.019143f, 0.000000f, 0.028537f, 0.000000f, -0.013210f, 0.000000f, 0.022881f, 0.000000f, 0.000000f, 0.000000f, -0.033157f};
	static const bool hoa = [] {
		const char* value = std::getenv("KYTY_AUDIO2_HOA");
		return value == nullptr || *value != '0';
	}();
	thread_local std::vector<float> mix;
	const auto channels = audioout2_data_format_channels(target.data_format);
	const auto frames   = target.samples_num;
	bool       mixed    = false;
	for (const auto& object: g_audioout2_ports) {
		if (!object.used || object.context != ctx || !audioout2_port_type_is_object(object.port_type) ||
		    object.pcm_data == nullptr || object.audio_format != AudioInternal::Format::FloatMono ||
		    object.samples_num != frames) {
			continue;
		}
		float left = HALF_POWER, right = HALF_POWER;
		if ((object.ambisonics & AUDIO_OUT2_AMBISONICS_CHANNEL) != 0) {
			const auto acn = object.ambisonics & ~AUDIO_OUT2_AMBISONICS_CHANNEL;
			if (hoa) {
				// KYTY_AUDIO2_HOA (default on): the whole fifth-order scene decoded for two speakers at +-30 degrees
				// (SN3D real harmonics, max-rE weights), scaled so that a source at a speaker's direction plays as loud
				// as the first-order decode did. The voices (the Maiden in Black's, 12-10) came only in the orders above
				// the first, which that decode dropped.
				if (acn >= 36) continue;
				// KYTY_AUDIO2_HOA_GAIN: the orders above the first scaled (an experiment: the voices' level there).
				static const float high_gain = [] {
					const char* value = std::getenv("KYTY_AUDIO2_HOA_GAIN");
					return value != nullptr ? static_cast<float>(std::atof(value)) : 1.0f;
				}();
				const float scale = acn >= 4 ? high_gain : 1.0f;
				left  = HoaLeft[acn] * scale;
				right = HoaRight[acn] * scale;
				if (left == 0.0f && right == 0.0f) continue;
			} else if (acn == 1) {
				right = -HALF_POWER;
			} else if (acn != 0) {
				continue;
			}
		}
		if (!mixed) {
			const auto* bed = static_cast<const float*>(target.pcm_data);
			mix.assign(bed, bed + static_cast<size_t>(frames) * channels);
			mixed = true;
		}
		const auto* in = static_cast<const float*>(object.pcm_data);
		left *= object.gain;
		right *= object.gain;
		for (uint32_t frame = 0; frame < frames; frame++) {
			mix[static_cast<size_t>(frame) * channels] += in[frame] * left;
			mix[static_cast<size_t>(frame) * channels + 1] += in[frame] * right;
		}
	}
	return mixed ? mix.data() : target.pcm_data;
}

static void audioout2_queue_context_audio(AudioOut2ContextHandle ctx, bool blocking) {
	std::vector<AudioInternal::OutputParam> params;
	params.reserve(AudioInternal::OUT_PORTS_MAX);

	g_audioout2_port_mutex.Lock();
	if (AudioInternal::StatsWanted()) {
		auto* ctx_stats = audioout2_ctx_stats_locked(ctx);
		if (ctx_stats != nullptr) ctx_stats->pushes++;
		for (auto& state: g_audioout2_ports) {
			if (!state.used || state.context != ctx || state.pcm_data == nullptr) continue;
			state.stat_pushes++;
			state.stat_peak = std::max(state.stat_peak, audioout2_port_peak(state));
			if (ctx_stats != nullptr && state.audio_handle > 0 && (state.port_type & 0xffu) == 0 &&
			    (state.audio_format == AudioInternal::Format::FloatStereo || state.audio_format == AudioInternal::Format::Float8Ch ||
			     state.audio_format == AudioInternal::Format::Float8ChStd))
				ctx_stats->float_main = true;
		}
		audioout2_report_locked();
	}
	bool objects_mixed = false;
	for (const auto& state: g_audioout2_ports) {
		if (state.used && state.context == ctx && state.audio_handle > 0 &&
		    state.pcm_data != nullptr && params.size() < AudioInternal::OUT_PORTS_MAX) {
			const void* data = state.pcm_data;
			if (!objects_mixed && (state.port_type & 0xffu) == 0 &&
			    (state.audio_format == AudioInternal::Format::FloatStereo ||
			     state.audio_format == AudioInternal::Format::Float8Ch ||
			     state.audio_format == AudioInternal::Format::Float8ChStd)) {
				data          = audioout2_mix_objects_locked(ctx, state);
				objects_mixed = true;
			}
			params.push_back(AudioInternal::OutputParam {state.audio_handle, data});
		}
	}
	g_audioout2_port_mutex.Unlock();

	if (params.empty()) {
		return;
	}

	(void)AudioInternal::AudioOutOutputs(params.data(), static_cast<uint32_t>(params.size()),
	                                     blocking);
}

static void audioout2_close_audio_handle(int audio_handle) {
	if (audio_handle > 0) {
		AudioInternal::AudioOutClose(audio_handle);
	}
}

int KYTY_SYSV_ABI AudioOut2Initialize() {
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI AudioOut2ContextResetParam(AudioOut2ContextParam* params) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(params == nullptr);

	std::memset(params, 0, sizeof(AudioOut2ContextParam));
	params->max_ports              = 256;
	params->max_object_ports       = 256;
	params->guarantee_object_ports = 0;
	params->queue_depth            = 4;
	params->num_grains             = 512;
	params->flags                  = 1;

	return OK;
}

int KYTY_SYSV_ABI AudioOut2ContextQueryMemory(const AudioOut2ContextParam* params,
                                              size_t*                      memory_size) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(params == nullptr);
	EXIT_NOT_IMPLEMENTED(memory_size == nullptr);

	const auto queue_depth = (params->queue_depth == 0 ? 4u : params->queue_depth);
	*memory_size           = 0x10000u + static_cast<size_t>(queue_depth) * 0x590u;

	LOGF("\t memory_size = 0x%016" PRIx64 "\n", static_cast<uint64_t>(*memory_size));

	return OK;
}

int KYTY_SYSV_ABI AudioOut2ContextCreate(const AudioOut2ContextParam* params, void* buffer,
                                         size_t buffer_size, AudioOut2ContextHandle* ctx) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(params == nullptr);
	EXIT_NOT_IMPLEMENTED(ctx == nullptr);

	*ctx = g_audioout2_next_context.fetch_add(1, std::memory_order_relaxed);

	g_audioout2_context_mutex.Lock();
	auto* state = audioout2_find_context_locked(0);
	if (state == nullptr) {
		for (auto& candidate: g_audioout2_contexts) {
			if (!candidate.used) {
				state = &candidate;
				break;
			}
		}
	}
	EXIT_NOT_IMPLEMENTED(state == nullptr);
	*state             = AudioOut2ContextState {};
	state->used        = true;
	state->handle      = *ctx;
	state->queue_depth = (params->queue_depth == 0 ? 4u : params->queue_depth);
	state->queued      = 0;
	state->num_grains  = (params->num_grains == 0 ? 512u : params->num_grains);
	state->last_update = LibKernel::KernelGetProcessTime();
	g_audioout2_context_mutex.Unlock();

	std::printf("AudioOut2 context %llu: max ports %u, max object ports %u, queue depth %u, grain %u samples, flags 0x%x\n",
	            static_cast<unsigned long long>(*ctx), params->max_ports, params->max_object_ports, params->queue_depth,
	            params->num_grains, params->flags);

	LOGF("\t buffer      = 0x%016" PRIx64 "\n"
	     "\t buffer_size = 0x%016" PRIx64 "\n"
	     "\t ctx         = 0x%016" PRIx64 "\n"
	     "\t queue_depth = %" PRIu32 ", num_grains = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(buffer), static_cast<uint64_t>(buffer_size), *ctx,
	     state->queue_depth, state->num_grains);

	return OK;
}

int KYTY_SYSV_ABI AudioOut2ContextDestroy(AudioOut2ContextHandle ctx) {
	PRINT_NAME();
	LOGF("\t ctx = 0x%016" PRIx64 "\n", ctx);

	g_audioout2_context_mutex.Lock();
	if (auto* state = audioout2_find_context_locked(ctx); state != nullptr) {
		*state = AudioOut2ContextState {};
	}
	g_audioout2_context_mutex.Unlock();

	std::array<int, 256> audio_handles {};
	size_t               audio_handles_num = 0;

	g_audioout2_port_mutex.Lock();
	for (auto& port_state: g_audioout2_ports) {
		if (port_state.used && port_state.context == ctx) {
			if (port_state.audio_handle > 0 && audio_handles_num < audio_handles.size()) {
				audio_handles[audio_handles_num++] = port_state.audio_handle;
			}
			port_state = AudioOut2PortStateEntry {};
		}
	}
	g_audioout2_port_mutex.Unlock();

	for (size_t i = 0; i < audio_handles_num; i++) {
		audioout2_close_audio_handle(audio_handles[i]);
	}

	return OK;
}

int KYTY_SYSV_ABI AudioOut2ContextSetAttributes(AudioOut2ContextHandle    ctx,
                                                const AudioOut2Attribute* attributes,
                                                uint32_t                  num) {
	PRINT_NAME();
	LOGF("\t ctx = 0x%016" PRIx64 ", num = %" PRIu32 "\n", ctx, num);
	EXIT_NOT_IMPLEMENTED(num != 0 && attributes == nullptr);
	return OK;
}

int KYTY_SYSV_ABI AudioOut2ContextAdvance(AudioOut2ContextHandle ctx) {
	g_audioout2_context_mutex.Lock();
	if (auto* state = audioout2_find_context_locked(ctx); state != nullptr) {
		audioout2_update_context_locked(state);
	}
	g_audioout2_context_mutex.Unlock();

	return OK;
}

int KYTY_SYSV_ABI AudioOut2ContextPush(AudioOut2ContextHandle ctx, uint32_t blocking) {
	uint32_t sleep_micros = audioout2_grain_micros(512);

	for (;;) {
		// Only a synchronous submission carrying PCM to a real device can rely on the SDL queue for
		// pacing. Async pushes must retain queue-depth backpressure, and a handle without PCM (or a
		// vibration/failed-open handle) has no downstream operation that can block this call.
		const bool use_device_clock =
		    blocking != 0 && audioout2_context_has_queueable_device(ctx);

		g_audioout2_context_mutex.Lock();
		if (auto* state = audioout2_find_context_locked(ctx); state != nullptr) {
			audioout2_update_context_locked(state);
			sleep_micros = audioout2_grain_micros(state->num_grains);
			if (state->queued < state->queue_depth || use_device_clock) {
				if (state->queued == 0) {
					state->last_update = LibKernel::KernelGetProcessTime();
				}
				if (state->queued < state->queue_depth) {
					state->queued++;
				}
				g_audioout2_context_mutex.Unlock();
				audioout2_queue_context_audio(ctx, blocking != 0);
				return OK;
			}
		}
		g_audioout2_context_mutex.Unlock();

		if (blocking == 0) {
			return AUDIO_OUT2_ERROR_NOT_READY;
		}

		Common::Thread::SleepMicro(sleep_micros);
	}
}

int KYTY_SYSV_ABI AudioOut2ContextGetQueueLevel(AudioOut2ContextHandle ctx, uint32_t* queue_level,
                                                uint32_t* available_queues) {
	if (queue_level != nullptr) {
		*queue_level = 0;
	}
	if (available_queues != nullptr) {
		*available_queues = 4;
	}

	g_audioout2_context_mutex.Lock();
	if (auto* state = audioout2_find_context_locked(ctx); state != nullptr) {
		audioout2_update_context_locked(state);
		if (queue_level != nullptr) {
			*queue_level = state->queued;
		}
		if (available_queues != nullptr) {
			*available_queues =
			    (state->queued < state->queue_depth ? state->queue_depth - state->queued : 0);
		}
	}
	g_audioout2_context_mutex.Unlock();

	return OK;
}

int KYTY_SYSV_ABI AudioOut2PortCreate(AudioOut2ContextHandle ctx, const AudioOut2PortParam* params,
                                      AudioOut2PortHandle* port) {
	EXIT_NOT_IMPLEMENTED(params == nullptr);
	EXIT_NOT_IMPLEMENTED(port == nullptr);

	const auto next_port    = g_audioout2_next_port.fetch_add(1, std::memory_order_relaxed);
	const auto audio_format = audioout2_data_format_to_audio_format(params->data_format);
	const auto audio_type   = audioout2_port_type_to_audio_out_type(params->port_type);

	g_audioout2_context_mutex.Lock();
	const auto* context_state = audioout2_find_context_locked(ctx);
	if (context_state == nullptr) {
		g_audioout2_context_mutex.Unlock();
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}
	const auto samples_num = context_state->num_grains == 0 ? 512u : context_state->num_grains;

	g_audioout2_port_mutex.Lock();
	AudioOut2PortStateEntry* port_state = nullptr;
	for (auto& candidate: g_audioout2_ports) {
		if (!candidate.used) {
			port_state = &candidate;
			break;
		}
	}
	if (port_state != nullptr) {
		*port_state               = AudioOut2PortStateEntry {};
		port_state->used          = true;
		port_state->handle        = next_port;
		port_state->context       = ctx;
		port_state->port_type     = params->port_type;
		port_state->data_format   = params->data_format;
		port_state->sampling_freq = params->sampling_freq;
		port_state->samples_num   = samples_num;
		port_state->audio_format  = audio_format;
	}
	g_audioout2_port_mutex.Unlock();
	g_audioout2_context_mutex.Unlock();

	if (port_state == nullptr) {
		return AUDIO_OUT2_ERROR_PORT_FULL;
	}

	int audio_handle = 0;

	if (audio_format != AudioInternal::Format::Unknown &&
	    !audioout2_port_type_is_object(params->port_type)) {
		audio_handle = AudioInternal::AudioOutOpen(audio_type, samples_num, params->sampling_freq,
		                                           audio_format);
	}

	g_audioout2_port_mutex.Lock();
	const bool reserved = port_state->used && port_state->handle == next_port;
	if (reserved) {
		port_state->audio_handle = audio_handle;
	}
	g_audioout2_port_mutex.Unlock();
	if (!reserved) {
		audioout2_close_audio_handle(audio_handle);
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}

	*port = next_port;

	{
		// (In the run log: every speaker port, and the first object ports.)
		static std::atomic<uint32_t> objects_logged {0};
		const bool object = audioout2_port_type_is_object(params->port_type);
		if (!object || objects_logged.fetch_add(1, std::memory_order_relaxed) < 8)
			std::printf("AudioOut2 port %llu: ctx %llu, type 0x%x (%s), format 0x%x, %u Hz%s\n", static_cast<unsigned long long>(next_port),
			            static_cast<unsigned long long>(ctx), params->port_type, object ? "object" : "speakers", params->data_format,
			            params->sampling_freq, audio_format == AudioInternal::Format::Unknown ? ", unknown format" : "");
	}

	if (next_port <= 16 || (next_port % 600) == 0) {
		PRINT_NAME();
		LOGF("\t ctx           = 0x%016" PRIx64 "\n"
		     "\t port          = 0x%016" PRIx64 "\n"
		     "\t port_type     = %" PRIu16 "\n"
		     "\t data_format   = 0x%08" PRIx32 "\n"
		     "\t sampling_freq = %" PRIu32 "\n",
		     ctx, *port, params->port_type, params->data_format, params->sampling_freq);
	}

	return OK;
}

int KYTY_SYSV_ABI AudioOut2PortDestroy(AudioOut2PortHandle port) {
	PRINT_NAME();
	LOGF("\t port = 0x%016" PRIx64 "\n", port);

	int audio_handle = 0;
	g_audioout2_port_mutex.Lock();
	if (auto* state = audioout2_find_port_locked(port); state != nullptr) {
		audio_handle = state->audio_handle;
		*state       = AudioOut2PortStateEntry {};
	}
	g_audioout2_port_mutex.Unlock();

	audioout2_close_audio_handle(audio_handle);

	return OK;
}

int KYTY_SYSV_ABI AudioOut2PortSetAttributes(AudioOut2PortHandle       port,
                                             const AudioOut2Attribute* attributes, uint32_t num) {
	EXIT_NOT_IMPLEMENTED(num != 0 && attributes == nullptr);

	const void* pcm_data       = nullptr;
	bool        has_pcm        = false;
	float       gain           = 1.0f;
	bool        has_gain       = false;
	uint32_t    ambisonics     = 0;
	bool        has_ambisonics = false;
	for (uint32_t i = 0; i < num; i++) {
		if (AudioInternal::StatsWanted()) {
			const auto id = std::min<uint32_t>(attributes[i].attribute_id, 32);
			g_audioout2_attr_counts[id].fetch_add(1, std::memory_order_relaxed);
			// (In the run log: the first values of the attributes the mix does not read.)
			static std::array<std::atomic<uint32_t>, 33> logged {};
			if (id != AUDIO_OUT2_PORT_ATTRIBUTE_ID_PCM && logged[id].fetch_add(1, std::memory_order_relaxed) < 12 &&
			    attributes[i].value != nullptr) {
				char bytes[100] {};
				size_t at = 0;
				const auto* raw = static_cast<const uint8_t*>(attributes[i].value);
				for (size_t b = 0; b < std::min<size_t>(attributes[i].value_size, 24); b++)
					at += static_cast<size_t>(std::snprintf(bytes + at, sizeof(bytes) - at, "%02x", raw[b]));
				float f[3] {};
				std::memcpy(f, raw, std::min<size_t>(attributes[i].value_size, sizeof(f)));
				std::printf("AudioOut2 port %llu attribute %u (%zu bytes): %s (as floats %g %g %g)\n", static_cast<unsigned long long>(port),
				            attributes[i].attribute_id, attributes[i].value_size, bytes, static_cast<double>(f[0]),
				            static_cast<double>(f[1]), static_cast<double>(f[2]));
			}
		}
		if (attributes[i].attribute_id == AUDIO_OUT2_PORT_ATTRIBUTE_ID_PCM &&
		    attributes[i].value != nullptr && attributes[i].value_size >= sizeof(AudioOut2Pcm)) {
			AudioOut2Pcm pcm {};
			std::memcpy(&pcm, attributes[i].value, sizeof(AudioOut2Pcm));
			pcm_data = pcm.data;
			has_pcm  = true;
		}
		// A mono port's gain (a float per channel).
		if (attributes[i].attribute_id == AUDIO_OUT2_PORT_ATTRIBUTE_ID_GAIN && attributes[i].value != nullptr &&
		    attributes[i].value_size == sizeof(float)) {
			std::memcpy(&gain, attributes[i].value, sizeof(float));
			has_gain = true;
		}
		if (attributes[i].attribute_id == AUDIO_OUT2_PORT_ATTRIBUTE_ID_AMBISONICS && attributes[i].value != nullptr &&
		    attributes[i].value_size == sizeof(uint32_t)) {
			std::memcpy(&ambisonics, attributes[i].value, sizeof(uint32_t));
			has_ambisonics = true;
		}
	}

	if (has_pcm || has_gain || has_ambisonics) {
		g_audioout2_port_mutex.Lock();
		if (auto* state = audioout2_find_port_locked(port); state != nullptr) {
			if (has_pcm) state->pcm_data = pcm_data;
			if (has_gain) state->gain = gain;
			if (has_ambisonics) state->ambisonics = ambisonics;
		}
		g_audioout2_port_mutex.Unlock();
	}

	return OK;
}

int KYTY_SYSV_ABI AudioOut2PortGetState(AudioOut2PortHandle port, AudioOut2PortState* state) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(state == nullptr);

	std::memset(state, 0, sizeof(AudioOut2PortState));
	state->output          = 1;
	state->num_channels    = 2;
	state->volume          = 127;
	state->reroute_counter = 0;
	state->flags           = 0;

	g_audioout2_port_mutex.Lock();
	if (auto* port_state = audioout2_find_port_locked(port); port_state != nullptr) {
		state->num_channels = audioout2_data_format_channels(port_state->data_format);
	}
	g_audioout2_port_mutex.Unlock();

	// LOGF("\t port = 0x%016" PRIx64 "\n", port);
	// LOGF("\t num_channels = %" PRIu8 "\n", state->num_channels);

	return OK;
}

int KYTY_SYSV_ABI AudioOut2GetSystemState(AudioOut2SystemState* state) {
	PRINT_NAME();
	EXIT_NOT_IMPLEMENTED(state == nullptr);
	std::memset(state, 0, sizeof(AudioOut2SystemState));
	return OK;
}

int KYTY_SYSV_ABI AudioOut2UserCreate(uint32_t user_id, AudioOut2UserHandle* handle) {
	PRINT_NAME();
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	*handle = static_cast<AudioOut2UserHandle>(
	    g_audioout2_next_user.fetch_add(1, std::memory_order_relaxed));
	LOGF("\t user_id = %" PRIu32 ", handle = 0x%016" PRIx64 "\n", user_id,
	     static_cast<uint64_t>(*handle));
	return OK;
}

int KYTY_SYSV_ABI AudioOut2UserDestroy(AudioOut2UserHandle handle) {
	PRINT_NAME();
	LOGF("\t handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(handle));
	return OK;
}

size_t KYTY_SYSV_ABI AudioOut2GetSpeakerArrayMemorySize(uint32_t num_speakers, uint8_t is_3d,
                                                        uint8_t is_ambisonics) {
	PRINT_NAME();

	const auto speakers = std::clamp<uint32_t>(num_speakers, 1, 32);
	const auto size = static_cast<size_t>(0x400 + speakers * (is_ambisonics != 0 ? 0x100 : 0x40) +
	                                      (is_3d != 0 ? 0x200 : 0));

	LOGF("\t num_speakers  = %" PRIu32 "\n"
	     "\t is_3d         = %" PRIu8 "\n"
	     "\t is_ambisonics = %" PRIu8 "\n"
	     "\t memory_size   = 0x%016" PRIx64 "\n",
	     num_speakers, is_3d, is_ambisonics, static_cast<uint64_t>(size));

	return size;
}

int KYTY_SYSV_ABI AudioOut2SpeakerArrayCreate(AudioOut2SpeakerArrayHandle* handle,
                                              const void* vbap_params, const void* ambi_params) {
	PRINT_NAME();
	EXIT_NOT_IMPLEMENTED(handle == nullptr);

	*handle = nullptr;

	g_audioout2_speaker_array_mutex.Lock();
	for (auto& state: g_audioout2_speaker_arrays) {
		if (!state.used) {
			state              = AudioOut2SpeakerArrayState {};
			state.used         = true;
			state.handle       = &state;
			state.num_speakers = 2;
			*handle            = state.handle;
			break;
		}
	}
	g_audioout2_speaker_array_mutex.Unlock();

	LOGF("\t handle      = 0x%016" PRIx64 "\n"
	     "\t vbap_params = 0x%016" PRIx64 "\n"
	     "\t ambi_params = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(*handle), reinterpret_cast<uint64_t>(vbap_params),
	     reinterpret_cast<uint64_t>(ambi_params));

	return (*handle != nullptr ? OK : AUDIO_OUT2_ERROR_PORT_FULL);
}

int KYTY_SYSV_ABI AudioOut2SpeakerArrayDestroy(AudioOut2SpeakerArrayHandle handle) {
	PRINT_NAME();
	LOGF("\t handle = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(handle));

	g_audioout2_speaker_array_mutex.Lock();
	for (auto& state: g_audioout2_speaker_arrays) {
		if (state.used && state.handle == handle) {
			state = AudioOut2SpeakerArrayState {};
			break;
		}
	}
	g_audioout2_speaker_array_mutex.Unlock();

	return OK;
}

int KYTY_SYSV_ABI AudioOut2GetSpeakerArrayCoefficients(
    AudioOut2SpeakerArrayHandle handle, AudioOut2Position pos, float spread, float* coefficients,
    uint32_t num_coefficients, uint8_t height_aware, float downmix_spread_radius) {
	g_audioout2_speaker_coeff_calls.fetch_add(1, std::memory_order_relaxed);
	PRINT_NAME();
	EXIT_NOT_IMPLEMENTED(coefficients == nullptr && num_coefficients != 0);

	if (coefficients != nullptr) {
		std::fill(coefficients, coefficients + num_coefficients, 0.0f);
		if (num_coefficients > 0) {
			coefficients[0] = 1.0f;
		}
		if (num_coefficients > 1) {
			coefficients[1] = 1.0f;
		}
	}

	LOGF("\t handle = 0x%016" PRIx64 ", coeffs = %" PRIu32
	     ", pos = (%f, %f, %f), spread = %f, height = %" PRIu8 ", downmix = %f\n",
	     reinterpret_cast<uint64_t>(handle), num_coefficients, static_cast<double>(pos.x),
	     static_cast<double>(pos.y), static_cast<double>(pos.z), static_cast<double>(spread),
	     height_aware, static_cast<double>(downmix_spread_radius));

	return OK;
}

int KYTY_SYSV_ABI AudioOut2GetSpeakerArrayAmbisonicsCoefficients(AudioOut2SpeakerArrayHandle handle,
                                                                 uint32_t ambisonics_channel,
                                                                 float*   coefficients,
                                                                 uint32_t num_coefficients) {
	g_audioout2_ambi_coeff_calls.fetch_add(1, std::memory_order_relaxed);
	PRINT_NAME();
	EXIT_NOT_IMPLEMENTED(coefficients == nullptr && num_coefficients != 0);

	if (coefficients != nullptr) {
		std::fill(coefficients, coefficients + num_coefficients, 0.0f);
		if (num_coefficients > 0) {
			coefficients[0] =
			    (ambisonics_channel == 0 || ambisonics_channel == 64 ? 0.70710677f : 1.0f);
		}
	}

	LOGF("\t handle = 0x%016" PRIx64 ", channel = %" PRIu32 ", coeffs = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(handle), ambisonics_channel, num_coefficients);

	return OK;
}

int KYTY_SYSV_ABI AudioOut2GetSpeakerInfo(AudioOut2SpeakerInfo* info, uint32_t flags) {
	g_audioout2_speaker_info_calls.fetch_add(1, std::memory_order_relaxed);
	EXIT_NOT_IMPLEMENTED(info == nullptr);
	std::memset(info, 0, sizeof(AudioOut2SpeakerInfo));
	info->type             = 0;
	info->available_bits   = 0x03;
	info->flags            = 0;
	info->speaker_angle[0] = {-30, 0};
	info->speaker_angle[1] = {30, 0};

	return OK;
}

int KYTY_SYSV_ABI AudioOut2SetSystemDebugState(const AudioOut2SystemDebugStateParam* param) {
	PRINT_NAME();
	LOGF("\t param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	return OK;
}

int KYTY_SYSV_ABI AudioOut2Set3DLatency(uint32_t user_id, uint32_t output, uint32_t latency_us) {
	// Not sure

	PRINT_NAME();
	LOGF("\t user_id = %" PRIu32 ", output = %" PRIu32 ", latency_us = %" PRIu32 "\n", user_id,
	     output, latency_us);

	if (output > AUDIO_OUT2_MASTERING_OUTPUT_RECORDING) {
		return AUDIO_OUT2_ERROR_INVALID_PARAM;
	}

	g_audioout2_latency_mutex.Lock();
	AudioOut2LatencyState* free_state = nullptr;
	for (auto& state: g_audioout2_latencies) {
		if (state.used && state.user_id == user_id && state.output == output) {
			state.latency_us = latency_us;
			g_audioout2_latency_mutex.Unlock();
			return OK;
		}
		if (!state.used && free_state == nullptr) {
			free_state = &state;
		}
	}

	if (free_state != nullptr) {
		*free_state = AudioOut2LatencyState {true, user_id, output, latency_us};
	}
	g_audioout2_latency_mutex.Unlock();

	return (free_state != nullptr ? OK : AUDIO_OUT2_ERROR_PORT_FULL);
}

int KYTY_SYSV_ABI AudioOut2MasteringInit(uint32_t flags) {
	PRINT_NAME();
	LOGF("\t flags = 0x%08" PRIx32 "\n", flags);
	return OK;
}

int KYTY_SYSV_ABI AudioOut2MasteringSetParam(const AudioOut2MasteringParamsHeader* param,
                                             uint32_t output, uint32_t flags) {
	PRINT_NAME();
	LOGF("\t param = 0x%016" PRIx64 ", output = %" PRIu32 ", flags = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(param), output, flags);
	return OK;
}

int KYTY_SYSV_ABI AudioOut2MasteringGetState(AudioOut2MasteringStatesHeader* state, uint32_t output,
                                             AudioOut2UserHandle user) {
	PRINT_NAME();
	LOGF("\t state = 0x%016" PRIx64 ", output = %" PRIu32 ", user = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(state), output, static_cast<uint64_t>(user));

	if (state == nullptr) {
		return AUDIO_OUT2_ERROR_MASTERING_INVALID_API_PARAM;
	}

	const auto states_id = state->states_id;
	switch (states_id) {
		case AUDIO_OUT2_MASTERING_STATES_ID_DEFAULT: {
			auto* full_state = reinterpret_cast<AudioOut2MasteringStates*>(state);
			std::memset(full_state, 0, sizeof(AudioOut2MasteringStates));
			full_state->states_header.states_id = AUDIO_OUT2_MASTERING_STATES_ID_DEFAULT;
			full_state->compressor_states.descriptor.id =
			    AUDIO_OUT2_MASTERING_STATES_STRUCT_ID_COMPRESSOR_DEFAULT;
			full_state->compressor_states.descriptor.size =
			    sizeof(AudioOut2MasteringCompressorStates);
			full_state->limiter_states.descriptor.id =
			    AUDIO_OUT2_MASTERING_STATES_STRUCT_ID_LIMITER_DEFAULT;
			full_state->limiter_states.descriptor.size = sizeof(AudioOut2MasteringLimiterStates);
			return OK;
		}
		case AUDIO_OUT2_MASTERING_STATES_ID_V2: {
			auto* full_state = reinterpret_cast<AudioOut2MasteringStatesV2*>(state);
			std::memset(full_state, 0, sizeof(AudioOut2MasteringStatesV2));
			full_state->states_header.states_id = AUDIO_OUT2_MASTERING_STATES_ID_V2;
			full_state->compressor_states.descriptor.id =
			    AUDIO_OUT2_MASTERING_STATES_STRUCT_ID_COMPRESSOR_V2;
			full_state->compressor_states.descriptor.size =
			    sizeof(AudioOut2MasteringCompressorStatesV2);
			full_state->limiter_states.descriptor.id =
			    AUDIO_OUT2_MASTERING_STATES_STRUCT_ID_LIMITER_DEFAULT;
			full_state->limiter_states.descriptor.size = sizeof(AudioOut2MasteringLimiterStates);
			return OK;
		}
		default: return AUDIO_OUT2_ERROR_MASTERING_INVALID_STATES_ID;
	}
}

int KYTY_SYSV_ABI AudioOut2MasteringTerm() {
	PRINT_NAME();
	return OK;
}

} // namespace AudioOut2
} // namespace Libs::Audio
