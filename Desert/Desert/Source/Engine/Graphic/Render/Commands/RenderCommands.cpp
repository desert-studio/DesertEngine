// The bodies of the render commands that call the renderer. The command headers only declare them, so the
// ECS systems that record commands see SceneRenderer by declaration (RenderCommand.hpp) and a renderer
// header edit rebuilds this file, not every system (UE: FSceneRenderer is private to the renderer module).
#include "DecalCommand.hpp"
#include "DrawGenericMeshCommand.hpp"
#include "DrawLandscapeGrassCommand.hpp"
#include "DrawLandscapeTileCommand.hpp"
#include "DrawMeshCommand.hpp"
#include "DrawSkinnedMeshCommand.hpp"
#include "DrawSlotMaterialMeshCommand.hpp"
#include "HeightFogCommand.hpp"
#include "HeterogeneousVolumeCommand.hpp"
#include "LocalFogVolumeCommand.hpp"
#include "PointLightCommand.hpp"
#include "ProceduralSkyCommand.hpp"
#include "SkyboxCommand.hpp"
#include "SpotLightCommand.hpp"
#include "VolumetricCloudCommand.hpp"
#include "WaterZoneCommand.hpp"
#include "WindGustCommand.hpp"
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>

namespace Desert::Graphic::Render
{
    void DecalCommand::Execute( SceneRenderer& renderer )
    {
    // COPIED, NOT MOVED: a scene with several views replays the same recorded command into each view's
    // renderer (SpotLightCommand.hpp says why a moved-from payload is a decal missing from one view).
    renderer.AddDecal( Decal );
    }

    void DrawGenericMeshCommand::Prepare()
    {
    if ( Mesh != nullptr )
        SceneRenderer::PrepareMesh( *Mesh );
    }

    void DrawGenericMeshCommand::Execute( SceneRenderer& renderer )
    {
    if ( !renderer.IsWithinMaxDrawDistance( Mesh, Transform, MaxDrawDistance ) )
        return;
    const Graphic::MaterialOverrides*                overrides  = &Overrides;
    const Graphic::RayTracing::RayTracingProgramHit* rayHit     = &RayHit;
    const std::string*                               shaderName = &ShaderName;
    // 0: the values are this command's own (hashed by the renderer); else the params' revision.
    SceneRenderer::GenericValuesRevision revision{};
    if ( Params != nullptr )
    {
        overrides         = &Params->Overrides;
        rayHit            = &Params->RayHit;
        revision.Revision = Params->Revision;
        if ( !Params->ShaderName.empty() )
        {
            shaderName             = &Params->ShaderName;
            revision.HasShaderName = true;
        }
    }
    renderer.SubmitGenericMesh( Entity, Mesh, Transform, *shaderName, *overrides, Outlined, DirectTexture,
                                DirectTextureSampler, CastShadows, VisibleInRayTracing, *rayHit,
                                revision );
    }

    void DrawLandscapeGrassCommand::Execute( SceneRenderer& renderer )
    {
    renderer.SubmitLandscapeGrass( *Draw ); // by reference: read until EndScene; the command outlives it
    }

    void DrawLandscapeTileCommand::Execute( SceneRenderer& renderer )
    {
    renderer.SubmitLandscapeTile( Heightmap, Tile, Overrides, Weights );
    }

    void DrawStaticMeshCommand::Prepare()
    {
    if ( Mesh != nullptr )
        SceneRenderer::PrepareMesh( *Mesh );
    }

    void DrawStaticMeshCommand::Execute( SceneRenderer& renderer )
    {
    if ( MaterialSlots && renderer.IsWithinMaxDrawDistance( Mesh, Transform, MaxDrawDistance ) )
        renderer.SubmitMesh( Mesh, MaterialSlots, Transform,
                             { .Entity                   = Entity,
                               .MotionPart               = MotionPart,
                               .SectionSlot              = SectionSlot,
                               .BoneMatrices             = {},
                               .Outlined                 = Outlined,
                               .HiddenSubmeshes          = HiddenSubmeshes,
                               .ForcedLOD                = ForcedLOD,
                               .LODBias                  = LODBias,
                               .CastShadows              = CastShadows,
                               .ReceiveShadows           = ReceiveShadows,
                               .CastContactShadow        = CastContactShadow,
                               .TranslucencySortPriority = TranslucencySortPriority } );
    }

    void DrawStaticSceneCommand::Prepare()
    {
    if ( !Scene )
        return;
    const uint64_t epoch = Desert::Mesh::GetStaleEpoch();
    if ( Scene->PreparedStaleEpoch == epoch )
        return;
    Scene->PreparedStaleEpoch = epoch;
    for ( const auto& chunk : Scene->Chunks )
        for ( const StaticSceneSection& section : chunk->Sections )
            if ( section.Mesh != nullptr )
                SceneRenderer::PrepareMesh( *section.Mesh );
    }

