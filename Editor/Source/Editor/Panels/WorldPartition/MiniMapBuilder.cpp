#include "MiniMapBuilder.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/CameraEntityView.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/SceneRenderCollectors.hpp>
#include <Engine/Core/Serialize/EditorRegions.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Texture/TextureService.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Timestep.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

// STB_IMAGE_WRITE_IMPLEMENTATION is compiled into stb_image.cpp; declarations only here.
#include <stb_image/stb_image_write.h>

namespace Desert::Editor
{
    namespace
    {
        // Frames recorded before a tile is read at the least: its materials and meshes are resolved while the
        // scene draws (LandscapeHLODBake and AssetThumbnailRenderer warm the same way).
        constexpr int kMiniMapWarmFrames = 60;
        // A tile whose textures are still streaming after this many frames is refused by name - a capture taken
        // then would bake grey placeholders into the map for good.
        constexpr int kMiniMapMaxTileFrames = 3600;
        // The camera hangs this high above the world and sees this deep: any height of the world lies between.
        constexpr float kMiniMapEyeHeightCm = 5.0e5f;
        constexpr float kMiniMapNearCm      = 100.0f;
        constexpr float kMiniMapFarCm       = 1.0e6f;

        [[nodiscard]] bool PositiveFinite( float value )
        {
            return std::isfinite( value ) && value > 0.0f;
        }

        // The G-buffer albedo is LINEAR (the surface and terrain G-buffer passes write Albedo into an RGBA8 UNORM
        // target); the minimap is stated sRGB, as every authored colour texture is.
        [[nodiscard]] uint8_t MiniMapLinearToSrgb8( uint8_t linear )
        {
            const float c = static_cast<float>( linear ) / 255.0f;
            const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow( c, 1.0f / 2.4f ) - 0.055f;
            return static_cast<uint8_t>( std::clamp( std::lround( s * 255.0f ), 0L, 255L ) );
        }

        [[nodiscard]] Common::ResultStr<std::vector<std::byte>> MiniMapEncodePng( const std::vector<uint8_t>& rgba,
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
    } // namespace

    std::filesystem::path MiniMapBuilder::TexturePath( const std::filesystem::path& scenePath )
    {
        return scenePath.parent_path() / ( scenePath.stem().string() + "_MiniMap.detex" );
    }

    Common::ResultStr<std::unique_ptr<MiniMapBuilder>>
    MiniMapBuilder::Start( std::filesystem::path                           scenePath,
                           ::Desert::Core::WorldPartitionMiniMapSerialized settings,
                           std::vector<std::string> dataLayers, const Assets::AssetManager* assets )
    {
        using Out = std::unique_ptr<MiniMapBuilder>;
        if ( !PositiveFinite( settings.WorldUnitsPerPixel ) || !PositiveFinite( settings.BuilderCellSize ) )
            return Common::MakeError<Out>(
                 std::format( "WorldUnitsPerPixel ({}) and BuilderCellSize ({}) must be positive and finite",
                              settings.WorldUnitsPerPixel, settings.BuilderCellSize ) );
        const float spanX = settings.MaxX - settings.MinX;
        const float spanZ = settings.MaxZ - settings.MinZ;
        if ( !PositiveFinite( spanX ) || !PositiveFinite( spanZ ) )
            return Common::MakeError<Out>( std::format( "the world rectangle X {}..{}, Z {}..{} is empty",
                                                        settings.MinX, settings.MaxX, settings.MinZ,
                                                        settings.MaxZ ) );
        const double width  = std::ceil( static_cast<double>( spanX ) / settings.WorldUnitsPerPixel );
        const double height = std::ceil( static_cast<double>( spanZ ) / settings.WorldUnitsPerPixel );
        if ( width > kMiniMapMaxSide || height > kMiniMapMaxSide )
            return Common::MakeError<Out>( std::format(
                 "the world spans {} x {} cm, {} x {} texels at {} cm per texel - above the {} texel limit; "
                 "raise WorldUnitsPerPixel to at least {}",
                 spanX, spanZ, width, height, settings.WorldUnitsPerPixel, kMiniMapMaxSide,
                 std::ceil( std::max( spanX, spanZ ) / static_cast<float>( kMiniMapMaxSide ) ) ) );
        return Common::MakeSuccess( Out(
             new MiniMapBuilder( std::move( scenePath ), std::move( settings ), std::move( dataLayers ), assets,
                                 static_cast<uint32_t>( width ), static_cast<uint32_t>( height ) ) ) );
    }

