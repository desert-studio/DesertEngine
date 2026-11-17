#pragma once

// THE D3D12 SHADER PATH'S ONE INTERFACE: what the shader compiler hands the Direct3D 12 runtime
// (Graphic/API/D3D12) for one program: the DXIL of every stage and the root signature the stages were compiled
// against. The runtime includes this header and nothing else from the compiler; it never sees SPIR-V, HLSL or DXC.
//
// HOW A PROGRAM GETS HERE (ShaderDxilCompile.cpp): the DSL text is compiled to SPIR-V exactly as for Vulkan
// (BuildShaderMap: parse, preprocess, shaderc, the program numbering passes, the SPIR-V cache and the shader map
// cache), then each stage's SPIR-V is cross-compiled to HLSL by SPIRV-Cross's HLSL backend and compiled to DXIL by
// DXC. The SPIR-V map stays the one source of a program's resources: the root signature below is derived from the
// SAME SPIRV-Cross reflection that emits the HLSL registers, so the two cannot disagree. UE compiles HLSL for
// D3D12 and cross-compiles it (ShaderConductor) for Vulkan/Metal; our source language is GLSL, so the direction is
// the other way round and the reflection is SPIR-V's, not D3D12's.
//
// THE REGISTER RULE (one rule, applied by the HLSL emission and the root signature alike):
//   Vulkan (set S, binding B) -> HLSL register B of its class (b = CBV, t = SRV, u = UAV, s = sampler) in space S.
//   A combined image sampler at (S, B) is two registers: t B and s B, both in space S.
//   Push constants, the base vertex/instance pair and the dispatch size live in kDxilRootConstantSpace.
// Every storage buffer is a UAV (RW[ByteAddress|Structured]Buffer), readonly or not, and every storage image too,
// so the class of a binding depends on its Vulkan descriptor type alone: the runtime creates storage buffers with
// D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS.

