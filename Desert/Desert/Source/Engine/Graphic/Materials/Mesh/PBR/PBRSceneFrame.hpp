#pragma once

#include <Engine/Core/Camera.hpp>
#include <Engine/Graphic/Clouds/CloudShadowPayload.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>
#include <Engine/Graphic/Materials/Mesh/PBR/MaterialPBRBase.hpp>
#include <Engine/Graphic/ShaderProtocols/DirectionLight.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    class Image2D;
    class ImageCube;

    /**
     * Everything the SCENE (not the object) contributes to a lit PBR draw: the camera, the lights, the
     * shadow cascades, the IBL environment and the cloud layer's shadow. Gathered ONCE per frame
     * (MeshRenderer::CaptureFrameState) and applied to whichever material is about to be bound — with or
     * without an instance, which is what lets the data-driven (shader-graph) queue be drawn with the same
     * snapshot the PBR queue is.
     *
     * It lives beside the materials rather than inside MeshRenderer because it is the PAYLOAD of the PBR
     * materials — a material's Bind() can then require the WHOLE snapshot instead of being handed a
     * selection of it. That is the defect this type was moved here to close: the skinned material used
     * to take the camera, the lights and the cloud shadow as three separate fields of its own Bind()
     * argument, so the two pieces of scene state that were NOT in that list — the shadow cascades and
     * the environment cubes — reached every static mesh in the engine and no skinned one.
     *
     * Neither slot was garbage, which is why nothing ever crashed or warned:
     * VulkanMaterialBackend::InitializeWithFallbacks seeds every declared binding before anything real
     * reaches it. `ShadowUB` therefore held the zero-filled dummy buffer — `u_ShadowParams.y == 0`, so
     * ShadowFactor() returned 1.0 and the cascades were silently OFF — and the environment trio held
     * its fallback images, which sample BLACK (the fallback cube is created with white pixels, but
     * VulkanImageCube::UploadData is an empty function, so nothing is ever put in it), leaving the
     * split-sum ambient at zero and a skinned surface lit by the sun and the anti-black floor alone.
     * Measured on a probe scene: a skinned box read a flat 0.453 luminance at saturation 0.008 against
     * an identical static cube's 0.703 lit / 0.566 shadowed at saturation 0.139 / 0.287.
     *
     * There is deliberately no second constructor and no per-field setter: the one producer fills it and
     * the one applier writes it. It is also the state that must eventually move out of the shared
     * material and into a per-renderer descriptor set (the contract's per-frame renderer state rule);
     * until then this is the single point every write goes through, rather than the same five calls
     * copied at each call site.
     */
    struct PBRSceneFrame
    {
        const Core::Camera* Camera = nullptr;

        const ShaderProtocols::PointLight*     PointLights     = nullptr;
        const ShaderProtocols::SpotLight*      SpotLights      = nullptr;
        const ShaderProtocols::DirectionLight* DirectionLights = nullptr;

        const glm::mat4* CascadeViewProj = nullptr; // MaterialPBRBase::kMaxCascades entries
        Image2D*         CascadeMaps[MaterialPBRBase::kMaxCascades] = {};
        // How many of the two above are REAL. The producing renderer's own shadow budget
        // (Graphic::ShadowQuality), not the ceiling: an asset preview allocates one cascade, and the
        // shader's cascade loop is driven by this number. It defaults to the ceiling so a snapshot built
        // by hand behaves as every snapshot did before the budget existed.
        uint32_t         CascadeCount = MaterialPBRBase::kMaxCascades;
        glm::vec4        CascadeTexelWorld{ 0.0f };
        float            ShadowBias      = 0.0f;
        bool             ShadowsEnabled  = true;
        int              ShadowDebugMode = 0;
        bool             ShowNormals     = false;
        bool             LightingDebug   = false;

        ImageCube* IrradianceMap  = nullptr;
        ImageCube* PrefilteredMap = nullptr;
        Image2D*   BrdfLut        = nullptr;
        // How the two cubes above are read — Environment::Look, captured with them from the same answer.
        SkyLook EnvironmentLook{};

        // The cloud layer's shadow on the sun — the SECOND occluder, beside the cascades above. It
        // belongs in this snapshot for the reason the snapshot exists: it is scene state, one per
        // frame, and it has to reach the opaque pass, the glass pass, the RSM and the skinned pass
        // identically. While it did not, the only surfaces in the engine that received a cloud shadow
        // were the ones a deferred composite happened to shade.
        CloudShadowInput CloudShadow;

        // Writes the whole snapshot onto @p material. One call, so a new piece of frame state can never
        // be applied at four of the five sites and forgotten at the fifth.
        //
        // It reaches materials of several different shaders (StaticMeshPBR, StaticMeshPBR_Instanced,
        // StaticMeshGlass, SkinnedMeshPBR, a generated shader-graph shader), which need not declare these
        // resources at the same SLOT NUMBERS — every write goes through Material::Get by NAME and every
        // lookup is guarded, which is what makes one applier able to serve all of them AND to cost the
        // ones that declare nothing nothing at all: an unlit graph material, the Unlit shader and the
        // text system's SDF quads receive exactly what they ask for.
        //
        // The MATERIAL overload is the one that matters. While the only entry point took a
        // MaterialInstance, the generic (data-driven) mesh path — which has no instance — could not be
        // handed the snapshot at all, and hand-filled three of its blocks and none of the rest. That is
        // why a custom-shader mesh had no environment, no cloud shadow and no punctual lights however
        // its shader was written.
        void ApplyTo( Material* material ) const;
        void ApplyTo( MaterialInstance* instance ) const;
    };
} // namespace Desert::Graphic
