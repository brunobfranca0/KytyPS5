// RDNA2 tessellation through native Vulkan stages (--tessellation): the LS, HS and TES programs'
// LDS and off-chip ring accesses, lowered to IR::TessellationAttribute reads and writes
// (Tessellation.cpp), as stage interface variables. Ported from the Wolverine fork (75160686,
// 96d62b33, 87971a28) onto this emitter's builder.
#include "common/assert.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

using Attribute = IR::TessellationAttribute;

bool IsInputAttribute(Attribute kind) {
	return kind == Attribute::ControlInput || kind == Attribute::EvaluationInput;
}

bool IsLocalAttribute(Attribute kind) {
	return kind == Attribute::LocalOutput || kind == Attribute::ControlInput;
}

uint32_t ArrayType(EmitterState& state, uint32_t element, uint32_t count) {
	return state.builder.Type(OpTypeArray, {element, ConstantU32(state, count)});
}

uint32_t LoadInvocationId(EmitterState& state) {
	const auto variable = InputVariableForKind(state, IR::StageInputKind::InvocationId);
	EXIT_IF(variable == 0);
	const auto value = state.builder.AllocateId();
	const auto bits  = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, TypeI32(state), value, variable});
	state.builder.AddFunction({OpBitcast, TypeU32(state), bits, value});
	return bits;
}

uint32_t TessellationPointer(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&       state    = ctx.state;
	const auto  kind     = static_cast<Attribute>(inst.Arg(0).U32());
	const auto& tess     = state.input_info.vertex->tess;
	const auto  variable = state.tess_variables.at(static_cast<uint32_t>(kind));
	EXIT_IF(variable == 0);
	const auto storage = IsInputAttribute(kind) ? StorageClassInput : StorageClassOutput;
	const auto pointer = state.builder.AllocateId();
	if (kind == Attribute::Factor) {
		EXIT_NOT_IMPLEMENTED(!inst.Arg(1).IsImmediate());
		const auto index = inst.Arg(1).U32() / 4u;
		const auto outer = index < 3u;
		EXIT_NOT_IMPLEMENTED(index >= 4u);
		state.builder.AddFunction({OpAccessChain, TypePointer(state, storage, TypeF32(state)), pointer,
		                           outer ? variable : state.tess_inner_variable,
		                           ConstantU32(state, outer ? index : index - 3u)});
		return pointer;
	}
	auto address = ctx.Arg(inst, 1);
	if (kind == Attribute::PatchOutput) {
		address = EmitBinaryU32(state, OpISub, address, ConstantU32(state, state.tess_patch_base));
	}
	const auto stride = IsLocalAttribute(kind) ? tess.ls_stride : tess.hs_stride;
	const auto offset = kind == Attribute::PatchOutput
	                        ? address
	                        : EmitBinaryU32(state, OpUMod, address, ConstantU32(state, stride));
	const auto attribute = EmitBinaryU32(state, OpShiftRightLogical, offset, ConstantU32(state, 4));
	const auto component =
	    EmitBinaryU32(state, OpBitwiseAnd, EmitBinaryU32(state, OpShiftRightLogical, offset, ConstantU32(state, 2)),
	                  ConstantU32(state, 3));
	const auto type = TypePointer(state, storage, TypeU32(state));
	if (kind == Attribute::LocalOutput || kind == Attribute::PatchOutput) {
		state.builder.AddFunction({OpAccessChain, type, pointer, variable, attribute, component});
	} else {
		const auto vertex = kind == Attribute::ControlOutput
		                        ? LoadInvocationId(state)
		                        : EmitBinaryU32(state, OpUDiv, address, ConstantU32(state, stride));
		state.builder.AddFunction({OpAccessChain, type, pointer, variable, vertex, attribute, component});
	}
	return pointer;
}

} // namespace

