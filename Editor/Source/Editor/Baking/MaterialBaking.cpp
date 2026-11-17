#include "MaterialBaking.hpp"

#include <Editor/Core/Selection/ModelingToolTarget.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Import/TextureDnD.hpp>
#include <Editor/Import/TextureImporter.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/CameraEntityView.hpp>
#include <Engine/Core/Formats/TextureIntent.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/SceneRenderCollectors.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Texture/TextureService.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Core.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Timestep.hpp>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <system_error>
#include <utility>

#include <stb_image/stb_image_write.h>

namespace Desert::Editor::Baking
{
    namespace
    {
        namespace fs  = std::filesystem;
        namespace Fmt = ::Desert::Core::Formats;

        // The proxy's side on the X/Z plane: the unit UV square drawn as a square of this many centimetres. Any
        // size frames the same texels (the camera's orthographic window is the square); a metre keeps the depth
        // range and the material's world-scaled inputs (a texture's world tiling) in an ordinary range.
        constexpr float kProxyExtentCm = 100.0f;
        // The camera looks straight down at the plane from this height, its depth range around it.
        constexpr float kEyeHeightCm = 1000.0f;
        constexpr float kNearCm      = 1.0f;
        constexpr float kFarCm       = 1.0e4f;
        // Frames recorded before the G-buffer is read: the renderer's stages, the material's textures streaming
        // in and the temporal history settling (LandscapeHLODBake's count).
        constexpr int kWarmFrames = 60;

        constexpr std::size_t kHalfTexelBytes = 8; // RGBA16F
        constexpr float       kFlatEpsilon    = 1.0e-6f;
        constexpr float       kMinTangentZ    = 1.0e-3f;

        [[nodiscard]] glm::vec4 HalfTexel( const std::vector<uint8_t>& texels, const std::size_t texel )
        {
            uint16_t halves[4] = {};
            std::memcpy( halves, texels.data() + texel * kHalfTexelBytes, sizeof( halves ) );
            return glm::vec4( glm::unpackHalf1x16( halves[0] ), glm::unpackHalf1x16( halves[1] ),
                              glm::unpackHalf1x16( halves[2] ), glm::unpackHalf1x16( halves[3] ) );
        }

        [[nodiscard]] uint8_t UnitToByte( const float value )
        {
            return static_cast<uint8_t>( std::clamp( std::lround( value * 255.0f ), 0L, 255L ) );
        }

        [[nodiscard]] float LinearToSrgb( const float linear )
        {
            const float c = std::clamp( linear, 0.0f, 1.0f );
            return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow( c, 1.0f / 2.4f ) - 0.055f;
        }

        // The texture coordinate a texel of the bake stands for: DesertMeshTextureUV of the stored UV (origin top
        // left, Graphic/ShaderProtocols/VertexInputs.hpp), so row 0 of every output is v = 0 of the texture.
        [[nodiscard]] glm::vec2 TextureUV( const ::Desert::Vertex& vertex )
        {
            return glm::vec2( vertex.TexCoord.x, 1.0f - vertex.TexCoord.y );
        }

        // Every triangle of the slot's submeshes, as its three vertices.
        template <typename Fn>
        Common::BoolResultStr ForEachSlotTriangle( const Geometry::RenderMeshData& mesh, const int slot, Fn&& fn )
        {
            if ( mesh.SubmeshMaterialIds.size() != mesh.Submeshes.size() )
                return Common::MakeError( std::format( "the mesh states {} material ids for {} submeshes",
                                                       mesh.SubmeshMaterialIds.size(), mesh.Submeshes.size() ) );
            for ( std::size_t s = 0; s < mesh.Submeshes.size(); ++s )
            {
                if ( mesh.SubmeshMaterialIds[s] != slot )
                    continue;
                const ::Desert::Submesh& submesh = mesh.Submeshes[s];
                for ( uint32_t t = submesh.IndexOffset / 3; t < ( submesh.IndexOffset + submesh.IndexCount ) / 3;
                      ++t )
                {
                    const ::Desert::Index&  triangle = mesh.Indices[t];
                    const ::Desert::Vertex* v[3]     = { &mesh.Vertices[submesh.VertexOffset + triangle.V1],
                                                         &mesh.Vertices[submesh.VertexOffset + triangle.V2],
                                                         &mesh.Vertices[submesh.VertexOffset + triangle.V3] };
                    fn( v );
                }
            }
            return BOOLSUCCESS;
        }

