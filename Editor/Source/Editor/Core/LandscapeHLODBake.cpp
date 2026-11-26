#include "LandscapeHLODBake.hpp"

#include "HLODBakeWriter.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/CameraEntityView.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/SceneRenderCollectors.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Core/Serialize/StoredAssetForm.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>
#include <LandscapeCore/Public/LandscapeWorldPartition.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Geometry/RenderMeshData.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Timestep.hpp>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>

namespace Desert::Editor
{
    namespace
    {
        // The long side of a baked tile, in texels (UE's landscape HLOD texture size default is 256 per
        // component; a whole tile here spans many components, so the bake is sized for the tile).
        constexpr uint32_t kBakeLongSide = 1024;
        // Frames recorded before the albedo is read: the landscape's material and its textures are resolved
        // while the scene draws (AssetThumbnailRenderer warms the same way).
        constexpr int kLandscapeBakeWarmFrames = 60;
        // The camera hangs this high above the tile and sees this deep: any landscape height lies between.
        constexpr float kLandscapeBakeEyeHeightCm = 5.0e5f;
        constexpr float kLandscapeBakeNearCm      = 100.0f;
        constexpr float kLandscapeBakeFarCm       = 1.0e6f;

        // THE NORMAL MAP, in the tangent frame the runtime shades the HLOD mesh with: every texel of the tile's
        // HLOD render mesh in UV space (HLODBake::RasterizeUV, the albedo's own layout), the G-buffer's world
        // normal taken into the frame the vertex shader interpolates there. Texels no triangle covers stay flat
        // (0, 0, 1).
        [[nodiscard]] std::vector<uint8_t> BakeTangentNormals( const ::Desert::Geometry::RenderMeshData& mesh,
                                                               const HLODBake::GBufferTexels&            texels )
        {
            std::vector<uint8_t> out = HLODBake::FlatNormals( texels.Width, texels.Height );
            HLODBake::RasterizeUV( mesh, texels.Width, texels.Height,
                                   [&]( std::size_t texel, const glm::vec3& weights,
                                        const std::array<const ::Desert::Vertex*, 3>& v )
                                   {
                                       static_cast<void>( HLODBake::EncodeTangentNormal(
                                            HLODBake::WorldNormal( texels, texel ),
                                            HLODBake::InterpolatedFrame( weights, v ), &out[texel * 4] ) );
                                   } );
            return out;
        }

        // Only the landscape (its roots and tiles) enters the bake's scene: anything standing on the ground
        // would otherwise be baked into it (UE renders the landscape proxy alone).
        void KeepOnlyLandscape( ::Desert::Core::LoadableScene& loaded )
        {
            namespace Rules     = ::Desert::Core::Rules;
            auto&      entities = loaded.Scene.Entities;
            const auto isLand   = []( const Assets::EntityData& record )
            {
                for ( const auto& [name, payload] : record.Components )
                    if ( name == LandscapeCore::kLandscapeRootComponent ||
                         name == LandscapeCore::kLandscapeTileComponent )
                        return true;
                return false;
            };
            entities.erase( std::remove_if( entities.begin(), entities.end(),
                                            [&]( const Assets::EntityData& record )
                                            { return !isLand( record ); } ),
                            entities.end() );
            // The bake's scene is no partitioned world: everything it holds is loaded and drawn.
            loaded.Scene.WorldPartition.reset();
        }
    } // namespace

    LandscapeHLODBake::LandscapeHLODBake( std::filesystem::path scenePath, ::Desert::Core::LoadableScene saved,
                                          std::vector<LandscapeHLODBakeTile> tiles,
                                          const Assets::AssetManager*        assets )
         : m_ScenePath( std::move( scenePath ) ), m_Saved( std::move( saved ) ), m_Tiles( std::move( tiles ) ),
           m_Assets( assets )
    {
        KeepOnlyLandscape( *m_Saved );
        if ( m_Tiles.empty() )
            m_Phase = Phase::Done;
    }

    LandscapeHLODBake::~LandscapeHLODBake()
    {
        m_Albedo.reset();
        m_GBufferB.reset();
        m_Emissive.reset();
        if ( !m_Renderer )
            return;
        Graphic::Renderer::GetInstance().WaitDeviceIdle();
        m_Scene.reset();
        m_Renderer.reset();
    }

    void LandscapeHLODBake::Fail( std::string error )
    {
        m_Error = std::move( error );
        m_Phase = Phase::Done;
    }

    Common::BoolResultStr LandscapeHLODBake::StartScene()
    {
        m_Scene = std::make_shared<::Desert::Core::Scene>( "LandscapeHLODBake", m_Renderer.get() );
        // A Scene has no systems of its own: without the render collectors LandscapeECSSystem never runs, the
        // bake renderer's terrain queue stays empty and every tile bakes the clear colour.
        ::Desert::Core::AddSceneRenderCollectors( *m_Scene );
        if ( auto inited = m_Scene->Init( ::Desert::Core::Scene::InitPurpose::Offscreen ); !inited )
            return Common::MakeError( "the bake scene failed to initialise: " + inited.GetError() );
        auto loaded = ::Desert::Core::SceneSerializer( m_Scene.get(), m_Assets )
                           .Deserialize( std::move( *m_Saved ), m_ScenePath.generic_string() );
        m_Saved.reset();
        if ( !loaded )
            return Common::MakeError( "the landscape did not load into the bake scene: " + loaded.GetError() );
        m_Camera = std::make_shared<::Desert::Core::GameplayCamera>();
        m_Camera->SetProjectionType( ::Desert::Core::ProjectionType::Orthographic );
        return BOOLSUCCESS;
    }

