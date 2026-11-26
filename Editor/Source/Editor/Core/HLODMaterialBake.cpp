#include "HLODMaterialBake.hpp"

#include "HLODBakeWriter.hpp"

#include <Engine/Core/Camera.hpp>
#include <Engine/Core/CameraEntityView.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldSystems.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Geometry/RenderMeshData.hpp>
#include <Engine/Geometry/TriangleBvh.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>

#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Timestep.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <utility>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Common/Json/Detail/RflBridge.hpp>

namespace Desert::Editor
{
    namespace
    {
        // Frames recorded before the G-buffer is read: the source materials and their textures are resolved while
        // the scene draws (the landscape bake and AssetThumbnailRenderer warm the same way).
        constexpr int kHLODMaterialBakeWarmFrames = 60;
        // The camera hangs this high above the proxy (which lies at y = 0) and sees this deep.
        constexpr float kHLODMaterialBakeEyeHeightCm = 1.0e4f;
        constexpr float kHLODMaterialBakeNearCm      = 100.0f;
        constexpr float kHLODMaterialBakeFarCm       = 2.0e4f;
        // MeshSimplify's projection reach, as a share of the source's bounding diagonal: a simplified texel looks
        // for the source surface this far out along its normal and as far in (UE ProjectionDistance; a simplified
        // HLOD strays from its source by a small share of the cell's own size).
        constexpr double kProjectionShareOfDiagonal = 0.05;

        // A source texel's material, as the readback holds it (linear values).
        struct SourceTexel
        {
            glm::vec3 BaseColor{ 0.0f }; // linear
            float     Metallic  = 0.0f;
            float     Roughness = 0.0f;
            float     AO        = 1.0f;
            glm::vec3 Normal{ 0.0f }; // world
            glm::vec3 Emissive{ 0.0f };
        };

        SourceTexel ReadTexel( const HLODBake::GBufferTexels& texels, std::size_t texel )
        {
            SourceTexel out;
            for ( int c = 0; c < 3; ++c )
                out.BaseColor[c] =
                     static_cast<float>( texels.A[texel * 4 + static_cast<std::size_t>( c )] ) / 255.0f;
            out.Metallic  = static_cast<float>( texels.A[texel * 4 + 3] ) / 255.0f;
            out.Roughness = HLODBake::Roughness( texels, texel );
            out.AO        = HLODBake::MaterialAO( texels, texel );
            out.Normal    = HLODBake::WorldNormal( texels, texel );
            out.Emissive  = HLODBake::EmissiveRadiance( texels, texel );
            return out;
        }

        SourceTexel Blend( const SourceTexel& a, const SourceTexel& b, float t )
        {
            SourceTexel out;
            out.BaseColor = glm::mix( a.BaseColor, b.BaseColor, t );
            out.Metallic  = glm::mix( a.Metallic, b.Metallic, t );
            out.Roughness = glm::mix( a.Roughness, b.Roughness, t );
            out.AO        = glm::mix( a.AO, b.AO, t );
            out.Normal    = glm::mix( a.Normal, b.Normal, t );
            out.Emissive  = glm::mix( a.Emissive, b.Emissive, t );
            return out;
        }

        // The images one material is written from: per output texel its value and whether a surface gave it one.
        struct OutputTexels
        {
            std::vector<SourceTexel> Values;
            std::vector<uint8_t>     Covered; // 1 = the texel has a value
            std::vector<uint8_t>     Normal;  // RGBA8 tangent space, flat where none
            std::vector<uint8_t>     NormalCovered;
        };