void DefineTessellationInterfaces(EmitterState& state) {
	if (state.stage != ShaderType::Local && state.stage != ShaderType::TessellationControl &&
	    state.stage != ShaderType::TessellationEvaluation) {
		return;
	}
	std::array<bool, 6> used {};
	uint32_t            patch_begin = UINT32_MAX;
	uint32_t            patch_end   = 0;
	for (const auto* block: state.program.blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() != IR::ValueOpcode::GetTessellationAttribute &&
			    inst.GetOpcode() != IR::ValueOpcode::SetTessellationAttribute) {
				continue;
			}
			const auto kind = inst.Arg(0).U32();
			used.at(kind)   = true;
			if (kind == static_cast<uint32_t>(Attribute::PatchOutput)) {
				EXIT_NOT_IMPLEMENTED(!inst.Arg(1).IsImmediate());
				patch_begin = std::min(patch_begin, inst.Arg(1).U32());
				patch_end   = std::max(patch_end, inst.Arg(1).U32() + 4u);
			}
		}
	}
	if (std::none_of(used.begin(), used.end(), [](bool value) { return value; })) {
		return;
	}
	const auto& tess = state.input_info.vertex->tess;
	for (uint32_t index = 0; index < used.size(); index++) {
		if (!used[index]) {
			continue;
		}
		const auto kind    = static_cast<Attribute>(index);
		const auto storage = IsInputAttribute(kind) ? StorageClassInput : StorageClassOutput;
		uint32_t   type    = 0;
		if (kind == Attribute::Factor) {
			type = ArrayType(state, TypeF32(state), 4u);
		} else if (kind == Attribute::PatchOutput) {
			state.tess_patch_base = patch_begin;
			type = ArrayType(state, TypeU32Vector(state, 4), (patch_end - patch_begin + 15u) / 16u);
		} else {
			const bool local  = IsLocalAttribute(kind);
			const auto stride = local ? tess.ls_stride : tess.hs_stride;
			type              = ArrayType(state, TypeU32Vector(state, 4), (stride + 15u) / 16u);
			if (kind != Attribute::LocalOutput) {
				type = ArrayType(state, type, local ? tess.input_control_points : tess.output_control_points);
			}
		}
		auto& variable = state.tess_variables[index];
		variable       = state.builder.DefineGlobalVariable(TypePointer(state, storage, type), storage);
		state.interface_variables.push_back(variable);
		state.builder.AddName(variable, "tess_attributes");
		if (kind == Attribute::Factor) {
			state.builder.AddAnnotation({OpDecorate, variable, DecorationBuiltIn, BuiltInTessLevelOuter});
			const auto inner = ArrayType(state, TypeF32(state), 2u);
			state.tess_inner_variable =
			    state.builder.DefineGlobalVariable(TypePointer(state, storage, inner), storage);
			state.interface_variables.push_back(state.tess_inner_variable);
			state.builder.AddName(state.tess_inner_variable, "tess_inner");
			state.builder.AddAnnotation(
			    {OpDecorate, state.tess_inner_variable, DecorationBuiltIn, BuiltInTessLevelInner});
			state.builder.AddAnnotation({OpDecorate, state.tess_inner_variable, DecorationPatch});
		} else {
			state.builder.AddAnnotation({OpDecorate, variable, DecorationLocation,
			                             kind == Attribute::PatchOutput ? (tess.hs_stride + 15u) / 16u : 0u});
		}
		if (kind == Attribute::Factor || kind == Attribute::PatchOutput) {
			state.builder.AddAnnotation({OpDecorate, variable, DecorationPatch});
		}
	}
}

void DefineTessellationExecutionModes(EmitterState& state) {
	const auto& tess = state.input_info.vertex->tess;
	// Triangle domains with fractional odd spacing and clockwise triangles, as the fork supports.
	EXIT_NOT_IMPLEMENTED(tess.domain != 1u || tess.partitioning != 2u || tess.output_topology != 2u);
	state.builder.RequireCapability(CapabilityTessellation);
	if (state.stage == ShaderType::TessellationControl) {
		state.builder.AddExecutionMode({state.main_func, ExecutionModeOutputVertices, tess.output_control_points});
	} else {
		state.builder.AddExecutionMode({state.main_func, ExecutionModeTriangles});
		state.builder.AddExecutionMode({state.main_func, ExecutionModeSpacingFractionalOdd});
		state.builder.AddExecutionMode({state.main_func, ExecutionModeVertexOrderCw});
	}
}

uint32_t EmitTessellationLaneId(EmitterState& state) {
	// A hull shader invocation is one output control point: its lane.
	return LoadInvocationId(state);
}

void EmitTessellationBarrier(EmitterState& state) {
	// Hull shader invocations share no workgroup memory (their LDS is lowered to stage interfaces).
	state.builder.AddFunction({OpControlBarrier, ConstantU32(state, ScopeWorkgroup),
	                           ConstantU32(state, ScopeInvocation), ConstantU32(state, MemorySemanticsNone)});
}

uint32_t EmitGetTessellationAttribute(ValueEmitContext& ctx, const IR::Inst& inst) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, 2), [&] {
		const auto value = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction({OpLoad, TypeU32(ctx.state), value, TessellationPointer(ctx, inst)});
		return value;
	});
}

void EmitSetTessellationAttribute(ValueEmitContext& ctx, const IR::Inst& inst) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, 3), [&] {
		auto value = ctx.Arg(inst, 2);
		if (inst.Arg(0).U32() == static_cast<uint32_t>(Attribute::Factor)) {
			const auto floating = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction({OpBitcast, TypeF32(ctx.state), floating, value});
			value = floating;
		}
		ctx.state.builder.AddFunction({OpStore, TessellationPointer(ctx, inst), value});
	});
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
