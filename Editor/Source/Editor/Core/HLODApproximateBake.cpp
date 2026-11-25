#include "HLODApproximateBake.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/CameraEntityView.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldSystems.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Core/Serialize/StoredAssetForm.hpp>
#include <Engine/Core/Serialize/WorldCells.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/RenderMeshData.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Timestep.hpp>

#include <glm/common.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <span>
#include <sstream>

// STB_IMAGE_WRITE_IMPLEMENTATION is compiled into stb_image.cpp; declarations only here.
#include <stb_image/stb_image_write.h>

namespace Desert::Editor
{
    namespace
    {
        // Frames recorded before the first photo is read: the sources' materials and textures are resolved while
        // the scene draws (LandscapeHLODBake and AssetThumbnailRenderer warm the same way). A later view only
        // moves the camera, so its frames need only reach the G-buffer.
        constexpr int kFirstWarmFrames = 60;
        constexpr int kViewWarmFrames  = 3;

        // StandardSurface, the material the shell's maps bind to (as LandscapeHLODBake's tiles).
        constexpr std::string_view kSurfaceShaderGuid = "4f1cac6af403a010c792d835dd6f7d44";
        constexpr std::string_view kSurfaceShaderPath = "engine:Shaders/Programs/Surface/StandardSurface.shader";
        constexpr std::string_view kAlbedoSlot        = "AlbedoTexture";
        // SurfaceResolveORM multiplies roughness by RoughnessFactor (schema default 0.5) and metallic by
        // MetallicFactor (default 0): the baked maps are the sources' own values, so both are 1.
        constexpr std::string_view kORMSlot         = "OrmTexture";
        constexpr std::string_view kRoughnessFactor = "RoughnessFactor";
        constexpr std::string_view kMetallicFactor  = "MetallicFactor";
        constexpr std::string_view kNormalSlot      = "NormalTexture";
        // The RGBA16F G-buffer targets: 4 half floats per texel.
        constexpr std::size_t kHalfTexelBytes = 8;

        [[nodiscard]] glm::vec4 HalfTexel( const std::vector<uint8_t>& texels, std::size_t texel )
        {
            uint16_t halves[4] = {};
            std::memcpy( halves, texels.data() + texel * kHalfTexelBytes, sizeof( halves ) );
            return glm::vec4( glm::unpackHalf1x16( halves[0] ), glm::unpackHalf1x16( halves[1] ),
                              glm::unpackHalf1x16( halves[2] ), glm::unpackHalf1x16( halves[3] ) );
        }

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

        // Writes one baked texture with the settings the bake authors and answers the GUID its header states.
        [[nodiscard]] Common::ResultStr<std::string>
        WriteBakedTexture( const std::filesystem::path& dir, const std::filesystem::path& file,
                           const std::string& sourceName, std::vector<std::byte> encoded,
                           ::Desert::Core::Formats::TextureIntent     intent,
                           ::Desert::Core::Formats::TextureColorSpace colorSpace )
        {
            Assets::TextureImportSettings settings;
            settings.Intent             = intent;
            settings.ColorSpace         = colorSpace;
            const std::string sourceKey = Common::AssetHandle::StableKeyForPath( dir / sourceName );
            auto written = Assets::WriteTextureSource( file, Common::Content::ContentKind::Texture, sourceKey,
                                                       std::move( encoded ), settings );
            if ( !written )
                return Common::MakeError<std::string>( written.GetError() );
            auto texture = Assets::ReadTextureSourceAssetFile( file );
            if ( !texture )
                return Common::MakeError<std::string>( texture.GetError() );
            if ( texture.GetValue().Import.Settings != settings )
            {
                Assets::TextureSourceAsset rewritten = texture.ExtractValue();
                rewritten.Import.Settings            = settings;
                if ( auto w = Assets::WriteTextureSourceAssetFile( file, rewritten ); !w.IsSuccess() )
                    return Common::MakeError<std::string>( w.GetError() );
                return Common::MakeSuccess( Common::Content::AssetGuidToText( rewritten.Guid ) );
            }
            return Common::MakeSuccess( Common::Content::AssetGuidToText( texture.GetValue().Guid ) );
        }