    // Straight down over the tile's centre, image up = world -Z, image right = world +X: the readback's row 0
    // is the tile's OriginZ edge, column 0 its OriginX edge — the HLOD mesh's UV (0,0) corner.
    void LandscapeHLODBake::FrameTile()
    {
        const ::Desert::Core::LandscapeHLODTileArea& tile     = m_Tiles[m_Next].Area;
        const float                        longSide = std::max( tile.SizeX, tile.SizeZ );
        const uint32_t                     width =
             std::max( 1u, static_cast<uint32_t>( std::lround( kBakeLongSide * tile.SizeX / longSide ) ) );
        const uint32_t height =
             std::max( 1u, static_cast<uint32_t>( std::lround( kBakeLongSide * tile.SizeZ / longSide ) ) );
        m_Scene->Resize( width, height );

        glm::mat4 world( 1.0f );
        world[0] = glm::vec4( 1.0f, 0.0f, 0.0f, 0.0f );  // right
        world[1] = glm::vec4( 0.0f, 0.0f, -1.0f, 0.0f ); // up
        world[2] = glm::vec4( 0.0f, 1.0f, 0.0f, 0.0f );  // backward (the camera looks down -Y)
        world[3] = glm::vec4( tile.OriginX + tile.SizeX * 0.5f, kLandscapeBakeEyeHeightCm,
                              tile.OriginZ + tile.SizeZ * 0.5f, 1.0f );
        m_Camera->SetOrthoSize( tile.SizeZ * 0.5f );
        m_Camera->SetView(
             ::Desert::Core::CameraEntityViewOf( world, 45.0f, kLandscapeBakeNearCm, kLandscapeBakeFarCm ), width,
             height, ::Desert::Core::Camera::kNoSourceEntity );
        m_Scene->PinActiveCamera( m_Camera );
        m_WarmFrames = kLandscapeBakeWarmFrames;
        m_Phase      = Phase::Frame;
    }

    Common::BoolResultStr LandscapeHLODBake::SaveTile( const std::vector<uint8_t>& rgba,
                                                       const std::vector<uint8_t>& gbufferB,
                                                       const std::vector<uint8_t>& emissive, uint32_t width,
                                                       uint32_t height )
    {
        const ::Desert::Core::LandscapeHLODTileArea& tile = m_Tiles[m_Next].Area;
        const HLODBake::GBufferTexels                texels{ rgba, gbufferB, emissive, width, height };
        if ( auto sizes = HLODBake::CheckSizes( texels ); !sizes )
            return sizes;
        const std::size_t     count = static_cast<std::size_t>( width ) * height;
        HLODBake::BakedImages images;
        images.Width  = width;
        images.Height = height;
        // The G-buffer albedo is LINEAR (TerrainGBuffer writes Albedo * Tint into an RGBA8 UNORM target); the
        // texture is stated sRGB, as every authored BaseColor is. ORM in the albedo's row order: r = material AO
        // (Emissive.a), g = roughness (B.a: the layers' roughness widened by the ground normal's spread across one
        // bake texel, Common/SpecularAA.glslh - the normal detail the baked normal map averages away, kept as the
        // cook's roughness), b = metallic 0 (the terrain writes none, TerrainGBuffer.shader).
        images.BaseColor.resize( count * 4 );
        std::vector<uint8_t> orm( count * 4 );
        for ( std::size_t i = 0; i < count; ++i )
        {
            for ( std::size_t k = 0; k < 3; ++k )
                images.BaseColor[i * 4 + k] =
                     HLODBake::LinearToSrgb8( static_cast<float>( rgba[i * 4 + k] ) / 255.0f );
            images.BaseColor[i * 4 + 3] = 255;
            orm[i * 4 + 0]              = HLODBake::UnitToByte( HLODBake::MaterialAO( texels, i ) );
            orm[i * 4 + 1]              = HLODBake::UnitToByte( HLODBake::Roughness( texels, i ) );
            orm[i * 4 + 2]              = 0;
            orm[i * 4 + 3]              = 255;
        }
        images.ORM = std::move( orm );
        // The normal map is stated in the tangent frame of the render mesh the runtime draws for this tile:
        // the same conversion ComponentRegistry's StaticMesh load makes from the EditMesh.
        auto dynamicMesh = ::Desert::Geometry::DynamicMeshFromSerialized( m_Tiles[m_Next].Mesh, "HLOD" );
        if ( !dynamicMesh )
            return Common::MakeError( "the tile's HLOD mesh did not load: " + dynamicMesh.GetError() );
        auto renderMesh = ::Desert::Geometry::ToRenderMesh( dynamicMesh.GetValue() );
        if ( !renderMesh )
            return Common::MakeError( "the tile's HLOD mesh has no render form: " + renderMesh.GetError() );
        images.Normal = BakeTangentNormals( renderMesh.GetValue(), texels );

        auto material = HLODBake::WriteBakedMaterial(
             m_ScenePath, std::to_string( static_cast<uint64_t>( tile.Tile ) ), std::move( images ) );
        if ( !material )
            return Common::MakeError( material.GetError() );
        m_Baked.push_back( LandscapeHLODBaked{ .Tile         = tile.Tile,
                                               .MaterialGuid = material.GetValue().Guid,
                                               .MaterialPath = material.GetValue().Path } );
        return BOOLSUCCESS;
    }