        HLODBake::BakedImages ToImages( OutputTexels                                       out,
                                        const ::Desert::Core::HLODMaterialMergeSerialized& settings,
                                        uint32_t width, uint32_t height )
        {
            const std::size_t     count = static_cast<std::size_t>( width ) * height;
            HLODBake::BakedImages images;
            images.Width  = width;
            images.Height = height;
            // Uncovered texels take the nearest covered value (UE's UV border smear) before anything is encoded.
            const std::vector<std::size_t> nearest = HLODBake::NearestCovered( out.Covered, width, height );
            const auto                     valueAt = [&]( std::size_t i ) -> const SourceTexel&
            { return nearest.empty() ? out.Values[i] : out.Values[nearest[i]]; };
            images.BaseColor.resize( count * 4 );
            for ( std::size_t i = 0; i < count; ++i )
            {
                const SourceTexel& v = valueAt( i );
                for ( std::size_t c = 0; c < 3; ++c )
                    images.BaseColor[i * 4 + c] = HLODBake::LinearToSrgb8( v.BaseColor[static_cast<int>( c )] );
                images.BaseColor[i * 4 + 3] = 255;
            }
            if ( settings.ORMMap )
            {
                std::vector<uint8_t> orm( count * 4 );
                for ( std::size_t i = 0; i < count; ++i )
                {
                    const SourceTexel& v = valueAt( i );
                    orm[i * 4 + 0]       = HLODBake::UnitToByte( v.AO );
                    orm[i * 4 + 1]       = HLODBake::UnitToByte( v.Roughness );
                    orm[i * 4 + 2]       = HLODBake::UnitToByte( v.Metallic );
                    orm[i * 4 + 3]       = 255;
                }
                images.ORM = std::move( orm );
            }
            if ( settings.NormalMap )
            {
                HLODBake::SmearUncovered( out.Normal, out.NormalCovered, width, height );
                images.Normal = std::move( out.Normal );
            }
            if ( settings.EmissiveMap )
            {
                // UE FFlattenMaterial::EmissiveScale: the texture holds radiance / the brightest channel.
                float brightest = 0.0f;
                for ( std::size_t i = 0; i < count; ++i )
                {
                    const glm::vec3& e = valueAt( i ).Emissive;
                    brightest          = std::max( { brightest, e.r, e.g, e.b } );
                }
                const float          scale = brightest > 0.0f ? brightest : 1.0f;
                std::vector<uint8_t> emissive( count * 4 );
                for ( std::size_t i = 0; i < count; ++i )
                {
                    const glm::vec3 e = valueAt( i ).Emissive / scale;
                    for ( int c = 0; c < 3; ++c )
                        emissive[i * 4 + static_cast<std::size_t>( c )] = HLODBake::LinearToSrgb8( e[c] );
                    emissive[i * 4 + 3] = 255;
                }
                images.Emissive      = std::move( emissive );
                images.EmissiveScale = scale;
            }
            return images;
        }
    } // namespace

    HLODMaterialBake::HLODMaterialBake( std::filesystem::path scenePath, std::string sceneText,
                                        std::vector<HLODMaterialBakeJob> jobs, const Assets::AssetManager* assets )
         : m_ScenePath( std::move( scenePath ) ), m_SceneText( std::move( sceneText ) ),
           m_Jobs( std::move( jobs ) ), m_Assets( assets )
    {
        if ( m_Jobs.empty() )
            m_Phase = Phase::Done;
    }

    HLODMaterialBake::~HLODMaterialBake()
    {
        m_A.reset();
        m_B.reset();
        m_Emissive.reset();
        if ( !m_Renderer )
            return;
        Graphic::Renderer::GetInstance().WaitDeviceIdle();
        m_Scene.reset();
        m_Renderer.reset();
    }

    void HLODMaterialBake::Fail( std::string error )
    {
        m_Error = std::move( error );
        m_Phase = Phase::Done;
    }