        // A material texture slot naming a baked texture, as the bakes write it.
        [[nodiscard]] Assets::MaterialAssetRef BakedSlot( std::string_view slotName, const std::string& guid,
                                                          const std::filesystem::path& file )
        {
            Assets::MaterialAssetRef ref;
            ref.Name = std::string( slotName );
            ref.Guid = guid;
            ref.Path = Common::AssetHandle::StableKeyForPath( file );
            return ref;
        }

        // The bake scene starts EMPTY (no record, no partition): StartCell instantiates one cell's parts at a
        // time, as UE captures only the HLOD's own actors, so neither a neighbouring cell nor the landscape stands
        // in front of the cell in its photos.
        void KeepNoRecords( ::Desert::Core::LoadableScene& loaded )
        {
            loaded.Scene.Entities.clear();
            loaded.Scene.WorldPartition.reset();
        }

        // The side of every photo of @p cell: an impostor frame's FrameResolution, an approximation's
        // CaptureResolution.
        [[nodiscard]] uint32_t CaptureSide( const HLODApproximateBakeCell& cell )
        {
            return static_cast<uint32_t>( cell.Impostor ? cell.Impostor->FrameResolution
                                                        : cell.Settings.CaptureResolution );
        }

        // The file stem of @p cell's outputs: I<key> for an impostor, A<key> for an approximation.
        [[nodiscard]] std::string CellName( const HLODApproximateBakeCell& cell )
        {
            return ( cell.Impostor ? "I" : "A" ) + cell.Key;
        }

        // ImpostorSurface, the material an impostor's atlas binds to (Programs/Surface/ImpostorSurface.shader).
        constexpr std::string_view kImpostorShaderGuid = "2f529b86e17c474b9d8a426559632481";
        constexpr std::string_view kImpostorShaderPath = "engine:Shaders/Programs/Surface/ImpostorSurface.shader";
    } // namespace

    HLODApproximateBake::HLODApproximateBake( std::filesystem::path scenePath, ::Desert::Core::LoadableScene saved,
                                              std::vector<HLODApproximateBakeCell> cells,
                                              const Assets::AssetManager*          assets )
         : m_ScenePath( std::move( scenePath ) ), m_Saved( std::move( saved ) ), m_Cells( std::move( cells ) ),
           m_Assets( assets )
    {
        KeepNoRecords( *m_Saved );
        if ( m_Cells.empty() )
            m_Phase = Phase::Done;
    }

    HLODApproximateBake::~HLODApproximateBake()
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

    void HLODApproximateBake::Fail( std::string error )
    {
        m_Error = std::move( error );
        m_Phase = Phase::Done;
    }

    Common::BoolResultStr HLODApproximateBake::StartScene()
    {
        m_Scene = std::make_shared<::Desert::Core::Scene>( "HLODApproximateBake", m_Renderer.get(),
                                                           ::Desert::Core::WorldKind::Capture );
        if ( auto made = ::Desert::Core::CreateWorldSystems( *m_Scene, {} ); !made )
            return Common::MakeError( made.GetError() );
        if ( auto inited = m_Scene->Init( ::Desert::Core::Scene::InitPurpose::Offscreen ); !inited )
            return Common::MakeError( "the bake scene failed to initialise: " + inited.GetError() );
        auto loaded = ::Desert::Core::SceneSerializer( m_Scene.get(), m_Assets )
                           .Deserialize( std::move( *m_Saved ), m_ScenePath.generic_string() );
        m_Saved.reset();
        if ( !loaded )
            return Common::MakeError( "the bake scene did not load: " + loaded.GetError() );
        m_Camera = std::make_shared<::Desert::Core::GameplayCamera>();
        m_Camera->SetProjectionType( ::Desert::Core::ProjectionType::Orthographic );
        return BOOLSUCCESS;
    }

