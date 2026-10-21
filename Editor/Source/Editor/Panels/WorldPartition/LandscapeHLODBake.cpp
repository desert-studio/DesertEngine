#include "LandscapeHLODBake.hpp"

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
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Timestep.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>

// STB_IMAGE_WRITE_IMPLEMENTATION is compiled into stb_image.cpp; declarations only here.
#include <stb_image/stb_image_write.h>

namespace Desert::Editor
{
    namespace
    {
        // The long side of a baked tile, in texels (UE's landscape HLOD texture size default is 256 per
        // component; a whole tile here spans many components, so the bake is sized for the tile).
        constexpr uint32_t kBakeLongSide = 1024;
        // Frames recorded before the albedo is read: the landscape's material and its textures are resolved
        // while the scene draws (AssetThumbnailRenderer warms the same way).
        constexpr int kWarmFrames = 60;
        // The camera hangs this high above the tile and sees this deep: any landscape height lies between.
        constexpr float kEyeHeightCm = 5.0e5f;
        constexpr float kNearCm      = 100.0f;
        constexpr float kFarCm       = 1.0e6f;

        // The engine's StandardSurface: the material the baked BaseColor binds to.
        constexpr std::string_view kSurfaceShaderGuid = "4f1cac6af403a010c792d835dd6f7d44";
        constexpr std::string_view kSurfaceShaderPath = "engine:Shaders/Programs/Surface/StandardSurface.shader";
        constexpr std::string_view kAlbedoSlot        = "u_AlbedoTexture";

        [[nodiscard]] uint8_t LinearToSrgb8( uint8_t linear )
        {
            const float c = static_cast<float>( linear ) / 255.0f;
            const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow( c, 1.0f / 2.4f ) - 0.055f;
            return static_cast<uint8_t>( std::clamp( std::lround( s * 255.0f ), 0L, 255L ) );
        }

        [[nodiscard]] Common::ResultStr<std::vector<std::byte>> EncodePng( const std::vector<uint8_t>& rgba,
                                                                           uint32_t width, uint32_t height )
        {
            std::vector<std::byte> out;
            stbi_flip_vertically_on_write( 0 );
            const int written = stbi_write_png_to_func(
                 []( void* context, void* data, int size )
                 {
                     auto*       bytes = static_cast<std::vector<std::byte>*>( context );
                     const auto* begin = static_cast<const std::byte*>( data );
                     bytes->insert( bytes->end(), begin, begin + size );
                 },
                 &out, static_cast<int>( width ), static_cast<int>( height ), 4, rgba.data(),
                 static_cast<int>( width ) * 4 );
            if ( written == 0 || out.empty() )
                return Common::MakeError<std::vector<std::byte>>(
                     std::format( "stbi_write_png_to_func refused a {}x{} RGBA8 image", width, height ) );
            return Common::MakeSuccess( std::move( out ) );
        }

        // The GUID an existing material file states, so a re-bake keeps the identity every reference holds.
        [[nodiscard]] std::optional<Common::Content::TextAssetHeaderSerialized>
        ExistingMaterialHeader( const std::filesystem::path& file )
        {
            std::ifstream in( file, std::ios::binary );
            if ( !in )
                return std::nullopt;
            std::stringstream text;
            text << in.rdbuf();
            auto parsed = Assets::ParseMaterialJson( file.generic_string(), text.str() );
            if ( !parsed )
            {
                LOG_WARN( "[Build HLODs] '{0}' is not a readable material ({1}); it is rewritten with a new GUID",
                          file.generic_string(), parsed.GetError() );
                return std::nullopt;
            }
            return parsed.GetValue().Header;
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
                    if ( name == Rules::kLandscapeRootComponent || name == Rules::kLandscapeTileComponent )
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
                                          std::vector<::Desert::Core::LandscapeHLODTileArea> tiles,
                                          const Assets::AssetManager*                        assets )
         : m_ScenePath( std::move( scenePath ) ), m_Saved( std::move( saved ) ), m_Tiles( std::move( tiles ) ),
           m_Assets( assets )
    {
        KeepOnlyLandscape( *m_Saved );
        if ( m_Tiles.empty() )
            m_Phase = Phase::Done;
    }

    LandscapeHLODBake::~LandscapeHLODBake()
    {
        m_Readback.reset();
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
        const ::Desert::Core::LandscapeHLODTileArea& tile     = m_Tiles[m_Next];
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
        world[3] =
             glm::vec4( tile.OriginX + tile.SizeX * 0.5f, kEyeHeightCm, tile.OriginZ + tile.SizeZ * 0.5f, 1.0f );
        m_Camera->SetOrthoSize( tile.SizeZ * 0.5f );
        m_Camera->SetView( ::Desert::Core::CameraEntityViewOf( world, 45.0f, kNearCm, kFarCm ), width, height,
                           ::Desert::Core::Camera::kNoSourceEntity );
        m_Scene->PinActiveCamera( m_Camera );
        m_WarmFrames = kWarmFrames;
        m_Phase      = Phase::Frame;
    }