        [[nodiscard]] std::string FileStem( const std::string& name )
        {
            std::string out;
            for ( const char c : name )
                out += ( std::isalnum( static_cast<unsigned char>( c ) ) || c == '_' || c == '-' ) ? c : '_';
            return out.empty() ? std::string( "Mesh" ) : out;
        }

        // One flattened property as a texture asset under Assets/Textures/Baked (MeshBakeNormalTool's writer): the
        // PNG source, imported with @p intent; a re-bake replaces the built texture the editor holds.
        Common::ResultStr<fs::path> WriteTexture( Assets::AssetManager& assets, const uint32_t size,
                                                  const std::vector<uint8_t>& rgba, const std::string& name,
                                                  const Fmt::TextureIntent intent )
        {
            using Out           = fs::path;
            const fs::path  dir = Common::Constants::Path::TEXTUREDIR_PATH / "Baked";
            std::error_code ec;
            fs::create_directories( dir, ec );
            if ( ec )
                return Common::MakeError<Out>(
                     std::format( "'{}' was not created: {}", dir.generic_string(), ec.message() ) );
            const fs::path png    = dir / ( name + ".png" );
            const int      side   = static_cast<int>( size );
            const int      stride = side * 4;
            if ( stbi_write_png( png.string().c_str(), side, side, 4, rgba.data(), stride ) == 0 )
                return Common::MakeError<Out>( std::format( "'{}' was not written", png.generic_string() ) );
            auto asset = TextureImporter::ImportSourceAsset( png, intent );
            if ( !asset.IsSuccess() )
                return Common::MakeError<Out>( std::format( "'{}': {}", png.generic_string(), asset.GetError() ) );
            Common::AssetHandle handle = TextureDnD::ResolveExisting( assets, png.string() );
            if ( !handle.IsNull() )
                Runtime::ResourceRegistry::GetTextureService()->EvictBuilt( handle );
            else
                handle = TextureDnD::ResolveOrImport( assets, png.string() );
            if ( handle.IsNull() )
                return Common::MakeError<Out>(
                     std::format( "'{}' was not registered as a texture", asset.GetValue().generic_string() ) );
            return Common::MakeSuccess( asset.GetValue() );
        }
    } // namespace

    Common::ResultStr<Geometry::RenderMeshData> MakeUVSpaceProxy( const Geometry::RenderMeshData& mesh,
                                                                  const int slot, const float extentCm )
    {
        using Out = Geometry::RenderMeshData;
        Out                              proxy;
        std::optional<::Desert::Submesh> first;
        for ( std::size_t s = 0; s < mesh.Submeshes.size() && s < mesh.SubmeshMaterialIds.size(); ++s )
            if ( mesh.SubmeshMaterialIds[s] == slot )
            {
                first = mesh.Submeshes[s];
                break;
            }
        if ( !first )
            return Common::MakeError<Out>( std::format( "the mesh has no submesh of material slot {}", slot ) );

        const auto walked =
             ForEachSlotTriangle( mesh, slot,
                                  [&]( const ::Desert::Vertex* const( &v )[3] )
                                  {
                                      const auto base = static_cast<uint32_t>( proxy.Vertices.size() );
                                      glm::vec3  at[3];
                                      for ( int corner = 0; corner < 3; ++corner )
                                      {
                                          ::Desert::Vertex vertex = *v[corner];
                                          const glm::vec2  uv     = TextureUV( vertex );
                                          // X = u, Z = v: the bake camera's right is +X and its up is -Z, so texel
                                          // column c is u and row r is v (FrameProxy).
                                          vertex.Position = glm::vec3( uv.x * extentCm, 0.0f, uv.y * extentCm );
                                          at[corner]      = vertex.Position;
                                          proxy.Vertices.push_back( vertex );
                                      }
                                      // The source's front face is the winding whose face normal follows its
                                      // vertex normals; in UV space that winding faces the camera (+Y) or, on a
                                      // mirrored island, away. Wound to face it, so a one-sided material's
                                      // back-face cull keeps every island and the cell sees a front face.
                                      const float     facing = glm::cross( at[1] - at[0], at[2] - at[0] ).y;
                                      ::Desert::Index triangle{};
                                      triangle.V1 = base;
                                      triangle.V2 = facing >= 0.0f ? base + 1 : base + 2;
                                      triangle.V3 = facing >= 0.0f ? base + 2 : base + 1;
                                      proxy.Indices.push_back( triangle );
                                  } );
        if ( !walked )
            return Common::MakeError<Out>( walked.GetError() );
        if ( proxy.Indices.empty() )
            return Common::MakeError<Out>( std::format( "material slot {} has no triangle", slot ) );

        ::Desert::Submesh one = *first;
        one.VertexOffset      = 0;
        one.IndexOffset       = 0;
        one.IndexCount        = static_cast<uint32_t>( proxy.Indices.size() * 3 );
        // The proxy's vertices are already where the camera frames them: no submesh transform moves them.
        one.Transform = glm::mat4( 1.0f );
        proxy.Submeshes.push_back( one );
        proxy.SubmeshMaterialIds.push_back( slot );
        return Common::MakeSuccess( std::move( proxy ) );
    }