    // The cell's parts merged again (the surface the photos must see) and its views framed around them.
    Common::BoolResultStr HLODApproximateBake::StartCell()
    {
        const HLODApproximateBakeCell&         cell = m_Cells[m_Next];
        ::Desert::Core::Rules::HLODMeshRequest request;
        if ( !cell.Impostor )
            request.Approximation = cell.Settings;
        request.Parts = cell.Parts;
        const std::span registries( &::Desert::Assets::ContentRegistry::Get(), 1 );
        auto            merged = ::Desert::Core::MergeHLODParts( request, registries );
        if ( !merged )
            return Common::MakeError( merged.GetError() );
        m_Source = std::move( merged.ExtractValue().Mesh );

        // The scene holds THIS cell's parts and nothing else: the previous cell's records go, this cell's come in
        // as the HLOD records an Instancing layer writes (Rules::HLODRecord - one InstancedStaticMesh block each,
        // world-space instances), so every photo sees exactly the surface m_Source answers ray casts for.
        for ( const Common::UUID& id : m_CellRecords )
            if ( const auto entity = m_Scene->FindEntityByID( id ); entity.has_value() )
                m_Scene->DestroyEntity( entity->get() );
        m_CellRecords.clear();
        std::vector<Assets::EntityData> records;
        records.reserve( cell.Parts.size() );
        for ( std::size_t i = 0; i < cell.Parts.size(); ++i )
        {
            const Common::UUID id =
                 ::Desert::Core::Rules::HLODRecordId( m_ScenePath.generic_string(), CellName( cell ), i );
            if ( id.IsNull() )
                return Common::MakeError( std::format( "part {} of the cell hashes to the null id", i ) );
            ::Desert::Core::Rules::HLODBatch part;
            part.Block = cell.Parts[i];
            records.push_back( ::Desert::Core::Rules::HLODRecord( part, id, i ) );
            m_CellRecords.push_back( id );
        }
        if ( auto made = ::Desert::Core::SceneSerializer( m_Scene.get(), m_Assets )
                              .InstantiateRecords( records, "HLODApproximateBake", nullptr );
             !made )
            return Common::MakeError( "the cell's parts did not load into the bake scene: " + made.GetError() );
        // The new parts' meshes, materials and textures resolve while the scene draws: the cell's first view
        // warms as long as the bake's first did.
        m_FirstFrame = true;
        glm::vec3 lo( std::numeric_limits<float>::max() );
        glm::vec3 hi( std::numeric_limits<float>::lowest() );
        for ( const int v : m_Source.VertexIndicesItr() )
        {
            const glm::vec3 p( m_Source.GetVertex( v ) );
            lo = glm::min( lo, p );
            hi = glm::max( hi, p );
        }
        m_Views = cell.Impostor ? ::Desert::Core::HLODImpostorViews( lo, hi, *cell.Impostor )
                                : ::Desert::Core::HLODCaptureViews( lo, hi );
        m_Photos.clear();
        m_View          = 0;
        const auto side = CaptureSide( cell );
        m_Scene->Resize( side, side );
        FrameView();
        return BOOLSUCCESS;
    }

    void HLODApproximateBake::FrameView()
    {
        const ::Desert::Core::HLODCaptureView& view = m_Views[m_View];
        const auto side = CaptureSide( m_Cells[m_Next] );
        glm::mat4  world( 1.0f );
        world[0] = glm::vec4( view.Right, 0.0f );
        world[1] = glm::vec4( view.Up, 0.0f );
        world[2] = glm::vec4( -view.Forward, 0.0f ); // backward: the camera looks down its -Z
        world[3] = glm::vec4( view.Eye, 1.0f );
        m_Camera->SetOrthoSize( view.HalfExtent );
        m_Camera->SetView( ::Desert::Core::CameraEntityViewOf( world, 45.0f, view.NearCm, view.FarCm ), side, side,
                           ::Desert::Core::Camera::kNoSourceEntity );
        m_Scene->PinActiveCamera( m_Camera );
        m_WarmFrames = m_FirstFrame ? kFirstWarmFrames : kViewWarmFrames;
        m_FirstFrame = false;
        m_Phase      = Phase::Frame;
    }

