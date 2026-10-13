#include "ReflectionCaptureRenderer.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/SceneRendererFrame.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <span>

namespace Desert::Graphic
{
    namespace
    {
        constexpr uint32_t kGroupSize = 8u;

        // THE SIX FACE VIEWS, in the order the assemble shader names them (u_FacePosX .. u_FaceNegZ). The up
        // vectors are any that are not parallel to the axis: the assemble projects through each view's own matrix,
        // so no cube-face convention is encoded here.
        constexpr std::array<glm::vec3, 6>        kFaceForward = { glm::vec3( 1, 0, 0 ), glm::vec3( -1, 0, 0 ),
                                                                   glm::vec3( 0, 1, 0 ), glm::vec3( 0, -1, 0 ),
                                                                   glm::vec3( 0, 0, 1 ), glm::vec3( 0, 0, -1 ) };
        constexpr std::array<glm::vec3, 6>        kFaceUp      = { glm::vec3( 0, 1, 0 ), glm::vec3( 0, 1, 0 ),
                                                                   glm::vec3( 0, 0, 1 ), glm::vec3( 0, 0, -1 ),
                                                                   glm::vec3( 0, 1, 0 ), glm::vec3( 0, 1, 0 ) };
        constexpr std::array<std::string_view, 6> kFaceNames   = { "u_FacePosX", "u_FaceNegX", "u_FacePosY",
                                                                   "u_FaceNegY", "u_FacePosZ", "u_FaceNegZ" };

        // A capture is written into the array every frame its face views render; it stops being re-rendered once
        // its face views have held one sky environment for this many consecutive frames. The face views bake their
        // own sky (asynchronously, SkyboxRenderer) and their clouds reproject a history: the first frames of a
        // freshly aimed face view are the unlit, cloudless picture, and a capture frozen there would keep it.
        constexpr uint32_t kCaptureSettleFrames = 8u;

        struct FacesBlock
        {
            std::array<glm::mat4, 6> ViewProjection;
            glm::vec4                Probe; // x = the distance a direction is projected at
        };

        struct FilterPush
        {
            float    Roughness  = 0.0f;
            uint32_t FirstLayer = 0u;
        };

        [[nodiscard]] uint32_t GroupCount( uint32_t texels )
        {
            return ( std::max( texels, 1u ) + kGroupSize - 1u ) / kGroupSize;
        }

        void HashBytes( uint64_t& hash, const void* data, size_t bytes )
        {
            const auto* p = static_cast<const unsigned char*>( data );
            for ( size_t i = 0; i < bytes; ++i )
            {
                hash ^= p[i];
                hash *= 1099511628211ull;
            }
        }