    void DilateIslands( std::vector<uint8_t>& rgba, std::vector<uint8_t> covered, const uint32_t size,
                        const uint32_t rings )
    {
        const auto at = [size]( const uint32_t x, const uint32_t y )
        { return static_cast<std::size_t>( y ) * size + x; };
        std::vector<std::size_t> frontier;
        std::vector<std::size_t> next;
        // The first ring: every empty texel touching a covered one.
        for ( uint32_t y = 0; y < size; ++y )
            for ( uint32_t x = 0; x < size; ++x )
            {
                if ( covered[at( x, y )] != 0 )
                    continue;
                bool touches = false;
                for ( int dy = -1; dy <= 1 && !touches; ++dy )
                    for ( int dx = -1; dx <= 1 && !touches; ++dx )
                    {
                        const int nx = static_cast<int>( x ) + dx;
                        const int ny = static_cast<int>( y ) + dy;
                        if ( nx < 0 || ny < 0 || nx >= static_cast<int>( size ) || ny >= static_cast<int>( size ) )
                            continue;
                        touches = covered[at( static_cast<uint32_t>( nx ), static_cast<uint32_t>( ny ) )] != 0;
                    }
                if ( touches )
                    frontier.push_back( at( x, y ) );
            }
        for ( uint32_t ring = 0; ring < rings && !frontier.empty(); ++ring )
        {
            // Each texel of the ring takes the mean of its covered neighbours as they stood BEFORE the ring, so
            // the smear grows evenly outward (UE PerformUVBorderSmear).
            std::vector<std::pair<std::size_t, glm::uvec4>> filled;
            filled.reserve( frontier.size() );
            for ( const std::size_t texel : frontier )
            {
                if ( covered[texel] != 0 )
                    continue;
                const auto x = static_cast<int>( texel % size );
                const auto y = static_cast<int>( texel / size );
                glm::uvec4 sum( 0u );
                uint32_t   count = 0;
                for ( int dy = -1; dy <= 1; ++dy )
                    for ( int dx = -1; dx <= 1; ++dx )
                    {
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if ( nx < 0 || ny < 0 || nx >= static_cast<int>( size ) || ny >= static_cast<int>( size ) )
                            continue;
                        const std::size_t n = at( static_cast<uint32_t>( nx ), static_cast<uint32_t>( ny ) );
                        if ( covered[n] == 0 )
                            continue;
                        sum += glm::uvec4( rgba[n * 4 + 0], rgba[n * 4 + 1], rgba[n * 4 + 2], rgba[n * 4 + 3] );
                        ++count;
                    }
                if ( count != 0 )
                    filled.emplace_back( texel, ( sum + glm::uvec4( count / 2 ) ) / count );
            }
            next.clear();
            for ( const auto& [texel, value] : filled )
            {
                for ( int c = 0; c < 4; ++c )
                    rgba[texel * 4 + c] = static_cast<uint8_t>( value[c] );
                covered[texel] = 1;
            }
            for ( const auto& [texel, value] : filled )
            {
                const auto x = static_cast<int>( texel % size );
                const auto y = static_cast<int>( texel / size );
                for ( int dy = -1; dy <= 1; ++dy )
                    for ( int dx = -1; dx <= 1; ++dx )
                    {
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if ( nx < 0 || ny < 0 || nx >= static_cast<int>( size ) || ny >= static_cast<int>( size ) )
                            continue;
                        const std::size_t n = at( static_cast<uint32_t>( nx ), static_cast<uint32_t>( ny ) );
                        if ( covered[n] == 0 )
                            next.push_back( n );
                    }
            }
            std::sort( next.begin(), next.end() );
            next.erase( std::unique( next.begin(), next.end() ), next.end() );
            std::swap( frontier, next );
        }
    }

