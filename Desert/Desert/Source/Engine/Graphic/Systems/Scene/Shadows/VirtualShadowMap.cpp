#include <Engine/Graphic/Systems/Scene/Shadows/VirtualShadowMap.hpp>

#include <Common/Core/Math/Rounding.hpp>
#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/LocalShadows.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/RayTracedSunShadow.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>
#include <vector>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct VirtualShadowMapPagesShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmFrameData );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmPageTable );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmPhysicalOwners );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmInvalidations );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmAllocator );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmLists );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmDirtyMask );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmLevelStats );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmLocalLights );
    };

    struct VirtualShadowMapProjectLocalShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmFrameData );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmPageTable );
        DESERT_SHADER_PARAMETER( StorageTexture, PhysicalPoolTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferNormalTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmLocalLights );
        DESERT_SHADER_PARAMETER( StorageTexture, LocalShadowMasksTexture );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct VirtualShadowMapPoolShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmPageTable );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmLists );
        DESERT_SHADER_PARAMETER( StorageTexture, PhysicalPoolTexture );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct VirtualShadowMapCullShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmFrameData );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmDirtyMask );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmCullInstances );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmCullDraws );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmCasterArguments );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmInstanceRemap );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct VirtualShadowMapProjectShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmFrameData );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmPageTable );
        DESERT_SHADER_PARAMETER( StorageTexture, PhysicalPoolTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferNormalTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, ShadowMaskTexture );
    };
} // namespace

namespace Desert::Graphic::System
{
    namespace
    {
        // One program per kernel, in VirtualShadowMapRenderer::Kernel order: the shader and the pass it adds.
        struct KernelName
        {
            const char* Shader;
            const char* Pass;
        };
        constexpr std::array<KernelName, 12> kKernelNames{ {
             { "VirtualShadowMapInvalidate", "VSM: Invalidate" },
             { "VirtualShadowMapInvalidateCasters", "VSM: Invalidate Casters" },
             { "VirtualShadowMapMark", "VSM: Mark" },
             { "VirtualShadowMapGather", "VSM: Gather Available Pages" },
             { "VirtualShadowMapAllocate", "VSM: Allocate" },
             { "VirtualShadowMapClearArgs", "VSM: Clear Args" },
             { "VirtualShadowMapClear", "VSM: Clear Pages" },
             { "VirtualShadowMapProject", "VSM: Project" },
             { "VirtualShadowMapDirtyMask", "VSM: Dirty Mask" },
             { "VirtualShadowMapCullCasters", "VSM: Cull Casters" },
             { "VirtualShadowMapMarkLocal", "VSM: Mark Local" },
             { "VirtualShadowMapProjectLocal", "VSM: Project Local" },
        } };

        constexpr uint32_t kGroup64    = 64; // LocalSize(64, 1, 1) of the page and physical-page kernels
        constexpr uint32_t kScreenTile = 8;  // LocalSize(8, 8, 1) of Mark and Project
        // VirtualShadowMapMarkLocal.shader's modes (RenderSize.w, VSM_MARK_*) and its coarse mip.
        constexpr uint32_t kVsmMarkPixels     = 0;
        constexpr uint32_t kVsmMarkReceivers  = 1;
        constexpr uint32_t kVsmMarkCoarse     = 2;
        constexpr uint32_t kVsmLocalCoarseMip = 6; // VSM_LOCAL_COARSE_MIP

        template <class T>
        std::span<const std::byte> Bytes( const std::vector<T>& values )
        {
            return std::as_bytes( std::span<const T>( values ) );
        }
    } // namespace

    glm::mat4 VsmLightBasis( const glm::vec3& lightDirection )
    {
        const glm::vec3 direction = glm::normalize( lightDirection );
        const glm::vec3 up =
             glm::abs( direction.y ) > 0.99f ? glm::vec3( 0.0f, 0.0f, 1.0f ) : glm::vec3( 0.0f, 1.0f, 0.0f );
        return glm::lookAt( -direction, glm::vec3( 0.0f ), up );
    }

    VsmClipmapLevel MakeVsmClipmapLevel( const glm::mat4& lightBasis, const glm::vec3& camera,
                                         const uint32_t levelIndex )
    {
        VsmClipmapLevel level;
        level.Radius     = std::ldexp( 1.0f, kVsmClipmapFirstLevel + static_cast<int>( levelIndex ) );
        level.PageWorld  = 2.0f * level.Radius / static_cast<float>( kVsmLevelPages );
        level.DepthRange = 2.0f * level.Radius * kVsmClipmapZRangeScale;

        const glm::vec3  cameraLight = glm::vec3( lightBasis * glm::vec4( camera, 1.0f ) );
        const glm::ivec2 centrePage( static_cast<int>( std::floor( cameraLight.x / level.PageWorld ) ),
                                     static_cast<int>( std::floor( cameraLight.y / level.PageWorld ) ) );
        level.WindowOrigin = centrePage - glm::ivec2( static_cast<int>( kVsmLevelPages / 2 ) );

        // The depth centre moves in quarters of the half range: the camera stays at least a quarter range from
        // either end, and a level's cached depth survives every camera move inside a step.
        const float depthStep = 0.25f * 0.5f * level.DepthRange;
        level.DepthCentre     = std::round( cameraLight.z / depthStep ) * depthStep;

        const float left   = static_cast<float>( level.WindowOrigin.x ) * level.PageWorld;
        const float bottom = static_cast<float>( level.WindowOrigin.y ) * level.PageWorld;
        const float side   = static_cast<float>( kVsmLevelPages ) * level.PageWorld;
        const float half   = 0.5f * level.DepthRange;
        // The view looks down -z: view z = -zNear maps to 0 (toward the sun), -zFar to 1.
        const glm::mat4 projection =
             glm::orthoRH_ZO( left, left + side, bottom, bottom + side, -( level.DepthCentre + half ),
                              -( level.DepthCentre - half ) );
        level.ViewProjection = projection * lightBasis;
        return level;
    }

    Common::BoolResultStr VirtualShadowMapRenderer::Initialize()
    {
        for ( uint32_t k = 0; k < KernelCount; ++k )
        {
            KernelProgram& kernel = m_Kernels[k];
            kernel.Program = Runtime::ResourceRegistry::GetShaderService()->GetByName( kKernelNames[k].Shader );
            if ( !kernel.Program )
                return Common::MakeError(
                     std::format( "Shadows: Virtual Shadow Map: shader {} not found", kKernelNames[k].Shader ) );
            const auto pipeline =
                 ComputePipeline::Create( { .Shader = kernel.Program, .DebugName = kKernelNames[k].Shader } );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            kernel.Pipeline = pipeline.GetValue();
        }
        return BOOLSUCCESS;
    }