    bool LandscapeHLODBake::Tick()
    {
        switch ( m_Phase )
        {
            case Phase::Build:
            {
                if ( !m_Renderer )
                {
                    const auto& first = m_Tiles.front().Area;
                    const float side  = std::max( first.SizeX, first.SizeZ );
                    m_Renderer        = Graphic::SceneRenderer::Create(
                         Graphic::ViewExtent{
                              std::max( 1u, static_cast<uint32_t>( kBakeLongSide * first.SizeX / side ) ),
                              std::max( 1u, static_cast<uint32_t>( kBakeLongSide * first.SizeZ / side ) ) },
                         Graphic::kThumbnailViewProfile );
                }
                if ( !m_Renderer->AdvanceRendererBuild() )
                    return false;
                if ( auto started = StartScene(); !started )
                {
                    Fail( started.GetError() );
                    return true;
                }
                FrameTile();
                return false;
            }
            case Phase::Frame:
            {
                if ( m_WarmFrames > 0 )
                {
                    if ( auto frame = m_Scene->OnUpdate( Common::Timestep( 0.016f ) ); !frame )
                        LOG_ERROR( "[Build HLODs] bake frame skipped: {0}", frame.GetError() );
                    --m_WarmFrames;
                    return false;
                }
                // The previous frame's render is on the queue; the copy is recorded behind it (no wait).
                // Attachments in TerrainGBuffer.shader's Out() order: 0 albedo, 1 normal + roughness, 3 emissive
                // + material AO.
                const auto& gbuffer  = m_Renderer->GetGBuffer();
                const auto  albedo   = gbuffer ? gbuffer->GetColorAttachmentImage( 0 ) : nullptr;
                const auto  bTarget  = gbuffer ? gbuffer->GetColorAttachmentImage( 1 ) : nullptr;
                const auto  emissive = gbuffer ? gbuffer->GetColorAttachmentImage( 3 ) : nullptr;
                if ( !albedo || !bTarget || !emissive )
                {
                    Fail( "the bake renderer has no G-buffer albedo / B / emissive target (is the scene's "
                          "rendering "
                          "path Deferred?)" );
                    return true;
                }
                auto albedoCopy   = albedo->BeginReadbackRGBA8();
                auto bCopy        = bTarget->BeginReadbackBytes();
                auto emissiveCopy = emissive->BeginReadbackBytes();
                if ( !albedoCopy || !bCopy || !emissiveCopy )
                {
                    Fail( "a G-buffer readback was refused: " +
                          ( !albedoCopy ? albedoCopy.GetError()
                                        : ( !bCopy ? bCopy.GetError() : emissiveCopy.GetError() ) ) );
                    return true;
                }
                m_Albedo         = albedoCopy.GetValue();
                m_GBufferB       = bCopy.GetValue();
                m_Emissive       = emissiveCopy.GetValue();
                m_ReadbackWidth  = albedo->GetWidth();
                m_ReadbackHeight = albedo->GetHeight();
                m_Phase          = Phase::Readback;
                return false;
            }
            case Phase::Readback:
            {
                if ( !m_Albedo->IsComplete() || !m_GBufferB->IsComplete() || !m_Emissive->IsComplete() )
                    return false;
                auto pixels = m_Albedo->ReadRGBA8();
                auto bBytes = m_GBufferB->ReadBytes();
                auto eBytes = m_Emissive->ReadBytes();
                m_Albedo.reset();
                m_GBufferB.reset();
                m_Emissive.reset();
                if ( !pixels || !bBytes || !eBytes )
                {
                    Fail( "a G-buffer readback failed: " +
                          ( !pixels ? pixels.GetError() : ( !bBytes ? bBytes.GetError() : eBytes.GetError() ) ) );
                    return true;
                }
                if ( auto saved = SaveTile( pixels.GetValue(), bBytes.GetValue(), eBytes.GetValue(),
                                            m_ReadbackWidth, m_ReadbackHeight );
                     !saved )
                {
                    Fail( std::format( "tile {}: {}", static_cast<uint64_t>( m_Tiles[m_Next].Area.Tile ),
                                       saved.GetError() ) );
                    return true;
                }
                if ( ++m_Next >= m_Tiles.size() )
                {
                    m_Phase = Phase::Done;
                    return true;
                }
                FrameTile();
                return false;
            }
            case Phase::Done:
                return true;
        }
        return true;
    }
} // namespace Desert::Editor