    MiniMapBuilder::MiniMapBuilder( std::filesystem::path                           scenePath,
                                    ::Desert::Core::WorldPartitionMiniMapSerialized settings,
                                    std::vector<std::string> dataLayers, const Assets::AssetManager* assets,
                                    uint32_t width, uint32_t height )
         : m_ScenePath( std::move( scenePath ) ), m_Settings( std::move( settings ) ),
           m_DataLayers( std::move( dataLayers ) ), m_Assets( assets ), m_Width( width ), m_Height( height ),
           m_Image( static_cast<std::size_t>( width ) * height * 4, 0 )
    {
        // The rectangle is exactly the image: its max edges sit on whole texels.
        m_Settings.MaxX = m_Settings.MinX + static_cast<float>( width ) * m_Settings.WorldUnitsPerPixel;
        m_Settings.MaxZ = m_Settings.MinZ + static_cast<float>( height ) * m_Settings.WorldUnitsPerPixel;
        // The builder's squares, in whole texels (UE iterates its IterativeCellSize the same way).
        const uint32_t side = std::max( 1u, static_cast<uint32_t>( std::floor( m_Settings.BuilderCellSize /
                                                                               m_Settings.WorldUnitsPerPixel ) ) );
        for ( uint32_t y = 0; y < m_Height; y += side )
            for ( uint32_t x = 0; x < m_Width; x += side )
                m_Tiles.push_back( Tile{ x, y, std::min( x + side, m_Width ), std::min( y + side, m_Height ) } );
        for ( std::size_t i = 3; i < m_Image.size(); i += 4 )
            m_Image[i] = 255;
    }

    MiniMapBuilder::~MiniMapBuilder()
    {
        m_Albedo.reset();
        if ( !m_Renderer )
            return;
        ReleaseScene();
        m_Renderer.reset();
    }

    void MiniMapBuilder::Fail( std::string error )
    {
        m_Error = std::move( error );
        m_Phase = Phase::Done;
    }

    void MiniMapBuilder::ReleaseScene()
    {
        if ( !m_Scene )
            return;
        // The frames recorded for the last tile still reference its scene's resources.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();
        m_Scene.reset();
    }

    // The tile's region of the saved world, read as the editor's open reads a region (always-loaded part plus the
    // cells the square selects, every data layer loaded), into a fresh offscreen scene framed straight down over
    // the square: image up = world -Z, image right = world +X, so the readback's row 0 is the square's MinZ edge.
    Common::BoolResultStr MiniMapBuilder::LoadTile()
    {
        ReleaseScene();
        const Tile&                                tile = m_Tiles[m_Next];
        const float                                upp  = m_Settings.WorldUnitsPerPixel;
        const float                                x0   = m_Settings.MinX + static_cast<float>( tile.X0 ) * upp;
        const float                                z0   = m_Settings.MinZ + static_cast<float>( tile.Y0 ) * upp;
        const float                                x1   = m_Settings.MinX + static_cast<float>( tile.X1 ) * upp;
        const float                                z1   = m_Settings.MinZ + static_cast<float>( tile.Y1 ) * upp;
        ::Desert::Core::EditorRegions::HeldRegions held;
        held.Regions.push_back( ::Desert::Core::Rules::CellBounds{ x0, z0, x1, z1 } );
        held.DataLayers.Loaded = m_DataLayers;
        auto read              = ::Desert::Core::EditorRegions::ReadForOpen( m_ScenePath, held );
        if ( !read )
            return Common::MakeError( read.GetError() );
        auto parsed = ::Desert::Core::ParseLoadableScene( m_ScenePath.string(), read.GetValue().Text );
        if ( !parsed )
            return Common::MakeError( parsed.GetError() );

        m_Scene = std::make_shared<::Desert::Core::Scene>( "MiniMapBuilder", m_Renderer.get() );
        // A Scene has no systems of its own: without the render collectors nothing reaches the renderer's queues.
        ::Desert::Core::AddSceneRenderCollectors( *m_Scene );
        if ( auto inited = m_Scene->Init( ::Desert::Core::Scene::InitPurpose::Offscreen ); !inited )
            return Common::MakeError( "the capture scene failed to initialise: " + inited.GetError() );
        auto loaded = ::Desert::Core::SceneSerializer( m_Scene.get(), m_Assets )
                           .Deserialize( std::move( parsed.GetValue() ), m_ScenePath.generic_string() );
        if ( !loaded )
            return Common::MakeError( "the region did not load into the capture scene: " + loaded.GetError() );

        const uint32_t width  = tile.X1 - tile.X0;
        const uint32_t height = tile.Y1 - tile.Y0;
        m_Scene->Resize( width, height );
        glm::mat4 world( 1.0f );
        world[0] = glm::vec4( 1.0f, 0.0f, 0.0f, 0.0f );  // right
        world[1] = glm::vec4( 0.0f, 0.0f, -1.0f, 0.0f ); // up
        world[2] = glm::vec4( 0.0f, 1.0f, 0.0f, 0.0f );  // backward (the camera looks down -Y)
        world[3] = glm::vec4( ( x0 + x1 ) * 0.5f, kMiniMapEyeHeightCm, ( z0 + z1 ) * 0.5f, 1.0f );
        m_Camera->SetOrthoSize( ( z1 - z0 ) * 0.5f );
        m_Camera->SetView( ::Desert::Core::CameraEntityViewOf( world, 45.0f, kMiniMapNearCm, kMiniMapFarCm ),
                           width, height, ::Desert::Core::Camera::kNoSourceEntity );
        m_Scene->PinActiveCamera( m_Camera );
        m_WarmFrames = kMiniMapWarmFrames;
        m_Waited     = 0;
        return BOOLSUCCESS;
    }

