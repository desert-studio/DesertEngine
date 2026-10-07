#pragma once

// VFX-05. A Curve module input becomes a LOOK-UP TABLE the simulation samples, so the curve's keys never reach
// the shader text and editing a key never rebuilds SPIR-V (the same rule as a Value input, VFX-04).
//
// Port of UE's curve data interface LUT (NiagaraDataInterfaceCurveBase.h:23 ShaderLUT, :99
// CurveLUTDefaultWidth = 128, :141 UpdateLUT; NiagaraDataInterfaceVectorCurve.cpp:76-114 UpdateTimeRanges +
// BuildLUT; the sampler NiagaraDataInterfaceVectorCurve.cpp:223-235 / NiagaraDataInterfaceCurveTemplate.ush
// GetCurveLUTIndices): the curve is sampled at CurveLUTDefaultWidth evenly spaced times between the earliest
// first key and the latest last key of its channels, the samples interleaved per channel; a read remaps the time
// into that range, CLAMPS it (so time outside the keys reads the end values — constant extrapolation, FRichCurve's
// default) and lerps two neighbouring samples. UE's per-system static buffer (StaticInputFloat + LUTOffset) is our
// CURVE ATLAS: one float array per system, every curve input's table at its own offset.
//
// Changed from UE: the UObject data interface is the `.dfx` input itself (VFXModuleInput::Curve), TArray is
// std::vector, and FRichCurve::Eval is the animation curve's own segment evaluator (KeyInterpolation.hpp) through
// EvaluateCurve below — one key maths for the Sequencer and for particles. The four numbers a read needs (min
// time, inverse range, offset, last sample index) are the input's parameter row (VFXStackCompiler), not text.

#include <Engine/Assets/Serialization/VFXSystem.hpp>
#include <Engine/VFX/VFXStackCompiler.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Desert::VFX
{
    /// UE UNiagaraDataInterfaceCurveBase::CurveLUTDefaultWidth: samples per channel.
    inline constexpr uint32_t kVFXCurveLUTWidth = 128;

    /**
     * @brief The value of one curve channel at @p time (the curve's own axis — normalised particle age for a
     * particle-group input).
     *
     * The adapter between the `.dfx` key (float time) and the animation curve's segment maths: the segment is
     * found by time and handed to Animation::EvaluateSegment with its span in curve units, so a tangent is value
     * per curve unit. Before the first key and after the last the end value holds (FRichCurve RCCE_Constant).
     * @p keys is non-empty and sorted by strictly increasing Time (the `.dfx` reader guarantees both).
     */
    [[nodiscard]] float EvaluateCurve( std::span<const Assets::Serialization::VFXCurveKey> keys, float time );

    /// UE ShaderLUT + LUTMinTime / LUTMaxTime / LUTInvTimeRange / LUTNumSamplesMinusOne, with LUTOffset into the
    /// system's atlas.
    struct VFXCurveLUTEntry
    {
        uint32_t Offset       = 0; ///< first float of this table in VFXCurveAtlas::Floats
        uint32_t Channels     = 0; ///< floats per sample (the input type's component count)
        uint32_t Samples      = 0; ///< kVFXCurveLUTWidth
        float    MinTime      = 0.0f;
        float    MaxTime      = 1.0f;
        float    InvTimeRange = 1.0f; ///< 0 when every channel is one key at one time: every sample is equal

        [[nodiscard]] bool operator==( const VFXCurveLUTEntry& ) const = default;
    };

    /**
     * @brief Port of UpdateTimeRanges + BuildLUT: appends the table of @p channels to @p atlas and returns where.
     *
     * Sample i is the curves at MinTime + i / (Samples - 1) * (MaxTime - MinTime), channels interleaved.
     */
    VFXCurveLUTEntry BakeCurveLUT( const std::vector<std::vector<Assets::Serialization::VFXCurveKey>>& channels,
                                   std::vector<float>& atlas, uint32_t samples = kVFXCurveLUTWidth );

    /// Which stack input a table belongs to.
    struct VFXCurveRef
    {
        std::size_t   Emitter = 0;
        VFXStackGroup Group   = VFXStackGroup::ParticleSpawn;
        uint32_t      Module  = 0; ///< index into the group's array
        std::string   Input;

        [[nodiscard]] bool operator==( const VFXCurveRef& ) const = default;
    };

    /// One system's curve atlas (UE FNiagaraSystemStaticBuffers' float half): uploaded once per system (VFX-07).
    struct VFXCurveAtlas
    {
        std::vector<float>                                    Floats;
        std::vector<std::pair<VFXCurveRef, VFXCurveLUTEntry>> Entries; ///< in stack order

        [[nodiscard]] const VFXCurveLUTEntry* Find( const VFXCurveRef& ref ) const;
    };

    /**
     * @brief Bakes every Curve input of every ENABLED module of the two particle groups of every emitter, in
     * emitter / group / module / input order.
     *
     * The offset is a parameter row component read back as a float, so an atlas past 2^24 floats is an error
     * rather than a rounded offset.
     */
    Common::ResultStr<VFXCurveAtlas> BuildCurveAtlas( const Assets::Serialization::VFXSystemData& system );

    /// The parameter row of a curve input: (MinTime, InvTimeRange, Offset, Samples - 1), what VFX_CurveSample
    /// reads.
    [[nodiscard]] glm::vec4 CurveParamRow( const VFXCurveLUTEntry& entry );

    /// The CPU twin of the contract's VFX_CurveSample (UE SampleCurveInternal<LUT>): components past Channels are
    /// 0.
    [[nodiscard]] glm::vec4 SampleCurveLUT( std::span<const float> atlas, const VFXCurveLUTEntry& entry,
                                            float time );
} // namespace Desert::VFX
