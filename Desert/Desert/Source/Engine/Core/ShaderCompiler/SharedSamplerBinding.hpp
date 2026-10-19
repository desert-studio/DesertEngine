#pragma once

#include <cstdint>

// THE ONE RULE THAT NUMBERS A SHARED SAMPLER STATE (MAT-BINDLESS-5).
//
// A shared sampler state (`u_LinearClampSampler`, `u_GlobalFilterSampler`, ...) is declared in the DSL WITHOUT a
// number: `Uniform sampler u_LinearClampSampler;`. The parser does not pick the number from the text it is
// translating (DShaderParser.cpp TranslateLayoutSugar): that text is one stage body, one Include block or one
// .glslh, it never sees the other stages or the included headers, and a header spells its buffer binding as a
// macro — which is how thirteen programs put a sampler on a binding a buffer already held (2154ca589). The parser
// marks the declaration with kAutoSamplerBinding instead, and once EVERY stage of the program is compiled
// (ShaderMapBuild.cpp AssignSharedSamplerBindings), where every number is a number whatever spelled it, the rule
// applies: in each descriptor set, the program's shared samplers take the bindings after the last binding any
// resource of any stage holds, in name order. One number per name for the whole program, so a sampler two stages
// declare is the same slot in both. The same pass refuses, naming the shader: a sampler (shared or numbered) on a
// binding another resource holds, and a shared sampler that no stage of the program samples.
//
// A sampler a C++ site binds BY NUMBER (ComputePipeline::SetSampler: the environment bakes, MediaYuvToRgb) keeps
// its explicit `Uniform(n)`; everything bound by name (RDG pass bindings, material slots) takes the rule.
namespace Desert::Core
{
    // The marker the DSL writes for `Uniform sampler <name>;`. Never a real slot: every marked binding is
    // renumbered before reflection sees the SPIR-V, and a non-sampler resource on it is refused.
    inline constexpr uint32_t kAutoSamplerBinding = 1023;
} // namespace Desert::Core
