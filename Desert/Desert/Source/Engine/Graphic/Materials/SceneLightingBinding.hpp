#pragma once

#include <Engine/Core/Camera.hpp>
#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>
#include <Engine/Graphic/Materials/Properties/Texture2DProperty.hpp>
#include <Engine/Graphic/Materials/Properties/TextureCubeProperty.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Graphic/ShaderProtocols/Camera.hpp>
#include <Engine/Graphic/ShaderProtocols/DirectionLight.hpp>
#include <Engine/Graphic/ShaderProtocols/Metadata.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Desert::Graphic
{
    class Image2D;
    class ImageCube;

    // ------------------------------------------------------------------------------------------------
    // THE ONE WRITER of each per-frame scene block a shader can receive: the camera, the light payloads,
    // the shadow cascades and the IBL trio. Same shape and same reason as Graphic::CloudShadowUpload next
    // door — every material in this engine binds by NAME, so one function serves every shader that
    // declares the block, whatever slot number it chose for it.
    //
    // These bodies used to be static members of the former lit base class taking a MaterialInstance*, which each
    // immediately turned into GetParentMaterial(). That signature is what kept them out of reach of the
    // generic (data-driven) mesh path, which draws through a Material with no instance at all — and the
    // consequence was not "generic materials get a bit less". It was that MeshRenderer::DrawGenericMeshes
    // grew its OWN filler for the three blocks it happened to need (CameraUB, TimeUB, DirectionLightsUB),
    // which is a second implementation of the same job, and the blocks it did not think of — the
    // environment cubes, the light counts, the point and spot buffers, the cloud shadow — reached the lit
    // materials and nothing else. A custom-shader mesh therefore could not be lit like the mesh beside it
    // however its shader was written.
    //
    // EVERY LOOKUP IS GUARDED. A material whose shader does not declare a block gets nothing written and
    // no complaint: that is not a silent fallback, it is the whole mechanism by which one frame-state
    // applier serves the lit shaders, the terrain, an unlit graph material and the text system's SDF
    // quads. `LightsMetadata` in particular used to be dereferenced unguarded, which was a null crash
    // waiting for the first material without it — and the first material without it is every unlit
    // generic shader the moment it is handed the same snapshot.
    //
    // The BLOCK LAYOUT and the block NAMES are not restated here; they are SceneResources', which is
    // where Desert/Tests/Engine/SceneFrameBinding asserts them against the reflected GLSL. One mirror, so a
    // writer and a test cannot end up describing two different ShadowUBs.
    // ------------------------------------------------------------------------------------------------

    /// The camera block (Common/CameraUB.glslh) — THE one write of CameraUB into a material: the block is
    /// ShaderProtocols::MakeCameraUB of the view being drawn (SceneRenderer::GetViewFrame for a scene view,
    /// MakeStillViewFrame for a camera that is not a view: a light, an editor tool camera, a thumbnail).
    /// The C++ struct and the reflected block have one size (camera_ub_layout_test), so the whole struct goes.
    inline void SceneCameraBind( Material* material, const ViewFrame& frame )
    {
        if ( material == nullptr )
            return;

        if ( auto* ub = material->Get<UniformBufferProperty>( ShaderProtocols::Camera::Name ) )
        {
            const auto data  = ShaderProtocols::MakeCameraUB( frame );
            const auto bytes = std::as_bytes( std::span{ &data, 1 } );
            ub->SetRawData( bytes.data(), bytes.size() );
        }
    }

    /// Seconds since engine start, for any shader declaring `TimeUB { vec4 TimeData; }` — the shader
    /// graph's Time node is the reason it exists. x = seconds, yzw reserved.
    inline void SceneTimeBind( Material* material, float seconds )
    {
        if ( !material )
            return;
        if ( auto* ub = material->Get<UniformBufferProperty>( SceneResources::kTimeBlockName ) )
        {
            const glm::vec4 data( seconds, 0.0f, 0.0f, 0.0f );
            const size_t    size = std::min( sizeof( data ), static_cast<size_t>( ub->GetUniform()->GetSize() ) );
            ub->SetRawData( reinterpret_cast<const std::byte*>( &data ), size );
        }
    }

    /// The three light payloads plus the counts every consumer loops over. Written together because a
    /// count that disagrees with its buffer reads past the end of it.
    inline void SceneLightsBind( Material* material, const ShaderProtocols::PointLight& point,
                                 const ShaderProtocols::SpotLight&      spot,
                                 const ShaderProtocols::DirectionLight& dir )
    {
        if ( !material )
            return;

        // Point and spot lights live in unbounded std430 storage buffers. An EMPTY list is not written:
        // the consumer loops 0..count and the count below is zero, so a stale buffer is never read, and
        // writing nothing keeps the descriptor's own allocation in place.
        if ( !point.PointLights.empty() )
            if ( auto* sb = material->Get<StorageBufferProperty>( point.Name ) )
                sb->SetRawData( reinterpret_cast<const std::byte*>( point.PointLights.data() ),
                                static_cast<uint32_t>( point.PointLights.size() *
                                                       sizeof( ShaderProtocols::PointLightPayload ) ) );

        if ( !spot.SpotLights.empty() )
            if ( auto* sb = material->Get<StorageBufferProperty>( spot.Name ) )
                sb->SetRawData( reinterpret_cast<const std::byte*>( spot.SpotLights.data() ),
                                static_cast<uint32_t>( spot.SpotLights.size() *
                                                       sizeof( ShaderProtocols::SpotLightPayload ) ) );

        if ( !dir.DirectionLights.empty() )
            if ( auto* ub = material->Get<UniformBufferProperty>( dir.Name ) )
            {
                const size_t size =
                     std::min( dir.DirectionLights.size() * sizeof( ShaderProtocols::DirectionLightPayload ),
                               static_cast<size_t>( ub->GetUniform()->GetSize() ) );
                ub->SetRawData( reinterpret_cast<const std::byte*>( dir.DirectionLights.data() ), size );
            }

        // `Name` is a static member and not part of the object, so the block is just the three counts.
        const uint32_t counts[3] = { static_cast<uint32_t>( dir.DirectionLights.size() ),
                                     static_cast<uint32_t>( point.PointLights.size() ),
                                     static_cast<uint32_t>( spot.SpotLights.size() ) };
        if ( auto* ub = material->Get<UniformBufferProperty>( ShaderProtocols::LightsMetadata::Name ) )
            ub->SetRawData( reinterpret_cast<const std::byte*>( counts ), sizeof( counts ) );
    }

    /// The cascaded directional shadow maps (R32F light-space depth) and their per-cascade light
    /// view-projections, bias, enable flag and debug switches — the `ShadowUB` block plus u_ShadowMap0..3.
    inline void SceneShadowBind( Material* material, const glm::mat4* cascadeViewProj, uint32_t numCascades,
                                 float bias, bool enabled, int debugMode, bool showNormals,
                                 const glm::vec4& cascadeWorldPerTexel, bool lightingDebug )
    {
        if ( material == nullptr || cascadeViewProj == nullptr )
            return;

        // The block's layout and its cascade count are NOT restated here. They are one mirror
        // (SceneResources::ShadowUBData / ::kMaxCascades), and the reason is the defect shape this whole
        // seam exists to remove: a second declaration of one layout is a disagreement waiting to happen,
        // and Desert/Tests/Engine/SceneFrameBinding asserts that mirror against the reflected GLSL block —
        // an assertion a private copy here would quietly stop covering.
        constexpr uint32_t kMaxCascades = SceneResources::kMaxCascades;

        SceneResources::ShadowUBData data{};

        const uint32_t n = numCascades < kMaxCascades ? numCascades : kMaxCascades;
        for ( uint32_t i = 0; i < kMaxCascades; ++i )
            data.LightViewProj[i] = ( i < n ) ? cascadeViewProj[i] : glm::mat4( 1.0f );
        data.Params =
             glm::vec4( bias, enabled ? 1.0f : 0.0f, static_cast<float>( debugMode ), static_cast<float>( n ) );
        data.DebugParams       = glm::vec4( showNormals ? 1.0f : 0.0f, lightingDebug ? 1.0f : 0.0f, 0.0f, 0.0f );
        data.CascadeTexelWorld = cascadeWorldPerTexel;

        if ( auto* ub = material->Get<UniformBufferProperty>( SceneResources::kShadowBlockName ) )
            ub->SetRawData( reinterpret_cast<const std::byte*>( &data ), sizeof( data ) );
    }

    /// The scene's sky look — rotation and gain — for any program that declares `SkyLookUB`. THE ONE
    /// WRITER: the lit materials (through SceneEnvironmentBind), the deferred composite and the skybox
    /// pass all call this, so no reader of the environment cubes can be handed a differently packed look.
    /// Written EVERY call, identity included, for the same reason the cubes are: a block that is not
    /// written keeps what the previous scene left in it.
    inline void SceneSkyLookBind( Material* material, const SkyLook& look )
    {
        if ( material == nullptr )
            return;
        if ( auto* ub = material->Get<UniformBufferProperty>( kSkyLookBlockName ) )
        {
            const SkyLookGPU data = ToGPU( look );
            ub->SetRawData( reinterpret_cast<const std::byte*>( &data ), sizeof( data ) );
        }
    }

    /// The look of the IBL inputs of Mesh/AmbientIBL.glslh. MESH-PB1: the cubes and the split-sum BRDF LUT
    /// themselves are pass parameters (SceneViewInputs, FrameGraphRefs.hpp); the text below is their history.
    ///
    /// THE TWO CUBES ARE SET EVERY CALL, null included — that is the relation this function exists to
    /// keep: **the environment a surface is shaded by belongs to the scene that surface is in**. The
    /// comment that used to stand here claimed a null "keeps the descriptor's dummy cube"; it kept the
    /// PREVIOUS SCENE's cube, because a slot that is not written is a slot that remembers, and this
    /// applier is called from a producer that restates absence every frame precisely so that it need not.
    /// Measured before the fix, CornellDemo (which has no sky at all) after one visit to Clouds_Protocol:
    /// every pixel changed, mean 0.477 -> 0.794, green-wall saturation 0.626 -> 0.313.
    ///
    /// The BRDF LUT keeps its guard, and the asymmetry is deliberate: it is a renderer-global built once
    /// by Renderer::Init and never released, so it is never legitimately absent — a null here is a
    /// startup-order fault, and overwriting a good LUT with the fallback would hide it.
    ///
    /// @p look is how the two cubes are read (Environment::Look); it travels with them because a cube
    /// bound without its look is the unturned sky under a turned backdrop.
    inline void SceneEnvironmentBind( Material* material, const SkyLook& look )
    {
        if ( !material )
            return;

        SceneSkyLookBind( material, look );
    }
} // namespace Desert::Graphic
