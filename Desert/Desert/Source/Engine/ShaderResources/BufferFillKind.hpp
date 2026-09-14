#pragma once

#include <cstdint>

namespace Desert::ShaderResources
{
    // ------------------------------------------------------------------------------------------------
    // HOW a shader-visible buffer is filled — a property of the buffer, not a convention held in a
    // developer's head.
    //
    // Two routes write these buffers and they are NOT interchangeable:
    //
    //   Whole  -- one memcpy of a C++ struct over the entire block
    //             (UniformBufferProperty::SetRawData). The engine protocol blocks take this route:
    //             CameraUB, TimeUB, DirectionLightsUB, LightsMetadata, ShadowUB, CloudShadowUB,
    //             SkyboxParamsUB, GridUB, SSAOUB, SSRUB, SSRResolveUB, SSRCompositeUB, GIResolveUB,
    //             TerrainUB. Their per-field shadow copies (FieldProperty::m_LocalData) are
    //             ALLOCATED AND NEVER WRITTEN -- Common::Memory::Buffer::Allocate is a bare
    //             `new std::byte[]` -- so those bytes are whatever the heap last held.
    //
    //   Fields -- field by field into the shadow copies, flushed afterwards (UpdateFields). Material
    //             parameters take this route: MaterialUB (shader graph + terrain), TonemapUB,
    //             DeferredUB, JFAFinalUB.
    //
    // Nothing on the buffer used to say which, and the price is on the record: a fix for material
    // flicker walked every uniform buffer of a material and flushed its fields, which memcpy'd
    // uninitialised shadow bytes over the camera matrices the renderer had written one call earlier,
    // and the probe scene rendered as bare sky. The remedy at the time was a hand-maintained set of
    // "buffers it is safe to flush" (DataDrivenMaterial::m_ParamBuffers) -- a second place obliged to
    // agree with the first, with nothing checking that it did. That is the defect class this programme
    // keeps paying for (Docs/Clouds/DEV_CONTRACT.md 2.3.1), so the set is gone and the buffer carries
    // the answer.
    //
    // A buffer claims its route on its FIRST write and refuses the other one by name from then on.
    // Claiming rather than declaring is deliberate: nothing in shader reflection distinguishes the two
    // -- `CameraUB` and `MaterialUB` reflect identically -- so the only honest source is the first
    // write, and the only useful thing to do with it is to make the second, contradicting write
    // impossible instead of silent.
    //
    // Storage buffers are a third case and need no state: ShaderResources::StorageBuffer has no fields
    // at all (GetFields() is empty by construction), so StorageBufferProperty::SetRawData is the only
    // route that exists and there is nothing to mix it with.
    // ------------------------------------------------------------------------------------------------

    enum class FillKind : uint8_t
    {
        Unclaimed = 0, ///< No write has happened yet; either route may still claim it.
        Whole     = 1, ///< Written in one piece; the per-field shadow copies are NOT a source of truth.
        Fields    = 2, ///< Written field by field; the per-field shadow copies ARE the source of truth.
    };

    /// Outcome of asking a buffer in state @p current to accept a write of kind @p requested.
    /// @c Next is the state the buffer must be left in -- on a refusal that is @p current unchanged,
    /// so a rejected write can never drag the buffer halfway into the other route.
    struct FillClaim
    {
        bool     Accepted;
        FillKind Next;
    };

    /// The whole relation, as one pure function so it can be asserted without a device.
    ///
    ///   * an Unclaimed buffer accepts either route and becomes it;
    ///   * a claimed buffer accepts its own route and stays put (writes are idempotent in kind);
    ///   * a claimed buffer REFUSES the other route and keeps its state;
    ///   * Unclaimed is a state, never a request -- "write me in no particular way" is not an operation.
    [[nodiscard]] constexpr FillClaim ClaimFill( FillKind current, FillKind requested )
    {
        if ( requested == FillKind::Unclaimed )
            return { false, current };
        if ( current == FillKind::Unclaimed || current == requested )
            return { true, requested };
        return { false, current };
    }

    /// Name used in the refusal message. Short and stable: it is read in a log next to a buffer name.
    [[nodiscard]] constexpr const char* FillKindName( FillKind kind )
    {
        switch ( kind )
        {
            case FillKind::Whole:
                return "whole-block";
            case FillKind::Fields:
                return "field-by-field";
            case FillKind::Unclaimed:
                break;
        }
        return "unwritten";
    }
} // namespace Desert::ShaderResources