    MaterialBake::MaterialBake( MaterialBakeRequest request ) : m_Request( std::move( request ) )
    {
        if ( m_Request.Size == 0 )
            Fail( "the bake size is 0" );
        else if ( m_Request.Material.IsNull() )
            Fail( std::format( "material slot {} names no material", m_Request.Slot ) );
    }

    MaterialBake::~MaterialBake()
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

    void MaterialBake::Fail( std::string error )
    {
        m_Error = std::move( error );
        m_Phase = Phase::Done;
    }

    Common::BoolResultStr MaterialBake::StartScene()
    {
        auto proxy = MakeUVSpaceProxy( m_Request.Mesh, m_Request.Slot, kProxyExtentCm );
        if ( !proxy )
            return Common::MakeError( proxy.GetError() );
        const Geometry::RenderMeshData& data = proxy.GetValue();

        m_Scene = std::make_shared<::Desert::Core::Scene>( "MaterialBake", m_Renderer.get() );
        ::Desert::Core::AddSceneRenderCollectors( *m_Scene );
        if ( auto inited = m_Scene->Init( ::Desert::Core::Scene::InitPurpose::Offscreen ); !inited )
            return Common::MakeError( "the bake scene failed to initialise: " + inited.GetError() );
        m_Scene->Resize( m_Request.Size, m_Request.Size );

        // The proxy is the entity's edited mesh, set through the one writer of an edited mesh and its render form
        // (ECS::SetEditableMesh) and the one writer of the component (ECS::SetStaticMesh): the welded proxy keeps
        // the source's normal, tangent, bitangent and UV as overlays, so its render form is the proxy again. Every
        // slot index its submesh may name draws the baked material.
        auto imported = Geometry::DynamicMeshFromRenderMesh( data );
        if ( !imported )
            return Common::MakeError( "the UV-space proxy did not convert: " + imported.GetError() );
        Geometry::ImportedDynamicMesh welded = imported.ExtractValue();
        ECS::StaticMeshDesc           desc;
        desc.MaterialSlots.assign( static_cast<std::size_t>( m_Request.Slot ) + 1, m_Request.Material );
        desc.CastShadows       = false;
        desc.CastContactShadow = false;
        if ( auto set = ECS::SetEditableMesh(
                  desc, std::make_shared<Geometry::DynamicMesh3>( std::move( welded.Mesh ) ) );
             !set )
            return Common::MakeError( "the UV-space proxy was refused: " + set.GetError() );
        ECS::Entity& entity = m_Scene->CreateNewEntity( "MaterialBakeProxy" );
        static_cast<void>( ECS::SetStaticMesh( *entity.GetRegistry(), entity.GetHandle(), desc ) );

        // Straight down at the plane: right = +X, up = -Z, so texel column c is u and row r is v; the
        // orthographic window is the proxy's square.
        glm::mat4 world( 1.0f );
        world[0] = glm::vec4( 1.0f, 0.0f, 0.0f, 0.0f );  // right
        world[1] = glm::vec4( 0.0f, 0.0f, -1.0f, 0.0f ); // up
        world[2] = glm::vec4( 0.0f, 1.0f, 0.0f, 0.0f );  // backward (the camera looks down -Y)
        world[3] = glm::vec4( kProxyExtentCm * 0.5f, kEyeHeightCm, kProxyExtentCm * 0.5f, 1.0f );
        m_Camera = std::make_shared<::Desert::Core::GameplayCamera>();
        m_Camera->SetProjectionType( ::Desert::Core::ProjectionType::Orthographic );
        m_Camera->SetOrthoSize( kProxyExtentCm * 0.5f );
        m_Camera->SetView( ::Desert::Core::CameraEntityViewOf( world, 45.0f, kNearCm, kFarCm ), m_Request.Size,
                           m_Request.Size, ::Desert::Core::Camera::kNoSourceEntity );
        m_Scene->PinActiveCamera( m_Camera );
        return BOOLSUCCESS;
    }