    Common::BoolResultStr HLODApproximateBake::SaveCell()
    {
        const HLODApproximateBakeCell& cell = m_Cells[m_Next];
        if ( cell.Impostor )
            return SaveImpostor();
        auto shell = ::Desert::Geometry::DynamicMeshFromSerialized( cell.Shell, "HLOD" );
        if ( !shell )
            return Common::MakeError( "the cell's shell did not load: " + shell.GetError() );
        // The tangent frame the runtime shades with: the conversion ComponentRegistry's StaticMesh load makes.
        auto render = ::Desert::Geometry::ToRenderMesh( shell.GetValue() );
        if ( !render )
            return Common::MakeError( "the cell's shell has no render form: " + render.GetError() );
        auto maps =
             ::Desert::Core::BakeApproximationTextures( render.GetValue(), m_Source, m_Photos, cell.Settings );
        if ( !maps )
            return Common::MakeError( maps.GetError() );
        const ::Desert::Core::HLODBakedTextures& baked = maps.GetValue();

        // The G-buffer's BaseColor is LINEAR (an RGBA8 UNORM target); the texture is stated sRGB, as every
        // authored BaseColor is.
        std::vector<uint8_t> srgb( baked.BaseColor.size() );
        for ( std::size_t i = 0; i < srgb.size(); i += 4 )
        {
            srgb[i + 0] = LinearToSrgb8( baked.BaseColor[i + 0] );
            srgb[i + 1] = LinearToSrgb8( baked.BaseColor[i + 1] );
            srgb[i + 2] = LinearToSrgb8( baked.BaseColor[i + 2] );
            srgb[i + 3] = 255;
        }
        auto albedoPng = EncodePng( srgb, baked.Size, baked.Size );
        auto ormPng    = albedoPng ? EncodePng( baked.ORM, baked.Size, baked.Size ) : albedoPng;
        auto normalPng = ormPng ? EncodePng( baked.Normal, baked.Size, baked.Size ) : ormPng;
        if ( !normalPng )
            return Common::MakeError( normalPng.GetError() );

        const std::filesystem::path dir        = ::Desert::Core::WorldCells::BuiltHLODDirectory( m_ScenePath );
        const std::string           name       = "A" + cell.Key;
        const std::filesystem::path texFile    = dir / ( "T_" + name + "_BaseColor.detex" );
        const std::filesystem::path ormFile    = dir / ( "T_" + name + "_ORM.detex" );
        const std::filesystem::path normalFile = dir / ( "T_" + name + "_Normal.detex" );
        const std::filesystem::path matFile    = dir / ( "M_" + name + ".demat" );
        std::error_code             ec;
        std::filesystem::create_directories( dir, ec );
        if ( ec )
            return Common::MakeError(
                 std::format( "'{}' could not be created: {}", dir.generic_string(), ec.message() ) );

        const auto writeTexture =
             [&]( const std::filesystem::path& file, const std::string& sourceName, std::vector<std::byte> encoded,
                  ::Desert::Core::Formats::TextureIntent     intent,
                  ::Desert::Core::Formats::TextureColorSpace colorSpace )
        { return WriteBakedTexture( dir, file, sourceName, std::move( encoded ), intent, colorSpace ); };
        auto albedoGuid = writeTexture( texFile, "T_" + name + "_BaseColor.png", std::move( albedoPng.GetValue() ),
                                        ::Desert::Core::Formats::TextureIntent::Colour,
                                        ::Desert::Core::Formats::TextureColorSpace::SRGB );
        if ( !albedoGuid )
            return Common::MakeError( albedoGuid.GetError() );
        auto ormGuid = writeTexture( ormFile, "T_" + name + "_ORM.png", std::move( ormPng.GetValue() ),
                                     ::Desert::Core::Formats::TextureIntent::PackedMasks,
                                     ::Desert::Core::Formats::TextureColorSpace::Linear );
        if ( !ormGuid )
            return Common::MakeError( ormGuid.GetError() );
        auto normalGuid = writeTexture( normalFile, "T_" + name + "_Normal.png", std::move( normalPng.GetValue() ),
                                        ::Desert::Core::Formats::TextureIntent::NormalMap,
                                        ::Desert::Core::Formats::TextureColorSpace::Linear );
        if ( !normalGuid )
            return Common::MakeError( normalGuid.GetError() );

        Assets::MaterialData material;
        material.Header = ExistingMaterialHeader( matFile ); // the GUID survives a re-bake
        material.Shader =
             Assets::AssetGuidRef{ std::string( kSurfaceShaderGuid ), std::string( kSurfaceShaderPath ) };
        material.Textures.push_back( BakedSlot( kAlbedoSlot, albedoGuid.GetValue(), texFile ) );
        material.Textures.push_back( BakedSlot( kORMSlot, ormGuid.GetValue(), ormFile ) );
        material.Textures.push_back( BakedSlot( kNormalSlot, normalGuid.GetValue(), normalFile ) );
        material.Params.push_back(
             Assets::MaterialShaderParam{ std::string( kRoughnessFactor ), glm::vec4( 1.0f ) } );
        material.Params.push_back(
             Assets::MaterialShaderParam{ std::string( kMetallicFactor ), glm::vec4( 1.0f ) } );
        material = Assets::StampMaterialHeader( std::move( material ) );
        if ( auto saved = Assets::WriteMaterialFile( matFile, material ); !saved )
            return Common::MakeError( saved.GetError() );

        Assets::ContentRegistry::NoteFile( texFile );
        Assets::ContentRegistry::NoteFile( ormFile );
        Assets::ContentRegistry::NoteFile( normalFile );
        Assets::ContentRegistry::NoteFile( matFile );
        // A scene-record reference, spelled as a scene stores a material (see LandscapeHLODBake::SaveTile).
        const auto materialForm = ::Desert::Core::Serialize::StoredFormFor( "MaterialAsset" );
        if ( !materialForm )
            return Common::MakeError( "the material reference has no stored form (StoredAssetForm.cpp)" );
        m_Baked.push_back(
             HLODApproximateBaked{ .Key          = cell.Key,
                                   .MaterialGuid = material.Header->Guid,
                                   .MaterialPath = ::Desert::Core::Serialize::RenderStoredForm(
                                        *materialForm, Common::AssetHandle::StableKeyForPath( matFile ) ) } );
        LOG_INFO( "[Build HLODs] captured approximation {0}: {1} photos, {2} texels seen, {3} filled -> {4}, {5}, "
                  "{6}, {7}",
                  name, m_Photos.size(), baked.CapturedTexels, baked.FilledTexels, texFile.generic_string(),
                  ormFile.generic_string(), normalFile.generic_string(), matFile.generic_string() );
        return BOOLSUCCESS;
    }