    Common::BoolResultStr LandscapeHLODBake::SaveTile( const std::vector<uint8_t>& rgba, uint32_t width,
                                                       uint32_t height )
    {
        const ::Desert::Core::LandscapeHLODTileArea& tile = m_Tiles[m_Next];
        if ( rgba.size() != static_cast<std::size_t>( width ) * height * 4 )
            return Common::MakeError( std::format( "the albedo readback holds {} bytes, expected {}x{} RGBA8",
                                                   rgba.size(), width, height ) );
        // The G-buffer albedo is LINEAR (TerrainGBuffer writes Albedo * Tint into an RGBA8 UNORM target); the
        // texture is stated sRGB, as every authored BaseColor is.
        std::vector<uint8_t> srgb( rgba.size() );
        for ( std::size_t i = 0; i < rgba.size(); i += 4 )
        {
            srgb[i + 0] = LinearToSrgb8( rgba[i + 0] );
            srgb[i + 1] = LinearToSrgb8( rgba[i + 1] );
            srgb[i + 2] = LinearToSrgb8( rgba[i + 2] );
            srgb[i + 3] = 255;
        }
        auto png = EncodePng( srgb, width, height );
        if ( !png )
            return Common::MakeError( png.GetError() );

        const std::filesystem::path dir      = ::Desert::Core::WorldCells::BuiltHLODDirectory( m_ScenePath );
        const std::string           tileName = std::to_string( static_cast<uint64_t>( tile.Tile ) );
        const std::filesystem::path texFile  = dir / ( "T_" + tileName + "_BaseColor.detex" );
        const std::filesystem::path matFile  = dir / ( "M_" + tileName + ".demat" );
        std::error_code             ec;
        std::filesystem::create_directories( dir, ec );
        if ( ec )
            return Common::MakeError(
                 std::format( "'{}' could not be created: {}", dir.generic_string(), ec.message() ) );

        Assets::TextureImportSettings settings;
        settings.Intent     = ::Desert::Core::Formats::TextureIntent::Colour;
        settings.ColorSpace = ::Desert::Core::Formats::TextureColorSpace::SRGB;
        const std::string sourceKey =
             Common::AssetHandle::StableKeyForPath( dir / ( "T_" + tileName + "_BaseColor.png" ) );
        auto written = Assets::WriteTextureSource( texFile, Common::Content::ContentKind::Texture, sourceKey,
                                                   std::move( png.GetValue() ), settings );
        if ( !written )
            return Common::MakeError( written.GetError() );
        auto texture = Assets::ReadTextureSourceAssetFile( texFile );
        if ( !texture )
            return Common::MakeError( texture.GetError() );

        Assets::MaterialData material;
        material.Header = ExistingMaterialHeader( matFile ); // the GUID survives a re-bake
        material.Shader =
             Assets::AssetGuidRef{ std::string( kSurfaceShaderGuid ), std::string( kSurfaceShaderPath ) };
        Assets::MaterialAssetRef albedo;
        albedo.Name = std::string( kAlbedoSlot );
        albedo.Guid = Common::Content::AssetGuidToText( texture.GetValue().Guid );
        albedo.Path = Common::AssetHandle::StableKeyForPath( texFile );
        material.Textures.push_back( std::move( albedo ) );
        material = Assets::StampMaterialHeader( std::move( material ) );
        if ( auto saved = Assets::WriteMaterialFile( matFile, material ); !saved )
            return Common::MakeError( saved.GetError() );

        Assets::ContentRegistry::NoteFile( texFile );
        Assets::ContentRegistry::NoteFile( matFile );
        // The HLOD block's slot path is a SCENE-RECORD reference, so it is spelled the way a scene stores a
        // material (StoredFormFor("MaterialAsset") = assets-relative), not as the tagged key the .demat's own
        // texture slot uses: a tagged key there is re-tagged on load ("assets:assets:...") and the slot stays
        // empty.
        const auto materialForm = Core::Serialize::StoredFormFor( "MaterialAsset" );
        if ( !materialForm )
            return Common::MakeError( "the material reference has no stored form (StoredAssetForm.cpp)" );
        m_Baked.push_back(
             LandscapeHLODBaked{ .Tile         = tile.Tile,
                                 .MaterialGuid = material.Header->Guid,
                                 .MaterialPath = Core::Serialize::RenderStoredForm(
                                      *materialForm, Common::AssetHandle::StableKeyForPath( matFile ) ) } );
        LOG_INFO( "[Build HLODs] baked tile {0}: {1}x{2} BaseColor -> {3}, {4}", tileName, width, height,
                  texFile.generic_string(), matFile.generic_string() );
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
                    const auto& first = m_Tiles.front();
                    const float side  = std::max( first.SizeX, first.SizeZ );
                    m_Renderer        = std::make_unique<Graphic::SceneRenderer>(
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
                const auto& gbuffer = m_Renderer->GetGBuffer();
                const auto  albedo  = gbuffer ? gbuffer->GetColorAttachmentImage( 0 ) : nullptr;
                if ( !albedo )
                {
                    Fail( "the bake renderer has no G-buffer albedo (is the scene's rendering path Deferred?)" );
                    return true;
                }
                auto readback = albedo->BeginReadbackRGBA8();
                if ( !readback )
                {
                    Fail( "the albedo readback was refused: " + readback.GetError() );
                    return true;
                }
                m_Readback       = readback.GetValue();
                m_ReadbackWidth  = albedo->GetWidth();
                m_ReadbackHeight = albedo->GetHeight();
                m_Phase          = Phase::Readback;
                return false;
            }
            case Phase::Readback:
            {
                if ( !m_Readback->IsComplete() )
                    return false;
                auto pixels = m_Readback->ReadRGBA8();
                m_Readback.reset();
                if ( !pixels )
                {
                    Fail( "the albedo readback failed: " + pixels.GetError() );
                    return true;
                }
                if ( auto saved = SaveTile( pixels.GetValue(), m_ReadbackWidth, m_ReadbackHeight ); !saved )
                {
                    Fail( std::format( "tile {}: {}", static_cast<uint64_t>( m_Tiles[m_Next].Tile ),
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
