#pragma once

// A SHADING MODEL IS DATA: one file, Editor/Resources/Shaders/ShadingModels/<Name>.shadingmodel.
//
// The file is a manifest followed by a GLSL body. The model's NAME is the file stem (the file never repeats it);
// its IDENTITY is the Guid, written once when the file is created (as an asset's) and never edited. Renaming the
// file renames the model; a material template names the model (`ShadingModel Toon`) and the parser resolves the
// name to the Guid, so the Guid is what the rest of the engine carries.
//
//     Guid <decimal 64-bit Common::UUID>;
//     Payload
//     {
//         <CustomData0|CustomData1> <DisplayName>;   // at most kMaxPayloadFloats lines, each pin at most once
//     }
//     Inputs
//     {
//         <SurfaceOutput field>;                     // every field the body's result depends on
//     }
//     <GLSL body: Evaluate and EvaluateAmbient, the signatures of ShadingModels/ShadingModelContract.glslh>
//
// Everything after the closing brace of Inputs, to the end of the file, is the body. Both blocks are required,
// and may be empty (Unlit's are). `//` comments are allowed anywhere in the manifest.
//
// The example — Toon.shadingmodel (a banded diffuse with a rim; the model the Toon check adds):
//
//     Guid 1484512969364128551;
//     Payload
//     {
//         CustomData0 Bands;      // 0..1 -> 2..8 light bands
//         CustomData1 RimWidth;
//     }
//     Inputs
//     {
//         BaseColor;
//         CustomData0;
//         CustomData1;
//     }
//
//     vec3 Evaluate( DesertLight L, DesertSurface S, DesertPayload P )
//     {
//         const float bands = mix( 2.0, 8.0, P.CustomData0 );
//         const float lit   = floor( max( dot( S.N, L.L ), 0.0 ) * L.Shadow * bands ) / bands;
//         const float rim   = smoothstep( 1.0 - P.CustomData1, 1.0, 1.0 - max( dot( S.N, S.V ), 0.0 ) );
//         return S.BaseColor * L.Radiance * ( lit + rim * lit );
//     }
//
//     vec3 EvaluateAmbient( DesertAmbient A, DesertSurface S, DesertPayload P )
//     {
//         return S.BaseColor * A.Irradiance * A.Occlusion;
//     }
//
// Refusals — every one names the file (ParseShadingModelManifest's error text starts with the path):
//   * no Guid, a Guid that does not parse, or the null Guid;
//   * a missing Payload or Inputs block, an unknown line inside one;
//   * Payload with more than kMaxPayloadFloats lines, a pin that is not CustomData0/CustomData1, a pin twice;
//   * an Inputs field that is not a SurfaceOutput field (checked by ShadingModelRegistry, which reads
//     SurfaceOutput from Mesh/Surface/SurfaceTypes.glslh — the struct is the one list of fields);
//   * a body that does not define both Evaluate and EvaluateAmbient.

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core::ShadingModels
{
    inline constexpr std::string_view kShadingModelExtension = ".shadingmodel";
    // Relative to the shader root (Editor/Resources/Shaders), which is also the #include root.
    inline constexpr std::string_view kShadingModelDirectory = "ShadingModels";
    inline constexpr std::string_view kContractInclude       = "ShadingModels/ShadingModelContract.glslh";
    // VIRTUAL (UE /Engine/Generated/): an include path the shader includer answers with the loaded set's
    // GenerateGlsl() text. No file exists at this path — the engine resource tree is source, never an output.
    inline constexpr std::string_view kGeneratedInclude = "Generated/ShadingModels.glslh";

    // The index is four bits of the shading word: at most 16 models, Unlit always 0.
    inline constexpr std::size_t kMaxShadingModels = 16;
    inline constexpr std::size_t kMaxPayloadFloats = 2;

    // The two models the engine itself relies on, by identity. Unlit gets index 0 (a G-buffer writer that forgets
    // the index shades as emission only — visible at once, as in UE, MSM_Unlit = 0). DefaultLit is the model of a
    // surface template that names none. Their files carry exactly these Guids.
    inline constexpr std::uint64_t kUnlitGuid      = 6633879868943240298ull;
    inline constexpr std::uint64_t kDefaultLitGuid = 6570679776162095495ull;

    // The payload pins, UE's CustomData0/1: SurfaceOutput fields the material writes and DesertPayload fields the
    // model reads. The order is the order of the shading word's payload fields below.
    inline constexpr std::array<std::string_view, kMaxPayloadFloats> kPayloadPins{ "CustomData0", "CustomData1" };

    // ------------------------------------------------------------------------------------------------------------
    // Where the payload lives: the shading word, GBufferC.w (RGBA32F), the one free space of the G-buffer
    // (Pass_GBuffer.glslh:7-10: A, B, Emissive and C.xyz are full). A 32-bit float holds every integer below
    // 2^24 exactly, so the word is 24 bits of fields. ShadingModelContract.glslh (DESERT_SHADING_WORD_*) is the
    // GLSL half of this table, and the ShadingModelContract suite holds the two equal.
    struct ShadingWordField
    {
        std::string_view Name;
        std::uint8_t     FirstBit;
        std::uint8_t     BitCount;
    };

    inline constexpr std::uint8_t kShadingWordExactBits = 24;

    inline constexpr std::array<ShadingWordField, 4> kShadingWordFields{ {
         { "INDEX", 0, 4 },    // shading-model index, 0..15
         { "TEXTURES", 4, 4 }, // sampled-texture count (Material Complexity), clamped to 15
         { "PAYLOAD0", 8, 8 }, // CustomData0, unorm8
         { "PAYLOAD1", 16, 8 } // CustomData1, unorm8
    } };

    // The word's fifth field is NOT a magnitude field, so it is not a row of kShadingWordFields (whose rows all
    // sit below kShadingWordExactBits): the float's sign bit, set = the surface does not receive the sun's
    // cascaded shadows (SurfaceOutput.ReceiveSunShadows < 0.5). The G-buffer writer marks it after packing; the
    // deferred reader takes the magnitude before unpacking. DESERT_SHADING_WORD_RECEIVE_SUN_SHADOWS_BIT is its
    // GLSL half.
    inline constexpr ShadingWordField kShadingWordSignField{ "RECEIVE_SUN_SHADOWS", 31, 1 };

    // ------------------------------------------------------------------------------------------------------------
    struct PayloadSlot
    {
        std::string Pin;         // one of kPayloadPins
        std::string DisplayName; // the material author's label
    };

    struct ShadingModelManifest
    {
        Common::UUID             Guid;
        std::string              Name;    // the file stem
        std::vector<PayloadSlot> Payload; // <= kMaxPayloadFloats
        std::vector<std::string> Inputs;  // SurfaceOutput field names, as written
        std::string              Body;    // the GLSL after the Inputs block
        std::filesystem::path    SourcePath;
    };

    // Parses one file's text. @p sourcePath gives the Name (its stem) and prefixes every error.
    // Does not check Inputs against SurfaceOutput (the registry does, holding the one field list).
    Common::ResultStr<ShadingModelManifest> ParseShadingModelManifest( std::string_view             text,
                                                                       const std::filesystem::path& sourcePath );
} // namespace Desert::Core::ShadingModels