    // One impostor cell: its photos packed into the octahedral atlas (the depth by ray casts into the same merged
    // mesh the photos show), the three maps and M_I<key> written, the card mesh around the atlas's sphere kept for
    // Build HLODs to store.
    Common::BoolResultStr HLODApproximateBake::SaveImpostor()
    {
        const HLODApproximateBakeCell& cell = m_Cells[m_Next];
        glm::vec3                      lo( std::numeric_limits<float>::max() );
        glm::vec3                      hi( std::numeric_limits<float>::lowest() );
        for ( const int v : m_Source.VertexIndicesItr() )
        {
            const glm::vec3 p( m_Source.GetVertex( v ) );
            lo = glm::min( lo, p );
            hi = glm::max( hi, p );
        }
        auto atlas = ::Desert::Core::BakeImpostorAtlas( m_Source, lo, hi, m_Photos, *cell.Impostor );
        if ( !atlas )
            return Common::MakeError( atlas.GetError() );
        const ::Desert::Core::HLODImpostorAtlas& baked = atlas.GetValue();

        // BaseColor: the G-buffer's linear colour stated sRGB, as every authored BaseColor; A (coverage) is
        // linear by definition and kept.
        std::vector<uint8_t> srgb( baked.BaseColor.size() );
        for ( std::size_t i = 0; i < srgb.size(); i += 4 )
        {
            srgb[i + 0] = LinearToSrgb8( baked.BaseColor[i + 0] );
            srgb[i + 1] = LinearToSrgb8( baked.BaseColor[i + 1] );
            srgb[i + 2] = LinearToSrgb8( baked.BaseColor[i + 2] );
            srgb[i + 3] = baked.BaseColor[i + 3];
        }
        auto albedoPng = EncodePng( srgb, baked.Side, baked.Side );
        auto depthPng  = albedoPng ? EncodePng( baked.NormalDepth, baked.Side, baked.Side ) : albedoPng;
        auto ormPng    = depthPng ? EncodePng( baked.ORM, baked.Side, baked.Side ) : depthPng;
        if ( !ormPng )
            return Common::MakeError( ormPng.GetError() );

        const std::filesystem::path dir       = cell.OutputDir.empty()
                                                     ? ::Desert::Core::WorldCells::BuiltHLODDirectory( m_ScenePath )
                                                     : cell.OutputDir;
        const std::string           name      = CellName( cell );
        const std::filesystem::path texFile   = dir / ( "T_" + name + "_BaseColor.detex" );
        const std::filesystem::path depthFile = dir / ( "T_" + name + "_NormalDepth.detex" );
        const std::filesystem::path ormFile   = dir / ( "T_" + name + "_ORM.detex" );
        const std::filesystem::path matFile   = dir / ( "M_" + name + ".demat" );
        std::error_code             ec;
        std::filesystem::create_directories( dir, ec );
        if ( ec )
            return Common::MakeError(
                 std::format( "'{}' could not be created: {}", dir.generic_string(), ec.message() ) );
        using Intent = ::Desert::Core::Formats::TextureIntent;
        using Space  = ::Desert::Core::Formats::TextureColorSpace;
        auto albedoGuid = WriteBakedTexture( dir, texFile, "T_" + name + "_BaseColor.png",
                                             std::move( albedoPng.GetValue() ), Intent::Colour, Space::SRGB );
        if ( !albedoGuid )
            return Common::MakeError( albedoGuid.GetError() );
        // Data, not NormalMap: BC5 would drop A, which carries the depth the pixel depth offset reads.
        auto depthGuid = WriteBakedTexture( dir, depthFile, "T_" + name + "_NormalDepth.png",
                                            std::move( depthPng.GetValue() ), Intent::Data, Space::Linear );
        if ( !depthGuid )
            return Common::MakeError( depthGuid.GetError() );
        auto ormGuid = WriteBakedTexture( dir, ormFile, "T_" + name + "_ORM.png", std::move( ormPng.GetValue() ),
                                          Intent::PackedMasks, Space::Linear );
        if ( !ormGuid )
            return Common::MakeError( ormGuid.GetError() );

        Assets::MaterialData material;
        material.Header = ExistingMaterialHeader( matFile ); // the GUID survives a re-bake
        material.Shader =
             Assets::AssetGuidRef{ std::string( kImpostorShaderGuid ), std::string( kImpostorShaderPath ) };
        material.Textures.push_back( BakedSlot( "BaseColorAtlas", albedoGuid.GetValue(), texFile ) );
        material.Textures.push_back( BakedSlot( "NormalDepthAtlas", depthGuid.GetValue(), depthFile ) );
        material.Textures.push_back( BakedSlot( "OrmAtlas", ormGuid.GetValue(), ormFile ) );
        const ::Desert::Core::HLODImpostorSettings& s = baked.Settings;
        const auto param = [&]( std::string_view paramName, glm::vec4 value )
        { material.Params.push_back( Assets::MaterialShaderParam{ std::string( paramName ), value } ); };
        param( "ImpostorSphere", glm::vec4( baked.Sphere.Centre, baked.Sphere.Radius ) );
        param( "FramesPerSide", glm::vec4( static_cast<float>( s.FramesPerSide ) ) );
        param( "FrameResolution", glm::vec4( static_cast<float>( s.FrameResolution ) ) );
        param( "Hemisphere", glm::vec4( s.Hemisphere ? 1.0f : 0.0f ) );
        material = Assets::StampMaterialHeader( std::move( material ) );
        if ( auto saved = Assets::WriteMaterialFile( matFile, material ); !saved )
            return Common::MakeError( saved.GetError() );

        Assets::ContentRegistry::NoteFile( texFile );
        Assets::ContentRegistry::NoteFile( depthFile );
        Assets::ContentRegistry::NoteFile( ormFile );
        Assets::ContentRegistry::NoteFile( matFile );
        const auto materialForm = ::Desert::Core::Serialize::StoredFormFor( "MaterialAsset" );
        if ( !materialForm )
            return Common::MakeError( "the material reference has no stored form (StoredAssetForm.cpp)" );
        m_Baked.push_back(
             HLODApproximateBaked{ .Key          = cell.Key,
                                   .MaterialGuid = material.Header->Guid,
                                   .MaterialPath = ::Desert::Core::Serialize::RenderStoredForm(
                                        *materialForm, Common::AssetHandle::StableKeyForPath( matFile ) ),
                                   .Card         = ::Desert::Core::HLODImpostorCardMesh( baked.Sphere ) } );
        LOG_INFO( "[Build HLODs] baked impostor {0}: {1} frames of {2}px, {3} texels covered, {4} depth misses -> "
                  "{5}, {6}, {7}, {8}",
                  name, m_Photos.size(), s.FrameResolution, baked.CoveredTexels, baked.DepthMisses,
                  texFile.generic_string(), depthFile.generic_string(), ormFile.generic_string(),
                  matFile.generic_string() );
        return BOOLSUCCESS;
    }

