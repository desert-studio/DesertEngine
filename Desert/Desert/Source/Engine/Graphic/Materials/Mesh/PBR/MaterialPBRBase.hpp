#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>

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
        // The scene-resource contract moved to SceneResources.hpp, where any surface template reaches it.
        // These forwarders remain only while MeshRenderer still names them (MAT1a-T3b removes the class).
        static constexpr uint32_t kMaxCascades = SceneResources::kMaxCascades;
        using ShadowUBData                     = SceneResources::ShadowUBData;

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