    bool MaterialBake::Tick()
    {
        switch ( m_Phase )
        {
            case Phase::Build:
            {
                if ( !m_Renderer )
                    m_Renderer = std::make_unique<Graphic::SceneRenderer>(
                         Graphic::ViewExtent{ m_Request.Size, m_Request.Size }, Graphic::kThumbnailViewProfile );
                if ( !m_Renderer->AdvanceRendererBuild() )
                    return false;
                if ( auto started = StartScene(); !started )
                {
                    Fail( started.GetError() );
                    return true;
                }
                m_WarmFrames = kWarmFrames;
                m_Phase      = Phase::Frame;
                return false;
            }
            case Phase::Frame:
            {
                if ( m_WarmFrames > 0 )
                {
                    if ( auto frame = m_Scene->OnUpdate( Common::Timestep( 0.016f ) ); !frame )
                        LOG_ERROR( "[Material Bake] bake frame skipped: {0}", frame.GetError() );
                    --m_WarmFrames;
                    return false;
                }
                const auto& gbuffer  = m_Renderer->GetGBuffer();
                const auto  albedo   = gbuffer ? gbuffer->GetColorAttachmentImage( 0 ) : nullptr;
                const auto  bTarget  = gbuffer ? gbuffer->GetColorAttachmentImage( 1 ) : nullptr;
                const auto  emissive = gbuffer ? gbuffer->GetColorAttachmentImage( 3 ) : nullptr;
                if ( !albedo || !bTarget || !emissive )
                {
                    Fail( "the bake renderer has no G-buffer albedo / B / emissive target" );
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
                m_Albedo   = albedoCopy.GetValue();
                m_GBufferB = bCopy.GetValue();
                m_Emissive = emissiveCopy.GetValue();
                m_Phase    = Phase::Readback;
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
                if ( auto flat = Flatten( pixels.GetValue(), bBytes.GetValue(), eBytes.GetValue() ); !flat )
                {
                    Fail( flat.GetError() );
                    return true;
                }
                m_Phase = Phase::Done;
                return true;
            }
            case Phase::Done:
                return true;
        }
        return true;
    }

    Common::BoolResultStr MaterialBake::Flatten( const std::vector<uint8_t>& albedo,
                                                 const std::vector<uint8_t>& gbufferB,
                                                 const std::vector<uint8_t>& emissive )
    {
        const uint32_t    size   = m_Request.Size;
        const std::size_t texels = static_cast<std::size_t>( size ) * size;
        if ( albedo.size() != texels * 4 )
            return Common::MakeError( std::format( "the albedo readback holds {} bytes, expected {}x{} RGBA8",
                                                   albedo.size(), size, size ) );
        if ( gbufferB.size() != texels * kHalfTexelBytes || emissive.size() != texels * kHalfTexelBytes )
            return Common::MakeError(
                 std::format( "the G-buffer B / emissive readbacks hold {} / {} bytes, expected {}x{} RGBA16F",
                              gbufferB.size(), emissive.size(), size, size ) );

        FlattenedMaterial out;
        out.Size = size;
        out.BaseColor.assign( texels * 4, 0 );
        out.Normal.assign( texels * 4, 0 );
        out.ORM.assign( texels * 4, 0 );
        out.Emissive.assign( texels * 4, 0 );
        out.OpacityMask.assign( texels * 4, 0 );
        // Inside the islands (the slot's triangles rasterised in UV space) and, of those, what the cell drew.
        std::vector<uint8_t> covered( texels, 0 );
        std::vector<uint8_t> drawn( texels, 0 );

        // The source's tangent frame per texel, as the cell interpolated it, takes the G-buffer's world normal
        // back into tangent space (UE bakes the Normal property in the mesh's own tangent frame).
        const glm::vec2 extent( static_cast<float>( size ) );
        const auto      edge = []( const glm::vec2& a, const glm::vec2& b, const glm::vec2& p )
        { return ( b.x - a.x ) * ( p.y - a.y ) - ( b.y - a.y ) * ( p.x - a.x ); };
        const auto walked = ForEachSlotTriangle(
             m_Request.Mesh, m_Request.Slot,
             [&]( const ::Desert::Vertex* const( &v )[3] )
             {
                 const glm::vec2 p[3] = { TextureUV( *v[0] ) * extent, TextureUV( *v[1] ) * extent,
                                          TextureUV( *v[2] ) * extent };
                 const float     area = edge( p[0], p[1], p[2] );
                 if ( std::abs( area ) < kFlatEpsilon )
                     return;
                 const long c0 =
                      std::max( 0L, std::lround( std::floor( std::min( { p[0].x, p[1].x, p[2].x } ) ) ) );
                 const long c1 = std::min( static_cast<long>( size ) - 1,
                                           std::lround( std::ceil( std::max( { p[0].x, p[1].x, p[2].x } ) ) ) );
                 const long r0 =
                      std::max( 0L, std::lround( std::floor( std::min( { p[0].y, p[1].y, p[2].y } ) ) ) );
                 const long r1 = std::min( static_cast<long>( size ) - 1,
                                           std::lround( std::ceil( std::max( { p[0].y, p[1].y, p[2].y } ) ) ) );
                 for ( long r = r0; r <= r1; ++r )
                     for ( long c = c0; c <= c1; ++c )
                     {
                         const glm::vec2 q( static_cast<float>( c ) + 0.5f, static_cast<float>( r ) + 0.5f );
                         const float     w0 = edge( p[1], p[2], q ) / area;
                         const float     w1 = edge( p[2], p[0], q ) / area;
                         const float     w2 = 1.0f - w0 - w1;
                         if ( w0 < -kFlatEpsilon || w1 < -kFlatEpsilon || w2 < -kFlatEpsilon )
                             continue;
                         const std::size_t texel =
                              static_cast<std::size_t>( r ) * size + static_cast<std::size_t>( c );
                         covered[texel]    = 1;
                         const glm::vec4 b = HalfTexel( gbufferB, texel );
                         const glm::vec3 world( b );
                         if ( glm::dot( world, world ) < kFlatEpsilon )
                             continue; // the clear: the masked cell's clip left nothing here
                         const glm::mat3 tbn( w0 * v[0]->Tangent + w1 * v[1]->Tangent + w2 * v[2]->Tangent,
                                              w0 * v[0]->Bitangent + w1 * v[1]->Bitangent + w2 * v[2]->Bitangent,
                                              w0 * v[0]->Normal + w1 * v[1]->Normal + w2 * v[2]->Normal );
                         if ( std::abs( glm::determinant( tbn ) ) < kFlatEpsilon )
                             continue;
                         glm::vec3 n = glm::inverse( tbn ) * glm::normalize( world );
                         if ( glm::dot( n, n ) < kFlatEpsilon )
                             continue;
                         n = glm::normalize( n );
                         if ( n.z < kMinTangentZ )
                             n = glm::normalize( glm::vec3( n.x, n.y, kMinTangentZ ) );
                         drawn[texel]              = 1;
                         out.Normal[texel * 4 + 0] = UnitToByte( n.x * 0.5f + 0.5f );
                         out.Normal[texel * 4 + 1] = UnitToByte( n.y * 0.5f + 0.5f );
                         out.Normal[texel * 4 + 2] = UnitToByte( n.z * 0.5f + 0.5f );
                         out.Normal[texel * 4 + 3] = 255;
                     }
             } );
        if ( !walked )
            return walked;

        // THE ISLAND'S EDGE IS WHERE BOTH RASTERISERS AGREE. The G-buffer is drawn with the view's temporal jitter
        // (up to half a texel) and the GPU's fill rule, the islands above by texel centre: a border texel the CPU
        // counts inside and the GPU left empty is a raster disagreement, not the material's mask, so it leaves the
        // island and the dilation fills it like any texel outside. Inside the island an empty texel is the mask.
        std::vector<uint8_t> rasterMiss( texels, 0 );
        for ( uint32_t y = 0; y < size; ++y )
            for ( uint32_t x = 0; x < size; ++x )
            {
                const std::size_t texel = static_cast<std::size_t>( y ) * size + x;
                if ( covered[texel] == 0 || drawn[texel] != 0 )
                    continue;
                for ( int dy = -1; dy <= 1 && rasterMiss[texel] == 0; ++dy )
                    for ( int dx = -1; dx <= 1 && rasterMiss[texel] == 0; ++dx )
                    {
                        const int  nx      = static_cast<int>( x ) + dx;
                        const int  ny      = static_cast<int>( y ) + dy;
                        const bool outside = nx < 0 || ny < 0 || nx >= static_cast<int>( size ) ||
                                             ny >= static_cast<int>( size ) ||
                                             covered[static_cast<std::size_t>( ny ) * size + nx] == 0;
                        rasterMiss[texel] = outside ? 1 : 0;
                    }
            }
        for ( std::size_t texel = 0; texel < texels; ++texel )
            if ( rasterMiss[texel] != 0 )
                covered[texel] = 0;

        // The emissive's scale first (UE FFlattenMaterial::EmissiveScale): the largest channel any drawn texel
        // holds.
        for ( std::size_t texel = 0; texel < texels; ++texel )
            if ( drawn[texel] != 0 )
            {
                const glm::vec4 e = HalfTexel( emissive, texel );
                out.EmissiveScale = std::max( { out.EmissiveScale, e.r, e.g, e.b } );
            }

        for ( std::size_t texel = 0; texel < texels; ++texel )
        {
            if ( covered[texel] == 0 )
                continue;
            ++out.CoveredTexels;
            const uint8_t mask = drawn[texel] != 0 ? 255 : 0;
            for ( int c = 0; c < 3; ++c )
                out.OpacityMask[texel * 4 + c] = mask;
            out.OpacityMask[texel * 4 + 3] = 255;
            if ( drawn[texel] == 0 )
                continue;
            ++out.OpaqueTexels;
            for ( int c = 0; c < 3; ++c )
                out.BaseColor[texel * 4 + c] =
                     UnitToByte( LinearToSrgb( static_cast<float>( albedo[texel * 4 + c] ) / 255.0f ) );
            out.BaseColor[texel * 4 + 3] = 255;
            const glm::vec4 b            = HalfTexel( gbufferB, texel );
            const glm::vec4 e            = HalfTexel( emissive, texel );
            out.ORM[texel * 4 + 0]       = UnitToByte( e.a );     // material AO
            out.ORM[texel * 4 + 1]       = UnitToByte( b.a );     // roughness
            out.ORM[texel * 4 + 2]       = albedo[texel * 4 + 3]; // metallic: G-buffer A alpha, linear UNORM
            out.ORM[texel * 4 + 3]       = 255;
            const float scale            = out.EmissiveScale > 0.0f ? out.EmissiveScale : 1.0f;
            out.Emissive[texel * 4 + 0]  = UnitToByte( LinearToSrgb( e.r / scale ) );
            out.Emissive[texel * 4 + 1]  = UnitToByte( LinearToSrgb( e.g / scale ) );
            out.Emissive[texel * 4 + 2]  = UnitToByte( LinearToSrgb( e.b / scale ) );
            out.Emissive[texel * 4 + 3]  = 255;
        }
        if ( out.CoveredTexels == 0 )
            return Common::MakeError( std::format( "material slot {} covers no texel of a {}x{} bake (its UVs lie "
                                                   "outside the unit square or are degenerate)",
                                                   m_Request.Slot, size, size ) );

        // Dilation: the drawn properties grow out of their islands, the mask out of the islands' coverage (a
        // masked-out texel inside an island stays 0).
        for ( std::vector<uint8_t>* image : { &out.BaseColor, &out.Normal, &out.ORM, &out.Emissive } )
            DilateIslands( *image, drawn, size, size );
        DilateIslands( out.OpacityMask, covered, size, size );
        m_Result = std::move( out );
        return BOOLSUCCESS;
    }

    namespace
    {
        constexpr std::array<const char*, 5> kPropertyNames = { "BaseColor", "Normal", "ORM", "Emissive",
                                                                "OpacityMask" };

        struct SelectionBake
        {
            std::unique_ptr<MaterialBake> Bake;
        };

        SelectionBake& Running()
        {
            static SelectionBake running;
            return running;
        }
    } // namespace

    Common::BoolResultStr StartBakeOfSelection( ::Desert::Core::Scene& scene, const int slot, const uint32_t size )
    {
        if ( Running().Bake )
            return Common::MakeError( "Bake Material to Textures: a bake is already running" );
        const auto& selection = ::Desert::Editor::Core::SelectionManager::GetSelection();
        if ( selection.size() != 1 )
            return Common::MakeError(
                 std::format( "Bake Material to Textures: select one entity, not {}", selection.size() ) );
        auto ref = scene.FindEntityByID( selection.front() );
        if ( !ref )
            return Common::MakeError( "Bake Material to Textures: the selected entity is not in the scene" );
        const ECS::Entity e = ref->get();
        if ( !e.HasComponent<ECS::StaticMeshComponent>() )
            return Common::MakeError( "Bake Material to Textures: the selected entity has no static mesh" );
        const std::string name =
             e.HasComponent<ECS::TagComponent>() ? e.GetComponent<ECS::TagComponent>().Tag : std::string( "Mesh" );
        const auto& component = e.GetComponent<ECS::StaticMeshComponent>();
        const auto& slots     = component.GetMaterialSlots();
        if ( slot < 0 || static_cast<std::size_t>( slot ) >= slots.size() || slots[slot].IsNull() )
            return Common::MakeError(
                 std::format( "Bake Material to Textures: '{}' has no material in slot {} ({} "
                              "slots)",
                              name, slot, slots.size() ) );
        auto mesh = GetToolTargetMesh( component );
        if ( !mesh.IsSuccess() || !mesh.GetValue().Mesh )
            return Common::MakeError(
                 std::format( "Bake Material to Textures: '{}': {}", name,
                              mesh.IsSuccess() ? std::string( "no mesh" ) : mesh.GetError() ) );
        auto render = Geometry::ToRenderMesh( *mesh.GetValue().Mesh );
        if ( !render.IsSuccess() )
            return Common::MakeError(
                 std::format( "Bake Material to Textures: '{}': {}", name, render.GetError() ) );

        // The slot each submesh DRAWS with (SectionMaterialSlot: a slot list shorter than the mesh's material ids
        // gives every remaining one its last slot), so the bake takes every triangle the renderer shades with it.
        Geometry::RenderMeshData source = render.ExtractValue();
        for ( int& id : source.SubmeshMaterialIds )
            id = static_cast<int>(
                 ::Desert::SectionMaterialSlot( static_cast<std::size_t>( std::max( id, 0 ) ), slots.size() ) );
        MaterialBakeRequest request;
        request.Mesh     = std::move( source );
        request.Slot     = slot;
        request.Material = slots[slot];
        request.Size     = size;
        request.Name     = std::format( "T_{}_{}", FileStem( name ), slot );
        Running().Bake   = std::make_unique<MaterialBake>( std::move( request ) );
        LOG_INFO( "Bake Material to Textures: '{}' slot {} at {}x{} started", name, slot, size, size );
        return BOOLSUCCESS;
    }

    void TickBakeOfSelection( Assets::AssetManager& assets )
    {
        SelectionBake& running = Running();
        if ( !running.Bake || !running.Bake->Tick() )
            return;
        const std::unique_ptr<MaterialBake> bake = std::move( running.Bake );
        if ( bake->Error() )
        {
            LOG_WARN( "Bake Material to Textures refused: {}", *bake->Error() );
            return;
        }
        const FlattenedMaterial&                         flat    = *bake->Result();
        const std::array<const std::vector<uint8_t>*, 5> images  = { &flat.BaseColor, &flat.Normal, &flat.ORM,
                                                                     &flat.Emissive, &flat.OpacityMask };
        const std::array<Fmt::TextureIntent, 5>          intents = {
             Fmt::TextureIntent::Colour, Fmt::TextureIntent::NormalMap, Fmt::TextureIntent::PackedMasks,
             Fmt::TextureIntent::Colour, Fmt::TextureIntent::Mask };
        for ( std::size_t p = 0; p < images.size(); ++p )
        {
            const std::string name    = std::format( "{}_{}", bake->Request().Name, kPropertyNames[p] );
            auto              written = WriteTexture( assets, flat.Size, *images[p], name, intents[p] );
            if ( !written )
            {
                LOG_WARN( "Bake Material to Textures: {}", written.GetError() );
                return;
            }
            LOG_INFO( "Bake Material to Textures: {}", written.GetValue().generic_string() );
        }
        LOG_INFO( "Bake Material to Textures: {} of {} island texels kept by the mask, emissive scale {}",
                  flat.OpaqueTexels, flat.CoveredTexels, flat.EmissiveScale );
    }

    bool IsBakeOfSelectionRunning()
    {
        return Running().Bake != nullptr;
    }
} // namespace Desert::Editor::Baking
