#include "ReflectionCaptureRenderer.hpp"

#include <Engine/Core/FrameManager.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/SceneRendererFrame.hpp>
#include <Engine/Graphic/SkyRules.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Common/Settings/Scalability.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <span>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct ReflectionCaptureAssembleShaderParameters
    {
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( StorageBuffer, ReflectionCaptureFacesUB );
        DESERT_SHADER_PARAMETER( StorageTexture, DestinationCubeTexture );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct ReflectionCaptureDownsampleShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, SourceCubeTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( StorageTexture, DestinationCubeTexture );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct ReflectionCaptureFilterShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, RadianceTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( StorageTexture, DestinationCubeTexture );
    };
} // namespace

namespace Desert::Graphic
{
    namespace
    {
        constexpr uint32_t kGroupSize = 8u;

        // THE SIX FACE VIEWS, in the order the assemble shader names them (FacePositiveXTexture .. FaceNegativeZTexture). The up
        // vectors are any that are not parallel to the axis: the assemble projects through each view's own matrix,
        // so no cube-face convention is encoded here.
        constexpr std::array<glm::vec3, 6>        kFaceForward = { glm::vec3( 1, 0, 0 ), glm::vec3( -1, 0, 0 ),
                                                                   glm::vec3( 0, 1, 0 ), glm::vec3( 0, -1, 0 ),
                                                                   glm::vec3( 0, 0, 1 ), glm::vec3( 0, 0, -1 ) };
        constexpr std::array<glm::vec3, 6>        kFaceUp      = { glm::vec3( 0, 1, 0 ), glm::vec3( 0, 1, 0 ),
                                                                   glm::vec3( 0, 0, 1 ), glm::vec3( 0, 0, -1 ),
                                                                   glm::vec3( 0, 1, 0 ), glm::vec3( 0, 1, 0 ) };
        constexpr std::array<std::string_view, 6> kFaceNames   = { "FacePositiveXTexture", "FaceNegativeXTexture", "FacePositiveYTexture",
                                                                   "FaceNegativeYTexture", "FacePositiveZTexture", "FaceNegativeZTexture" };
        constexpr auto kFaceTextures = RDG::ParametersOf<RDG::ShaderResourceKind::Texture>( kFaceNames );

        // A face view has settled once it held one sky environment for this many consecutive renders of its own
        // after its first. The sky is the scene's (lent, never baked by the face), so what is left to converge is
        // the face's own: its sky-view LUT is filled for the NEXT render (SkyboxRenderer: the first render of a
        // view draws a black sky), so one render after the first is a whole picture...
        constexpr uint32_t kCaptureSettleFramesStill = 1u;
        // ...unless the face draws a cloud layer, which reprojects a history: the first frames of a freshly aimed
        // face are the cloudless picture, and a cube written from there would keep it.
        constexpr uint32_t kCaptureSettleFrames = 8u;
        // ...and after this many renders whatever its sky does: a sky that re-bakes while the face renders (the
        // face views follow the scene's sky) would otherwise hold the one-face-per-frame budget on one capture
        // forever. Three settle windows: a still sky settles long before it.
        constexpr uint32_t kCaptureFrameBudget = 3u * kCaptureSettleFrames;

        struct FacesBlock
        {
            std::array<glm::mat4, 6> ViewProjection;
            glm::vec4                Probe; // x = the distance a direction is projected at
        };

        struct FilterPush
        {
            uint32_t Mip        = 0u;
            uint32_t MaxMip     = 0u;
            uint32_t FirstLayer = 0u;
        };

        // ReflectionCaptureDiffuse.shader's push block.
        struct DiffusePush
        {
            glm::vec4 Estimator{ 0.0f }; // x = 1 the radiance cache, 0 the sky's irradiance; y = point count
            glm::vec4 SkyYawCosSin{ 1.0f, 0.0f, 0.0f, 0.0f };
            glm::vec4 SkyGain{ 1.0f };
        };
        // ReflectionCaptureDiffuse.shader's push block and one row of its point list (std430).
        // ReflectionCaptureBlend.shader's push block.
        struct BlendPush
        {
            uint32_t LiveLayer    = 0u;
            uint32_t StagingLayer = 0u;
            float    Weight       = 0.0f;
            float    Pad          = 0.0f;
        };
        struct DiffusePointGpu
        {
            glm::vec4  Point{ 0.0f };    // xyz = the capture point (world cm)
            glm::uvec4 CubeForced{ 0u }; // x = cube index, y = 1 measure unconditionally (the cube being written)
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