    bool VirtualShadowMapRenderer::AcquirePersistent( RDG::Builder& graph, const uint32_t physicalPages,
                                                      const uint32_t localViews, RDG::TextureRef& pool,
                                                      RDG::BufferRef& pageTable, RDG::BufferRef& owners )
    {
        if ( m_PhysicalPages == physicalPages && m_LocalViews == localViews )
        {
            pool      = graph.RegisterExternal( m_Pool, "VSM.PhysicalPool" );
            pageTable = graph.RegisterExternal( m_PageTable, "VSM.PageTable" );
            owners    = graph.RegisterExternal( m_Owners, "VSM.PhysicalOwners" );
            return false;
        }
        m_Pool      = {};
        m_PageTable = {};
        m_Owners    = {};

        const uint32_t rows = Common::Math::DivideRoundUp( physicalPages, kVsmPoolPagesPerRow );
        pool = graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width  = kVsmPoolPagesPerRow * kVsmPageSize,
                                                                  .Height = rows * kVsmPageSize },
                                                      .Format = Core::Formats::ImageFormat::R32_UINT },
                                    "VSM.PhysicalPool" );
        // Written by "VSM: Clear Pages" and the casters, read by "VSM: Project": the last use of a frame is the
        // projection's read, the next frame's first the clear's write.
        graph.Extract( pool, m_Pool, RDG::Access::StorageRead, { RDG::Access::StorageWrite } );

        // Every entry unmapped (flags 0), every physical page free: the pool's texels are never read before a
        // clear.
        // The sun's levels, then kVsmLocalViewEntries per local view slot.
        const std::vector<VsmPageEntryGpu> entries(
             static_cast<std::size_t>( kVsmClipmapLevelCount ) * kVsmPagesPerLevel +
             static_cast<std::size_t>( localViews ) * kVsmLocalViewEntries );
        pageTable = graph.CreateBuffer( RDG::BufferDesc{ .Bytes = entries.size() * sizeof( VsmPageEntryGpu ) },
                                        "VSM.PageTable" );
        graph.QueueBufferUpload( pageTable, Bytes( entries ) );
        graph.Extract( pageTable, m_PageTable, RDG::Access::StorageWrite );

        const std::vector<uint32_t> unowned( physicalPages, kVsmInvalid );
        owners = graph.CreateBuffer( RDG::BufferDesc{ .Bytes = unowned.size() * sizeof( uint32_t ) },
                                     "VSM.PhysicalOwners" );
        graph.QueueBufferUpload( owners, Bytes( unowned ) );
        graph.Extract( owners, m_Owners, RDG::Access::StorageWrite );

        m_PhysicalPages = physicalPages;
        m_LocalViews    = localViews;
        return true;
    }

    uint32_t VirtualShadowMapRenderer::PlaceLocalLights( const std::span<const VirtualShadowMapLocalLight> locals,
                                                         const glm::vec3& camera, const float pixelFootprint,
                                                         const float lodBias, const uint64_t frameIndex,
                                                         std::vector<VsmLocalLightGpu>& gpuLights,
                                                         std::vector<VsmLocalRaster>&   rasters )
    {
        // Lights gone for kVsmLocalRetireFrames give their slots back; the rest start the frame unchanged.
        std::erase_if( m_LocalPlacements, [frameIndex]( const LocalPlacement& placement )
                       { return placement.LastFrame + kVsmLocalRetireFrames < frameIndex; } );
        for ( LocalPlacement& placement : m_LocalPlacements )
        {
            placement.Reset      = false;
            placement.FrameLight = kVsmInvalid;
        }

        uint32_t capacity = std::max( m_LocalViews, kVsmMinLocalViews );
        // The first run of @p views free view slots, growing the table when there is none.
        const auto placeViews = [this, &capacity]( const uint32_t views )
        {
            std::vector<bool> used( capacity, false );
            for ( const LocalPlacement& placement : m_LocalPlacements )
                for ( uint32_t v = 0; v < placement.Views; ++v )
                    if ( placement.FirstView + v < capacity )
                        used[placement.FirstView + v] = true;
            uint32_t run = 0;
            for ( uint32_t slot = 0; slot < capacity; ++slot )
            {
                run = used[slot] ? 0u : run + 1u;
                if ( run == views )
                    return slot + 1u - views;
            }
            const uint32_t first = capacity;
            while ( capacity < first + views )
                capacity *= 2u;
            return first;
        };

        gpuLights.clear();
        rasters.clear();
        gpuLights.reserve( locals.size() );
        for ( std::size_t i = 0; i < locals.size(); ++i )
        {
            const VirtualShadowMapLocalLight& light = locals[i];
            const bool     point = light.CosOuter <= kVsmLocalPointCone + 0.5f;
            const uint32_t views = point ? kPointShadowFaces : 1u;
            const float    farPlane  = std::max( light.Radius, 1.0f );
            const float    nearPlane = LocalShadowNear( farPlane, light.SourceRadius );
            // The atlas's views (LocalShadows.hpp), at the virtual map's side: the guard band is a fraction of a
            // texel there.
            std::array<glm::mat4, 6> viewProjection{};
            float                    tanHalf = GuardedTanHalf( 1.0f, kVsmVirtualResolution );
            if ( point )
            {
                for ( uint32_t face = 0; face < kPointShadowFaces; ++face )
                    viewProjection[face] =
                         PointShadowFaceViewProj( light.Position, face, nearPlane, farPlane, kVsmVirtualResolution );
            }
            else
            {
                const float outer = std::acos( std::clamp( light.CosOuter, -1.0f, 1.0f ) );
                viewProjection[0] = SpotShadowViewProj( light.Position, light.SpotDirection, outer, nearPlane,
                                                        farPlane, kVsmVirtualResolution );
                tanHalf           = GuardedTanHalf(
                     std::tan( std::clamp( outer, glm::radians( 1.0f ), glm::radians( 89.0f ) ) ),
                     kVsmVirtualResolution );
            }
            const float texelAtUnit = 2.0f * tanHalf / static_cast<float>( kVsmVirtualResolution );

            auto placement = std::find_if( m_LocalPlacements.begin(), m_LocalPlacements.end(),
                                           [&light]( const LocalPlacement& p ) { return p.SourceId == light.SourceId; } );
            if ( placement != m_LocalPlacements.end() && placement->Views != views )
            {
                m_LocalPlacements.erase( placement ); // a point became a spot or back: new slots
                placement = m_LocalPlacements.end();
            }
            if ( placement == m_LocalPlacements.end() )
            {
                LocalPlacement fresh;
                fresh.SourceId  = light.SourceId;
                fresh.Views     = views;
                fresh.FirstView = placeViews( views );
                m_LocalPlacements.push_back( fresh );
                placement = std::prev( m_LocalPlacements.end() );
            }
            // A light that moved, turned or changed its cone or reach redraws every page (UE: a moved local
            // light invalidates its whole virtual shadow map).
            if ( placement->ViewProjection != viewProjection || placement->Near != nearPlane ||
                 placement->Far != farPlane )
                placement->Reset = true;
            placement->ViewProjection = viewProjection;
            placement->Near           = nearPlane;
            placement->Far            = farPlane;
            placement->TexelAtUnit    = texelAtUnit;
            placement->LastFrame      = frameIndex;
            placement->FrameLight     = static_cast<uint32_t>( i );

            // The finest mip any receiver of the light can ask for (VsmLocalMip): the nearest receiver to the
            // camera at the far end of the light's reach.
            const float nearestToCamera = std::max( glm::length( camera - light.Position ) - farPlane, 1.0f );
            const float ratio = nearestToCamera * pixelFootprint / std::max( farPlane * texelAtUnit, 1.0e-6f );
            const float finest =
                 std::clamp( std::floor( std::log2( std::max( ratio, 1.0e-6f ) ) + lodBias ), 0.0f,
                             static_cast<float>( kVsmLocalMipCount - 1u ) );
            const auto     fineMip   = static_cast<uint32_t>( finest );
            const uint32_t firstLevel = kVsmClipmapLevelCount + placement->FirstView * kVsmLocalMipCount;

            gpuLights.push_back( VsmLocalLightGpu{
                 .Position = glm::vec4( light.Position, farPlane ),
                 .Spot     = glm::vec4( point ? glm::vec3( 0.0f ) : glm::normalize( light.SpotDirection ),
                                        point ? kVsmLocalPointCone : light.CosOuter ),
                 .Views    = glm::uvec4( firstLevel, views, light.MaskSlot, fineMip ),
                 .Params   = glm::vec4( nearPlane, farPlane, texelAtUnit, light.SourceRadius ) } );
            for ( uint32_t v = 0; v < views; ++v )
                for ( uint32_t mip = fineMip; mip < kVsmLocalMipCount; ++mip )
                    rasters.push_back( VsmLocalRaster{ .Level           = firstLevel + v * kVsmLocalMipCount + mip,
                                                       .ResolutionPages = VsmLocalMipPages( mip ),
                                                       .Light           = static_cast<uint32_t>( i ),
                                                       .View            = v,
                                                       .Mip             = mip,
                                                       .ViewProjection  = viewProjection[v] } );
        }
        return capacity;
    }

    void VirtualShadowMapRenderer::WriteLocalLevels( std::vector<VsmLevelGpu>& levels, const bool resetAll ) const
    {
        for ( const LocalPlacement& placement : m_LocalPlacements )
        {
            for ( uint32_t v = 0; v < placement.Views; ++v )
            {
                const uint32_t view = placement.FirstView + v;
                for ( uint32_t mip = 0; mip < kVsmLocalMipCount; ++mip )
                {
                    const uint32_t level = kVsmClipmapLevelCount + view * kVsmLocalMipCount + mip;
                    levels[level]        = VsmLevelGpu{
                                .ViewProjection = placement.ViewProjection[v],
                                .Window = glm::ivec4( 0, 0, static_cast<int>( mip ), placement.Reset || resetAll ? 1 : 0 ),
                                .Extent = glm::vec4( 0.0f, placement.Far, placement.Near,
                                                     placement.TexelAtUnit * static_cast<float>( 1u << mip ) ),
                                .Pages  = glm::uvec4( kVsmClipmapLevelCount * kVsmPagesPerLevel +
                                                          view * kVsmLocalViewEntries + VsmLocalMipEntryOffset( mip ),
                                                      VsmLocalMipPages( mip ), kVsmLevelKindLocal,
                                                      placement.FrameLight ) };
                }
            }
        }
    }

    void VirtualShadowMapRenderer::ReadStats( const uint32_t slot )
    {
        const RDG::ExternalBuffer& readback = m_StatsReadback[slot];
        if ( !readback.Physical )
            return; // the slot has not been used yet
        const std::span<const std::byte>                   bytes  = readback.Physical->ReadForHost();
        const std::vector<std::pair<uint32_t, uint32_t>>& owners = m_StatsLevelLights[slot];
        const std::size_t levels = std::min( owners.size(), bytes.size() / sizeof( glm::uvec4 ) );
        m_LocalStats.clear();
        for ( std::size_t level = 0; level < levels; ++level )
        {
            const auto [sourceId, views] = owners[level];
            if ( sourceId == 0 )
                continue;
            glm::uvec4 counts{ 0u };
            std::memcpy( &counts, bytes.data() + level * sizeof( glm::uvec4 ), sizeof( counts ) );
            auto stats = std::find_if( m_LocalStats.begin(), m_LocalStats.end(),
                                       [sourceId]( const VsmLocalLightStats& s ) { return s.SourceId == sourceId; } );
            if ( stats == m_LocalStats.end() )
            {
                m_LocalStats.push_back( VsmLocalLightStats{ .SourceId = sourceId, .Views = views } );
                stats = std::prev( m_LocalStats.end() );
            }
            stats->Requested += counts.x;
            stats->Rendered += counts.y;
            stats->Cached += counts.z;
            stats->Coarse += counts.w & 0xFFFFu;
            stats->Receivers += counts.w >> 16u;
        }
    }

    bool VirtualShadowMapRenderer::UpdateClipmap( const glm::vec3& directionToSun, const glm::vec3& camera,
                                                  std::array<VsmLevelGpu, kVsmClipmapLevelCount>& levels )
    {
        const glm::vec3 lightDirection = -glm::normalize( directionToSun );
        // Any turn of the sun redraws every page (UE: a moved light invalidates its whole VSM).
        const bool lightChanged = !m_LightDirection.has_value() || *m_LightDirection != lightDirection;
        m_LightDirection        = lightDirection;
        const glm::mat4 basis   = VsmLightBasis( lightDirection );
        for ( uint32_t i = 0; i < kVsmClipmapLevelCount; ++i )
        {
            const VsmClipmapLevel level      = MakeVsmClipmapLevel( basis, camera, i );
            const bool            depthMoved = !m_DepthCentresValid[i] || m_DepthCentres[i] != level.DepthCentre;
            m_DepthCentres[i]                = level.DepthCentre;
            m_DepthCentresValid[i]           = true;
            levels[i]                        = VsmLevelGpu{ .ViewProjection = level.ViewProjection,
                                                            .Window         = glm::ivec4( level.WindowOrigin,
                                                                                          kVsmClipmapFirstLevel + static_cast<int>( i ),
                                                           depthMoved ? 1 : 0 ),
                                                            .Extent         = glm::vec4( level.PageWorld, level.DepthRange, level.Radius,
                                                                                         level.PageWorld / static_cast<float>( kVsmPageSize ) ),
                                                            .Pages = glm::uvec4( i * kVsmPagesPerLevel, kVsmLevelPages,
                                                                                 kVsmLevelKindClipmap, kVsmInvalid ) };
        }
        return lightChanged;
    }

    void VirtualShadowMapRenderer::ReportOverflow( const uint32_t slot )
    {
        const RDG::ExternalBuffer& readback = m_AllocatorReadback[slot];
        if ( !readback.Physical )
            return; // the slot has not been used yet
        const std::span<const std::byte> bytes = readback.Physical->ReadForHost();
        if ( bytes.size() < kVsmOverflowOffset + sizeof( uint32_t ) )
        {
            LOG_ERROR(
                 "[Shadows] Virtual Shadow Map: the allocator readback holds {} bytes, the overflow count is at "
                 "{}; the pool overflow is not known",
                 bytes.size(), kVsmOverflowOffset );
            return;
        }
        uint32_t overflow = 0;
        std::memcpy( &overflow, bytes.data() + kVsmOverflowOffset, sizeof( overflow ) );
        if ( overflow == m_ReportedOverflow )
            return;
        m_ReportedOverflow = overflow;
        if ( overflow == 0 )
        {
            LOG_INFO( "[Shadows] Virtual Shadow Map: the physical page pool holds every requested page again" );
        }
        else
        {
            LOG_WARN( "[Shadows] Virtual Shadow Map: physical page pool overflow - {} requested page(s) found no "
                      "page among Shadows.VirtualMaxPhysicalPages = {}; their receivers are shaded from a coarser "
                      "level",
                      overflow, m_ReadbackPhysicalPages[slot] );
        }
    }

    Common::ResultStr<VirtualShadowMapFrame> VirtualShadowMapRenderer::AddPagePasses(
         RDG::Builder& graph, const VirtualShadowMapViewInputs& view, const VirtualShadowMapSun* const sun,
         const std::span<const VirtualShadowMapLocalLight> locals, const std::span<const RDG::TextureRef> receiverDepths,
         const VirtualShadowMapQuality& quality, const std::span<const VsmInvalidationBox> invalidations,
         const bool staticCastersChanged )
    {
        if ( !m_Kernels[Project].Pipeline )
            return Common::MakeError<VirtualShadowMapFrame>(
                 "Shadows: Virtual Shadow Map: the pipelines are not initialised" );
        // The sun's pages are marked from the G-buffer alone; the local lights' also from the receivers' depths
        // and their coarse mips, so a view without a G-buffer (the forward shading path) maps local lights only.
        const bool gbuffer = view.Depth.IsValid() && view.Normal.IsValid();
        if ( view.Width == 0 || view.Height == 0 || ( sun != nullptr && !gbuffer ) )
            return Common::MakeError<VirtualShadowMapFrame>(
                 std::format( "Shadows: Virtual Shadow Map: the view has no G-buffer to mark the sun's pages from "
                              "({}x{})",
                              view.Width, view.Height ) );
        if ( !view.NoDepth.IsValid() )
            return Common::MakeError<VirtualShadowMapFrame>(
                 "Shadows: Virtual Shadow Map: the view names no stand-in for an absent depth (NoDepth)" );
        if ( quality.MaxPhysicalPages == 0 )
            return Common::MakeError<VirtualShadowMapFrame>(
                 "Shadows: Virtual Shadow Map: Shadows.VirtualMaxPhysicalPages is 0" );

        // THE SUN'S CLIPMAP, when there is a sun: a turn of the sun redraws its levels (Window.w), not the local
        // lights' pages.
        std::array<VsmLevelGpu, kVsmClipmapLevelCount> clipmap{};
        if ( sun != nullptr )
        {
            const bool lightChanged = UpdateClipmap( sun->DirectionToSun, view.CameraPosition, clipmap );
            if ( lightChanged )
                for ( VsmLevelGpu& level : clipmap )
                    level.Window.w = 1;
        }
        else
        {
            m_LightDirection.reset(); // the next sun starts its levels anew
        }

        // THE LOCAL LIGHTS' VIEWS in the table, and the mips each one draws this frame.
        const float pixelFootprint =
             2.0f / ( std::abs( view.Projection[1][1] ) * static_cast<float>( view.Height ) );
        std::vector<VsmLocalLightGpu> gpuLights;
        VirtualShadowMapFrame         frame;
        const uint32_t                localViews =
             PlaceLocalLights( locals, view.CameraPosition, pixelFootprint, quality.ResolutionLodBias, view.FrameIndex,
                               gpuLights, frame.LocalRasters );

        RDG::BufferRef owners;
        const bool     made = AcquirePersistent( graph, quality.MaxPhysicalPages, localViews, frame.PhysicalPool,
                                                 frame.PageTable, owners );
        const uint32_t resetAll   = made || staticCastersChanged ? 1u : 0u;
        const uint32_t levelCount = kVsmClipmapLevelCount + m_LocalViews * kVsmLocalMipCount;
        frame.LevelCount          = levelCount;
        frame.HasSun              = sun != nullptr;
        frame.LocalLightCount     = static_cast<uint32_t>( gpuLights.size() );

        std::vector<VsmLevelGpu> levels( levelCount );
        if ( sun != nullptr )
            std::copy( clipmap.begin(), clipmap.end(), levels.begin() );
        WriteLocalLevels( levels, resetAll != 0u );
        frame.LevelViewProjection.resize( levelCount, glm::mat4( 1.0f ) );
        for ( uint32_t i = 0; i < levelCount; ++i )
        {
            frame.LevelViewProjection[i] = levels[i].ViewProjection;
            if ( i < kVsmClipmapLevelCount ) // the sun's levels: what the virtual geometry's cut measures in
                frame.LevelTexelWorld[i] = levels[i].Extent.w;
        }

        // VsmFrameData: the view's constants, then the levels.
        const glm::vec3 toSun = sun != nullptr ? glm::normalize( sun->DirectionToSun ) : glm::vec3( 0.0f, 1.0f, 0.0f );
        const VsmFrameGpu header{
             .InvViewProjection = view.InvJitteredViewProjection,
             .CameraFootprint   = glm::vec4( view.CameraPosition, pixelFootprint ),
             .Lod   = glm::vec4( quality.ResolutionLodBias, static_cast<float>( kVsmClipmapLevelCount ),
                                 static_cast<float>( kVsmClipmapFirstLevel ),
                                 static_cast<float>( static_cast<uint32_t>( view.FrameIndex & 0xFFFFu ) ) ),
             .ToSun = glm::vec4( toSun, sun != nullptr ? glm::radians( std::max( sun->SourceAngleDegrees, 0.0f ) )
                                                       : 0.0f ),
             .Smrt  = glm::vec4( static_cast<float>( quality.SmrtRayCount ),
                                 static_cast<float>( quality.SmrtSamplesPerRay ), kVsmNormalBiasTexels,
                                 kVsmSmrtRayLengthPages ),
             .SmrtLocal = glm::vec4( static_cast<float>( quality.SmrtSamplesPerRayLocal ),
                                     kVsmSmrtExtrapolateMaxSlopeLocal, 0.0f, 0.0f ) };
        std::vector<std::byte> frameBytes( sizeof( VsmFrameGpu ) + levels.size() * sizeof( VsmLevelGpu ) );
        std::memcpy( frameBytes.data(), &header, sizeof( header ) );
        std::memcpy( frameBytes.data() + sizeof( header ), levels.data(), levels.size() * sizeof( VsmLevelGpu ) );
        frame.FrameData = graph.CreateBuffer( RDG::BufferDesc{ .Bytes = frameBytes.size() }, "VSM.FrameData" );
        graph.QueueBufferUpload( frame.FrameData, frameBytes );

        // VsmLocalLights (at least one record: a buffer is never empty).
        {
            std::vector<VsmLocalLightGpu> records = gpuLights;
            if ( records.empty() )
                records.emplace_back();
            frame.LocalLights =
                 graph.CreateBuffer( RDG::BufferDesc{ .Bytes = records.size() * sizeof( VsmLocalLightGpu ) },
                                     "VSM.LocalLights" );
            graph.QueueBufferUpload( frame.LocalLights, Bytes( records ) );
        }

        const uint32_t       pages = quality.MaxPhysicalPages;
        const RDG::BufferRef allocator =
             graph.CreateBuffer( RDG::BufferDesc{ .Bytes = kVsmAllocatorBytes }, "VSM.Allocator" );
        const std::vector<uint32_t> zeroCounters( kVsmAllocatorBytes / sizeof( uint32_t ), 0u );
        graph.QueueBufferUpload( allocator, Bytes( zeroCounters ) );
        const RDG::BufferRef lists =
             graph.CreateBuffer( RDG::BufferDesc{ .Bytes = 3ull * pages * sizeof( uint32_t ) }, "VSM.Lists" );
        // "VSM: Dirty Mask"'s per-level page counts, for the host (GetLocalLightStats).
        const std::vector<glm::uvec4> zeroStats( levelCount, glm::uvec4( 0u ) );
        const RDG::BufferRef          levelStats =
             graph.CreateBuffer( RDG::BufferDesc{ .Bytes = zeroStats.size() * sizeof( glm::uvec4 ) },
                                 "VSM.LevelStats" );
        graph.QueueBufferUpload( levelStats, Bytes( zeroStats ) );

        // The overflow and the page counts this slot's previous frame counted, then this frame's handed to the host
        // in their place (read when the slot comes round again).
        const uint32_t slots = std::max( 1u, EngineContext::GetInstance().GetMaxFramesInFlight() );
        if ( m_AllocatorReadback.size() != slots )
        {
            m_AllocatorReadback.assign( slots, RDG::ExternalBuffer{} );
            m_ReadbackPhysicalPages.assign( slots, 0u );
            m_StatsReadback.assign( slots, RDG::ExternalBuffer{} );
            m_StatsLevelLights.assign( slots, {} );
        }
        const uint32_t slot = static_cast<uint32_t>( view.FrameIndex % slots );
        ReportOverflow( slot );
        ReadStats( slot );
        m_AllocatorReadback[slot]     = RDG::ExternalBuffer{};
        m_ReadbackPhysicalPages[slot] = pages;
        graph.Extract( allocator, m_AllocatorReadback[slot], RDG::Access::HostRead );
        m_StatsReadback[slot] = RDG::ExternalBuffer{};
        graph.Extract( levelStats, m_StatsReadback[slot], RDG::Access::HostRead );
        std::vector<std::pair<uint32_t, uint32_t>>& levelLights = m_StatsLevelLights[slot];
        levelLights.assign( levelCount, { 0u, 0u } );
        for ( const LocalPlacement& placement : m_LocalPlacements )
            for ( uint32_t level = kVsmClipmapLevelCount + placement.FirstView * kVsmLocalMipCount;
                  level < kVsmClipmapLevelCount + ( placement.FirstView + placement.Views ) * kVsmLocalMipCount;
                  ++level )
                levelLights[level] = { placement.SourceId, placement.Views };

        // VsmRasterLevel of the levels drawn this frame: the sun's, and the local mips (LocalRasters).
        frame.RasterLevel.resize( levelCount );
        const auto rasterLevel = [&graph, &frame]( const uint32_t level )
        {
            const glm::uvec4 value( level, 0u, 0u, 0u );
            frame.RasterLevel[level] = graph.CreateBuffer( RDG::BufferDesc{ .Bytes = sizeof( value ) },
                                                           std::format( "VSM.RasterLevel{}", level ) );
            graph.QueueBufferUpload( frame.RasterLevel[level],
                                     std::as_bytes( std::span<const glm::uvec4>( &value, 1 ) ) );
        };
        if ( sun != nullptr )
            for ( uint32_t i = 0; i < kVsmClipmapLevelCount; ++i )
                rasterLevel( i );
        for ( const VsmLocalRaster& raster : frame.LocalRasters )
            rasterLevel( raster.Level );

        const RDG::BufferRef  frameData = frame.FrameData;
        const RDG::BufferRef  pageTable = frame.PageTable;
        const RDG::TextureRef pool      = frame.PhysicalPool;
        constexpr auto        kR        = RDG::Access::StorageRead;
        constexpr auto        kW        = RDG::Access::StorageWrite;

        // One dispatch of kernel @p k: its block declared by @p declare, @p push (16 B) when the kernel has one.
        const auto dispatch =
             [&]( const Kernel k, auto&& declare, const std::optional<glm::uvec4> push, const glm::uvec3 groups )
        {
            const KernelProgram& kernel = m_Kernels[k];
            graph.AddPass(
                 kKernelNames[k].Pass, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     RDG::BindingBlockBuilder block =
                          pass.Bindings( kernel.Layout.Get( kernel.Program ),
                                         Renderer::GetPipelineRouteFill( *kernel.Pipeline ) );
                     declare( block );
                     if ( push )
                         block.PushConstantBytes( static_cast<uint32_t>( sizeof( glm::uvec4 ) ) );
                 },
                 [pipeline = kernel.Pipeline, push, groups,
                  name     = kKernelNames[k].Pass]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS_DYNAMIC( name );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     if ( push )
                         bindings.PushConstants( &*push, static_cast<uint32_t>( sizeof( glm::uvec4 ) ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, groups.x, groups.y,
                                                                     groups.z );
                 } );
        };
        const uint32_t entryGroups = Common::Math::DivideRoundUp( kVsmPagesPerLevel, kGroup64 );

        dispatch(
             Invalidate,
             [&]( RDG::BindingBlockBuilder& block )
             {
                 block.Storage( VirtualShadowMapPagesShaderParameters::VsmFrameData, frameData, kR )
                      .Storage( VirtualShadowMapPagesShaderParameters::VsmPageTable, pageTable, kW )
                      .Storage( VirtualShadowMapPagesShaderParameters::VsmPhysicalOwners, owners, kW );
             },
             glm::uvec4( resetAll, 0u, 0u, 0u ), glm::uvec3( entryGroups, levelCount, 1u ) );

        // Nothing to invalidate per caster when every page was just dropped.
        if ( !invalidations.empty() && resetAll == 0u )
        {
            std::vector<glm::vec4> boxes;
            boxes.reserve( invalidations.size() * 2 );
            for ( const VsmInvalidationBox& box : invalidations )
            {
                boxes.emplace_back( box.Min, 0.0f );
                boxes.emplace_back( box.Max, 0.0f );
            }
            const RDG::BufferRef boxBuffer = graph.CreateBuffer(
                 RDG::BufferDesc{ .Bytes = boxes.size() * sizeof( glm::vec4 ) }, "VSM.Invalidations" );
            graph.QueueBufferUpload( boxBuffer, Bytes( boxes ) );
            dispatch(
                 InvalidateCasters,
                 [&]( RDG::BindingBlockBuilder& block )
                 {
                     block.Storage( VirtualShadowMapPagesShaderParameters::VsmFrameData, frameData, kR )
                          .Storage( VirtualShadowMapPagesShaderParameters::VsmPageTable, pageTable, kW )
                          .Storage( VirtualShadowMapPagesShaderParameters::VsmInvalidations, boxBuffer, kR );
                 },
                 std::nullopt,
                 glm::uvec3( static_cast<uint32_t>( invalidations.size() ), levelCount, 1u ) );
        }

        if ( sun != nullptr )
            dispatch(
                 Mark,
                 [&]( RDG::BindingBlockBuilder& block )
                 {
                     block.Storage( VirtualShadowMapPagesShaderParameters::VsmFrameData, frameData, kR )
                          .Storage( VirtualShadowMapPagesShaderParameters::VsmPageTable, pageTable, kW )
                          .Sampler( VirtualShadowMapPagesShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .Texture( VirtualShadowMapPagesShaderParameters::GBufferDepthTexture, view.Depth,
                                    RDG::Access::SampledCompute, RDG::SubresourceRange::All() );
                 },
                 glm::uvec4( view.Width, view.Height, 0u, 0u ),
                 glm::uvec3( Common::Math::DivideRoundUp( view.Width, kScreenTile ),
                             Common::Math::DivideRoundUp( view.Height, kScreenTile ), 1u ) );

        if ( !gpuLights.empty() )
        {
            const RDG::BufferRef localLights = frame.LocalLights;
            const auto           lightCount  = static_cast<uint32_t>( gpuLights.size() );
            // One "VSM: Mark Local" kernel, three receivers (VirtualShadowMapMarkLocal.shader VSM_MARK_*): the
            // G-buffer's pixels, each forward receiver's own depth, every light's coarse mip.
            const auto markLocal = [&]( const RDG::TextureRef depth, const uint32_t mode, const glm::uvec3 groups )
            {
                dispatch(
                     MarkLocal,
                     [&]( RDG::BindingBlockBuilder& block )
                     {
                         block.Storage( VirtualShadowMapPagesShaderParameters::VsmFrameData, frameData, kR )
                              .Storage( VirtualShadowMapPagesShaderParameters::VsmPageTable, pageTable, kW )
                              .Sampler( VirtualShadowMapPagesShaderParameters::PointClampSampler,
                                        RDG::SamplerDesc::PointClamp() )
                              .Texture( VirtualShadowMapPagesShaderParameters::GBufferDepthTexture, depth,
                                        RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                              .Storage( VirtualShadowMapPagesShaderParameters::VsmLocalLights, localLights, kR );
                     },
                     glm::uvec4( view.Width, view.Height, lightCount, mode ), groups );
            };
            const glm::uvec3 screen( Common::Math::DivideRoundUp( view.Width, kScreenTile ),
                                     Common::Math::DivideRoundUp( view.Height, kScreenTile ), 1u );
            // The view's opaque depth: the G-buffer's, or the forward path's depth prepass (no normal: the local
            // lights' pages need the pixel's position alone).
            if ( view.Depth.IsValid() )
                markLocal( view.Depth, kVsmMarkPixels, screen );
            for ( const RDG::TextureRef& receiver : receiverDepths )
                if ( receiver.IsValid() )
                    markLocal( receiver, kVsmMarkReceivers, screen );
            // The coarse mip has at most 2 x 2 pages per view (VSM_LOCAL_COARSE_MIP), a light at most six views:
            // one 8 x 8 group per light covers them.
            static_assert( VsmLocalMipPages( kVsmLocalCoarseMip ) * VsmLocalMipPages( kVsmLocalCoarseMip ) <=
                                kScreenTile &&
                           kPointShadowFaces <= kScreenTile );
            // The coarse mode reads no depth: the binding takes the G-buffer's, or the view's stand-in without one.
            markLocal( view.Depth.IsValid() ? view.Depth : view.NoDepth, kVsmMarkCoarse,
                       glm::uvec3( 1u, 1u, lightCount ) );
        }

        dispatch(
             Gather,
             [&]( RDG::BindingBlockBuilder& block )
             {
                 block.Storage( VirtualShadowMapPagesShaderParameters::VsmPageTable, pageTable, kR )
                      .Storage( VirtualShadowMapPagesShaderParameters::VsmPhysicalOwners, owners, kR )
                      .Storage( VirtualShadowMapPagesShaderParameters::VsmAllocator, allocator, kW )
                      .Storage( VirtualShadowMapPagesShaderParameters::VsmLists, lists, kW );
             },
             glm::uvec4( pages, 0u, 0u, 0u ),
             glm::uvec3( Common::Math::DivideRoundUp( pages, kGroup64 ), 1u, 1u ) );

        dispatch(
             Allocate,
             [&]( RDG::BindingBlockBuilder& block )
             {
                 block.Storage( VirtualShadowMapPagesShaderParameters::VsmFrameData, frameData, kR )
                      .Storage( VirtualShadowMapPagesShaderParameters::VsmPageTable, pageTable, kW )
                      .Storage( VirtualShadowMapPagesShaderParameters::VsmPhysicalOwners, owners, kW )
                      .Storage( VirtualShadowMapPagesShaderParameters::VsmAllocator, allocator, kW )
                      .Storage( VirtualShadowMapPagesShaderParameters::VsmLists, lists, kW );
             },
             glm::uvec4( pages, 0u, 0u, 0u ), glm::uvec3( entryGroups, levelCount, 1u ) );

        // The cells of each level holding a page drawn this frame, for the casters' cull.
        frame.DirtyMask =
             graph.CreateBuffer( RDG::BufferDesc{ .Bytes = uint64_t{ levelCount } * kVsmMaskWordsPerLevel *
                                                           sizeof( uint32_t ) },
                                 "VSM.DirtyMask" );
        const std::vector<uint32_t> zeroMask( size_t{ levelCount } * kVsmMaskWordsPerLevel, 0u );
        graph.QueueBufferUpload( frame.DirtyMask, Bytes( zeroMask ) );
        const RDG::BufferRef dirtyMask = frame.DirtyMask;
        dispatch(
             DirtyMask,
             [&]( RDG::BindingBlockBuilder& block )
             {
                 block.Storage( VirtualShadowMapPagesShaderParameters::VsmFrameData, frameData, kR )
                      .Storage( VirtualShadowMapPagesShaderParameters::VsmPageTable, pageTable, kR )
                      .Storage( VirtualShadowMapPagesShaderParameters::VsmDirtyMask, dirtyMask, kW )
                      .Storage( VirtualShadowMapPagesShaderParameters::VsmLevelStats, levelStats, kW );
             },
             std::nullopt, glm::uvec3( entryGroups, levelCount, 1u ) );

        dispatch(
             ClearArgs, [&]( RDG::BindingBlockBuilder& block )
             { block.Storage( VirtualShadowMapPagesShaderParameters::VsmAllocator, allocator, kW ); },
             std::nullopt, glm::uvec3( 1u ) );

        // "VSM: Clear Pages": one group per dirty page, sized on the GPU.
        {
            const KernelProgram& kernel = m_Kernels[Clear];
            const glm::uvec4     push( pages, 0u, 0u, 0u );
            graph.AddPass(
                 kKernelNames[Clear].Pass, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Read( allocator, RDG::Access::IndirectArgs );
                     pass.Bindings( kernel.Layout.Get( kernel.Program ),
                                    Renderer::GetPipelineRouteFill( *kernel.Pipeline ) )
                          .Storage( VirtualShadowMapPoolShaderParameters::VsmPageTable, pageTable, kR )
                          .Storage( VirtualShadowMapPoolShaderParameters::VsmLists, lists, kR )
                          .Storage( VirtualShadowMapPoolShaderParameters::PhysicalPoolTexture, pool, kW )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( push ) ) );
                 },
                 [pipeline = kernel.Pipeline, push,
                  allocator]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "VSM: Clear Pages" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::DispatchComputeIndirect( bindings, *pipeline, allocator,
                                                               kVsmClearArgsOffset );
                 } );
        }
        return Common::MakeSuccess( frame );
    }

    Common::ResultStr<VsmCulledCasters>
    VirtualShadowMapRenderer::AddCasterCulling( RDG::Builder& graph, const VirtualShadowMapFrame& frame,
                                                const uint32_t level, const VsmCasterCull& cull ) const
    {
        const KernelProgram& kernel = m_Kernels[CullCasters];
        if ( !kernel.Pipeline )
            return Common::MakeError<VsmCulledCasters>(
                 "Shadows: Virtual Shadow Map: the pipelines are not initialised" );
        if ( cull.Instances.empty() || cull.Draws.empty() || cull.Arguments.empty() || cull.RemapSize == 0 )
            return Common::MakeError<VsmCulledCasters>(
                 std::format( "VSM: Cull Casters L{}: nothing to cull ({} instances, {} draws, {} argument words)",
                              level, cull.Instances.size(), cull.Draws.size(), cull.Arguments.size() ) );
        const auto instances = static_cast<uint32_t>( cull.Instances.size() );
        // One thread per instance, the groups spread over y past the dispatch limit of x.
        constexpr uint32_t kMaxGroupsX = 65535u;
        const uint32_t     groups      = Common::Math::DivideRoundUp( instances, kGroup64 );
        const glm::uvec3 grid( std::min( groups, kMaxGroupsX ), Common::Math::DivideRoundUp( groups, kMaxGroupsX ),
                               1u );

        VsmCulledCasters culled;
        culled.Arguments =
             graph.CreateBuffer( RDG::BufferDesc{ .Bytes = cull.Arguments.size() * sizeof( uint32_t ) },
                                 std::format( "VSM.CasterArguments{}", level ) );
        graph.QueueBufferUpload( culled.Arguments, Bytes( cull.Arguments ) );
        culled.InstanceRemap =
             graph.CreateBuffer( RDG::BufferDesc{ .Bytes = uint64_t{ cull.RemapSize } * sizeof( uint32_t ) },
                                 std::format( "VSM.InstanceRemap{}", level ) );
        const RDG::BufferRef draws =
             graph.CreateBuffer( RDG::BufferDesc{ .Bytes = cull.Draws.size() * sizeof( VsmCullDrawGpu ) },
                                 std::format( "VSM.CullDraws{}", level ) );
        graph.QueueBufferUpload( draws, Bytes( cull.Draws ) );
        const RDG::BufferRef boxes =
             graph.CreateBuffer( RDG::BufferDesc{ .Bytes = cull.Instances.size() * sizeof( VsmCullInstanceGpu ) },
                                 std::format( "VSM.CullInstances{}", level ) );
        graph.QueueBufferUpload( boxes, Bytes( cull.Instances ) );

        // x = level, y = instance count, z = threads per row of groups (the y stride of the instance index).
        const glm::uvec4 push( level, instances, grid.x * kGroup64, 0u );
        graph.AddPass(
             std::format( "VSM: Cull Casters L{}", level ), RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( kernel.Layout.Get( kernel.Program ),
                                Renderer::GetPipelineRouteFill( *kernel.Pipeline ) )
                      .Storage( VirtualShadowMapCullShaderParameters::VsmFrameData, frame.FrameData,
                                RDG::Access::StorageRead )
                      .Storage( VirtualShadowMapCullShaderParameters::VsmDirtyMask, frame.DirtyMask,
                                RDG::Access::StorageRead )
                      .Storage( VirtualShadowMapCullShaderParameters::VsmCullInstances, boxes,
                                RDG::Access::StorageRead )
                      .Storage( VirtualShadowMapCullShaderParameters::VsmCullDraws, draws,
                                RDG::Access::StorageRead )
                      .Storage( VirtualShadowMapCullShaderParameters::VsmCasterArguments, culled.Arguments,
                                RDG::Access::StorageWrite )
                      .Storage( VirtualShadowMapCullShaderParameters::VsmInstanceRemap, culled.InstanceRemap,
                                RDG::Access::StorageWrite )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( push ) ) );
             },
             [pipeline = kernel.Pipeline, push, grid]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 DESERT_PROFILE_PASS( "VSM: Cull Casters" );
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                 return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, grid.x, grid.y, grid.z );
             } );
        return Common::MakeSuccess( culled );
    }

    Common::ResultStr<RDG::TextureRef>
    VirtualShadowMapRenderer::AddProjection( RDG::Builder& graph, const VirtualShadowMapFrame& frame,
                                             const VirtualShadowMapViewInputs& view )
    {
        const KernelProgram& kernel = m_Kernels[Project];
        if ( !kernel.Pipeline )
            return Common::MakeError<RDG::TextureRef>(
                 "Shadows: Virtual Shadow Map: the pipelines are not initialised" );
        const RDG::TextureRef mask =
             graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = view.Width, .Height = view.Height },
                                                    .Format = ViewTargetFormats::kDenoiserSignal },
                                  kShadowMaskSunName );
        graph.SetFaultDefault( mask, RDG::FaultDefault::White );
        const uint32_t width  = view.Width;
        const uint32_t height = view.Height;
        graph.AddPass(
             kKernelNames[Project].Pass, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( kernel.Layout.Get( kernel.Program ),
                                Renderer::GetPipelineRouteFill( *kernel.Pipeline ) )
                      .Storage( VirtualShadowMapProjectShaderParameters::VsmFrameData, frame.FrameData,
                                RDG::Access::StorageRead )
                      .Storage( VirtualShadowMapProjectShaderParameters::VsmPageTable, frame.PageTable,
                                RDG::Access::StorageRead )
                      .Storage( VirtualShadowMapProjectShaderParameters::PhysicalPoolTexture, frame.PhysicalPool,
                                RDG::Access::StorageRead )
                      .Sampler( VirtualShadowMapProjectShaderParameters::PointClampSampler,
                                RDG::SamplerDesc::PointClamp() )
                      .Texture( VirtualShadowMapProjectShaderParameters::GBufferDepthTexture, view.Depth,
                                RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Texture( VirtualShadowMapProjectShaderParameters::GBufferNormalTexture, view.Normal,
                                RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Storage( VirtualShadowMapProjectShaderParameters::ShadowMaskTexture, mask,
                                RDG::Access::StorageWrite );
             },
             [pipeline = kernel.Pipeline, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 DESERT_PROFILE_PASS( "VSM: Project" );
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::DispatchCompute(
                      bindings, *pipeline, Common::Math::DivideRoundUp( width, kScreenTile ),
                      Common::Math::DivideRoundUp( height, kScreenTile ), 1 );
             } );
        return Common::MakeSuccess( mask );
    }

    Common::BoolResultStr VirtualShadowMapRenderer::AddLocalProjection( RDG::Builder&                     graph,
                                                                        const VirtualShadowMapFrame&      frame,
                                                                        const VirtualShadowMapViewInputs& view,
                                                                        const RDG::TextureRef             masks )
    {
        const KernelProgram& kernel = m_Kernels[ProjectLocal];
        if ( !kernel.Pipeline )
            return Common::MakeError( "Shadows: Virtual Shadow Map: the pipelines are not initialised" );
        if ( frame.LocalLightCount == 0 )
            return BOOLSUCCESS; // no local light in view: the composite reads no layer
        if ( !masks.IsValid() )
            return Common::MakeError( "Shadows: Virtual Shadow Map: the local lights have no screen-mask array" );
        const glm::uvec4 push( view.Width, view.Height, frame.LocalLightCount, 0u );
        graph.AddPass(
             kKernelNames[ProjectLocal].Pass, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( kernel.Layout.Get( kernel.Program ),
                                Renderer::GetPipelineRouteFill( *kernel.Pipeline ) )
                      .Storage( VirtualShadowMapProjectLocalShaderParameters::VsmFrameData, frame.FrameData,
                                RDG::Access::StorageRead )
                      .Storage( VirtualShadowMapProjectLocalShaderParameters::VsmPageTable, frame.PageTable,
                                RDG::Access::StorageRead )
                      .Storage( VirtualShadowMapProjectLocalShaderParameters::PhysicalPoolTexture, frame.PhysicalPool,
                                RDG::Access::StorageRead )
                      .Sampler( VirtualShadowMapProjectLocalShaderParameters::PointClampSampler,
                                RDG::SamplerDesc::PointClamp() )
                      .Texture( VirtualShadowMapProjectLocalShaderParameters::GBufferDepthTexture, view.Depth,
                                RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Texture( VirtualShadowMapProjectLocalShaderParameters::GBufferNormalTexture, view.Normal,
                                RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                      .Storage( VirtualShadowMapProjectLocalShaderParameters::VsmLocalLights, frame.LocalLights,
                                RDG::Access::StorageRead )
                      .Storage( VirtualShadowMapProjectLocalShaderParameters::LocalShadowMasksTexture, masks,
                                RDG::Access::StorageWrite )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( push ) ) );
             },
             [pipeline = kernel.Pipeline, push]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 DESERT_PROFILE_PASS( "VSM: Project Local" );
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                 return Renderer::DispatchCompute(
                      bindings, *pipeline, Common::Math::DivideRoundUp( push.x, kScreenTile ),
                      Common::Math::DivideRoundUp( push.y, kScreenTile ), push.z );
             } );
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::System
