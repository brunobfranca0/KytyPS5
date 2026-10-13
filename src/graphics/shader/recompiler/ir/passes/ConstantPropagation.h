#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

void ConstantPropagationPass(const BlockList& blocks);

// Set while a tessellation stage (LS, HS, TES; --tessellation) is translated: folds the bit-field
// extracts of a shifted value's known-zero low bits (the HS's packed control point ID). Off for
// other stages, so their SPIR-V (and the pipeline caches built from it) stays as it was.
inline thread_local bool g_fold_shifted_bitfield_extracts = false;

} // namespace Libs::Graphics::ShaderRecompiler::IR