    Common::BoolResultStr HLODMaterialBake::StartJob()
    {
        const HLODMaterialBakeJob& job = m_Jobs[m_Next];
        if ( m_Scene )
        {
            // The previous job's scene goes once the GPU no longer reads it.
            Graphic::Renderer::GetInstance().WaitDeviceIdle();
            m_Scene.reset();
        }
        auto parsed = ::Desert::Core::ParseLoadableScene( m_ScenePath.string(), m_SceneText );
        if ( !parsed )
            return Common::MakeError( "the saved scene does not parse: " + parsed.GetError() );
        ::Desert::Core::LoadableScene loaded = parsed.ExtractValue();
        // Only the proxy is drawn: the world's own records would stand between the camera and it.
        loaded.Scene.Entities.clear();
        loaded.Scene.WorldPartition.reset();
        Assets::EntityData proxy;
        proxy.id  = Common::UUID( static_cast<std::uint64_t>( m_Next + 1 ) );
        proxy.Tag = std::format( "HLOD bake proxy {}", job.Source.Key );
        proxy.Components[std::string( ::Desert::Core::Rules::kPrimitiveComponent )] =
             Common::Json::FromStruct( job.Source.Proxy );
        loaded.Scene.Entities.push_back( std::move( proxy ) );

        m_Scene = std::make_shared<::Desert::Core::Scene>( "HLODMaterialBake", m_Renderer.get(),
                                                           ::Desert::Core::WorldKind::Capture );
        // Without the render collectors the static mesh queue stays empty and every job bakes the clear colour.
        if ( auto made = ::Desert::Core::CreateWorldSystems( *m_Scene, {} ); !made )
            return Common::MakeError( made.GetError() );
        if ( auto inited = m_Scene->Init( ::Desert::Core::Scene::InitPurpose::Offscreen ); !inited )
            return Common::MakeError( "the bake scene failed to initialise: " + inited.GetError() );
        auto deserialized = ::Desert::Core::SceneSerializer( m_Scene.get(), m_Assets )
                                 .Deserialize( std::move( loaded ), m_ScenePath.generic_string() );
        if ( !deserialized )
            return Common::MakeError( "the proxy did not load into the bake scene: " + deserialized.GetError() );
        if ( !m_Camera )
        {
            m_Camera = std::make_shared<::Desert::Core::GameplayCamera>();
            m_Camera->SetProjectionType( ::Desert::Core::ProjectionType::Orthographic );
        }

        // Straight down over the atlas square, image up = world -Z, image right = world +X: the readback's row 0
        // is atlas v = 0, column 0 atlas u = 0 (HLODBakeWriter.hpp's layout), as the landscape bake frames a tile.
        const auto side = static_cast<uint32_t>( job.Source.Settings.TextureSize );
        m_Scene->Resize( side, side );
        glm::mat4 world( 1.0f );
        world[0] = glm::vec4( 1.0f, 0.0f, 0.0f, 0.0f );  // right
        world[1] = glm::vec4( 0.0f, 0.0f, -1.0f, 0.0f ); // up
        world[2] = glm::vec4( 0.0f, 1.0f, 0.0f, 0.0f );  // backward (the camera looks down -Y)
        world[3] = glm::vec4( ::Desert::Core::kHLODBakeProxySideCm * 0.5f, kHLODMaterialBakeEyeHeightCm,
                              ::Desert::Core::kHLODBakeProxySideCm * 0.5f, 1.0f );
        m_Camera->SetOrthoSize( ::Desert::Core::kHLODBakeProxySideCm * 0.5f );
        m_Camera->SetView(
             ::Desert::Core::CameraEntityViewOf( world, 45.0f, kHLODMaterialBakeNearCm, kHLODMaterialBakeFarCm ),
             side, side, ::Desert::Core::Camera::kNoSourceEntity );
        m_Scene->PinActiveCamera( m_Camera );
        m_WarmFrames = kHLODMaterialBakeWarmFrames;
        m_Phase      = Phase::Frame;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr HLODMaterialBake::SaveJob( std::vector<uint8_t> a, std::vector<uint8_t> b,
                                                     std::vector<uint8_t> emissive, uint32_t width,
                                                     uint32_t height )
    {
        const HLODMaterialBakeJob&    job = m_Jobs[m_Next];
        const HLODBake::GBufferTexels source{ std::move( a ), std::move( b ), std::move( emissive ), width,
                                              height };
        if ( auto sizes = HLODBake::CheckSizes( source ); !sizes )
            return sizes;
        if ( !job.Mesh.EditMesh )
            return Common::MakeError( "the built HLOD carries no mesh" );
        // The frame the runtime shades the built HLOD with: the same conversion ComponentRegistry's StaticMesh
        // load makes from the EditMesh.
        auto dynamicMesh = ::Desert::Geometry::DynamicMeshFromSerialized( *job.Mesh.EditMesh, "HLOD" );
        if ( !dynamicMesh )
            return Common::MakeError( "the built HLOD's mesh did not load: " + dynamicMesh.GetError() );
        auto renderMesh = ::Desert::Geometry::ToRenderMesh( dynamicMesh.GetValue() );
        if ( !renderMesh )
            return Common::MakeError( "the built HLOD's mesh has no render form: " + renderMesh.GetError() );
        const ::Desert::Geometry::RenderMeshData& mesh = renderMesh.GetValue();

        const std::size_t count = static_cast<std::size_t>( width ) * height;
        OutputTexels      out;
        out.Values.resize( count );
        out.Covered.assign( count, 0 );
        out.Normal = HLODBake::FlatNormals( width, height );
        out.NormalCovered.assign( count, 0 );

        if ( job.Source.Source.Triangles.empty() )
        {
            // MeshMerge: the built mesh is the atlas the proxy was laid out on - the readback texel for texel.
            for ( std::size_t i = 0; i < count; ++i )
                if ( HLODBake::Covered( source, i ) )
                {
                    out.Values[i]  = ReadTexel( source, i );
                    out.Covered[i] = 1;
                }
            HLODBake::RasterizeUV(
                 mesh, width, height,
                 [&]( std::size_t texel, const glm::vec3& weights,
                      const std::array<const ::Desert::Vertex*, 3>& v )
                 {
                     if ( out.Covered[texel] == 0 )
                         return;
                     if ( HLODBake::EncodeTangentNormal( out.Values[texel].Normal,
                                                         HLODBake::InterpolatedFrame( weights, v ),
                                                         &out.Normal[texel * 4] ) )
                         out.NormalCovered[texel] = 1;
                 } );
        }
        else
        {
            // MeshSimplify: every texel of the simplified mesh's own atlas, projected along its normal onto the
            // source surface, takes the flattened source there.
            const ::Desert::Geometry::BakeSurface& surface = job.Source.Source;
            const ::Desert::Geometry::TriangleBvh  bvh =
                 ::Desert::Geometry::TriangleBvh::Build( surface.Positions, surface.Triangles );
            glm::dvec3 lo( std::numeric_limits<double>::max() );
            glm::dvec3 hi( std::numeric_limits<double>::lowest() );
            for ( const glm::dvec3& p : surface.Positions )
            {
                lo = glm::min( lo, p );
                hi = glm::max( hi, p );
            }
            const double reach = std::max( 1.0, glm::length( hi - lo ) * kProjectionShareOfDiagonal );
            // The flattened source with its own empty texels smeared, sampled bilinearly at a source atlas UV.
            std::vector<uint8_t> sourceCovered( count, 0 );
            for ( std::size_t i = 0; i < count; ++i )
                sourceCovered[i] = HLODBake::Covered( source, i ) ? 1 : 0;
            const std::vector<std::size_t> sourceNearest =
                 HLODBake::NearestCovered( sourceCovered, width, height );
            if ( sourceNearest.empty() )
                return Common::MakeError(
                     "the flattened source holds no texel: no source material drew on its proxy" );
            const auto fetch = [&]( long c, long r )
            {
                c = std::clamp( c, 0L, static_cast<long>( width ) - 1 );
                r = std::clamp( r, 0L, static_cast<long>( height ) - 1 );
                return ReadTexel(
                     source,
                     sourceNearest[static_cast<std::size_t>( r ) * width + static_cast<std::size_t>( c )] );
            };
            const auto sample = [&]( const glm::dvec2& uv )
            {
                const double x  = uv.x * width - 0.5;
                const double y  = uv.y * height - 0.5;
                const double fx = std::floor( x );
                const double fy = std::floor( y );
                const auto   c  = static_cast<long>( fx );
                const auto   r  = static_cast<long>( fy );
                const auto   tx = static_cast<float>( x - fx );
                const auto   ty = static_cast<float>( y - fy );
                return Blend( Blend( fetch( c, r ), fetch( c + 1, r ), tx ),
                              Blend( fetch( c, r + 1 ), fetch( c + 1, r + 1 ), tx ), ty );
            };
            // The texels the simplified mesh covers (the last triangle over a texel wins, as BakeMeshMaps' pass
            // 1).
            struct Sample
            {
                bool                                   Set = false;
                glm::vec3                              Weights{ 0.0f };
                std::array<const ::Desert::Vertex*, 3> Corners{};
            };
            std::vector<Sample> samples( count );
            HLODBake::RasterizeUV( mesh, width, height,
                                   [&]( std::size_t texel, const glm::vec3& weights,
                                        const std::array<const ::Desert::Vertex*, 3>& v )
                                   { samples[texel] = Sample{ true, weights, v }; } );
            const auto evaluateRow = [&]( std::size_t row )
            {
                for ( std::size_t col = 0; col < width; ++col )
                {
                    const std::size_t texel = row * width + col;
                    const Sample&     s     = samples[texel];
                    if ( !s.Set )
                        continue;
                    glm::dvec3 p( 0.0 );
                    for ( int c = 0; c < 3; ++c )
                        p += static_cast<double>( s.Weights[c] ) *
                             glm::dvec3( s.Corners[static_cast<std::size_t>( c )]->Position );
                    const glm::mat3  frame = HLODBake::InterpolatedFrame( s.Weights, s.Corners );
                    const glm::dvec3 n     = glm::dvec3( frame[2] );
                    if ( glm::length( n ) < 1e-12 )
                        continue;
                    // UE GetDetailMeshTrianglePoint_Raycast: from the point pushed out along its normal by the
                    // reach, back along -normal across twice the reach, nearest hit.
                    const glm::dvec3 dir    = -glm::normalize( n );
                    const glm::dvec3 origin = p - dir * reach;
                    const double     o[3]   = { origin.x, origin.y, origin.z };
                    const double     d[3]   = { dir.x, dir.y, dir.z };
                    const auto       hit    = bvh.ClosestHit( o, d, 0.0, 2.0 * reach );
                    if ( hit.Triangle < 0 )
                        continue;
                    const auto        ht  = static_cast<std::size_t>( hit.Triangle );
                    const glm::uvec3& tri = surface.Triangles[ht];
                    // Barycentrics of the hit in its triangle (A, A + E1, A + E2).
                    const glm::dvec3 rel   = origin + dir * hit.T - bvh.A[ht];
                    const double     d00   = glm::dot( bvh.E1[ht], bvh.E1[ht] );
                    const double     d01   = glm::dot( bvh.E1[ht], bvh.E2[ht] );
                    const double     d11   = glm::dot( bvh.E2[ht], bvh.E2[ht] );
                    const double     d20   = glm::dot( rel, bvh.E1[ht] );
                    const double     d21   = glm::dot( rel, bvh.E2[ht] );
                    const double     denom = d00 * d11 - d01 * d01;
                    if ( std::abs( denom ) < 1e-18 )
                        continue;
                    const double     b1 = ( d11 * d20 - d01 * d21 ) / denom;
                    const double     b2 = ( d00 * d21 - d01 * d20 ) / denom;
                    const glm::dvec2 uv = surface.UVs[tri[0]] * ( 1.0 - b1 - b2 ) + surface.UVs[tri[1]] * b1 +
                                          surface.UVs[tri[2]] * b2;
                    out.Values[texel]  = sample( uv );
                    out.Covered[texel] = 1;
                    if ( HLODBake::EncodeTangentNormal( out.Values[texel].Normal, frame, &out.Normal[texel * 4] ) )
                        out.NormalCovered[texel] = 1;
                }
            };
            Common::JobSystem::Get().ParallelFor( static_cast<std::size_t>( height ), evaluateRow );
            if ( std::none_of( out.Covered.begin(), out.Covered.end(), []( uint8_t c ) { return c != 0; } ) )
                return Common::MakeError( std::format(
                     "no texel of the simplified mesh found the source surface within {:.1f} cm along its normal",
                     reach ) );
        }

        auto material =
             HLODBake::WriteBakedMaterial( m_ScenePath, "HLOD_" + job.Source.Key,
                                           ToImages( std::move( out ), job.Source.Settings, width, height ) );
        if ( !material )
            return Common::MakeError( material.GetError() );
        HLODMaterialBaked baked;
        baked.Key                = job.Source.Key;
        baked.Mesh               = job.Mesh;
        baked.Mesh.MaterialSlots = std::vector<::Desert::Assets::AssetGuidRef>{
             ::Desert::Assets::AssetGuidRef{ material.GetValue().Guid, material.GetValue().Path } };
        m_Baked.push_back( std::move( baked ) );
        return BOOLSUCCESS;
    }

    bool HLODMaterialBake::Tick()
    {
        switch ( m_Phase )
        {
            case Phase::Build:
            {
                if ( !m_Renderer )
                {
                    const auto side = static_cast<uint32_t>( m_Jobs.front().Source.Settings.TextureSize );
                    m_Renderer      = Graphic::SceneRenderer::Create( Graphic::ViewExtent{ side, side },
                                                                      Graphic::kThumbnailViewProfile );
                }
                if ( !m_Renderer->AdvanceRendererBuild() )
                    return false;
                if ( auto started = StartJob(); !started )
                {
                    Fail( std::format( "HLOD {}: {}", m_Jobs[m_Next].Source.Key, started.GetError() ) );
                    return true;
                }
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
                // The previous frame's render is on the queue; the copies are recorded behind it (no wait).
                // Attachments in GBufferOutputs.glslh's order: 0 BaseColor + Metallic, 1 normal + roughness,
                // 3 emissive + material AO.
                const auto& gbuffer  = m_Renderer->GetGBuffer();
                const auto  aTarget  = gbuffer ? gbuffer->GetColorAttachmentImage( 0 ) : nullptr;
                const auto  bTarget  = gbuffer ? gbuffer->GetColorAttachmentImage( 1 ) : nullptr;
                const auto  emissive = gbuffer ? gbuffer->GetColorAttachmentImage( 3 ) : nullptr;
                if ( !aTarget || !bTarget || !emissive )
                {
                    Fail( "the bake renderer has no G-buffer A / B / emissive target (is the scene's rendering "
                          "path "
                          "Deferred?)" );
                    return true;
                }
                auto aCopy        = aTarget->BeginReadbackRGBA8();
                auto bCopy        = bTarget->BeginReadbackBytes();
                auto emissiveCopy = emissive->BeginReadbackBytes();
                if ( !aCopy || !bCopy || !emissiveCopy )
                {
                    Fail(
                         "a G-buffer readback was refused: " +
                         ( !aCopy ? aCopy.GetError() : ( !bCopy ? bCopy.GetError() : emissiveCopy.GetError() ) ) );
                    return true;
                }
                m_A              = aCopy.GetValue();
                m_B              = bCopy.GetValue();
                m_Emissive       = emissiveCopy.GetValue();
                m_ReadbackWidth  = aTarget->GetWidth();
                m_ReadbackHeight = aTarget->GetHeight();
                m_Phase          = Phase::Readback;
                return false;
            }
            case Phase::Readback:
            {
                if ( !m_A->IsComplete() || !m_B->IsComplete() || !m_Emissive->IsComplete() )
                    return false;
                auto aBytes = m_A->ReadRGBA8();
                auto bBytes = m_B->ReadBytes();
                auto eBytes = m_Emissive->ReadBytes();
                m_A.reset();
                m_B.reset();
                m_Emissive.reset();
                if ( !aBytes || !bBytes || !eBytes )
                {
                    Fail( "a G-buffer readback failed: " +
                          ( !aBytes ? aBytes.GetError() : ( !bBytes ? bBytes.GetError() : eBytes.GetError() ) ) );
                    return true;
                }
                if ( auto saved = SaveJob( aBytes.ExtractValue(), bBytes.ExtractValue(), eBytes.ExtractValue(),
                                           m_ReadbackWidth, m_ReadbackHeight );
                     !saved )
                {
                    Fail( std::format( "HLOD {}: {}", m_Jobs[m_Next].Source.Key, saved.GetError() ) );
                    return true;
                }
                if ( ++m_Next >= m_Jobs.size() )
                {
                    m_Phase = Phase::Done;
                    return true;
                }
                if ( auto started = StartJob(); !started )
                {
                    Fail( std::format( "HLOD {}: {}", m_Jobs[m_Next].Source.Key, started.GetError() ) );
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