    void MiniMapBuilder::Blit( const std::vector<uint8_t>& rgba )
    {
        const Tile&       tile  = m_Tiles[m_Next];
        const std::size_t width = tile.X1 - tile.X0;
        for ( uint32_t row = 0; row < tile.Y1 - tile.Y0; ++row )
        {
            const uint8_t* from = rgba.data() + static_cast<std::size_t>( row ) * width * 4;
            uint8_t* to = m_Image.data() + ( static_cast<std::size_t>( tile.Y0 + row ) * m_Width + tile.X0 ) * 4;
            for ( std::size_t i = 0; i < width * 4; i += 4 )
            {
                to[i + 0] = MiniMapLinearToSrgb8( from[i + 0] );
                to[i + 1] = MiniMapLinearToSrgb8( from[i + 1] );
                to[i + 2] = MiniMapLinearToSrgb8( from[i + 2] );
                to[i + 3] = 255;
            }
        }
    }

    Common::BoolResultStr MiniMapBuilder::Save()
    {
        auto png = MiniMapEncodePng( m_Image, m_Width, m_Height );
        if ( !png )
            return Common::MakeError( png.GetError() );
        const std::filesystem::path file = TexturePath( m_ScenePath );
        // The reference names the texture as TextureService::HandleForPath reads it: relative to the assets root.
        const std::filesystem::path assetsRoot =
             std::filesystem::absolute( Common::Constants::Path::ASSETS_PATH ).lexically_normal();
        const std::filesystem::path relative =
             std::filesystem::absolute( file ).lexically_normal().lexically_relative( assetsRoot );
        if ( relative.empty() || relative.generic_string().starts_with( ".." ) )
            return Common::MakeError( std::format( "'{}' is outside the assets root '{}'", file.generic_string(),
                                                   assetsRoot.generic_string() ) );

        Assets::TextureImportSettings settings;
        settings.Intent             = ::Desert::Core::Formats::TextureIntent::Colour;
        settings.ColorSpace         = ::Desert::Core::Formats::TextureColorSpace::SRGB;
        const std::string sourceKey = Common::AssetHandle::StableKeyForPath(
             file.parent_path() / ( m_ScenePath.stem().string() + "_MiniMap.png" ) );
        auto written = Assets::WriteTextureSource( file, Common::Content::ContentKind::Texture, sourceKey,
                                                   std::move( png.GetValue() ), settings );
        if ( !written )
            return Common::MakeError( written.GetError() );
        auto texture = Assets::ReadTextureSourceAssetFile( file );
        if ( !texture )
            return Common::MakeError( texture.GetError() );
        std::string guid = Common::Content::AssetGuidToText( texture.GetValue().Guid );
        // THE BUILD AUTHORS THESE SETTINGS (as LandscapeHLODBake's textures): WriteTextureSource keeps an existing
        // asset's settings, and a built minimap has no person behind them; the GUID is kept.
        if ( texture.GetValue().Import.Settings != settings )
        {
            Assets::TextureSourceAsset rewritten = texture.ExtractValue();
            rewritten.Import.Settings            = settings;
            if ( auto w = Assets::WriteTextureSourceAssetFile( file, rewritten ); !w.IsSuccess() )
                return Common::MakeError( w.GetError() );
        }
        Assets::ContentRegistry::NoteFile( file );
        m_Result          = m_Settings;
        m_Result->Texture = Assets::AssetGuidRef{ std::move( guid ), relative.generic_string() };
        LOG_INFO( "[Build Minimap] {0}: {1}x{2} texels, {3} cm per texel, {4} tile(s) of {5} cm, X {6}..{7}, Z "
                  "{8}..{9}",
                  file.generic_string(), m_Width, m_Height, m_Settings.WorldUnitsPerPixel, m_Tiles.size(),
                  m_Settings.BuilderCellSize, m_Settings.MinX, m_Settings.MaxX, m_Settings.MinZ, m_Settings.MaxZ );
        return BOOLSUCCESS;
    }

