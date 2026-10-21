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
#include <cmath>
#include <cstring>
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
        // SurfaceResolveORM (Common/SurfaceInputs.glslh) reads r = AO, g = Roughness, b = Metallic and MULTIPLIES
        // roughness by RoughnessFactor (schema default 0.5): the baked roughness is the terrain's own, so 1.
        // AOStrength (1) and MetallicFactor (0) defaults already take the map as it is.
        constexpr std::string_view kORMSlot         = "u_ORMTexture";
        constexpr std::string_view kRoughnessFactor = "RoughnessFactor";
        // StandardSurface's tangent-space normal map (Intent NormalMap; NormalScale defaults to 1, the baked
        // normal is the terrain's own, so no parameter).
        constexpr std::string_view kNormalSlot = "u_NormalTexture";
        // The RGBA16F G-buffer targets: 4 half floats per texel, alpha at byte 6.
        constexpr std::size_t kHalfTexelBytes = 8;
        constexpr std::size_t kHalfAlphaByte  = 6;

        [[nodiscard]] float HalfAlpha( const std::vector<uint8_t>& texels, std::size_t texel )
        {
            uint16_t half = 0;
            std::memcpy( &half, texels.data() + texel * kHalfTexelBytes + kHalfAlphaByte, sizeof( half ) );
            return glm::unpackHalf1x16( half );
        }

        [[nodiscard]] uint8_t UnitToByte( float value )
        {
            return static_cast<uint8_t>( std::clamp( std::lround( value * 255.0f ), 0L, 255L ) );
        }

        // G-buffer B's xyz: the signed WORLD normal TerrainGBuffer writes (`oGBufferB = vec4(s.N, s.Roughness)`).
        [[nodiscard]] glm::vec3 HalfNormal( const std::vector<uint8_t>& texels, std::size_t texel )
        {
            uint16_t halves[3] = {};
            std::memcpy( halves, texels.data() + texel * kHalfTexelBytes, sizeof( halves ) );
            return glm::vec3( glm::unpackHalf1x16( halves[0] ), glm::unpackHalf1x16( halves[1] ),
                              glm::unpackHalf1x16( halves[2] ) );
        }

        // The z a stored tangent normal keeps at least: a world normal at or behind the mesh's surface plane
        // (a slope the coarse HLOD mesh flattens) is tipped back to the hemisphere the decoder reconstructs
        // (Common/TangentNormal.glslh derives z from x and y).
        constexpr float kMinTangentZ = 1.0e-3f;
        constexpr float kFlatEpsilon = 1.0e-6f;

        // THE NORMAL MAP, in the tangent frame the runtime shades the HLOD mesh with. Every render triangle is
        // rasterised in UV space over the W x H readback (texel (c, r) = UV ((c + 0.5) / W, (r + 0.5) / H): row
        // 0 is the tile's OriginZ edge, column 0 its OriginX edge, the albedo's own layout), its T, B, N
        // interpolated barycentrically as the rasteriser interpolates the vertex shader's normalised frame
        // (Mesh/Surface/Vertex_Static.glslh), and the G-buffer's world normal taken into that frame:
        // normalize(TBN * n) — what SurfaceTypes.glslh computes from the stored texel — gives the world normal
        // back. Texels no triangle covers stay flat (0, 0, 1).
        [[nodiscard]] std::vector<uint8_t> BakeTangentNormals( const ::Desert::Geometry::RenderMeshData& mesh,
                                                               const std::vector<uint8_t>&               gbufferB,
                                                               uint32_t width, uint32_t height )
        {
            std::vector<uint8_t> out( static_cast<std::size_t>( width ) * height * 4 );
            for ( std::size_t i = 0; i < out.size(); i += 4 )
            {
                out[i + 0] = 128;
                out[i + 1] = 128;
                out[i + 2] = 255;
                out[i + 3] = 255;
            }
            const glm::vec2 extent( static_cast<float>( width ), static_cast<float>( height ) );
            const auto      edge = []( const glm::vec2& a, const glm::vec2& b, const glm::vec2& p )
            { return ( b.x - a.x ) * ( p.y - a.y ) - ( b.y - a.y ) * ( p.x - a.x ); };
            for ( const auto& submesh : mesh.Submeshes )
                for ( uint32_t t = submesh.IndexOffset / 3; t < ( submesh.IndexOffset + submesh.IndexCount ) / 3;
                      ++t )
                {
                    // Index = one triangle; the submesh range is in uint32_t units (DynamicMeshRenderConversion).
                    const ::Desert::Index&  triangle = mesh.Indices[t];
                    const ::Desert::Vertex* v[3]     = { &mesh.Vertices[submesh.VertexOffset + triangle.V1],
                                                         &mesh.Vertices[submesh.VertexOffset + triangle.V2],
                                                         &mesh.Vertices[submesh.VertexOffset + triangle.V3] };
                    const glm::vec2 p[3] = { v[0]->TexCoord * extent, v[1]->TexCoord * extent,
                                             v[2]->TexCoord * extent };
                    const float     area = edge( p[0], p[1], p[2] );
                    if ( std::abs( area ) < kFlatEpsilon )
                        continue;
                    const glm::vec3 vT[3] = { glm::normalize( v[0]->Tangent ), glm::normalize( v[1]->Tangent ),
                                              glm::normalize( v[2]->Tangent ) };
                    const glm::vec3 vB[3] = { glm::normalize( v[0]->Bitangent ), glm::normalize( v[1]->Bitangent ),
                                              glm::normalize( v[2]->Bitangent ) };
                    const glm::vec3 vN[3] = { glm::normalize( v[0]->Normal ), glm::normalize( v[1]->Normal ),
                                              glm::normalize( v[2]->Normal ) };
                    const float     minX  = std::min( { p[0].x, p[1].x, p[2].x } );
                    const float     maxX  = std::max( { p[0].x, p[1].x, p[2].x } );
                    const float     minY  = std::min( { p[0].y, p[1].y, p[2].y } );
                    const float     maxY  = std::max( { p[0].y, p[1].y, p[2].y } );
                    const long      c0    = std::max( 0L, std::lround( std::floor( minX - 0.5f ) ) );
                    const long      c1 =
                         std::min( static_cast<long>( width ) - 1, std::lround( std::ceil( maxX - 0.5f ) ) );
                    const long r0 = std::max( 0L, std::lround( std::floor( minY - 0.5f ) ) );
                    const long r1 =
                         std::min( static_cast<long>( height ) - 1, std::lround( std::ceil( maxY - 0.5f ) ) );
                    for ( long r = r0; r <= r1; ++r )
                        for ( long c = c0; c <= c1; ++c )
                        {
                            const glm::vec2 q( static_cast<float>( c ) + 0.5f, static_cast<float>( r ) + 0.5f );
                            // A shared edge's texels belong to both triangles; the frame is continuous there.
                            const float w0 = edge( p[1], p[2], q ) / area;
                            const float w1 = edge( p[2], p[0], q ) / area;
                            const float w2 = 1.0f - w0 - w1;
                            if ( w0 < -kFlatEpsilon || w1 < -kFlatEpsilon || w2 < -kFlatEpsilon )
                                continue;
                            const std::size_t texel = static_cast<std::size_t>( r ) * width + c;
                            const glm::vec3   world = HalfNormal( gbufferB, texel );
                            if ( glm::dot( world, world ) < kFlatEpsilon )
                                continue; // the clear colour: nothing drew here
                            const glm::mat3 tbn( w0 * vT[0] + w1 * vT[1] + w2 * vT[2],
                                                 w0 * vB[0] + w1 * vB[1] + w2 * vB[2],
                                                 w0 * vN[0] + w1 * vN[1] + w2 * vN[2] );
                            if ( std::abs( glm::determinant( tbn ) ) < kFlatEpsilon )
                                continue;
                            glm::vec3 n = glm::inverse( tbn ) * glm::normalize( world );
                            if ( glm::dot( n, n ) < kFlatEpsilon )
                                continue;
                            n = glm::normalize( n );
                            if ( n.z < kMinTangentZ )
                                n = glm::normalize( glm::vec3( n.x, n.y, kMinTangentZ ) );
                            out[texel * 4 + 0] = UnitToByte( n.x * 0.5f + 0.5f );
                            out[texel * 4 + 1] = UnitToByte( n.y * 0.5f + 0.5f );
                            out[texel * 4 + 2] = UnitToByte( n.z * 0.5f + 0.5f );
                        }
                }
            return out;
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
        world[3] =
             glm::vec4( tile.OriginX + tile.SizeX * 0.5f, kEyeHeightCm, tile.OriginZ + tile.SizeZ * 0.5f, 1.0f );
        m_Camera->SetOrthoSize( tile.SizeZ * 0.5f );
        m_Camera->SetView( ::Desert::Core::CameraEntityViewOf( world, 45.0f, kNearCm, kFarCm ), width, height,
                           ::Desert::Core::Camera::kNoSourceEntity );
        m_Scene->PinActiveCamera( m_Camera );
        m_WarmFrames = kWarmFrames;
        m_Phase      = Phase::Frame;
    }

    Common::BoolResultStr LandscapeHLODBake::SaveTile( const std::vector<uint8_t>& rgba,
                                                       const std::vector<uint8_t>& gbufferB,
                                                       const std::vector<uint8_t>& emissive, uint32_t width,
                                                       uint32_t height )
    {
        const ::Desert::Core::LandscapeHLODTileArea& tile   = m_Tiles[m_Next].Area;
        const std::size_t                            texels = static_cast<std::size_t>( width ) * height;
        if ( rgba.size() != texels * 4 )
            return Common::MakeError( std::format( "the albedo readback holds {} bytes, expected {}x{} RGBA8",
                                                   rgba.size(), width, height ) );
        if ( gbufferB.size() != texels * kHalfTexelBytes || emissive.size() != texels * kHalfTexelBytes )
            return Common::MakeError(
                 std::format( "the G-buffer B / emissive readbacks hold {} / {} bytes, expected {}x{} RGBA16F",
                              gbufferB.size(), emissive.size(), width, height ) );
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
        // ORM in the albedo's row order: r = material AO (Emissive.a), g = roughness (B.a), b = metallic 0 (the
        // terrain writes none, TerrainGBuffer.shader).
        std::vector<uint8_t> orm( texels * 4 );
        for ( std::size_t i = 0; i < texels; ++i )
        {
            orm[i * 4 + 0] = UnitToByte( HalfAlpha( emissive, i ) );
            orm[i * 4 + 1] = UnitToByte( HalfAlpha( gbufferB, i ) );
            orm[i * 4 + 2] = 0;
            orm[i * 4 + 3] = 255;
        }
        auto ormPng = EncodePng( orm, width, height );
        if ( !ormPng )
            return Common::MakeError( ormPng.GetError() );
        // The normal map is stated in the tangent frame of the render mesh the runtime draws for this tile:
        // the same conversion ComponentRegistry's StaticMesh load makes from the EditMesh.
        auto dynamicMesh = ::Desert::Geometry::DynamicMeshFromSerialized( m_Tiles[m_Next].Mesh, "HLOD" );
        if ( !dynamicMesh )
            return Common::MakeError( "the tile's HLOD mesh did not load: " + dynamicMesh.GetError() );
        auto renderMesh = ::Desert::Geometry::ToRenderMesh( dynamicMesh.GetValue() );
        if ( !renderMesh )
            return Common::MakeError( "the tile's HLOD mesh has no render form: " + renderMesh.GetError() );
        auto normalPng =
             EncodePng( BakeTangentNormals( renderMesh.GetValue(), gbufferB, width, height ), width, height );
        if ( !normalPng )
            return Common::MakeError( normalPng.GetError() );

        const std::filesystem::path dir      = ::Desert::Core::WorldCells::BuiltHLODDirectory( m_ScenePath );
        const std::string           tileName = std::to_string( static_cast<uint64_t>( tile.Tile ) );
        const std::filesystem::path texFile  = dir / ( "T_" + tileName + "_BaseColor.detex" );
        const std::filesystem::path ormFile  = dir / ( "T_" + tileName + "_ORM.detex" );
        const std::filesystem::path normalFile = dir / ( "T_" + tileName + "_Normal.detex" );
        const std::filesystem::path matFile  = dir / ( "M_" + tileName + ".demat" );
        std::error_code             ec;
        std::filesystem::create_directories( dir, ec );
        if ( ec )
            return Common::MakeError(
                 std::format( "'{}' could not be created: {}", dir.generic_string(), ec.message() ) );

        // Writes one baked texture and answers the GUID its header states.
        const auto writeTexture =
             [&]( const std::filesystem::path& file, const std::string& sourceName, std::vector<std::byte> encoded,
                  ::Desert::Core::Formats::TextureIntent     intent,
                  ::Desert::Core::Formats::TextureColorSpace colorSpace ) -> Common::ResultStr<std::string>
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
            // THE BAKE AUTHORS THESE SETTINGS. WriteTextureSource keeps an existing asset's settings (the
            // re-import rule for a person's texture); a baked tile texture has no person behind it, so an
            // earlier bake's settings (an ORM written as Data before PackedMasks existed) are replaced here,
            // GUID kept, and the next cook re-runs on the new intent (it is in the cook signature).
            if ( texture.GetValue().Import.Settings != settings )
            {
                Assets::TextureSourceAsset rewritten = texture.ExtractValue();
                rewritten.Import.Settings            = settings;
                if ( auto w = Assets::WriteTextureSourceAssetFile( file, rewritten ); !w.IsSuccess() )
                    return Common::MakeError<std::string>( w.GetError() );
                return Common::MakeSuccess( Common::Content::AssetGuidToText( rewritten.Guid ) );
            }
            return Common::MakeSuccess( Common::Content::AssetGuidToText( texture.GetValue().Guid ) );
        };
        auto albedoGuid = writeTexture( texFile, "T_" + tileName + "_BaseColor.png", std::move( png.GetValue() ),
                                        ::Desert::Core::Formats::TextureIntent::Colour,
                                        ::Desert::Core::Formats::TextureColorSpace::SRGB );
        if ( !albedoGuid )
            return Common::MakeError( albedoGuid.GetError() );
        auto ormGuid = writeTexture( ormFile, "T_" + tileName + "_ORM.png", std::move( ormPng.GetValue() ),
                                     ::Desert::Core::Formats::TextureIntent::PackedMasks,
                                     ::Desert::Core::Formats::TextureColorSpace::Linear );
        if ( !ormGuid )
            return Common::MakeError( ormGuid.GetError() );
        auto normalGuid =
             writeTexture( normalFile, "T_" + tileName + "_Normal.png", std::move( normalPng.GetValue() ),
                           ::Desert::Core::Formats::TextureIntent::NormalMap,
                           ::Desert::Core::Formats::TextureColorSpace::Linear );
        if ( !normalGuid )
            return Common::MakeError( normalGuid.GetError() );

        Assets::MaterialData material;
        material.Header = ExistingMaterialHeader( matFile ); // the GUID survives a re-bake
        material.Shader =
             Assets::AssetGuidRef{ std::string( kSurfaceShaderGuid ), std::string( kSurfaceShaderPath ) };
        Assets::MaterialAssetRef albedo;
        albedo.Name = std::string( kAlbedoSlot );
        albedo.Guid = albedoGuid.GetValue();
        albedo.Path = Common::AssetHandle::StableKeyForPath( texFile );
        material.Textures.push_back( std::move( albedo ) );
        Assets::MaterialAssetRef ormRef;
        ormRef.Name = std::string( kORMSlot );
        ormRef.Guid = ormGuid.GetValue();
        ormRef.Path = Common::AssetHandle::StableKeyForPath( ormFile );
        material.Textures.push_back( std::move( ormRef ) );
        Assets::MaterialAssetRef normalRef;
        normalRef.Name = std::string( kNormalSlot );
        normalRef.Guid = normalGuid.GetValue();
        normalRef.Path = Common::AssetHandle::StableKeyForPath( normalFile );
        material.Textures.push_back( std::move( normalRef ) );
        material.Params.push_back(
             Assets::MaterialShaderParam{ std::string( kRoughnessFactor ), glm::vec4( 1.0f ) } );
        material = Assets::StampMaterialHeader( std::move( material ) );
        if ( auto saved = Assets::WriteMaterialFile( matFile, material ); !saved )
            return Common::MakeError( saved.GetError() );

        Assets::ContentRegistry::NoteFile( texFile );
        Assets::ContentRegistry::NoteFile( ormFile );
        Assets::ContentRegistry::NoteFile( normalFile );
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
        LOG_INFO( "[Build HLODs] baked tile {0}: {1}x{2} BaseColor + ORM + Normal -> {3}, {4}, {5}, {6}", tileName,
                  width, height, texFile.generic_string(), ormFile.generic_string(), normalFile.generic_string(),
                  matFile.generic_string() );
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