    void DrawStaticSceneCommand::Execute( SceneRenderer& renderer )
    {
    if ( Scene )
        renderer.SubmitStaticScene( *Scene );
    }

    void DrawInstancedSceneCommand::Prepare()
    {
    if ( !Scene )
        return;
    const uint64_t epoch = Desert::Mesh::GetStaleEpoch();
    if ( Scene->PreparedStaleEpoch == epoch )
        return;
    Scene->PreparedStaleEpoch = epoch;
    for ( const InstancedSceneRow& row : Scene->Rows )
        if ( row.Mesh != nullptr )
            SceneRenderer::PrepareMesh( *row.Mesh );
    }

    void DrawInstancedSceneCommand::Execute( SceneRenderer& renderer )
    {
    if ( Scene && Frame )
        renderer.SubmitInstancedScene( *Scene, *Frame );
    }

    void DrawSkinnedMeshCommand::Prepare()
    {
    if ( Mesh != nullptr )
        SceneRenderer::PrepareMesh( *Mesh );
    }

    void DrawSkinnedMeshCommand::Execute( SceneRenderer& renderer )
    {
    // CastShadows must survive this hop: the flag is consumed three links away (the cascade pass
    // skips !CastShadows on SkinnedMeshRenderData), and a default here would silently re-enable
    // the shadow for every skinned mesh whose component turned it off.
    renderer.SubmitMesh( Mesh, MaterialSlot, Transform,
                         { .Entity        = Entity,
                           .BoneMatrices  = BoneMatrices,
                           .MorphWeights  = MorphWeights,
                           .MorphRevision = MorphRevision,
                           .LodFeedback   = LodFeedback,
                           .Leader        = Leader,
                           .Outlined      = Outlined,
                           .CastShadows   = CastShadows } );
    }

    void DrawSlotMaterialMeshCommand::Prepare()
    {
    if ( Mesh != nullptr )
        SceneRenderer::PrepareMesh( *Mesh );
    }

    void DrawSlotMaterialMeshCommand::Execute( SceneRenderer& renderer )
    {
    if ( !renderer.IsWithinMaxDrawDistance( Mesh, Transform, MaxDrawDistance ) )
        return;
    renderer.SubmitSlotMaterialMesh( Entity, Mesh, Transform, SlotMaterial, VisibleSubmeshMask, Outlined,
                                     CastShadows, MotionPart );
    }

    void HeightFogCommand::Execute( SceneRenderer& renderer )
    {
    renderer.SetHeightFog( Present, Data, FogHeightY );
    }

    void HeterogeneousVolumeCommand::Execute( SceneRenderer& renderer )
    {
    // Copied, not moved: the same recorded command is replayed into every view's renderer.
    renderer.AddHeterogeneousVolume( Volume );
    }

    void LocalFogVolumeCommand::Execute( SceneRenderer& renderer )
    {
    // Copied, not moved: the same recorded command is replayed into every view's renderer.
    renderer.AddLocalFogVolume( Volume );
    }

    void PointLightCommand::Execute( SceneRenderer& renderer )
    {
    // COPIED, NOT MOVED, and that is a correctness fix rather than a style choice. A scene with
    // several views replays the SAME recorded command into each view's renderer (Scene::OnUpdate),
    // so a command that hands its payload away serves the first view and gives the second one a
    // moved-from value. For a light that is a light which exists in one viewport and not in the
    // other, with nothing in the log.
    renderer.AddPointLight( Light, Shadow, Function );
    }

    void ProceduralSkyCommand::Execute( SceneRenderer& renderer )
    {
    renderer.SetProceduralSky( Enabled, SunDir, BakeNow, Sky, Fx, Light1 );
    }

    void SkyboxCommand::Execute( SceneRenderer& renderer )
    {
    renderer.SetEnvironment( Skybox, Look );
    }

    void SpotLightCommand::Execute( SceneRenderer& renderer )
    {
    // COPIED, NOT MOVED, and that is a correctness fix rather than a style choice. A scene with
    // several views replays the SAME recorded command into each view's renderer (Scene::OnUpdate),
    // so a command that hands its payload away serves the first view and gives the second one a
    // moved-from value. For a light that is a light which exists in one viewport and not in the
    // other, with nothing in the log.
    renderer.AddSpotLight( Light, Shadow, Function );
    }

    void VolumetricCloudCommand::Execute( SceneRenderer& renderer )
    {
    renderer.SetVolumetricClouds( Present, Data, WindOffset, WindDirection, HeroClouds );
    }

    void WaterZoneCommand::Execute( SceneRenderer& renderer )
    {
    renderer.SubmitWaterZone( Zone ); // by reference: read until EndScene, which this command outlives
    }

    void WindGustCommand::Execute( SceneRenderer& renderer )
    {
    renderer.SetWindGust( Gust, PrevGust, DirectionalVelocityXZ );
    }

    void LocalWindSourceCommand::Execute( SceneRenderer& renderer )
    {
    renderer.AddLocalWindSource( Source );
    }
} // namespace Desert::Graphic::Render