        // THE EXACT PART OF THE LIGHTING A SCENE'S CAPTURES SEE, apart from the captures themselves: the sky bake
        // (@p environment: its identity changes only when the sky's own rule re-baked it - a sun past its Rebake
        // Sun Angle, the clouds, the sky's look) and every visible point and spot light's colour, intensity,
        // reach, cone and transform - an edit, never a drift, so any change counts. The suns are not in it: they
        // move every frame of a running day and are compared by ReflectionCaptureSunChanged's threshold instead.
        // Hashed field by field, not as raw component bytes: the components carry editor-only fields (ShowRadius,
        // ShowCone) and padding that would relight on nothing.
        [[nodiscard]] uint64_t LightingSignature( const entt::registry& registry, uint64_t environment )
        {
            uint64_t hash = 14695981039346656037ull;
            HashBytes( hash, &environment, sizeof( environment ) );
            const auto hashTransform = [&hash]( const ECS::TransformComponent& transform )
            {
                HashBytes( hash, &transform.Translation, sizeof( transform.Translation ) );
                HashBytes( hash, &transform.Rotation, sizeof( transform.Rotation ) );
            };
            const auto points = registry.view<const ECS::PointLightComponent, const ECS::TransformComponent>();
            for ( const entt::entity entity : points )
            {
                if ( ECS::IsHidden( registry, entity ) )
                    continue;
                const ECS::TransformComponent& transform = points.get<const ECS::TransformComponent>( entity );
                const ECS::PointLightData&     d  = points.get<const ECS::PointLightComponent>( entity ).Data;
                const auto                     id = entt::to_integral( entity );
                HashBytes( hash, &id, sizeof( id ) );
                HashBytes( hash, &d.Color, sizeof( d.Color ) );
                HashBytes( hash, &d.Intensity, sizeof( d.Intensity ) );
                HashBytes( hash, &d.IntensityUnits, sizeof( d.IntensityUnits ) );
                HashBytes( hash, &d.Radius, sizeof( d.Radius ) );
                HashBytes( hash, &d.MinRadius, sizeof( d.MinRadius ) );
                HashBytes( hash, &d.Falloff, sizeof( d.Falloff ) );
                HashBytes( hash, &d.CastShadows, sizeof( d.CastShadows ) );
                hashTransform( transform );
            }
            const auto spots = registry.view<const ECS::SpotLightComponent, const ECS::TransformComponent>();
            for ( const entt::entity entity : spots )
            {
                if ( ECS::IsHidden( registry, entity ) )
                    continue;
                const ECS::TransformComponent& transform = spots.get<const ECS::TransformComponent>( entity );
                const ECS::SpotLightData&      d         = spots.get<const ECS::SpotLightComponent>( entity ).Data;
                const auto                     id        = entt::to_integral( entity );
                HashBytes( hash, &id, sizeof( id ) );
                HashBytes( hash, &d.Color, sizeof( d.Color ) );
                HashBytes( hash, &d.Intensity, sizeof( d.Intensity ) );
                HashBytes( hash, &d.IntensityUnits, sizeof( d.IntensityUnits ) );
                HashBytes( hash, &d.Range, sizeof( d.Range ) );
                HashBytes( hash, &d.InnerConeAngle, sizeof( d.InnerConeAngle ) );
                HashBytes( hash, &d.OuterConeAngle, sizeof( d.OuterConeAngle ) );
                HashBytes( hash, &d.Falloff, sizeof( d.Falloff ) );
                HashBytes( hash, &d.CastShadows, sizeof( d.CastShadows ) );
                hashTransform( transform );
            }
            return hash;
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

    bool ReflectionCaptureSunChanged( const std::vector<ShaderProtocols::DirectionLightPayload>& reference,
                                      const std::vector<ShaderProtocols::DirectionLightPayload>& current,
                                      const float                                                thresholdDeg )
    {
        if ( reference.size() != current.size() )
            return true;
        for ( size_t i = 0; i < current.size(); ++i )
        {
            if ( ShouldRebakeSkyEnvironment( glm::vec3( reference[i].Direction ),
                                             glm::vec3( current[i].Direction ), thresholdDeg, true, true, false, 0,
                                             0, 0, 0 ) )
                return true;
            const glm::vec3 was    = glm::vec3( reference[i].ColorIntensity ) * reference[i].ColorIntensity.w;
            const glm::vec3 now    = glm::vec3( current[i].ColorIntensity ) * current[i].ColorIntensity.w;
            const float     wasSum = was.r + was.g + was.b;
            const float     nowSum = now.r + now.g + now.b;
            if ( ( wasSum > 0.0f ) != ( nowSum > 0.0f ) )
                return true; // a sun switched on or off
            if ( wasSum <= 0.0f )
                continue;
            // Each channel's share of the sun's illuminance: an even scaling keeps every share and is left to the
            // normalisation; a hue shift moves them.
            for ( int c = 0; c < 3; ++c )
            {
                const float a = std::max( was[c] / wasSum, 1e-4f );
                const float b = std::max( now[c] / nowSum, 1e-4f );
                if ( std::max( a, b ) / std::min( a, b ) > kReflectionCaptureSunRatio )
                    return true;
            }
        }
        return false;
    }

    float ReflectionCaptureRenderer::SunAngleThreshold( const entt::registry& registry )
    {
        // The primary sky is the lowest entity (SelectPrimarySky), as the sky system picks it.
        std::optional<entt::entity> primary;
        const auto                  skies = registry.view<const ECS::SkyAtmosphereComponent>();
        for ( const entt::entity entity : skies )
            if ( !primary || entt::to_integral( entity ) < entt::to_integral( *primary ) )
                primary = entity;
        // Without a procedural sky the sun still lights the cubes: the field's own default is the threshold.
        return primary ? skies.get<const ECS::SkyAtmosphereComponent>( *primary ).Data.RebakeSunAngleThreshold
                       : ECS::SkyAtmosphereData{}.RebakeSunAngleThreshold;
    }

    uint32_t ReflectionCaptureFaceSize()
    {
        return static_cast<uint32_t>( Common::Scalability::QualityState::Resolved().As<int>(
             Common::Scalability::Parameter::ReflectionCaptureSize ) );
    }

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
                                                   const std::vector<float>&                  presence )
    {
        std::vector<const ReflectionCaptureEntry*> readable;
        for ( const ReflectionCaptureEntry& capture : captures )
            if ( capture.Gpu.ShapeCube.y < presence.size() && presence[capture.Gpu.ShapeCube.y] > 0.0f )
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
        {
            ReflectionCaptureGpu gpu = readable[i]->Gpu;
            gpu.ShapeCube.z          = std::bit_cast<uint32_t>( std::min( presence[gpu.ShapeCube.y], 1.0f ) );
            std::memcpy( bytes.data() + sizeof( header ) + i * sizeof( ReflectionCaptureGpu ), &gpu,
                         sizeof( ReflectionCaptureGpu ) );
        }
        return bytes;
    }

