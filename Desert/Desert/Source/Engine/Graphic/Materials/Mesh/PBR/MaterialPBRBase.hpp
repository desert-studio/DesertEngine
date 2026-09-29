#pragma once

#include <Engine/Graphic/Materials/Material.hpp>

#include <glm/glm.hpp>

#include <cstdint>

namespace Desert::Graphic
{
    // Shared base for PBR materials. It carries no per-frame uploads of its own: the scene's contribution
    // to a lit draw is Graphic::PBRSceneFrame, and PBRSceneFrame::ApplyTo writes it through the one set of
    // writers in Engine/Graphic/Materials/SceneLightingBinding.hpp, which take a Material and therefore
    // reach the skinned path and the generic (data-driven) path as well as this one. Material parameters
    // themselves travel as a Materials[] row (Core/Formats/MaterialParamRow.hpp).
    //
    // What is left here is the half of the CPU/GLSL contract that a test can hold still: how many cascades
    // the ShadowUB block carries, its byte layout, and the NAMES every writer looks the blocks up under.
    class MaterialPBRBase : public Material
    {
    public:
        // How many shadow cascades the ShadowUB block below carries, and therefore how many cascade maps
        // a lit draw binds. ONE number for the whole chain: this mirror, PBRSceneFrame::CascadeMaps and
        // MeshRenderer::kNumCascades are all defined from it, so a fifth cascade cannot be added to one
        // of them and forgotten in the other two.
        static constexpr uint32_t kMaxCascades = 4;

        // The C++ half of the `ShadowUB` block every lit mesh shader declares — the three mesh PBR
        // shaders, and since the shader graph's Lit surfaces compile the same shared shading text, those
        // too. It is PUBLIC because it is one half of a pair that must agree byte for byte, and the other
        // half is GLSL: reflecting the block and comparing it with this is the only way to assert that
        // agreement on a machine with no device (Desert/Tests/Engine/PBRSceneFrame).
        struct ShadowUBData
        {
            glm::mat4 LightViewProj[kMaxCascades];
            glm::vec4 Params;            // x = bias, y = enabled, z = debug mode, w = cascade count
            glm::vec4 DebugParams;       // x = show normals, y = lighting debug
            glm::vec4 CascadeTexelWorld; // per-cascade world size of one shadow-map texel
        };

        // THE names the writers look up. A material binds by NAME (Material::Get), so these — and not the
        // slot numbers, which differ between the static shader, the skinned one and a generated graph
        // shader — are what the shaders and the CPU have to agree on. Named constants rather than literals
        // at the writer, so the test can assert the agreement against the same strings the writer uses.
        static constexpr const char* kShadowBlockName              = "ShadowUB";
        static constexpr const char* kShadowMapNames[kMaxCascades] = { "u_ShadowMap0", "u_ShadowMap1",
                                                                       "u_ShadowMap2", "u_ShadowMap3" };
        static constexpr const char* kEnvIrradianceName            = "u_EnvIrradianceTex";
        static constexpr const char* kEnvSpecularName              = "u_EnvSpecularTex";
        static constexpr const char* kBrdfLutName                  = "u_BRDFLUTTexture";

    protected:
        MaterialPBRBase( std::string&& debugName, std::string&& shaderName );
        ~MaterialPBRBase() override = default;

        // The five per-frame uploads (camera / lights / cascades / environment / cloud shadow) used to be
        // static members here taking a MaterialInstance*, with a forwarder apiece on StaticMaterialPBR.
        // That signature is what kept them out of reach of the generic mesh path, which draws through a
        // Material with no instance at all — so the bodies are in SceneLightingBinding.hpp taking a
        // Material*, and PBRSceneFrame::ApplyTo is their one caller. Neither the forwarders nor the
        // adapters have a caller left, so neither exists.
    };
} // namespace Desert::Graphic
