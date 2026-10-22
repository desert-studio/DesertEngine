#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Common/Settings/RenderMethods.hpp>
#include <Engine/Assets/StaticLighting/LightingBuildData.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>

// LIGHTMASS CONTRACT (owner backlog LIGHTMASS, 10-21) — HOW THE RENDERER READS THE BAKE. Header contract only.
//
// UE counterparts: FLightMapInteraction / FShadowMapInteraction and the per-primitive FLightmapSceneShaderData
// (GPU Scene's lightmap rows), LightmapCommon.ush GetLightMapColorHQ, the base pass's
// GetPrecomputedIndirectLightingAndSkyLight, FVolumetricLightmapSceneData + VolumetricLightmapShared.ush,
// the GBuffer's PrecomputedShadowFactors (GBufferE), the stationary light's DistanceFieldShadowMap path.
//
// COEXISTENCE WITH THE GI METHODS — the rule (Graphic::ResolveRenderMethods stays the one point):
//   - GIMethod::Lightmaps (rung 1 of the ladder, already declared in Common/Settings/RenderMethods.hpp) is the
//     ONLY method that reads the bake for indirect light. Its implementation flips IsImplemented(Lightmaps).
//     It needs the G-buffer (NeedsGBuffer flips true for it) because direct lighting stays deferred.
//   - Every other method ignores the indirect bake, as UE's Lumen does (UE: with Lumen, precomputed lighting is
//     hidden). Then Static and Stationary lights are drawn as Movable — direct light and shadows by the
//     resolved methods — and the overlay states "Static lighting unused: GI = <method>". Not silent, and not a
//     fallback: it is the method's definition (UE r.AllowStaticLighting=0 behaves the same).
//   - A view resolving to Lightmaps with no (or a stale) build renders what the build has and the overlay
//     lists the unbuilt / stale count (UE "LIGHTING NEEDS TO BE REBUILT"); a primitive with NO record is lit by
//     the volumetric lightmap like a Movable one, a scene with no build at all is refused at the resolution
//     (downgrade to the next rung, one log line, the existing mechanism).
//   - Reflections are independent: Captures / ScreenSpace / traced reflections run over a lightmapped GI as
//     today; the reflection captures are captured with the lightmapped lighting (UE does the same).
//   - Shadows (Q2, lead 10-22): a Static light is drawn by NO runtime shadow pass on a Lightmaps view (no VSM
//     page, no cascade, no traced ray: its shadow is in the lightmap). A Stationary light's shadow is
//     min( baked distance-field shadow map channel, the resolved shadow method over MOVABLE casters only ) under
//     EVERY shadow method: VirtualMaps / CascadedMaps render only Movable casters into its pages / cascades,
//     HardwareTraced / Stochastic trace with the instance mask excluding Static primitives (UE: the CSM covers
//     Movable casters, the shadow map the static ones). Static geometry is never shadowed twice.
//
// WHERE PER-PIXEL BAKE SAMPLING HAPPENS: in the base (G-buffer) pass of a Static primitive, which alone has the
// lightmap UV. It writes the baked indirect diffuse irradiance (SH evaluated at the shading normal, times the
// diffuse colour — UE adds it into scene colour in the base pass) into the scene colour target, and the
// stationary shadow factors (4 channels) into one RGBA8 G-buffer target that exists only on a Lightmaps view
// (UE GBufferE). Movable primitives read the volumetric lightmap per pixel in the base pass too (UE: per-pixel
// VLM for opaque). Translucency, fog, hair, particles read the volume.
namespace Desert::Graphic::StaticLighting
{
    // Per GPU-scene row of a lightmapped primitive (UE FLightmapSceneShaderData): uploaded once at registration.
    // shader mirror: ShaderProtocols/StaticLighting.glslh (new with the implementation).
    struct LightmapRowGpu
    {
        glm::vec4  LightmapScaleBias{ 0.0f }; // xy scale, zw bias into the page
        glm::vec4  ShadowMapScaleBias{ 0.0f };
        glm::vec4  CoefficientScale[2]{};
        glm::vec4  CoefficientAdd[2]{};
        glm::uvec4 PagesAndChannels{ 0u }; // x lightmap page, y shadow page, z channel mask (4 bits), w flags
    };
    static_assert( sizeof( LightmapRowGpu ) == 112 );