    // One face view: a complete scene renderer and the camera it is opened with.
    struct ReflectionCaptureRenderer::FaceView
    {
        class Camera final : public Core::Camera
        {
        public:
            void Aim( const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up, uint32_t faceSize )
            {
                m_Position   = position;
                m_ViewMatrix = glm::lookAt( position, position + forward, up );
                SetFOV( 90.0f );
                UpdateProjectionMatrix( faceSize, faceSize );
            }
        };

        std::unique_ptr<SceneRenderer> Renderer;
        Camera                         View;
    };

    ReflectionCaptureRenderer::ReflectionCaptureRenderer( const ResourceOwnerInstance scene ) : m_Scene( scene )
    {
    }

    ReflectionCaptureRenderer::~ReflectionCaptureRenderer() = default;

    bool ReflectionCaptureRenderer::EnsurePipelines()
    {
        if ( m_Assemble && m_Downsample && m_Filter && m_Diffuse && m_Blend )
            return true;
        if ( m_PipelinesFailed )
            return false;
        constexpr std::string_view kAssemble   = "ReflectionCaptureAssemble";
        constexpr std::string_view kDownsample = "ReflectionCaptureDownsample";
        constexpr std::string_view kFilter     = "ReflectionCaptureFilter";
        constexpr std::string_view kDiffuse    = "ReflectionCaptureDiffuse";
        constexpr std::string_view kBlend      = "ReflectionCaptureBlend";
        m_Assemble                             = MakeCompute( kAssemble );
        m_Downsample                           = MakeCompute( kDownsample );
        m_Filter                               = MakeCompute( kFilter );
        m_Diffuse                              = MakeCompute( kDiffuse );
        m_Blend                                = MakeCompute( kBlend );
        m_PipelinesFailed = !( m_Assemble && m_Downsample && m_Filter && m_Diffuse && m_Blend );
        m_FailedProgram   = !m_Assemble     ? kAssemble
                            : !m_Downsample ? kDownsample
                            : !m_Filter     ? kFilter
                            : !m_Diffuse    ? kDiffuse
                            : !m_Blend      ? kBlend
                                            : "";
        return !m_PipelinesFailed;
    }

