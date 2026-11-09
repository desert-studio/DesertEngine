#pragma once

// The SPIRV-Cross half of the DXIL path (ShaderBinaryDXIL.hpp): one program's SPIR-V stages -> the root signature
// and each stage's HLSL, emitted against it. Internal to the shader compiler; the D3D12 runtime reads
// ShaderBinaryDXIL.hpp only. The DXC half (DXIL
// step 2, REMAINDER-DX12-SHADERS-1) hands the HLSL to DXC.

#include <Engine/Core/ShaderCompiler/ShaderBinaryDXIL.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core
{
    struct DxilHlslStage
    {
        Formats::ShaderStage         Stage = Formats::ShaderStage::Vertex;
        std::string                  Hlsl;
        std::vector<DxilVertexInput> VertexInputs;
    };

    struct DxilHlslProgram
    {
        RootSignatureDesc          RootSignature;
        std::vector<DxilHlslStage> Stages; // the input's order
    };

    // Reflects every stage, derives the root signature (the register rule), then emits each stage's HLSL with
    // exactly those registers. Refusals name @p shaderName and the stage (ShaderBinaryDXIL.hpp lists them).
    Common::ResultStr<DxilHlslProgram> CrossCompileProgramToHlsl( const std::vector<ShaderMapStage>& stages,
                                                                  std::string_view                   shaderName );

    // The DXC target profile of @p stage at kDxilShaderModel ("vs_6_6"), or empty for a stage D3D12 cannot run
    // from this path (the ray tracing pipeline's stages).
    std::wstring DxilTargetProfile( Formats::ShaderStage stage );
} // namespace Desert::Core