        [[nodiscard]] std::shared_ptr<ComputePipeline> MakeCompute( std::string_view name )
        {
            const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( std::string( name ) );
            if ( !shader )
            {
                LOG_ERROR( "[ReflectionCaptures] missing compute shader '{}'", name );
                return nullptr;
            }
            const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = std::string( name ) } );
            if ( !built )
            {
                LOG_ERROR( "[ReflectionCaptures] {}: {}", name, built.GetError() );
                return nullptr;
            }
            return built.GetValue();
        }
    } // namespace

    std::vector<ReflectionCaptureEntry> GatherReflectionCaptures( const entt::registry& registry )
    {
        std::vector<ReflectionCaptureEntry> captures;
        const auto view = registry.view<const ECS::ReflectionCaptureComponent, const ECS::TransformComponent>();
        for ( const entt::entity entity : view )
        {
            if ( ECS::IsHidden( registry, entity ) )
                continue;
            const ECS::ReflectionCaptureData& data =
                 view.get<const ECS::ReflectionCaptureComponent>( entity ).Data;
            const ECS::TransformComponent& transform = view.get<const ECS::TransformComponent>( entity );

            const bool      box        = data.Shape == ECS::ReflectionCaptureShape::Box;
            const glm::vec3 extent     = glm::max( data.BoxExtent, glm::vec3( 1.0f ) );
            const float     radius     = box ? glm::length( extent ) : std::max( data.InfluenceRadius, 1.0f );
            const glm::mat4 boxToWorld = glm::translate( glm::mat4( 1.0f ), transform.Translation ) *
                                         glm::toMat4( glm::quat( transform.Rotation ) ) *
                                         glm::scale( glm::mat4( 1.0f ), extent );

            ReflectionCaptureEntry entry;
            entry.Entity                      = entity;
            entry.InfluenceRadius             = radius;
            entry.CapturePosition             = transform.Translation + data.CaptureOffset;
            entry.Gpu.PositionRadius          = glm::vec4( transform.Translation, radius );
            entry.Gpu.CaptureOffsetBrightness = glm::vec4( data.CaptureOffset, std::max( data.Brightness, 0.0f ) );
            entry.Gpu.BoxExtentTransition     = glm::vec4( extent, std::max( data.BoxTransitionDistance, 0.0f ) );
            entry.Gpu.ShapeCube               = glm::uvec4( box ? 1u : 0u, 0u, 0u, 0u );
            entry.Gpu.WorldToBox              = glm::inverse( boxToWorld );
            captures.push_back( entry );
        }
        // The cube index is the place in ENTITY order: stable while the set of captures is.
        std::sort( captures.begin(), captures.end(),
                   []( const ReflectionCaptureEntry& a, const ReflectionCaptureEntry& b )
                   { return entt::to_integral( a.Entity ) < entt::to_integral( b.Entity ); } );
        for ( size_t i = 0; i < captures.size(); ++i )
            captures[i].Gpu.ShapeCube.y = static_cast<uint32_t>( i );
        return captures;
    }

    std::vector<std::byte> PackReflectionCaptures( const std::vector<ReflectionCaptureEntry>& captures,
                                                   const std::vector<bool>&                   captured )
    {
        std::vector<const ReflectionCaptureEntry*> readable;
        for ( const ReflectionCaptureEntry& capture : captures )
            if ( capture.Gpu.ShapeCube.y < captured.size() && captured[capture.Gpu.ShapeCube.y] )
                readable.push_back( &capture );
        // Smallest influence first: the shader composites front to back (UE sorts its captures the same way).
        std::stable_sort( readable.begin(), readable.end(),
                          []( const ReflectionCaptureEntry* a, const ReflectionCaptureEntry* b )
                          { return a->InfluenceRadius < b->InfluenceRadius; } );

        const glm::uvec4       header( static_cast<uint32_t>( readable.size() ), 0u, 0u, 0u );
        std::vector<std::byte> bytes( sizeof( header ) +
                                      sizeof( ReflectionCaptureGpu ) * std::max<size_t>( readable.size(), 1u ) );
        std::memcpy( bytes.data(), &header, sizeof( header ) );
        for ( size_t i = 0; i < readable.size(); ++i )
            std::memcpy( bytes.data() + sizeof( header ) + i * sizeof( ReflectionCaptureGpu ), &readable[i]->Gpu,
                         sizeof( ReflectionCaptureGpu ) );
        return bytes;
    }

    // One face view: a complete scene renderer and the camera it is opened with.
    struct ReflectionCaptureRenderer::FaceView
    {
        class Camera final : public Core::Camera
        {
        public:
            void Aim( const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up )
            {
                m_Position   = position;
                m_ViewMatrix = glm::lookAt( position, position + forward, up );
                SetFOV( 90.0f );
                UpdateProjectionMatrix( kReflectionCaptureFaceSize, kReflectionCaptureFaceSize );
            }
        };

        std::unique_ptr<SceneRenderer> Renderer;
        Camera                         View;
    };

    ReflectionCaptureRenderer::ReflectionCaptureRenderer( bool ownsCaptures ) : m_OwnsCaptures( ownsCaptures )
    {
    }

    ReflectionCaptureRenderer::~ReflectionCaptureRenderer() = default;

    bool ReflectionCaptureRenderer::EnsurePipelines()
    {
        if ( m_Assemble && m_Downsample && m_Filter )
            return true;
        if ( m_PipelinesFailed )
            return false;
        m_Assemble        = MakeCompute( "ReflectionCaptureAssemble" );
        m_Downsample      = MakeCompute( "ReflectionCaptureDownsample" );
        m_Filter          = MakeCompute( "ReflectionCaptureFilter" );
        m_PipelinesFailed = !( m_Assemble && m_Downsample && m_Filter );
        return !m_PipelinesFailed;
    }

    void ReflectionCaptureRenderer::RenderCaptureViews( const Core::Scene&                           scene,
                                                        const std::function<void( SceneRenderer& )>& record,
                                                        Common::Timestep timestep, Common::Timestep realTimestep,
                                                        const ShaderProtocols::DirectionLight& dirLights,
                                                        uint64_t                               environment )
    {
        m_FacesRenderedThisFrame = false;
        if ( !m_OwnsCaptures )
            return;

        std::vector<ReflectionCaptureEntry> captures  = GatherReflectionCaptures( scene.GetRegistry() );
        uint64_t                            signature = 14695981039346656037ull;
        HashBytes( signature, &environment, sizeof( environment ) );
        for ( const ReflectionCaptureEntry& capture : captures )
        {
            const auto id = entt::to_integral( capture.Entity );
            HashBytes( signature, &id, sizeof( id ) );
            HashBytes( signature, &capture.Gpu, sizeof( capture.Gpu ) );
        }
        if ( signature != m_Signature )
        {
            // UE recaptures every capture when any of them changes: a moved capture changes what its neighbours
            // see too, and a re-baked sky changes all of them.
            m_Captures  = std::move( captures );
            m_Signature = signature;
            m_Captured.assign( m_Captures.size(), false );
            m_Done.assign( m_Captures.size(), false );
            m_Pending = kNoCapture;
        }
        if ( m_Pending == kNoCapture )
        {
            const auto next = std::find( m_Done.begin(), m_Done.end(), false );
            if ( next == m_Done.end() )
                return;
            m_Pending            = static_cast<uint32_t>( next - m_Done.begin() );
            m_PendingSettled     = 0;
            m_PendingEnvironment = 0;
        }

        const ReflectionCaptureEntry&   capture = m_Captures[m_Pending];
        const SceneRenderer::UpdateInfo info{ timestep, realTimestep, dirLights };
        for ( size_t f = 0; f < m_Faces.size(); ++f )
        {
            if ( !m_Faces[f] )
            {
                m_Faces[f]           = std::make_unique<FaceView>();
                m_Faces[f]->Renderer = std::make_unique<SceneRenderer>(
                     ViewExtent{ kReflectionCaptureFaceSize, kReflectionCaptureFaceSize },
                     kReflectionCaptureViewProfile );
                m_Faces[f]->Renderer->Init();
                m_Faces[f]->Renderer->Resize( kReflectionCaptureFaceSize, kReflectionCaptureFaceSize );
            }
            FaceView& face = *m_Faces[f];
            face.View.Aim( capture.CapturePosition, kFaceForward[f], kFaceUp[f] );
            if ( const auto begun = face.Renderer->BeginScene( scene, &face.View ); !begun )
            {
                LOG_ERROR( "[ReflectionCaptures] capture {} face {}: the face view did not open: {}", m_Pending, f,
                           begun.GetError() );
                return;
            }
            record( *face.Renderer );
            face.Renderer->OnUpdate( info );
            if ( const auto ended = face.Renderer->EndScene(); !ended )
            {
                LOG_ERROR( "[ReflectionCaptures] capture {} face {}: the face view did not close: {}", m_Pending,
                           f, ended.GetError() );
                return;
            }
        }
        m_FacesRenderedThisFrame = true;

        // Settled once the face views have held ONE sky environment for kCaptureSettleFrames frames.
        uint64_t faceEnvironment = 0;
        if ( const auto env = m_Faces[0]->Renderer->GetEnvironment();
             env.has_value() && env->PreFilteredMap.IsValid() )
            faceEnvironment = ( static_cast<uint64_t>( env->PreFilteredMap.Value.Index ) << 32u ) |
                              env->PreFilteredMap.Value.Generation;
        if ( faceEnvironment != 0 && faceEnvironment == m_PendingEnvironment )
            ++m_PendingSettled;
        else
            m_PendingSettled = 0;
        m_PendingEnvironment = faceEnvironment;
    }

    void ReflectionCaptureRenderer::AddCapturePasses( RDG::Builder& graph, FrameTextures& textures,
                                                      RDG::TextureRef array, uint32_t cube, bool black )
    {
        // The six face views (or black, for the cubes of a fresh array nothing has captured yet).
        std::array<RDG::TextureRef, 6> faces;
        FacesBlock                     block{};
        block.Probe = glm::vec4( 100.0f, 0.0f, 0.0f, 0.0f );
        for ( size_t f = 0; f < faces.size(); ++f )
        {
            block.ViewProjection[f] = glm::mat4( 1.0f );
            faces[f]                = textures.System.Black;
            if ( black )
                continue;
            const FaceView& face    = *m_Faces[f];
            glm::mat4       view    = face.View.GetViewMatrix();
            view[3]                 = glm::vec4( 0.0f, 0.0f, 0.0f, 1.0f ); // a direction is projected, not a point
            block.ViewProjection[f] = face.View.GetProjectionMatrix() * view;
            const RDG::TextureRef imported = textures.Import( face.Renderer->GetSceneColorImage(),
                                                              std::format( "ReflectionCapture.Face{}", f ) );
            if ( !imported.IsValid() )
            {
                LOG_ERROR( "[ReflectionCaptures] capture {}: face view {} has no scene colour", cube, f );
                return;
            }
            faces[f] = imported;
        }
        const RDG::BufferRef facesBuffer =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( FacesBlock ) }, "ReflectionCapture.Faces" );
        graph.QueueBufferUpload( facesBuffer, std::as_bytes( std::span( &block, 1 ) ) );

        const RDG::TextureRef radiance =
             graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width  = kReflectionCaptureFaceSize,
                                                                .Height = kReflectionCaptureFaceSize },
                                                    .Format = kReflectionCaptureFormat,
                                                    .Mips   = kReflectionCaptureMips,
                                                    .Layers = 6,
                                                    .Dim    = RDG::TextureDim::Cube },
                                  "ReflectionCapture.Radiance" );

        graph.AddPass(
             std::format( "ReflectionCapture: Assemble {}", cube ), RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 auto bindings = pass.Bindings( m_AssembleLayout.Get( m_Assemble->GetSpecification().Shader ),
                                                Renderer::GetPipelineRouteFill( *m_Assemble ) );
                 for ( size_t f = 0; f < faces.size(); ++f )
                     bindings.Sampled( kFaceNames[f], faces[f], RDG::Access::SampledCompute,
                                       RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearClamp() );
                 bindings.Storage( "ReflectionCaptureFacesUB", facesBuffer, RDG::Access::StorageRead )
                      .Storage( "u_Output", radiance, RDG::Access::StorageWrite, 0 );
             },
             [pipeline = m_Assemble]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::GetInstance().DispatchCompute( bindings, *pipeline,
                                                                 GroupCount( kReflectionCaptureFaceSize ),
                                                                 GroupCount( kReflectionCaptureFaceSize ), 6 );
             } );

        for ( uint32_t mip = 1; mip < kReflectionCaptureMips; ++mip )
            graph.AddPass(
                 std::format( "ReflectionCapture: Downsample {} mip {}", cube, mip ), RDG::PassFlags::Compute,
                 [&, mip]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_DownsampleLayout.Get( m_Downsample->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *m_Downsample ) )
                          .Sampled( "u_Source", radiance, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::Mip( mip - 1 ), RDG::SamplerDesc::LinearClamp() )
                          .Storage( "u_Output", radiance, RDG::Access::StorageWrite, mip );
                 },
                 [pipeline = m_Downsample, mip]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     const uint32_t          size = std::max( 1u, kReflectionCaptureFaceSize >> mip );
                     return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, GroupCount( size ),
                                                                     GroupCount( size ), 6 );
                 } );

        for ( uint32_t mip = 0; mip < kReflectionCaptureMips; ++mip )
            graph.AddPass(
                 std::format( "ReflectionCapture: Filter {} mip {}", cube, mip ), RDG::PassFlags::Compute,
                 [&, mip]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_FilterLayout.Get( m_Filter->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *m_Filter ) )
                          .Sampled( "u_Radiance", radiance, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearClamp() )
                          .Storage( "u_Output", array, RDG::Access::StorageWrite, mip )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( FilterPush ) ) );
                 },
                 [pipeline = m_Filter, mip, cube]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     // The sky's ramp (ComputeImages::ProccessForImageCubeMips): roughness = mip / (mips - 1).
                     const FilterPush  push{ static_cast<float>( mip ) /
                                                 static_cast<float>( kReflectionCaptureMips - 1u ),
                                            cube * 6u };
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, sizeof( push ) );
                     const uint32_t size = std::max( 1u, kReflectionCaptureFaceSize >> mip );
                     return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, GroupCount( size ),
                                                                     GroupCount( size ), 6 );
                 } );
    }

    void ReflectionCaptureRenderer::AddPasses( RDG::Builder& graph, FrameTextures& textures )
    {
        if ( !EnsurePipelines() )
            return; // logged once by MakeCompute; the composite then reports its unbound inputs by name

        // Arrays retired by a resize stay alive while the frames in flight may still sample them.
        constexpr size_t kRetiredKept = 3;
        if ( m_Retired.size() > kRetiredKept )
            m_Retired.erase( m_Retired.begin(), m_Retired.end() - kRetiredKept );

        const uint32_t cubes =
             std::max<uint32_t>( m_OwnsCaptures ? static_cast<uint32_t>( m_Captures.size() ) : 0u, 1u );
        RDG::TextureRef array;
        bool            fresh = false;
        if ( !m_ArrayBuilt || m_ArrayCubes != cubes )
        {
            if ( m_ArrayBuilt )
                m_Retired.push_back( std::make_unique<RDG::ExternalTexture>( std::move( m_Array ) ) );
            m_Array = RDG::ExternalTexture{};
            array   = graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width  = kReflectionCaptureFaceSize,
                                                                         .Height = kReflectionCaptureFaceSize },
                                                             .Format = kReflectionCaptureFormat,
                                                             .Mips   = kReflectionCaptureMips,
                                                             .Layers = 6u * cubes,
                                                             .Dim    = RDG::TextureDim::CubeArray },
                                           "ReflectionCaptures.Cubes" );
            graph.Extract( array, m_Array, RDG::Access::SampledGraphics );
            m_ArrayCubes = cubes;
            m_ArrayBuilt = true;
            fresh        = true;
        }
        else
            array = graph.RegisterExternal( m_Array, "ReflectionCaptures.Cubes" );

        // Every layer of a fresh array is written before anything reads it: black, until its capture lands.
        if ( fresh )
            for ( uint32_t cube = 0; cube < cubes; ++cube )
                AddCapturePasses( graph, textures, array, cube, true );

        if ( m_FacesRenderedThisFrame && m_Pending < m_Captures.size() )
        {
            AddCapturePasses( graph, textures, array, m_Pending, false );
            m_Captured[m_Pending] = true;
            if ( m_PendingSettled >= kCaptureSettleFrames )
            {
                m_Done[m_Pending] = true;
                m_Pending         = kNoCapture;
            }
        }
        m_FacesRenderedThisFrame = false;

        const std::vector<std::byte> list =
             m_OwnsCaptures ? PackReflectionCaptures( m_Captures, m_Captured ) : PackReflectionCaptures( {}, {} );
        const RDG::BufferRef buffer =
             graph.CreateBuffer( RDG::BufferDesc{ list.size() }, "ReflectionCaptures.List" );
        graph.QueueBufferUpload( buffer, list );

        textures.Transients.ReflectionCaptureCubes = array;
        textures.Transients.ReflectionCaptures     = buffer;
    }
} // namespace Desert::Graphic