    bool HLODApproximateBake::Tick()
    {
        switch ( m_Phase )
        {
            case Phase::Build:
            {
                if ( !m_Renderer )
                {
                    const auto side = CaptureSide( m_Cells.front() );
                    m_Renderer      = Graphic::SceneRenderer::Create( Graphic::ViewExtent{ side, side },
                                                                      Graphic::kThumbnailViewProfile );
                }
                if ( !m_Renderer->AdvanceRendererBuild() )
                    return false;
                if ( auto started = StartScene(); !started )
                {
                    Fail( started.GetError() );
                    return true;
                }
                if ( auto cell = StartCell(); !cell )
                {
                    Fail( std::format( "approximation {}: {}", m_Cells[m_Next].Key, cell.GetError() ) );
                    return true;
                }
                return false;
            }
            case Phase::Frame:
            {
                if ( m_WarmFrames > 0 )
                {
                    if ( auto frame = m_Scene->OnUpdate( Common::Timestep( 0.016f ) ); !frame )
                        LOG_ERROR( "[Build HLODs] capture frame skipped: {0}", frame.GetError() );
                    --m_WarmFrames;
                    return false;
                }
                // GBufferOutputs.glslh: 0 BaseColor + Metallic, 1 world Normal + Roughness, 3 Emissive + AO.
                const auto& gbuffer  = m_Renderer->GetGBuffer();
                const auto  albedo   = gbuffer ? gbuffer->GetColorAttachmentImage( 0 ) : nullptr;
                const auto  bTarget  = gbuffer ? gbuffer->GetColorAttachmentImage( 1 ) : nullptr;
                const auto  emissive = gbuffer ? gbuffer->GetColorAttachmentImage( 3 ) : nullptr;
                if ( !albedo || !bTarget || !emissive )
                {
                    Fail( "the capture renderer has no G-buffer A / B / emissive target (is its path Deferred?)" );
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
                const std::size_t count = static_cast<std::size_t>( m_ReadbackWidth ) * m_ReadbackHeight;
                if ( pixels.GetValue().size() != count * 4 ||
                     bBytes.GetValue().size() != count * kHalfTexelBytes ||
                     eBytes.GetValue().size() != count * kHalfTexelBytes )
                {
                    Fail( std::format( "capture {}: the readbacks hold {} / {} / {} bytes, expected {}x{}",
                                       m_Cells[m_Next].Key, pixels.GetValue().size(), bBytes.GetValue().size(),
                                       eBytes.GetValue().size(), m_ReadbackWidth, m_ReadbackHeight ) );
                    return true;
                }
                ::Desert::Core::HLODCapturePhoto photo;
                photo.View              = m_Views[m_View];
                photo.Width             = m_ReadbackWidth;
                photo.Height            = m_ReadbackHeight;
                photo.BaseColorMetallic = pixels.ExtractValue();
                photo.NormalRoughness.resize( count );
                photo.Occlusion.resize( count );
                for ( std::size_t i = 0; i < count; ++i )
                {
                    photo.NormalRoughness[i] = HalfTexel( bBytes.GetValue(), i );
                    photo.Occlusion[i]       = HalfTexel( eBytes.GetValue(), i ).w;
                }
                m_Photos.push_back( std::move( photo ) );
                if ( ++m_View < m_Views.size() )
                {
                    FrameView();
                    return false;
                }
                if ( auto saved = SaveCell(); !saved )
                {
                    Fail( std::format( "approximation {}: {}", m_Cells[m_Next].Key, saved.GetError() ) );
                    return true;
                }
                m_Photos.clear();
                if ( ++m_Next >= m_Cells.size() )
                {
                    m_Phase = Phase::Done;
                    return true;
                }
                if ( auto cell = StartCell(); !cell )
                {
                    Fail( std::format( "approximation {}: {}", m_Cells[m_Next].Key, cell.GetError() ) );
                    return true;
                }
                return false;
            }
            case Phase::Done:
                return true;
        }
        return true;
    }
} // namespace Desert::Editor