    void ReflectionCaptureRenderer::RenderCaptureViews( const Core::Scene&                           scene,
                                                        const std::function<void( SceneRenderer& )>& record,
                                                        Common::Timestep timestep, Common::Timestep realTimestep,
                                                        const ShaderProtocols::DirectionLight& dirLights,
                                                        const std::optional<Environment>&      environment )
    {
        m_WriteCapture = false;

        const auto& frames = Engine::FrameManager::GetInstance();
        m_RetiringFaces.Collect( frames.GetAbsoluteFrameCount(), frames.GetMaxFramesInFlight() );

        const uint32_t faceSize = ReflectionCaptureFaceSize();
        if ( !std::has_single_bit( faceSize ) )
        {
            if ( faceSize != m_FaceSize )
                LOG_ERROR( "[ReflectionCaptures] Reflections.CaptureSize {} is not a power of two: the scene's "
                           "reflection captures are not rendered.",
                           faceSize );
            m_FaceSize = faceSize;
            m_Captures.clear();
            m_Captured.clear();
            m_Presence.clear();
            m_Done.clear();
            m_Signature         = 0;
            m_LightingSignature = 0;
            m_Pending           = kNoCapture;
            m_BlendCube         = kNoCapture;
            return;
        }
        if ( m_DrivenFrame == ~0ull )
            LOG_INFO( "[ReflectionCaptures] first fill: driven from frame {}", frames.GetAbsoluteFrameCount() );
        m_DrivenFrame = frames.GetAbsoluteFrameCount();
        if ( faceSize != m_FaceSize )
        {
            // A new face size re-renders every capture at it (UE: r.ReflectionCaptureResolution recaptures).
            m_FaceSize = faceSize;
            for ( std::unique_ptr<FaceView>& face : m_Faces )
                if ( face )
                    face->Renderer->Resize( faceSize, faceSize );
        }

        std::vector<ReflectionCaptureEntry> captures  = GatherReflectionCaptures( scene.GetRegistry() );
        uint64_t                            signature = 14695981039346656037ull;
        HashBytes( signature, &faceSize, sizeof( faceSize ) );
        for ( const ReflectionCaptureEntry& capture : captures )
        {
            const auto id = entt::to_integral( capture.Entity );
            HashBytes( signature, &id, sizeof( id ) );
            HashBytes( signature, &capture.Gpu, sizeof( capture.Gpu ) );
        }
        const uint64_t lighting = LightingSignature( scene.GetRegistry(), EnvironmentIdentity( environment ) );
        if ( signature != m_Signature )
        {
            // UE recaptures every capture when any of them changes: a moved capture changes what its neighbours
            // see too. The cube indices may have moved with the set, so nothing is read until it is rendered
            // again.
            m_Captures          = std::move( captures );
            m_Signature         = signature;
            m_LightingSignature = lighting;
            m_ReferenceSuns     = dirLights.DirectionLights;
            m_Captured.assign( m_Captures.size(), false );
            m_Presence.assign( m_Captures.size(), 0u );
            m_Done.assign( m_Captures.size(), false );
            m_Pending   = kNoCapture;
            m_BlendCube = kNoCapture;
            // The round starts at capture 0: the one after the "last", which is the final index.
            m_LastCaptured = m_Captures.empty() ? 0u : static_cast<uint32_t>( m_Captures.size() - 1u );
        }
        else if ( lighting != m_LightingSignature ||
                  ReflectionCaptureSunChanged( m_ReferenceSuns, dirLights.DirectionLights,
                                               SunAngleThreshold( scene.GetRegistry() ) ) )
        {
            // RELIT: every cube is stale but still the best answer there is - it stays readable while the round
            // renders it again. The capture in flight is finished under the suns it started with (restarting it
            // would never finish under a lighting that changes faster than six faces settle) and stays stale.
            m_LightingSignature = lighting;
            m_ReferenceSuns     = dirLights.DirectionLights;
            m_Done.assign( m_Captures.size(), false );
            m_PendingRelit = m_Pending != kNoCapture;
        }
        if ( m_Pending == kNoCapture )
        {
            // Round-robin from the one after the last capture rendered: a lighting that changes every frame still
            // refreshes every capture in turn.
            const size_t count = m_Done.size();
            size_t       next  = count;
            for ( size_t step = 1; step <= count && next == count; ++step )
            {
                const size_t candidate = ( static_cast<size_t>( m_LastCaptured ) + step ) % count;
                if ( !m_Done[candidate] )
                    next = candidate;
            }
            if ( next == count )
            {
                // EVERY CAPTURE IS SETTLED: the six face views are released. Each is a complete SceneRenderer -
                // G-buffer, its own sky bake, cascades, the local-light shadow atlas - and holding them between
                // recaptures kept six views' worth of targets alive for nothing (measured on Bistro: the atlas,
                // the cascades and the sky bake x7 instead of x1, 1.4 GB). UE's capture renderers are transient
                // the same way. A recapture builds them again; their GPU objects go through the device's
                // deferred deletion, but a view's pipelines are destroyed with it: the views are parked until every
                // frame that can have recorded their passes has retired.
                for ( std::unique_ptr<FaceView>& face : m_Faces )
                    if ( face )
                        m_RetiringFaces.Park( std::move( face ), frames.GetAbsoluteFrameCount() );
                return;
            }
            m_Pending      = static_cast<uint32_t>( next );
            m_PendingSuns  = dirLights;
            m_PendingRelit = false;
            m_FaceRenders.fill( 0u );
            m_FaceHeld.fill( 0u );
            m_FaceStreamed.fill( false );
            m_FaceSettle.fill( kCaptureSettleFrames );
            m_FaceEnvironment.fill( 0u );
            m_LastFace = static_cast<uint32_t>( m_Faces.size() - 1u );
        }

        // ONE FACE THIS FRAME: the next one after the last that has not settled.
        const auto settled = [this]( size_t f )
        { return m_FaceHeld[f] >= m_FaceSettle[f] || m_FaceRenders[f] >= kCaptureFrameBudget; };
        size_t face = m_Faces.size();
        for ( size_t step = 1; step <= m_Faces.size() && face == m_Faces.size(); ++step )
        {
            const size_t candidate = ( static_cast<size_t>( m_LastFace ) + step ) % m_Faces.size();
            if ( !settled( candidate ) )
                face = candidate;
        }
        if ( face == m_Faces.size() )
        {
            // Every face settled and the cube is not written yet (no view wrote it last frame): ask again.
            m_WriteCapture = true;
            return;
        }

        const ReflectionCaptureEntry& capture = m_Captures[m_Pending];
        // The suns the capture started with: the six faces are one lighting instant, and a face view's cascades
        // stay cached (a sun that moves every frame re-renders every face's static cascade every render).
        const SceneRenderer::UpdateInfo info{ timestep, realTimestep, m_PendingSuns };
        if ( !m_Faces[face] )
        {
            m_Faces[face] = std::make_unique<FaceView>();
            // The face views render the SCENE's captures, not a viewer's picture: their rows are the scene's
            // ("scene #N", RT-LEDGER-4), so a capture is in the scene's budget entry and in no view's.
            m_Faces[face]->Renderer = std::make_unique<SceneRenderer>( ViewExtent{ faceSize, faceSize },
                                                                       kReflectionCaptureViewProfile, m_Scene );
            m_Faces[face]->Renderer->Init();
            m_Faces[face]->Renderer->Resize( faceSize, faceSize );
        }
        FaceView& view = *m_Faces[face];
        view.View.Aim( capture.CapturePosition, kFaceForward[face], kFaceUp[face], faceSize );
        // The scene's sky light lights the face (UE: a capture reads the scene's sky light and bakes none).
        view.Renderer->BorrowEnvironment( environment );
        if ( const auto begun = view.Renderer->BeginScene( scene, &view.View ); !begun )
        {
            LOG_ERROR( "[ReflectionCaptures] capture {} face {}: the face view did not open: {}", m_Pending, face,
                       begun.GetError() );
            return;
        }
        record( *view.Renderer );
        view.Renderer->OnUpdate( info );
        if ( const auto ended = view.Renderer->EndScene(); !ended )
        {
            LOG_ERROR( "[ReflectionCaptures] capture {} face {}: the face view did not close: {}", m_Pending, face,
                       ended.GetError() );
            return;
        }
        m_LastFace = static_cast<uint32_t>( face );
        ++m_FaceRenders[face];

        // Held while the face's sky environment (the scene's) stays the one it had at its previous render, and
        // only by a render that drew every texture the scene asked for: a texture still read, cooked or uploaded
        // draws its slot's white default, and a cube of white walls went 26 % darker over the Bistro running day
        // when its re-capture with the real textures landed (REFL-NORM-SMOOTH). A scene with NO sky has no
        // environment to wait for: its defined answer (zero sky light) is as stable as a captured one, so it
        // holds too — waiting for a sky that cannot arrive kept the face views rendering every frame, forever,
        // and never wrote the capture.
        constexpr uint64_t kNoSkyEnvironment = ~uint64_t{ 0 };
        const uint64_t     faceEnvironment   = view.Renderer->GetSkyLightState() == SkyLightState::NoSky
                                                    ? kNoSkyEnvironment
                                                    : view.Renderer->GetEnvironmentIdentity();
        const bool         streaming         = Runtime::ResourceRegistry::GetTextureService()->InFlight() > 0;
        m_FaceStreamed[face]                 = streaming;
        if ( !streaming && faceEnvironment != 0 && faceEnvironment == m_FaceEnvironment[face] )
            ++m_FaceHeld[face];
        else
            m_FaceHeld[face] = 0;
        m_FaceEnvironment[face] = faceEnvironment;
        m_FaceSettle[face] =
             view.Renderer->DrawsVolumetricClouds() ? kCaptureSettleFrames : kCaptureSettleFramesStill;

        bool all      = true;
        bool streamed = false;
        for ( size_t f = 0; f < m_Faces.size(); ++f )
        {
            all      = all && settled( f );
            streamed = streamed || m_FaceStreamed[f];
        }
        m_WriteCapture = all;
        // A face the frame budget settled under textures still in flight: the cube is written (it is better than
        // none) and stays stale, so the round renders it again.
        m_PendingRelit = m_PendingRelit || ( all && streamed );
    }

