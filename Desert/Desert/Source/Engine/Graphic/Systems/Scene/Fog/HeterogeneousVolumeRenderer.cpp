#include "HeterogeneousVolumeRenderer.hpp"

#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/Math/Rounding.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr const char* kCacheShaderName    = "HeterogeneousVolumeLightingCache";
        constexpr const char* kMarchShaderName    = "HeterogeneousVolumeMarch";
        constexpr const char* kUpsampleShaderName = "HeterogeneousVolumeUpsample";
        // The same over-operator as the clouds (CloudComposite.shader): scene = rgb x PreExposure + scene x a.
        constexpr const char* kCompositeShaderName = "CloudComposite";

        constexpr uint32_t kCacheGroup  = 4; // LocalSize(4, 4, 4)
        constexpr uint32_t kScreenGroup = 8; // LocalSize(8, 8, 1)
        // Steps per volume along one eye ray (a 32nd of the smallest half size each, at most this many).
        constexpr uint32_t kMaxStepsPerVolume = 48;
        // UE r.HeterogeneousVolumes history weight analogue: the jittered steps integrate over ~8 frames.
        constexpr float kHistoryWeight = 0.85f;

        // The box's screen footprint: false when it is entirely outside the view frustum. @p rect is the uv
        // rectangle (min xy, max zw) of its corners; the whole screen when a corner is behind the camera.
        bool ProjectBox( const glm::mat4& viewProjection, const glm::mat4& localToWorld, glm::vec4& rect )
        {
            glm::vec4 corners[8];
            for ( int i = 0; i < 8; ++i )
            {
                const glm::vec4 local( ( i & 1 ) ? 1.0f : -1.0f, ( i & 2 ) ? 1.0f : -1.0f,
                                       ( i & 4 ) ? 1.0f : -1.0f, 1.0f );
                corners[i] = viewProjection * ( localToWorld * local );
            }
            // Outside one clip plane with all eight corners = not visible (reverse Z: near is z = w, far z = 0;
            // the far plane is not tested - an infinite or distant far never hides a box).
            const auto allOutside = [&]( auto&& outside ) -> bool
            {
                for ( const auto& c : corners )
                    if ( !outside( c ) )
                        return false;
                return true;
            };
            if ( allOutside( []( const glm::vec4& c ) { return c.x < -c.w; } ) ||
                 allOutside( []( const glm::vec4& c ) { return c.x > c.w; } ) ||
                 allOutside( []( const glm::vec4& c ) { return c.y < -c.w; } ) ||
                 allOutside( []( const glm::vec4& c ) { return c.y > c.w; } ) ||
                 allOutside( []( const glm::vec4& c ) { return c.w <= 0.0f; } ) )
                return false;
            glm::vec2 lo( 1.0f ), hi( 0.0f );
            for ( const auto& c : corners )
            {
                if ( c.w <= 1e-3f )
                {
                    rect = glm::vec4( 0.0f, 0.0f, 1.0f, 1.0f );
                    return true;
                }
                const glm::vec2 ndc( c.x / c.w, c.y / c.w );
                // The engine's screen uv: y down (VolumetricFogWorldPosition's inverse).
                const glm::vec2 uv( ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f );
                lo = glm::min( lo, uv );
                hi = glm::max( hi, uv );
            }
            rect = glm::vec4( glm::clamp( lo, glm::vec2( 0.0f ), glm::vec2( 1.0f ) ),
                              glm::clamp( hi, glm::vec2( 0.0f ), glm::vec2( 1.0f ) ) );
            return true;
        }
    } // namespace

    HeterogeneousVolumeRenderer::~HeterogeneousVolumeRenderer() = default;

    Common::BoolResultStr HeterogeneousVolumeRenderer::Initialize()
    {
        if ( !CreatePipelines() )
            return Common::MakeError(
                 std::format( "HeterogeneousVolumeRenderer: the programs could not be resolved "
                              "({} / {} / {} / {})",
                              kCacheShaderName, kMarchShaderName, kUpsampleShaderName, kCompositeShaderName ) );
        // Non-persistent: one copy per (frame x recording renderer slot), Docs/RENDERER_FRAME_STATE.md.
        m_LightingParams = ShaderResources::StorageBuffer::Create(
             "HeterogeneousVolumes.LightingParams", static_cast<uint32_t>( sizeof( VolumetricFogGpuPayload ) ),
             kVolumetricFogParamsBinding, /*persistent=*/false );
        m_VolumesBuffer = ShaderResources::StorageBuffer::Create(
             "HeterogeneousVolumes.Volumes",
             static_cast<uint32_t>( sizeof( HeterogeneousVolumeGpu ) * kHeterogeneousVolumeMaxVolumes ),
             kHeterogeneousVolumesBinding, /*persistent=*/false );
        m_MarchParams = ShaderResources::StorageBuffer::Create(
             "HeterogeneousVolumes.MarchParams", static_cast<uint32_t>( sizeof( HeterogeneousVolumeMarchGpu ) ),
             kHeterogeneousVolumeParamsBinding, /*persistent=*/false );
        if ( !m_LightingParams || !m_VolumesBuffer || !m_MarchParams )
            return Common::MakeError( "HeterogeneousVolumeRenderer: could not create the parameter buffers" );
        m_CompositeMaterial = std::make_unique<MaterialCloudComposite>();
        if ( const auto valid = m_CacheLayout.Validate( m_CachePipeline->GetShader(),
                                                        HeterogeneousVolumeLightingCacheShaderParameters::kAll );
             !valid )
            return valid;
        if ( const auto valid = m_MarchLayout.Validate( m_MarchPipeline->GetShader(),
                                                        HeterogeneousVolumeMarchShaderParameters::kAll );
             !valid )
            return valid;
        if ( const auto valid = m_UpsampleLayout.Validate( m_UpsamplePipeline->GetShader(),
                                                           HeterogeneousVolumeUpsampleShaderParameters::kAll );
             !valid )
            return valid;
        if ( const auto valid = m_CompositeLayout.Validate( m_CompositePipeline->GetSpecification().Shader,
                                                            VolumetricCloudCompositeShaderParameters::kAll );
             !valid )
            return valid;
        return BOOLSUCCESS;
    }

    Core::ShaderVariant HeterogeneousVolumeRenderer::LightingCacheVsmVariant()
    {
        return Core::ShaderVariant{ .Defines = { std::string( kLocalShadowVsmDefine ) }, .ShippedByCook = true };
    }

    std::vector<Core::CookedProgramVariant> HeterogeneousVolumeRenderer::ProgramVariants()
    {
        return { { std::string( kCacheShaderName ), LightingCacheVsmVariant() } };
    }

    Common::BoolResultStr HeterogeneousVolumeRenderer::EnsureLightingCacheVsmPipeline()
    {
        if ( m_CacheVsmPipeline )
            return BOOLSUCCESS;
        if ( !m_CacheVsmRefusal.empty() )
            return Common::MakeError( m_CacheVsmRefusal );
        const auto refuse = [this]( std::string why ) -> Common::BoolResultStr
        {
            m_CacheVsmRefusal = std::move( why );
            LOG_ERROR( "[HeterogeneousVolume] the volumes are not drawn under Shadows.Method VirtualMaps: {}",
                       m_CacheVsmRefusal );
            return Common::MakeError( m_CacheVsmRefusal );
        };
        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
            return refuse( "no shader service" );
        auto acquired = shaderService->AcquireVariant( kCacheShaderName, LightingCacheVsmVariant() );
        if ( !acquired )
            return refuse( std::format( "{} ({}) was refused: {}", kCacheShaderName, kLocalShadowVsmDefine,
                                        acquired.GetError() ) );
        const auto  shader    = acquired.ExtractValue();
        std::string debugName = std::string( kCacheShaderName ) + "|DESERT_LOCAL_SHADOW_VSM";
        const auto  pipeline  = ComputePipeline::Create( { .Shader = shader, .DebugName = debugName } );
        if ( !pipeline )
            return refuse( pipeline.GetError() );
        if ( const auto valid = m_CacheVsmLayout.Validate(
                  shader, HeterogeneousVolumeLightingCacheShaderParameters::kVirtualLocal );
             !valid )
            return refuse( valid.GetError() );
        m_CacheVsmPipeline = pipeline.GetValue();
        return BOOLSUCCESS;
    }

    bool HeterogeneousVolumeRenderer::CreatePipelines()
    {
        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
            return false;
        const auto create = [&]( const char* name, std::shared_ptr<ComputePipeline>& out ) -> bool
        {
            const auto shader = shaderService->GetByName( name );
            if ( !shader )
            {
                LOG_ERROR( "[HeterogeneousVolume] Compute shader '{}' is not registered. Expected "
                           "Editor/Resources/Shaders/Programs/HeterogeneousVolumes/{}.shader.",
                           name, name );
                return false;
            }
            const auto pipeline = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
            if ( !pipeline )
            {
                LOG_ERROR( "[HeterogeneousVolume] {}: {}", name, pipeline.GetError() );
                return false;
            }
            out = pipeline.GetValue();
            return true;
        };
        if ( !create( kCacheShaderName, m_CachePipeline ) || !create( kMarchShaderName, m_MarchPipeline ) ||
             !create( kUpsampleShaderName, m_UpsamplePipeline ) )
            return false;

        const auto compositeShader = shaderService->GetByName( kCompositeShaderName );
        if ( !compositeShader )
        {
            LOG_ERROR( "[HeterogeneousVolume] Graphics shader '{}' is not registered.", kCompositeShaderName );
            return false;
        }
        GraphicsPipelineSpecification spec;
        spec.DebugName    = "HeterogeneousVolumeComposite";
        spec.Shader       = compositeShader;
        spec.TargetLayout = SceneTargetLayout();
        // Occlusion was resolved by the march (cut at the scene depth) and the depth-aware upsample.
        spec.DepthTestEnabled  = false;
        spec.DepthWriteEnabled = false;
        spec.CullMode          = CullMode::None;
        spec.Topology          = PrimitiveTopology::Triangles;
        // scene = medium.rgb * One + scene * medium.a: premultiplied, alpha carrying TRANSMITTANCE.
        spec.BlendEnable         = true;
        spec.SrcColorBlendFactor = BlendFactor::One;
        spec.DstColorBlendFactor = BlendFactor::SrcAlpha;
        const auto composite     = GraphicsPipeline::Create( spec );
        if ( !composite )
        {
            LOG_ERROR( "[HeterogeneousVolume] the composite pipeline was not built: {}", composite.GetError() );
            return false;
        }
        m_CompositePipeline = composite.GetValue();
        return true;
    }

    bool HeterogeneousVolumeRenderer::EnsureCache()
    {
        if ( m_Cache )
            return true;
        const Core::Formats::Image3DSpecification spec{
             .Tag        = "HeterogeneousVolumes.LightingCache",
             .Width      = kHeterogeneousVolumeCacheResolution,
             .Height     = kHeterogeneousVolumeCacheResolution,
             .Depth      = kHeterogeneousVolumeCacheResolution * kHeterogeneousVolumeMaxVolumes,
             .Format     = Core::Formats::ImageFormat::RGBA16F,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        };
        m_Cache = Image3D::Create( spec );
        if ( !m_Cache )
            LOG_ERROR( "[HeterogeneousVolume] the lighting cache ({}^3 x {} RGBA16F) could not be created; the "
                       "volumes are not drawn this frame",
                       kHeterogeneousVolumeCacheResolution, kHeterogeneousVolumeMaxVolumes );
        return m_Cache != nullptr;
    }

    bool HeterogeneousVolumeRenderer::EnsureViewImages( const uint32_t width, const uint32_t height )
    {
        if ( m_ViewSize == glm::uvec2( width, height ) && m_March[0] && m_March[1] && m_Guide && m_Upsampled )
            return true;
        const uint32_t halfW = std::max( ( width + 1u ) / 2u, 1u );
        const uint32_t halfH = std::max( ( height + 1u ) / 2u, 1u );
        const auto     make  = []( const char* tag, uint32_t w, uint32_t h, Core::Formats::ImageFormat format )
        {
            const Core::Formats::Image2DSpecification spec{
                 .Tag        = tag,
                 .Width      = w,
                 .Height     = h,
                 .Format     = format,
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
            };
            return Image2D::Create( spec );
        };
        m_March[0]  = make( "HeterogeneousVolumes.MarchA", halfW, halfH, Core::Formats::ImageFormat::RGBA16F );
        m_March[1]  = make( "HeterogeneousVolumes.MarchB", halfW, halfH, Core::Formats::ImageFormat::RGBA16F );
        m_Guide     = make( "HeterogeneousVolumes.Guide", halfW, halfH, Core::Formats::ImageFormat::R32F );
        m_Upsampled = make( "HeterogeneousVolumes.Upsampled", width, height, Core::Formats::ImageFormat::RGBA16F );
        if ( !m_March[0] || !m_March[1] || !m_Guide || !m_Upsampled )
        {
            LOG_ERROR( "[HeterogeneousVolume] the view images ({}x{}) could not be created; the volumes are not "
                       "drawn this frame",
                       width, height );
            m_March     = {};
            m_Guide     = nullptr;
            m_Upsampled = nullptr;
            m_ViewSize  = glm::uvec2( 0u );
            return false;
        }
        m_ViewSize = glm::uvec2( width, height );
        m_History.Invalidate();
        return true;
    }

    ComputeNodeList HeterogeneousVolumeRenderer::DeclareLightingNodes(
         Common::Memory::FrameArena& arena, const FrameGraphRefs& refs, const ViewFrame& frame,
         const VolumetricFogSunInput& sun, const bool virtualLocalShadows,
         const std::span<const HeterogeneousVolumeGpu> volumes )
    {
        ComputeNodeList nodes = MakeComputeNodeList( arena );
        // Once per view frame: the froxels' path and the translucency's both ask, the first one declares.
        if ( m_LitFrame == frame.FrameIndex )
            return nodes;
        m_LitFrame     = frame.FrameIndex;
        m_LitThisFrame = false;
        m_Composite    = false;
        m_ShadowInput  = HeterogeneousVolumeShadowInput{};
        if ( volumes.empty() || !m_CachePipeline || !m_LightingParams || !m_VolumesBuffer )
            return nodes;

        // THE PER-VIEW CULL: only the boxes that reach this view's frustum are lit and marched. None = zero cost.
        std::array<HeterogeneousVolumeGpu, kHeterogeneousVolumeMaxVolumes> visible{};
        uint32_t                                                           count = 0;
        glm::vec4                                                          rect( 1.0f, 1.0f, 0.0f, 0.0f );
        for ( const HeterogeneousVolumeGpu& volume : volumes )
        {
            glm::vec4 boxRect;
            if ( count == kHeterogeneousVolumeMaxVolumes ||
                 !ProjectBox( frame.ViewProjection, volume.LocalToWorld, boxRect ) )
                continue;
            visible[count]             = volume;
            visible[count].Blackbody.y = static_cast<float>( count );
            rect                       = glm::vec4( glm::min( glm::vec2( rect ), glm::vec2( boxRect ) ),
                                                    glm::max( glm::vec2( rect.z, rect.w ), glm::vec2( boxRect.z, boxRect.w ) ) );
            ++count;
        }
        if ( count == 0 )
            return nodes;

        const FrameTransients& t = refs.Transients;
        if ( !t.Lights.Point.IsValid() || !t.Lights.Spot.IsValid() ||
             ( !virtualLocalShadows && !t.Lights.ShadowViews.IsValid() ) || !t.Lights.GridParams.IsValid() ||
             !t.Lights.GridCells.IsValid() || !t.ShadowCascades.IsValid() ||
             ( virtualLocalShadows && !t.Vsm.IsValid() ) )
        {
            LOG_ERROR(
                 "[HeterogeneousVolume] the view has no light grid, cascade array or virtual shadow map this "
                 "frame: the {} volumes on screen are not drawn",
                 count );
            return nodes;
        }
        if ( virtualLocalShadows && !EnsureLightingCacheVsmPipeline() )
            return nodes;
        if ( !EnsureCache() )
            return nodes;
        ComputePipeline* const pipeline  = virtualLocalShadows ? m_CacheVsmPipeline.get() : m_CachePipeline.get();
        ShaderBindingLayoutCache& layout = virtualLocalShadows ? m_CacheVsmLayout : m_CacheLayout;

        const AtmosphereEnv&    atmosphere = m_SceneRenderer->GetAtmosphere();
        VolumetricFogGpuPayload p{};
        VolumetricFogRenderer::PackVolumeLighting( p, frame, sun, atmosphere, glm::vec3( 1.0f ),
                                                   t.RainOcclusionMap.IsValid() );
        p.GridPixel = glm::vec4( 1.0f, static_cast<float>( frame.Split.Render.Width ),
                                 static_cast<float>( frame.Split.Render.Height ), 0.0f );
        p.LocalFog  = glm::uvec4( 0u, count, 0u, 0u );
        if ( const auto uploaded = m_LightingParams->SetData( &p, static_cast<uint32_t>( sizeof( p ) ) );
             !uploaded.IsSuccess() )
        {
            LOG_ERROR( "[HeterogeneousVolume] not drawn this frame, the lighting block was not uploaded: {}",
                       uploaded.GetError() );
            return nodes;
        }
        if ( const auto uploaded = m_VolumesBuffer->SetData(
                  visible.data(), static_cast<uint32_t>( sizeof( HeterogeneousVolumeGpu ) * count ) );
             !uploaded.IsSuccess() )
        {
            LOG_ERROR( "[HeterogeneousVolume] not drawn this frame, the volumes were not uploaded: {}",
                       uploaded.GetError() );
            return nodes;
        }
        pipeline->SetStorageBuffer( kVolumetricFogParamsBinding, m_LightingParams.get() );
        pipeline->SetStorageBuffer( kHeterogeneousVolumesBinding, m_VolumesBuffer.get() );

        const std::shared_ptr<Image> distantSkyLight =
             atmosphere.DistantSkyLight
                  ? std::shared_ptr<Image>( atmosphere.DistantSkyLight )
                  : FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA8F );
        // Named here, not as a conditional inside the by-value argument (MSVC 19.44 hangs on that shape).
        std::string distantSkyLightName =
             atmosphere.DistantSkyLight ? "Sky.DistantLight" : "HeterogeneousVolumes.DistantSkyLightFallback";
        RDG::TextureRef rainOcclusionMap = refs.System.White;
        if ( t.RainOcclusionMap.IsValid() )
            rainOcclusionMap = t.RainOcclusionMap;

        using Cache = HeterogeneousVolumeLightingCacheShaderParameters;
        ComputeNodeDeclaration cache( arena );
        cache.Name = "HeterogeneousVolumes: LightingCache";
        auto block = cache.Access.Bindings( layout.Get( pipeline->GetShader() ),
                                            Renderer::GetPipelineRouteFill( *pipeline ) );
        block.Storage( Cache::LightingCacheTexture, std::shared_ptr<Image>( m_Cache ), RDG::Access::StorageWrite,
                       "HeterogeneousVolumes.LightingCache" )
             .Sampler( Cache::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
             .Texture( Cache::ShadowCascadesTexture, t.ShadowCascades, RDG::Access::SampledCompute,
                       RDG::SubresourceRange::All() )
             .Sampler( Cache::LinearRepeatSampler, RDG::SamplerDesc::LinearRepeat() )
             .Texture( Cache::DistantSkyLightTexture, distantSkyLight, RDG::Access::SampledCompute,
                       std::move( distantSkyLightName ) )
             .Storage( Cache::PointLightsUB, t.Lights.Point, RDG::Access::StorageRead )
             .Storage( Cache::SpotLightsUB, t.Lights.Spot, RDG::Access::StorageRead )
             .BindlessRead( t.Lights.LightFunctionAtlas, RDG::Access::SampledCompute )
             .Storage( Cache::LightGridParamsUB, t.Lights.GridParams, RDG::Access::StorageRead )
             .Storage( Cache::LightGridCellsUB, t.Lights.GridCells, RDG::Access::StorageRead )
             .Texture( Cache::CloudShadowMapTexture, CloudShadowMapOrWhite( refs ), RDG::Access::SampledCompute,
                       RDG::SubresourceRange::All() )
             .Texture( Cache::RainOcclusionMapTexture, rainOcclusionMap, RDG::Access::SampledCompute,
                       RDG::SubresourceRange::All() );
        if ( virtualLocalShadows )
        {
            block.Storage( kVsmFrameDataBlock, t.Vsm.FrameData, RDG::Access::StorageRead )
                 .Storage( kVsmPageTableBlock, t.Vsm.PageTable, RDG::Access::StorageRead )
                 .Storage( kVsmPhysicalPool, t.Vsm.PhysicalPool, RDG::Access::StorageRead )
                 .Storage( kVsmLocalLightsBlock, t.Vsm.LocalLights, RDG::Access::StorageRead );
        }
        else
        {
            const RDG::TextureRef atlas = t.LocalShadowAtlas.IsValid() ? t.LocalShadowAtlas : refs.System.White;
            block.Texture( Cache::LocalShadowAtlasTexture, atlas, RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() )
                 .Sampler( Cache::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                 .Storage( Cache::LocalShadowViewsUB, t.Lights.ShadowViews, RDG::Access::StorageRead );
        }
        cache.Record = [pipeline, count]( RDG::PassContext& context,
                                          const FrameGraphRefs& ) -> Common::BoolResultStr
        {
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            const uint32_t          groups =
                 Common::Math::DivideRoundUp( kHeterogeneousVolumeCacheResolution, kCacheGroup );
            return Renderer::DispatchCompute( bindings, *pipeline, groups, groups, groups * count );
        };
        nodes.push_back( std::move( cache ) );

        m_LitThisFrame = true;
        m_ScreenRect   = rect;
        m_ShadowInput =
             HeterogeneousVolumeShadowInput{ std::shared_ptr<Image>( m_Cache ), m_VolumesBuffer.get(), count };
        return nodes;
    }

    ComputeNodeList HeterogeneousVolumeRenderer::DeclareViewNodes( Common::Memory::FrameArena& arena,
                                                                   const FrameGraphRefs&       refs,
                                                                   const ViewFrame&            frame )
    {
        ComputeNodeList nodes = MakeComputeNodeList( arena );
        m_Composite           = false;
        if ( !m_LitThisFrame || m_LitFrame != frame.FrameIndex || !m_MarchPipeline || !m_UpsamplePipeline ||
             !m_MarchParams )
            return nodes;
        const uint32_t width  = frame.Split.Render.Width;
        const uint32_t height = frame.Split.Render.Height;
        if ( width == 0 || height == 0 || !EnsureViewImages( width, height ) )
            return nodes;
        const std::shared_ptr<Image2D> depth = m_SceneRenderer->GetComputeSceneDepth();
        if ( !depth )
        {
            LOG_ERROR(
                 "[HeterogeneousVolume] no single-sample scene depth this frame: the volumes are not drawn" );
            return nodes;
        }

        const bool     historyValid = m_History.ReadableIn( frame );
        const uint32_t current      = historyValid ? 1u - m_Current : m_Current;
        const uint32_t history      = 1u - current;
        const uint32_t halfW        = ( width + 1u ) / 2u;
        const uint32_t halfH        = ( height + 1u ) / 2u;

        HeterogeneousVolumeMarchGpu march{};
        march.Counts  = glm::uvec4( m_ShadowInput.Count, static_cast<uint32_t>( frame.FrameIndex & 0xffffffffu ),
                                   historyValid ? 1u : 0u, kMaxStepsPerVolume );
        march.Target  = glm::vec4( static_cast<float>( halfW ), static_cast<float>( halfH ),
                                   static_cast<float>( width ), static_cast<float>( height ) );
        march.History = glm::vec4( kHistoryWeight, 0.0f, 0.0f, 0.0f );
        march.Rect    = m_ScreenRect;
        if ( const auto uploaded = m_MarchParams->SetData( &march, static_cast<uint32_t>( sizeof( march ) ) );
             !uploaded.IsSuccess() )
        {
            LOG_ERROR( "[HeterogeneousVolume] not drawn this frame, the march block was not uploaded: {}",
                       uploaded.GetError() );
            return nodes;
        }
        m_MarchPipeline->SetStorageBuffer( kVolumetricFogParamsBinding, m_LightingParams.get() );
        m_MarchPipeline->SetStorageBuffer( kHeterogeneousVolumesBinding, m_VolumesBuffer.get() );
        m_MarchPipeline->SetStorageBuffer( kHeterogeneousVolumeParamsBinding, m_MarchParams.get() );
        m_UpsamplePipeline->SetStorageBuffer( kVolumetricFogParamsBinding, m_LightingParams.get() );
        m_UpsamplePipeline->SetStorageBuffer( kHeterogeneousVolumeParamsBinding, m_MarchParams.get() );

        const std::string currentName =
             current == 0 ? "HeterogeneousVolumes.MarchA" : "HeterogeneousVolumes.MarchB";
        // On a reset the pass reads no history: the slot holds the 2D fallback and Counts.z = 0.
        std::shared_ptr<Image> historyImage =
             FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA8F );
        std::string historyName = "HeterogeneousVolumes.HistoryReset";
        if ( historyValid )
        {
            historyImage = std::shared_ptr<Image>( m_March[history] );
            historyName  = history == 0 ? "HeterogeneousVolumes.MarchA" : "HeterogeneousVolumes.MarchB";
        }

        using March = HeterogeneousVolumeMarchShaderParameters;
        ComputeNodeDeclaration marchNode( arena );
        marchNode.Name = "HeterogeneousVolumes: March";
        marchNode.Access
             .Bindings( m_MarchLayout.Get( m_MarchPipeline->GetShader() ),
                        Renderer::GetPipelineRouteFill( *m_MarchPipeline ) )
             .Storage( March::MarchTexture, std::shared_ptr<Image>( m_March[current] ), RDG::Access::StorageWrite,
                       currentName )
             .Storage( March::MarchGuideTexture, std::shared_ptr<Image>( m_Guide ), RDG::Access::StorageWrite,
                       "HeterogeneousVolumes.Guide" )
             .Texture( March::MarchHistoryTexture, historyImage, RDG::Access::SampledCompute, historyName )
             .Texture( March::LightingCacheTexture, std::shared_ptr<Image>( m_Cache ), RDG::Access::SampledCompute,
                       "HeterogeneousVolumes.LightingCache" )
             .Texture( March::SceneDepthTexture, std::shared_ptr<Image>( depth ), RDG::Access::SampledCompute,
                       "SceneDepth.Compute" )
             .Sampler( March::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
             .Sampler( March::PointClampSampler, RDG::SamplerDesc::PointClamp() );
        marchNode.Record = [this, current, halfW, halfH, frameIndex = frame.FrameIndex](
                                RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
        {
            const RDG::PassBindings     bindings( context, context.GetBindingBlock( 0 ) );
            const Common::BoolResultStr dispatched = Renderer::DispatchCompute(
                 bindings, *m_MarchPipeline, Common::Math::DivideRoundUp( halfW, kScreenGroup ),
                 Common::Math::DivideRoundUp( halfH, kScreenGroup ), 1 );
            if ( dispatched )
            {
                m_Current = current;
                m_History.Stamp( frameIndex );
            }
            return dispatched;
        };
        nodes.push_back( std::move( marchNode ) );

        using Upsample = HeterogeneousVolumeUpsampleShaderParameters;
        ComputeNodeDeclaration upsample( arena );
        upsample.Name = "HeterogeneousVolumes: Upsample";
        upsample.Access
             .Bindings( m_UpsampleLayout.Get( m_UpsamplePipeline->GetShader() ),
                        Renderer::GetPipelineRouteFill( *m_UpsamplePipeline ) )
             .Storage( Upsample::UpsampledTexture, std::shared_ptr<Image>( m_Upsampled ),
                       RDG::Access::StorageWrite, "HeterogeneousVolumes.Upsampled" )
             .Texture( Upsample::MarchTexture, std::shared_ptr<Image>( m_March[current] ),
                       RDG::Access::SampledCompute, currentName )
             .Texture( Upsample::MarchGuideTexture, std::shared_ptr<Image>( m_Guide ), RDG::Access::SampledCompute,
                       "HeterogeneousVolumes.Guide" )
             .Texture( Upsample::SceneDepthTexture, std::shared_ptr<Image>( depth ), RDG::Access::SampledCompute,
                       "SceneDepth.Compute" )
             .Sampler( Upsample::PointClampSampler, RDG::SamplerDesc::PointClamp() );
        upsample.Record = [this, width, height]( RDG::PassContext& context,
                                                 const FrameGraphRefs& ) -> Common::BoolResultStr
        {
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::DispatchCompute( bindings, *m_UpsamplePipeline,
                                              Common::Math::DivideRoundUp( width, kScreenGroup ),
                                              Common::Math::DivideRoundUp( height, kScreenGroup ), 1 );
        };
        nodes.push_back( std::move( upsample ) );

        // The composite writes the radiance at the scene colour's pre-exposure (the clouds' material, its one
        // value).
        m_CompositeMaterial->SetExposure( glm::vec4( frame.PreExposure, 0.0f, 0.0f, 0.0f ) );
        m_Composite = true;
        return nodes;
    }

    SystemRasterPass HeterogeneousVolumeRenderer::CompositePass()
    {
        SystemRasterPass config;
        const auto       target = m_TargetFramebuffer.lock();
        if ( !target || !m_CompositePipeline || !m_Composite )
            return config;
        config.Name        = "HeterogeneousVolumes: Composite";
        config.ExecuteFunc = [this]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
        {
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DrawFullscreen( bindings, *m_CompositePipeline,
                                                           m_CompositeMaterial->GetMaterialExecutor() );
        };
        config.TargetFramebuffer = target;
        config.Declare           = [this]( RenderPassDeclaration& declared, const FrameGraphRefs& )
        {
            const MaterialExecutor& executor = *m_CompositeMaterial->GetMaterialExecutor();
            declared.Prepares( executor );
            declared
                 .Bindings( m_CompositeLayout.Get( m_CompositePipeline->GetSpecification().Shader ),
                            executor.GetRouteFill() )
                 .Texture( VolumetricCloudCompositeShaderParameters::CloudScatterTexture,
                           std::shared_ptr<Image>( m_Upsampled ), RDG::Access::SampledGraphics,
                           "HeterogeneousVolumes.Upsampled" )
                 .Sampler( VolumetricCloudCompositeShaderParameters::LinearRepeatSampler,
                           RDG::SamplerDesc::LinearRepeat() );
        };
        return config;
    }
} // namespace Desert::Graphic::System
