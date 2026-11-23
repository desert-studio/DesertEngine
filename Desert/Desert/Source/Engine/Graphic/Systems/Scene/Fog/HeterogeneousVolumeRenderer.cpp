#include "HeterogeneousVolumeRenderer.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/SparseVolumeAsset.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Math/Rounding.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr const char* kVolumeCacheShaderName    = "HeterogeneousVolumeLightingCache";
        constexpr const char* kVolumeMarchShaderName    = "HeterogeneousVolumeMarch";
        constexpr const char* kVolumeUpsampleShaderName = "HeterogeneousVolumeUpsample";
        // The same over-operator as the clouds (CloudComposite.shader): scene = rgb x PreExposure + scene x a.
        constexpr const char* kVolumeCompositeShaderName = "CloudComposite";

        constexpr uint32_t kCacheGroup  = 4; // LocalSize(4, 4, 4)
        constexpr uint32_t kScreenGroup = 8; // LocalSize(8, 8, 1)
        // Steps per volume along one eye ray (a 32nd of the smallest half size each, at most this many).
        constexpr uint32_t kMaxStepsPerVolume = 48;
        // UE r.HeterogeneousVolumes history weight analogue: the jittered steps integrate over ~8 frames.
        constexpr float kHistoryWeight = 0.85f;
        // A baked volume's bricks stay resident this many frames after it was last on screen (a turn away and
        // back does not re-read the file).
        constexpr uint64_t kSparseEvictFrames = 600;
        // The brick atlas's cap: 4096 bricks of 18^3 RG16F = 91 MiB (UE r.SparseVolumeTexture.Streaming pool
        // analogue). Beyond it the least recently seen volumes are not placed (said by name in the log).
        constexpr uint32_t kSparseAtlasMaxBricks = 4096;
        constexpr uint32_t kBrickPhysical        = Assets::kSparseVolumeBrickPhysical;

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
                              kVolumeCacheShaderName, kVolumeMarchShaderName, kVolumeUpsampleShaderName,
                              kVolumeCompositeShaderName ) );
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
        // The page-table slot's stand-in while no baked volume is drawn (never read then: Sparse.y = 0).
        m_SparsePagesFallback = ShaderResources::StorageBuffer::Create(
             "HeterogeneousVolumes.SparsePagesFallback", 16u, kSparseVolumePagesBinding, /*persistent=*/true );
        if ( !m_LightingParams || !m_VolumesBuffer || !m_MarchParams || !m_SparsePagesFallback )
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
        return { { std::string( kVolumeCacheShaderName ), LightingCacheVsmVariant() } };
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
        auto acquired = shaderService->AcquireVariant( kVolumeCacheShaderName, LightingCacheVsmVariant() );
        if ( !acquired )
            return refuse( std::format( "{} ({}) was refused: {}", kVolumeCacheShaderName, kLocalShadowVsmDefine,
                                        acquired.GetError() ) );
        const auto  shader    = acquired.ExtractValue();
        std::string debugName = std::string( kVolumeCacheShaderName ) + "|DESERT_LOCAL_SHADOW_VSM";
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
        if ( !create( kVolumeCacheShaderName, m_CachePipeline ) ||
             !create( kVolumeMarchShaderName, m_MarchPipeline ) ||
             !create( kVolumeUpsampleShaderName, m_UpsamplePipeline ) )
            return false;

        const auto compositeShader = shaderService->GetByName( kVolumeCompositeShaderName );
        if ( !compositeShader )
        {
            LOG_ERROR( "[HeterogeneousVolume] Graphics shader '{}' is not registered.",
                       kVolumeCompositeShaderName );
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
         const std::span<const HeterogeneousVolumeInstance> volumes )
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
        // A baked volume is drawn once its bricks are placed in the atlas (TouchSparse starts / collects the read;
        // RepackSparse places). Until then it is skipped, never drawn as something else.
        std::array<const HeterogeneousVolumeInstance*, kHeterogeneousVolumeMaxVolumes> onScreen{};
        std::array<glm::vec4, kHeterogeneousVolumeMaxVolumes>                          rects{};
        uint32_t                                                                       seen = 0;
        for ( const HeterogeneousVolumeInstance& instance : volumes )
        {
            if ( seen == kHeterogeneousVolumeMaxVolumes ||
                 !ProjectBox( frame.ViewProjection, instance.Gpu.LocalToWorld, rects[seen] ) )
                continue;
            if ( instance.SparseVolume != 0 && !TouchSparse( instance.SparseVolume, frame.FrameIndex ) )
                continue;
            onScreen[seen++] = &instance;
        }
        // Eviction runs only on frames with a volume on screen (the zero-cost rule): what was not seen for
        // kSparseEvictFrames leaves the atlas at this re-pack.
        for ( auto it = m_Sparse.begin(); it != m_Sparse.end(); )
        {
            if ( !it->second.Loading && it->second.LastSeen + kSparseEvictFrames < frame.FrameIndex )
            {
                m_SparseDirty = m_SparseDirty || it->second.Placed;
                it            = m_Sparse.erase( it );
            }
            else
                ++it;
        }
        if ( m_SparseDirty )
            RepackSparse();
        std::array<HeterogeneousVolumeGpu, kHeterogeneousVolumeMaxVolumes> visible{};
        uint32_t                                                           count = 0;
        glm::vec4                                                          rect( 1.0f, 1.0f, 0.0f, 0.0f );
        m_SparseDrawn = false;
        for ( uint32_t i = 0; i < seen; ++i )
        {
            HeterogeneousVolumeGpu volume = onScreen[i]->Gpu;
            if ( onScreen[i]->SparseVolume != 0 )
            {
                const auto resident = m_Sparse.find( onScreen[i]->SparseVolume );
                if ( resident == m_Sparse.end() || !resident->second.Placed || !m_SparseAtlas || !m_SparsePages )
                    continue;
                const Assets::SparseVolumeData& data = resident->second.Data;
                volume.Sparse      = glm::uvec4( resident->second.FirstPage, data.Pages );
                volume.SparseScale = glm::vec4( glm::vec3( data.Resolution ), data.PeakTemperature );
                m_SparseDrawn      = true;
            }
            const glm::vec4& boxRect   = rects[i];
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
        // The page tables: uploaded every frame a baked volume is drawn (a per-frame buffer, a few KiB), the
        // fallback slot otherwise. The march binds the same buffer and the same atlas (m_SparseAtlasBinding).
        ShaderResources::StorageBuffer* pages = m_SparsePagesFallback.get();
        if ( m_SparseDrawn )
        {
            const auto uploaded = m_SparsePages->SetData(
                 m_SparsePageTable.data(), static_cast<uint32_t>( m_SparsePageTable.size() * sizeof( uint32_t ) ) );
            if ( !uploaded.IsSuccess() )
            {
                LOG_ERROR( "[HeterogeneousVolume] not drawn this frame, the sparse page tables were not uploaded: {}",
                           uploaded.GetError() );
                return nodes;
            }
            pages = m_SparsePages.get();
        }
        pipeline->SetStorageBuffer( kSparseVolumePagesBinding, pages );
        if ( m_MarchPipeline )
            m_MarchPipeline->SetStorageBuffer( kSparseVolumePagesBinding, pages );
        m_SparseAtlasBinding     = FallbackTextures::Get().GetBlackTexture3D();
        m_SparseAtlasBindingName = "HeterogeneousVolumes.SparseAtlasFallback";
        if ( m_SparseDrawn )
        {
            m_SparseAtlasBinding     = std::shared_ptr<Image>( m_SparseAtlas );
            m_SparseAtlasBindingName = "HeterogeneousVolumes.SparseAtlas";
        }

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
                       RDG::SubresourceRange::All() )
             .Texture( Cache::SparseVolumeAtlasTexture, m_SparseAtlasBinding, RDG::Access::SampledCompute,
                       m_SparseAtlasBindingName );
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
             .Sampler( March::PointClampSampler, RDG::SamplerDesc::PointClamp() )
             .Texture( March::SparseVolumeAtlasTexture, m_SparseAtlasBinding, RDG::Access::SampledCompute,
                       m_SparseAtlasBindingName );
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

    bool HeterogeneousVolumeRenderer::TouchSparse( const uint64_t handle, const uint64_t frameIndex )
    {
        auto [it, inserted] = m_Sparse.try_emplace( handle );
        SparseResident& resident = it->second;
        resident.LastSeen        = frameIndex;
        if ( inserted )
        {
            // The first sight: the read goes to a Background job (it is the file's size in I/O and a decode),
            // and the volume is not drawn until it lands.
            const std::string key = Assets::ContentRegistry::KeyForHandle( handle );
            if ( key.empty() )
            {
                resident.Loading = false;
                resident.Failed  = true;
                LOG_ERROR( "[HeterogeneousVolume] sparse volume {:#x}: no content registry row holds the handle (the "
                           ".dsvol is missing or not yet discovered); the volume is not drawn",
                           handle );
                return false;
            }
            const std::filesystem::path file = Common::AssetHandle::PathForStableKey( key );
            resident.Pending = Common::JobSystem::Get().Async(
                 [file]() noexcept { return Assets::SparseVolumeAsset::ReadFile( file ); },
                 Common::JobPriority::Background );
            return false;
        }
        if ( resident.Loading )
        {
            if ( resident.Pending.wait_for( std::chrono::seconds( 0 ) ) != std::future_status::ready )
                return false;
            auto read        = resident.Pending.get();
            resident.Loading = false;
            if ( !read )
            {
                resident.Failed = true;
                LOG_ERROR( "[HeterogeneousVolume] sparse volume {:#x} is not drawn: {}", handle, read.GetError() );
                return false;
            }
            resident.Data = read.ExtractValue();
            m_SparseDirty = true;
            LOG_INFO( "[HeterogeneousVolume] sparse volume {:#x} resident: {}x{}x{} voxels, {} of {} pages hold "
                      "medium",
                      handle, resident.Data.Resolution.x, resident.Data.Resolution.y, resident.Data.Resolution.z,
                      resident.Data.BrickCount(), resident.Data.PageCount() );
            return true;
        }
        return !resident.Failed;
    }

    void HeterogeneousVolumeRenderer::RepackSparse()
    {
        m_SparseDirty = false;
        // Most recently seen first: when the cap binds, what is on screen now wins.
        std::vector<std::pair<uint64_t, SparseResident*>> loaded;
        for ( auto& [handle, resident] : m_Sparse )
        {
            resident.Placed = false;
            if ( !resident.Loading && !resident.Failed )
                loaded.emplace_back( handle, &resident );
        }
        std::sort( loaded.begin(), loaded.end(),
                   []( const auto& a, const auto& b ) { return a.second->LastSeen > b.second->LastSeen; } );
        uint32_t bricks = 0;
        uint32_t pages  = 0;
        for ( auto& [handle, resident] : loaded )
        {
            const uint32_t count = resident->Data.BrickCount();
            if ( bricks + count > kSparseAtlasMaxBricks )
            {
                LOG_WARN( "[HeterogeneousVolume] sparse volume {:#x} ({} bricks) does not fit the {}-brick atlas "
                          "beside the volumes seen more recently; not drawn until they leave",
                          handle, count, kSparseAtlasMaxBricks );
                continue;
            }
            resident->Placed     = true;
            resident->FirstBrick = bricks;
            resident->FirstPage  = pages;
            bricks += count;
            pages += static_cast<uint32_t>( resident->Data.PageCount() );
        }
        m_SparseAtlas = nullptr;
        m_SparsePageTable.clear();
        if ( bricks == 0 )
            return;

        // The atlas: a near-cube of bricks (the shader derives the grid from the texture size / 18).
        const uint32_t gx    = static_cast<uint32_t>( std::ceil( std::cbrt( static_cast<double>( bricks ) ) ) );
        const uint32_t gy    = gx;
        const uint32_t gz    = ( bricks + gx * gy - 1u ) / ( gx * gy );
        const uint32_t w     = gx * kBrickPhysical;
        const uint32_t h     = gy * kBrickPhysical;
        const uint32_t d     = gz * kBrickPhysical;
        const size_t   texel = 4u; // RG16F
        std::vector<unsigned char> atlas( size_t( w ) * h * d * texel, 0u );
        m_SparsePageTable.reserve( pages );
        for ( auto& [handle, resident] : loaded )
        {
            if ( !resident->Placed )
                continue;
            const Assets::SparseVolumeData& data = resident->Data;
            for ( const uint32_t entry : data.PageTable )
                m_SparsePageTable.push_back( entry == 0u ? 0u : entry + resident->FirstBrick );
            const uint32_t count = data.BrickCount();
            for ( uint32_t b = 0; b < count; ++b )
            {
                const uint32_t slot = resident->FirstBrick + b;
                const uint32_t sx   = slot % gx;
                const uint32_t sy   = ( slot / gx ) % gy;
                const uint32_t sz   = slot / ( gx * gy );
                const uint16_t* src = data.Bricks.data() + size_t( b ) * Assets::kSparseVolumeBrickVoxels * 2u;
                for ( uint32_t z = 0; z < kBrickPhysical; ++z )
                    for ( uint32_t y = 0; y < kBrickPhysical; ++y )
                    {
                        const size_t row = ( ( size_t( sz ) * kBrickPhysical + z ) * h + sy * kBrickPhysical + y ) * w +
                                           size_t( sx ) * kBrickPhysical;
                        std::memcpy( atlas.data() + row * texel,
                                     src + ( size_t( z ) * kBrickPhysical + y ) * kBrickPhysical * 2u,
                                     kBrickPhysical * texel );
                    }
            }
        }
        // A fresh image per re-pack (the backend has no 3D re-upload path, as the cloud noise service notes); the
        // previous one goes through Image3D's deletion queue once the frames still reading it drop it.
        const Core::Formats::Image3DSpecification spec{
             .Tag        = "HeterogeneousVolumes.SparseAtlas",
             .Width      = w,
             .Height     = h,
             .Depth      = d,
             .Format     = Core::Formats::ImageFormat::RG16F,
             .Data       = std::move( atlas ),
             .Properties = Core::Formats::Sample,
        };
        m_SparseAtlas = Image3D::Create( spec );
        if ( !m_SparseAtlas )
        {
            LOG_ERROR( "[HeterogeneousVolume] the sparse brick atlas ({}x{}x{} RG16F, {} bricks) could not be created; "
                       "the baked volumes are not drawn",
                       w, h, d, bricks );
            for ( auto& [handle, resident] : loaded )
                resident->Placed = false;
            m_SparsePageTable.clear();
            return;
        }
        const uint32_t needed = static_cast<uint32_t>( m_SparsePageTable.size() );
        if ( !m_SparsePages || m_SparsePageCapacity < needed )
        {
            m_SparsePageCapacity = std::max( needed, 4096u );
            m_SparsePages        = ShaderResources::StorageBuffer::Create(
                 "HeterogeneousVolumes.SparsePages", m_SparsePageCapacity * static_cast<uint32_t>( sizeof( uint32_t ) ),
                 kSparseVolumePagesBinding, /*persistent=*/false );
            if ( !m_SparsePages )
            {
                LOG_ERROR( "[HeterogeneousVolume] the sparse page-table buffer ({} entries) could not be created; the "
                           "baked volumes are not drawn",
                           m_SparsePageCapacity );
                m_SparsePageCapacity = 0;
                m_SparseAtlas        = nullptr;
                for ( auto& [handle, resident] : loaded )
                    resident->Placed = false;
            }
        }
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