    // What this frame's passes read of the bake: graph imports of the persistent atlas textures, valid exactly
    // when Active (the view resolved to Lightmaps and a build is registered).
    struct StaticLightingFrame
    {
        bool            Active = false;
        RDG::BufferRef  LightmapRows; // LightmapRowGpu per (GPU-scene row, LOD) (zero row = not lightmapped)
        RDG::TextureRef LightmapPages[static_cast<uint32_t>( Assets::LightmapTexture::Count )]; // 2D arrays
        RDG::TextureRef ShadowMapPages; // 2D array, RGBA8 = 4 stationary channels
        RDG::TextureRef VolumeIndirection;
        RDG::TextureRef VolumeAmbientVector;
        RDG::TextureRef VolumeSHCoefficients[6];
        RDG::TextureRef VolumeSkyBentNormal;
        RDG::TextureRef VolumeDirectionalShadowing;
        RDG::BufferRef VolumeUniforms; // bounds, indirection size, brick atlas size (UE FVolumetricLightmapParams)
        uint32_t       StationaryLightCount = 0;
    };

    // The scene's ONE owner of the registered build: per scene, not per view (the bake is world data); the frame
    // record above is per view (PerViewState rule: what differs per view is the method resolution).
    class StaticLightingRenderer
    {
    public:
        // The scene's lighting build changed (loaded, rebaked, cleared with null). Uploads the rows of every
        // registered primitive and imports the pages; logs the stale count.
        Common::BoolResultStr SetBuild( std::shared_ptr<const Assets::LightingBuildDataAsset> build );

        // A lit primitive joined / left the GPU scene (its row's lightmap record looked up by entity UUID).
        // Q6: one LightmapRowGpu per LOD, at LightmapRows[the row's lightmap base + lod]; the base pass picks the
        // row of the LOD it draws.
        void OnPrimitiveRegistered( uint32_t gpuSceneRow, const Common::UUID& entity, uint32_t instanceIndex,
                                    uint32_t lodCount, uint64_t liveInputHash );
        void OnPrimitiveRemoved( uint32_t gpuSceneRow );

        // How a light of the given authored mobility is drawn by a view whose GI resolved to `gi`: Static ->
        // baked-only under Lightmaps / Movable otherwise; Stationary -> its baked channel, or Movable when it has
        // none or the view is not Lightmaps. The deferred lighting and the shadow passes ask THIS, not the
        // component's field.
        enum class LightTreatment : uint8_t
        {
            BakedOnly,       // nothing at runtime
            StationaryBaked, // dynamic direct over the baked static shadow channel
            Movable,
        };
        [[nodiscard]] LightTreatment TreatmentOf( const Common::UUID& light, Common::Scalability::GIMethod gi ) const;

        // Which casters the runtime shadow method draws for a light of this treatment (Q2): BakedOnly -> none (no
        // shadow pass), StationaryBaked -> Movable only (min'ed with the baked channel), Movable -> all.
        enum class RuntimeShadowCasters : uint8_t
        {
            None,
            MovableOnly,
            All,
        };
        [[nodiscard]] static RuntimeShadowCasters ShadowCastersOf( LightTreatment treatment );

        // This view's record; Active only for gi == Lightmaps with a build.
        [[nodiscard]] StaticLightingFrame AddFrame( RDG::Builder& graph, Common::Scalability::GIMethod gi );

        // "Lighting needs to be rebuilt": primitives and lights whose InputHash disagrees, plus unbaked ones.
        [[nodiscard]] uint32_t StaleCount() const;
    };
} // namespace Desert::Graphic::StaticLighting