    bool MiniMapBuilder::Tick()
    {
        switch ( m_Phase )
        {
            case Phase::Build:
            {
                if ( !m_Renderer )
                {
                    const Tile& first = m_Tiles.front();
                    m_Renderer        = std::make_unique<Graphic::SceneRenderer>(
                         Graphic::ViewExtent{ first.X1 - first.X0, first.Y1 - first.Y0 },
                         Graphic::kThumbnailViewProfile );
                }
                if ( !m_Renderer->AdvanceRendererBuild() )
                    return false;
                m_Camera = std::make_shared<::Desert::Core::GameplayCamera>();
                m_Camera->SetProjectionType( ::Desert::Core::ProjectionType::Orthographic );
                m_Phase = Phase::Load;
                return false;
            }
            case Phase::Load:
            {
                if ( auto loaded = LoadTile(); !loaded )
                {
                    Fail( std::format( "tile {} of {}: {}", m_Next + 1, m_Tiles.size(), loaded.GetError() ) );
                    return true;
                }
                m_Phase = Phase::Frame;
                return false;
            }
            case Phase::Frame:
            {
                if ( auto frame = m_Scene->OnUpdate( Common::Timestep( 0.016f ) ); !frame )
                    LOG_ERROR( "[Build Minimap] capture frame skipped: {0}", frame.GetError() );
                ++m_Waited;
                if ( m_WarmFrames > 0 )
                {
                    --m_WarmFrames;
                    return false;
                }
                // The tile is read only once what it draws has streamed in.
                if ( Runtime::ResourceRegistry::GetTextureService()->InFlight() > 0 )
                {
                    if ( m_Waited < kMiniMapMaxTileFrames )
                        return false;
                    Fail( std::format( "tile {} of {}: textures were still streaming after {} frames", m_Next + 1,
                                       m_Tiles.size(), m_Waited ) );
                    return true;
                }
                // The previous frame's render is on the queue; the copy is recorded behind it (no wait).
                const auto& gbuffer = m_Renderer->GetGBuffer();
                const auto  albedo  = gbuffer ? gbuffer->GetColorAttachmentImage( 0 ) : nullptr;
                if ( !albedo )
                {
                    Fail( "the capture renderer has no G-buffer albedo target (is the scene's rendering path "
                          "Deferred?)" );
                    return true;
                }
                const Tile& tile = m_Tiles[m_Next];
                if ( albedo->GetWidth() != tile.X1 - tile.X0 || albedo->GetHeight() != tile.Y1 - tile.Y0 )
                {
                    Fail( std::format( "tile {} of {}: the G-buffer is {}x{}, the tile {}x{}", m_Next + 1,
                                       m_Tiles.size(), albedo->GetWidth(), albedo->GetHeight(), tile.X1 - tile.X0,
                                       tile.Y1 - tile.Y0 ) );
                    return true;
                }
                auto copy = albedo->BeginReadbackRGBA8();
                if ( !copy )
                {
                    Fail( "the G-buffer readback was refused: " + copy.GetError() );
                    return true;
                }
                m_Albedo = copy.GetValue();
                m_Phase  = Phase::Readback;
                return false;
            }
            case Phase::Readback:
            {
                if ( !m_Albedo->IsComplete() )
                    return false;
                auto pixels = m_Albedo->ReadRGBA8();
                m_Albedo.reset();
                if ( !pixels )
                {
                    Fail( "the G-buffer readback failed: " + pixels.GetError() );
                    return true;
                }
                const Tile& tile = m_Tiles[m_Next];
                if ( pixels.GetValue().size() !=
                     static_cast<std::size_t>( tile.X1 - tile.X0 ) * ( tile.Y1 - tile.Y0 ) * 4 )
                {
                    Fail( std::format( "tile {} of {}: the readback holds {} bytes, expected {}x{} RGBA8",
                                       m_Next + 1, m_Tiles.size(), pixels.GetValue().size(), tile.X1 - tile.X0,
                                       tile.Y1 - tile.Y0 ) );
                    return true;
                }
                Blit( pixels.GetValue() );
                LOG_INFO( "[Build Minimap] tile {0} of {1} captured", m_Next + 1, m_Tiles.size() );
                if ( ++m_Next < m_Tiles.size() )
                {
                    m_Phase = Phase::Load;
                    return false;
                }
                ReleaseScene();
                if ( auto saved = Save(); !saved )
                    Fail( saved.GetError() );
                m_Phase = Phase::Done;
                return true;
            }
            case Phase::Done:
                return true;
        }
        return true;
    }
} // namespace Desert::Editor