    void ReflectionCaptureRenderer::AddCapturePasses( RDG::Builder& graph, FrameTextures& textures,
                                                      RDG::TextureRef array, uint32_t cube, uint32_t faceSize,
                                                      bool black )
    {
        const uint32_t mips = Core::Formats::MipChainLength( faceSize );
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
             graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = faceSize, .Height = faceSize },
                                                    .Format = kReflectionCaptureFormat,
                                                    .Mips   = mips,
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
                     bindings
                          .Texture( kFaceTextures[f], faces[f], RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Sampler( ReflectionCaptureAssembleShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() );
                 bindings
                      .Storage( ReflectionCaptureAssembleShaderParameters::ReflectionCaptureFacesUB, facesBuffer,
                                RDG::Access::StorageRead )
                      .Storage( ReflectionCaptureAssembleShaderParameters::DestinationCubeTexture, radiance,
                                RDG::Access::StorageWrite, 0 );
             },
             [pipeline = m_Assemble, faceSize]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, GroupCount( faceSize ),
                                                                 GroupCount( faceSize ), 6 );
             } );

        for ( uint32_t mip = 1; mip < mips; ++mip )
            graph.AddPass(
                 std::format( "ReflectionCapture: Downsample {} mip {}", cube, mip ), RDG::PassFlags::Compute,
                 [&, mip]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_DownsampleLayout.Get( m_Downsample->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *m_Downsample ) )
                          .Texture( ReflectionCaptureDownsampleShaderParameters::SourceCubeTexture, radiance,
                                    RDG::Access::SampledCompute, RDG::SubresourceRange::Mip( mip - 1 ) )
                          .Sampler( ReflectionCaptureDownsampleShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Storage( ReflectionCaptureDownsampleShaderParameters::DestinationCubeTexture, radiance,
                                    RDG::Access::StorageWrite, mip );
                 },
                 [pipeline = m_Downsample, mip, faceSize]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     const uint32_t          size = std::max( 1u, faceSize >> mip );
                     return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, GroupCount( size ),
                                                                     GroupCount( size ), 6 );
                 } );

        for ( uint32_t mip = 0; mip < mips; ++mip )
            graph.AddPass(
                 std::format( "ReflectionCapture: Filter {} mip {}", cube, mip ), RDG::PassFlags::Compute,
                 [&, mip]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_FilterLayout.Get( m_Filter->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *m_Filter ) )
                          .Texture( ReflectionCaptureFilterShaderParameters::RadianceTexture, radiance,
                                    RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                          .Sampler( ReflectionCaptureFilterShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Storage( ReflectionCaptureFilterShaderParameters::DestinationCubeTexture, array,
                                    RDG::Access::StorageWrite, mip )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( FilterPush ) ) );
                 },
                 [pipeline = m_Filter, mip, mips, cube,
                  faceSize]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     // The shader derives the roughness from the mip: the engine's one ramp
                     // (Common/ReflectionCaptureMip.glslh), the sky's too.
                     const FilterPush  push{ mip, mips - 1u, cube * 6u };
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, sizeof( push ) );
                     const uint32_t size = std::max( 1u, faceSize >> mip );
                     return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, GroupCount( size ),
                                                                     GroupCount( size ), 6 );
                 } );
    }

    std::pair<RDG::TextureRef, bool>
    ReflectionCaptureRenderer::AcquireArray( RDG::Builder& graph, FrameTextures& textures, SharedArray& array,
                                             uint32_t cubes, uint32_t faceSize, std::string_view name )
    {
        if ( array.Built && array.Cubes == cubes && array.FaceSize == faceSize )
            return { graph.RegisterExternal( array.Texture, name ), false };

        if ( array.Built )
            m_Retired.push_back( std::make_unique<RDG::ExternalTexture>( std::move( array.Texture ) ) );
        array.Texture = RDG::ExternalTexture{};
        const RDG::TextureRef created =
             graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = faceSize, .Height = faceSize },
                                                    .Format = kReflectionCaptureFormat,
                                                    .Mips   = Core::Formats::MipChainLength( faceSize ),
                                                    .Layers = 6u * cubes,
                                                    .Dim    = RDG::TextureDim::CubeArray },
                                  name );
        {
            // THE ARRAY IS THE SCENE'S, whichever view's graph creates it: the extraction is attributed to the
            // scene, and the backend's pool opens the array's ledger row under it (RDG::Builder::Extract).
            const ResourceAttributionScope sceneScope( ResourceOwner::SceneRenderer, m_Scene );
            graph.Extract( created, array.Texture, RDG::Access::SampledGraphics );
        }
        array.Cubes    = cubes;
        array.FaceSize = faceSize;
        array.Built    = true;
        // Every layer of a fresh array is written before anything reads it: black, until its capture lands.
        for ( uint32_t cube = 0; cube < cubes; ++cube )
            AddCapturePasses( graph, textures, created, cube, faceSize, true );
        return { created, true };
    }

    void ReflectionCaptureRenderer::AddPasses( RDG::Builder& graph, FrameTextures& textures, bool readsCaptures )
    {
        m_GraphArray = RDG::TextureRef{};
        // The capture list every lit pass binds (ReflectionCapturesUB), as this graph's buffer.
        const auto publishList = [&graph, &textures]( const std::vector<std::byte>& list )
        {
            const RDG::BufferRef buffer =
                 graph.CreateBuffer( RDG::BufferDesc{ list.size() }, "ReflectionCaptures.List" );
            graph.QueueBufferUpload( buffer, list );
            textures.Transients.ReflectionCaptures = buffer;
        };
        // Nothing to read: the engine's black cube array (UE GBlackCubeArrayTexture) and the empty list, so the
        // lighting that binds them still draws and reads no capture. No compute pass is needed to make it.
        const auto publishNone = [&]()
        {
            textures.Transients.ReflectionCaptureCubes = textures.System.BlackCubeArray;
            publishList( PackReflectionCaptures( {}, std::vector<float>{} ) );
        };
        if ( !EnsurePipelines() )
        {
            // No cube can be assembled on this device (logged once by MakeCompute). The views refuse the
            // Reflections methods that read captures by FailedProgram()'s name (SceneRenderer::BeginScene).
            publishNone();
            return;
        }

        constexpr size_t kRetiredKept = 3;
        if ( m_Retired.size() > kRetiredKept )
            m_Retired.erase( m_Retired.begin(), m_Retired.end() - kRetiredKept );

        // The views that read no captures, and every view while the face size is refused.
        const bool capturing = readsCaptures && std::has_single_bit( m_FaceSize );
        if ( !capturing )
        {
            publishNone();
            return;
        }

        // One cube per capture, and the staging cube a recapture is written into before the live one blends to it.
        const uint32_t cubes = static_cast<uint32_t>( m_Captures.size() ) + 1u;
        const auto [array, created] =
             AcquireArray( graph, textures, m_Cubes, cubes, m_FaceSize, "ReflectionCaptures.Cubes" );
        if ( created )
        {
            std::fill( m_Captured.begin(), m_Captured.end(), false ); // a reshaped array holds nothing yet
            m_BlendCube = kNoCapture;
        }

        m_GraphArray                               = array;
        textures.Transients.ReflectionCaptureCubes = array;
        publishList( PackReflectionCaptures( m_Captures, CapturePresence() ) );
    }

    std::vector<float> ReflectionCaptureRenderer::CapturePresence() const
    {
        // A cube written in frame F is first read in F + 1 at 1/K, and fully from F + K on (AddCaptureUpdate
        // counts the frames after the one that wrote it).
        std::vector<float> presence( m_Captured.size(), 0.0f );
        for ( size_t cube = 0; cube < presence.size(); ++cube )
            if ( m_Captured[cube] )
                presence[cube] =
                     static_cast<float>( std::min( m_Presence[cube] + 1u, kReflectionCaptureBlendFrames ) ) /
                     static_cast<float>( kReflectionCaptureBlendFrames );
        return presence;
    }

    void ReflectionCaptureRenderer::AddBlendPasses( RDG::Builder& graph, const RDG::TextureRef array )
    {
        const uint32_t mips = Core::Formats::MipChainLength( m_FaceSize );
        BlendPush      push;
        push.LiveLayer    = m_BlendCube * 6u;
        push.StagingLayer = static_cast<uint32_t>( m_Captures.size() ) * 6u;
        push.Weight       = 1.0f / static_cast<float>( kReflectionCaptureBlendFrames - m_BlendStep );
        for ( uint32_t mip = 0; mip < mips; ++mip )
            graph.AddPass(
                 std::format( "ReflectionCapture: Blend {} mip {}", m_BlendCube, mip ), RDG::PassFlags::Compute,
                 [&, mip]( RDG::PassBuilder& pass )
                 {
                     // Read-modify-write of this mip: the staging layers are read, the live ones written.
                     pass.Bindings( m_BlendLayout.Get( m_Blend->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *m_Blend ) )
                          .Storage( "CubesTexture", array, RDG::Access::StorageWrite, mip )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( BlendPush ) ) );
                 },
                 [pipeline = m_Blend, push, mip,
                  faceSize = m_FaceSize]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, sizeof( push ) );
                     const uint32_t size = std::max( 1u, faceSize >> mip );
                     return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, GroupCount( size ),
                                                                     GroupCount( size ), 6 );
                 } );
    }

    void ReflectionCaptureRenderer::AddDiffusePass( RDG::Builder& graph, FrameTextures& textures,
                                                    const RDG::TextureRef array, const uint32_t forcedCube,
                                                    const uint32_t stagingSource, const bool stagingForced,
                                                    const bool                       forceAll,
                                                    const ReflectionCaptureLighting& lighting )
    {
        // Both estimators answer at once - the cube's own brightness under the radiance cache, the sky's
        // irradiance otherwise: a cube is measured when written and again when the estimator changes (forceAll);
        // in between a re-measure would store the same value.
        if ( forcedCube == kNoCapture && !stagingForced && !forceAll )
            return;
        // The written cubes (a cube nothing captured is black and never read: its diffuse means nothing).
        std::vector<DiffusePointGpu> points;
        for ( const ReflectionCaptureEntry& capture : m_Captures )
        {
            const uint32_t cube = capture.Gpu.ShapeCube.y;
            if ( cube != forcedCube && !( cube < m_Captured.size() && m_Captured[cube] ) )
                continue;
            DiffusePointGpu row;
            row.Point      = glm::vec4( capture.CapturePosition, 1.0f );
            row.CubeForced = glm::uvec4( cube, forceAll || cube == forcedCube ? 1u : 0u, 0u, 0u );
            points.push_back( row );
        }
        // The staging cube holds the recapture of stagingSource: measured at that capture's point, into its own
        // layer, when written or when the estimator changed (it is blended into the live cube with that measure).
        if ( stagingSource < m_Captures.size() && ( stagingForced || forceAll ) )
        {
            DiffusePointGpu row;
            row.Point      = glm::vec4( m_Captures[stagingSource].CapturePosition, 1.0f );
            row.CubeForced = glm::uvec4( static_cast<uint32_t>( m_Captures.size() ), 1u, 0u, 0u );
            points.push_back( row );
        }
        if ( points.empty() )
            return;

        const FrameGraphRefs refs         = textures.GraphRefs();
        const RDG::BufferRef pointsBuffer = graph.CreateBuffer(
             RDG::BufferDesc{ points.size() * sizeof( DiffusePointGpu ) }, "ReflectionCapture.DiffusePoints" );
        graph.QueueBufferUpload( pointsBuffer, std::as_bytes( std::span( points ) ) );
        DiffusePush push;
        push.Estimator =
             glm::vec4( lighting.RadianceCache ? 1.0f : 0.0f, static_cast<float>( points.size() ), 0.0f, 0.0f );
        push.SkyYawCosSin = lighting.SkyYawCosSin;
        push.SkyGain      = lighting.SkyGain;

        // Under the radiance cache the shader reads the cube itself and leaves the sky unread.
        const RDG::TextureRef sky =
             lighting.EnvIrradiance.IsValid() ? lighting.EnvIrradiance : refs.System.BlackCube;
        const uint32_t lastMip = Core::Formats::MipChainLength( m_FaceSize ) - 1u;
        const auto     count   = static_cast<uint32_t>( points.size() );

        graph.AddPass(
             "ReflectionCapture: Diffuse", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 constexpr RDG::Access kR  = RDG::Access::StorageRead;
                 constexpr RDG::Access kS  = RDG::Access::SampledCompute;
                 const auto            all = RDG::SubresourceRange::All();
                 pass.Bindings( m_DiffuseLayout.Get( m_Diffuse->GetSpecification().Shader ),
                                Renderer::GetPipelineRouteFill( *m_Diffuse ) )
                      .Texture( "EnvironmentIrradianceTexture", sky, kS, all )
                      .Sampler( "LinearClampSampler", RDG::SamplerDesc::LinearClamp() )
                      .Storage( "ReflectionCaptureDiffusePointsUB", pointsBuffer, kR )
                      // Read-modify-write of the last mip: its rgb is the roughest radiance and stays.
                      .Storage( "CubesTexture", array, RDG::Access::StorageWrite, lastMip )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( DiffusePush ) ) );
             },
             [pipeline = m_Diffuse, push, count]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( &push, sizeof( push ) );
                 return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, count, 1u, 1u );
             } );
    }

    void ReflectionCaptureRenderer::AddCaptureUpdate( RDG::Builder& graph, FrameTextures& textures,
                                                      const ReflectionCaptureLighting& lighting )
    {
        const RDG::TextureRef array = m_GraphArray;
        m_GraphArray                = RDG::TextureRef{};
        // The first view that reads captures in a frame writes and measures; the views after it read the array as
        // that view's graph left it.
        const uint64_t frame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        if ( !array.IsValid() || frame == m_MeasuredFrame )
            return;
        m_MeasuredFrame = frame;

        // The estimator of this frame (MeasureKey): a new one re-measures every written cube.
        uint64_t       key       = 14695981039346656037ull;
        const uint32_t estimator = lighting.RadianceCache ? 1u : 0u;
        HashBytes( key, &estimator, sizeof( estimator ) );
        if ( !lighting.RadianceCache )
        {
            // The sky's estimator is its bake read through its look.
            HashBytes( key, &lighting.Environment, sizeof( lighting.Environment ) );
            HashBytes( key, &lighting.SkyYawCosSin, sizeof( lighting.SkyYawCosSin ) );
            HashBytes( key, &lighting.SkyGain, sizeof( lighting.SkyGain ) );
        }
        const bool remeasureAll = key != m_MeasuredKey;
        m_MeasuredKey           = key;

        // The first cubes' fade-in counts this frame (CapturePresence: the next list reads one step further).
        bool filled = !m_Captured.empty();
        for ( size_t cube = 0; cube < m_Captured.size(); ++cube )
        {
            if ( m_Captured[cube] && m_Presence[cube] < kReflectionCaptureBlendFrames )
                ++m_Presence[cube];
            filled = filled && m_Captured[cube] && m_Presence[cube] >= kReflectionCaptureBlendFrames;
        }
        if ( filled && !m_FirstFillDone )
            LOG_INFO( "[ReflectionCaptures] first fill: {} capture(s) written and faded in by frame {}",
                      m_Captured.size(), frame );
        m_FirstFillDone = m_FirstFillDone || filled;

        uint32_t   firstWrite    = kNoCapture;
        bool       stagingForced = false;
        const bool recapture     = m_Pending < m_Captures.size() && m_Captured[m_Pending];
        // A recapture waits for the blend in progress to end: the staging cube holds that one's new cube.
        if ( m_WriteCapture && m_Pending < m_Captures.size() && !( recapture && m_BlendCube != kNoCapture ) )
        {
            const uint32_t capture = m_Pending;
            if ( recapture )
            {
                // Into the staging cube; the live one is read meanwhile and blends toward it from this frame.
                AddCapturePasses( graph, textures, array, static_cast<uint32_t>( m_Captures.size() ), m_FaceSize,
                                  false );
                m_BlendCube   = capture;
                m_BlendStep   = 0u;
                stagingForced = true;
            }
            else
            {
                // A first cube is written in place: nothing read it before, its coverage fades in instead.
                AddCapturePasses( graph, textures, array, capture, m_FaceSize, false );
                m_Captured[capture] = true;
                m_Presence[capture] = 0u;
                firstWrite          = capture;
            }
            m_Done[capture] = !m_PendingRelit;
            m_LastCaptured  = capture;
            m_Pending       = kNoCapture;
            m_WriteCapture  = false;
        }
        AddDiffusePass( graph, textures, array, firstWrite, m_BlendCube, stagingForced, remeasureAll, lighting );
        if ( m_BlendCube != kNoCapture )
        {
            AddBlendPasses( graph, array );
            if ( ++m_BlendStep >= kReflectionCaptureBlendFrames )
                m_BlendCube = kNoCapture;
        }
    }

    bool ReflectionCaptureRenderer::FirstFillPending() const
    {
        if ( m_FirstFillDone || m_PipelinesFailed || m_Captures.empty() || m_DrivenFrame == ~0ull )
            return false;
        // Driven this frame or the last: a scene no view reads captures for (or one not rendered) waits for none.
        return Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() <= m_DrivenFrame + 1u;
    }

    std::vector<glm::vec3> ReflectionCaptureRenderer::CapturePoints() const
    {
        std::vector<glm::vec3> points;
        points.reserve( m_Captures.size() );
        for ( const ReflectionCaptureEntry& capture : m_Captures )
            points.push_back( capture.CapturePosition );
        return points;
    }
} // namespace Desert::Graphic