#include <Engine/Core/Formats/Shader.hpp>
#include <Engine/Core/ShaderCompiler/ShaderMapCache.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core
{
    // THE DXIL TARGET IS THE OLDEST ACCEPTED DEVICE, NOT THE CURRENT ONE (the rule ShaderTarget.hpp states for
    // SPIR-V): DXIL is cached and shipped, so its shader model is fixed. 6.6 is UE 5's SM6 floor (dynamic
    // resources, 64-bit atomics, wave ops, 16-bit types, mesh shaders and inline ray query from 6.5); every D3D12
    // device the runtime accepts must report D3D_SHADER_MODEL_6_6. The value is 10 * major + minor.
    inline constexpr uint32_t kDxilShaderModel = 66;

    // The register space of the program-wide root constants; far above any descriptor set a shader declares.
    inline constexpr uint32_t kDxilRootConstantSpace = 900;
    // Their b registers inside it.
    inline constexpr uint32_t kDxilPushConstantRegister       = 0; // the stages' push constant block
    inline constexpr uint32_t kDxilBaseVertexInstanceRegister = 1; // int SPIRV_Cross_BaseVertex, _BaseInstance
    inline constexpr uint32_t kDxilNumWorkgroupsRegister      = 2; // uint3 SPIRV_Cross_NumWorkgroups_count

    // A range's Count when the shader declared a runtime-sized array (the bindless texture table): D3D12's
    // UINT_MAX, an unbounded range. Such a range is always ALONE in its table (D3D12 allows nothing after it).
    inline constexpr uint32_t kDxilUnboundedCount = 0xffffffffu;

    // The root signature's DWORD budget (D3D12: 64). Root constants cost one per 32-bit value, a table one.
    inline constexpr uint32_t kDxilRootSignatureDwordLimit = 64;

    // Values equal to D3D12_DESCRIPTOR_RANGE_TYPE, so the runtime casts.
    enum class DxilRangeType : uint8_t
    {
        Srv     = 0,
        Uav     = 1,
        Cbv     = 2,
        Sampler = 3
    };

    // Values equal to D3D12_SHADER_VISIBILITY.
    enum class DxilShaderVisibility : uint8_t
    {
        All      = 0,
        Vertex   = 1,
        Hull     = 2,
        Domain   = 3,
        Geometry = 4,
        Pixel    = 5
    };

    // What a root parameter carries, so the runtime knows what to write into it at draw/dispatch time.
    enum class DxilRootSource : uint8_t
    {
        PushConstants,      // root constants: the bytes the Vulkan path pushes (vkCmdPushConstants), offset 0
        BaseVertexInstance, // root constants: { int baseVertex, int baseInstance } of the draw (D3D's SV_VertexID
                            // and SV_InstanceID start at 0; Vulkan's gl_VertexIndex/gl_InstanceIndex do not)
        NumWorkgroups,      // root constants: { uint x, y, z } of the dispatch (HLSL has no gl_NumWorkGroups)
        DescriptorTable     // a CBV/SRV/UAV or sampler table of one descriptor set
    };

    // One D3D12_DESCRIPTOR_RANGE1, plus where it came from.
    struct DxilDescriptorRange
    {
        DxilRangeType Type          = DxilRangeType::Srv;
        uint32_t      BaseRegister  = 0; // == the Vulkan binding (the register rule)
        uint32_t      RegisterSpace = 0; // == the Vulkan set
        uint32_t      Count         = 1; // descriptors; kDxilUnboundedCount for a runtime array
        uint32_t      OffsetInTable = 0; // descriptors from the table's start; ranges are packed in binding order
        std::string   Name; // the resource's name in the SPIR-V (diagnostics, the runtime's binding map)

        bool operator==( const DxilDescriptorRange& ) const = default;
    };

    // One D3D12_ROOT_PARAMETER1. RootConstants sources use ShaderRegister/RegisterSpace/Num32BitValues; a table
    // uses Set, SamplerTable and Ranges.
    struct DxilRootParameter
    {
        DxilRootSource       Source         = DxilRootSource::DescriptorTable;
        DxilShaderVisibility Visibility     = DxilShaderVisibility::All;
        uint32_t             ShaderRegister = 0;
        uint32_t             RegisterSpace  = 0;
        uint32_t             Num32BitValues = 0;
        uint32_t             Set            = 0;     // the Vulkan descriptor set a table holds
        bool                 SamplerTable   = false; // true: lives in the sampler heap (D3D12 keeps them apart)
        std::vector<DxilDescriptorRange> Ranges;

        bool operator==( const DxilRootParameter& ) const = default;
    };

    // The program's root signature, in parameter order: root constants first (PushConstants, BaseVertexInstance,
    // NumWorkgroups, each only when a stage uses it), then per descriptor set ascending: its bounded CBV/SRV/UAV
    // table, its bounded sampler table, then one table per unbounded range in binding order.
    struct RootSignatureDesc
    {
        std::vector<DxilRootParameter> Parameters;
        // D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT: the vertex stage reads vertex inputs.
        bool AllowInputAssemblerInputLayout = false;

        bool operator==( const RootSignatureDesc& ) const = default;
    };

    // One vertex input: its D3D12_INPUT_ELEMENT_DESC SemanticName is "TEXCOORD" and SemanticIndex is the Vulkan
    // location (SPIRV-Cross's default semantic), so the runtime pairs it with the Vulkan path's vertex layout by
    // location alone.
    struct DxilVertexInput
    {
        uint32_t Location = 0;

        bool operator==( const DxilVertexInput& ) const = default;
    };
    inline constexpr std::string_view kDxilVertexInputSemantic = "TEXCOORD";

    // One stage's DXIL container, as DXC wrote it (signed by the DXC validator, dxil.dll, beside dxcompiler.dll;
    // an unsigned container is refused at compile time, never handed over). The entry point is "main".
    struct ShaderBinaryDXIL
    {
        Formats::ShaderStage         Stage = Formats::ShaderStage::Vertex;
        std::vector<uint8_t>         Dxil;
        std::vector<DxilVertexInput> VertexInputs; // vertex stage only, ascending by location

        bool operator==( const ShaderBinaryDXIL& ) const = default;
    };

    // A program for D3D12: its stages ascending by stage value (the SPIR-V map's order) and their root signature.
    // The metadata (parameters, render state, pass names) is the SPIR-V map's ShaderProgramMeta; this map does not
    // copy it.
    struct ShaderMapDXIL
    {
        std::vector<ShaderBinaryDXIL> Stages;
        RootSignatureDesc             RootSignature;

        bool operator==( const ShaderMapDXIL& ) const = default;
    };

    // The root signature alone, from a program's SPIR-V stages (what BuildShaderMapDXIL compiles against).
    Common::ResultStr<RootSignatureDesc> DeriveRootSignatureDXIL( const std::vector<ShaderMapStage>& stages,
                                                                  std::string_view                   shaderName );

} // namespace Desert::Core
