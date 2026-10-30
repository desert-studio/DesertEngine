#include <Engine/Graphic/GpuDriven/InstanceCullValidation.hpp>

#include <Common/Core/GlslAsCpp.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Math/AABB.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Engine/Geometry/MeshBounds.hpp>
#include <Engine/Graphic/GpuDriven/GpuScene.hpp>
#include <Engine/Graphic/InstanceCullDistance.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/VirtualShadowMap.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

// The shadow wind distance's rule as InstanceCull.shader runs it: the same text, compiled as C++ (the shader root
// is on the Engine include path, premake5.lua). At global scope, as MeshRendererShadow.cpp includes it, so a unity
// blob holding both (one copy behind the include guard) finds it from either.
namespace
{
    using glm::dot;
    using glm::mat4;
    using glm::vec3;
    using glm::vec4;
    DESERT_GLSL_AS_CPP_BEGIN
#include <Common/ShadowWind.glslh>
    DESERT_GLSL_AS_CPP_END
} // namespace

namespace Desert::Graphic::GpuDriven
{
    namespace
    {
        constexpr uint32_t kComparedBits = CullVisibility::kInView | CullVisibility::kLodMask;
        constexpr uint64_t kMaskWords =
             static_cast<uint64_t>( System::kVsmClipmapLevelCount ) * System::kVsmMaskWordsPerLevel;

        // Table references the CPU cannot follow (the GPU would read outside the table): counted per view, so a
        // corrupt row is named in the log rather than taken for a cull mismatch.
        struct BadRows
        {
            uint32_t Primitive   = 0;
            uint32_t WorldBounds = 0;
            uint32_t LodSizes    = 0;
        };

        // A page coordinate clamped to [-1, kVsmLevelPages] before the int conversion: GLSL's ivec2(floor(x)) of a
        // corner far off the level is still clamped by the caller; in C++ an out-of-range float -> int is UB.
        int PageOf( const float page )
        {
            if ( std::isnan( page ) )
                return -1;
            return static_cast<int>(
                 std::clamp( std::floor( page ), -1.0f, static_cast<float>( System::kVsmLevelPages ) ) );
        }

        // InstanceCull.shader CoversDrawnVsmCell (:138-171), over the host copy of "VSM: Dirty Mask".
        bool CoversDrawnVsmCell( const GpuCullView& view, const glm::vec3& boxMin, const glm::vec3& boxMax,
                                 std::span<const uint32_t> mask )
        {
            glm::vec2 first( 1.0e30f );
            glm::vec2 last( -1.0e30f );
            float     nearest = 1.0e30f;
            for ( uint32_t c = 0; c < 8; ++c )
            {
                const glm::vec3 corner( ( c & 1u ) != 0u ? boxMax.x : boxMin.x,
                                        ( c & 2u ) != 0u ? boxMax.y : boxMin.y,
                                        ( c & 4u ) != 0u ? boxMax.z : boxMin.z );
                const glm::vec4 clip = view.ViewProjection * glm::vec4( corner, 1.0f );
                const glm::vec2 page =
                     ( glm::vec2( clip ) * 0.5f + 0.5f ) * static_cast<float>( System::kVsmLevelPages );
                first   = glm::min( first, page );
                last    = glm::max( last, page );
                nearest = std::min( nearest, clip.z );
            }
            if ( nearest > 1.0f )
                return false;
            const int        lastPage = static_cast<int>( System::kVsmLevelPages ) - 1;
            const glm::ivec2 lowPage( std::max( PageOf( first.x ), 0 ), std::max( PageOf( first.y ), 0 ) );
            const glm::ivec2 highPage( std::min( PageOf( last.x ), lastPage ),
                                       std::min( PageOf( last.y ), lastPage ) );
            if ( lowPage.x > highPage.x || lowPage.y > highPage.y )
                return false;
            const int        cellPages = static_cast<int>( System::kVsmMaskCellPages );
            const glm::ivec2 lowCell   = lowPage / cellPages;
            const glm::ivec2 highCell  = highPage / cellPages;
            for ( int y = lowCell.y; y <= highCell.y; ++y )
            {
                for ( int x = lowCell.x; x <= highCell.x; ++x )
                {
                    const uint32_t index =
                         static_cast<uint32_t>( y ) * System::kVsmMaskCells + static_cast<uint32_t>( x );
                    const uint64_t word =
                         static_cast<uint64_t>( view.VsmLevel ) * System::kVsmMaskWordsPerLevel + index / 32u;
                    if ( word < mask.size() && ( mask[word] & ( 1u << ( index % 32u ) ) ) != 0u )
                        return true;
                }
            }
            return false;
        }
    } // namespace

    void InstanceCullValidation::Refuse( std::string message )
    {
        if ( message != m_LastError )
        {
            LOG_ERROR( "[GPUDriven] Cull validation: {}", message );
        }
        m_LastError = std::move( message );
    }

    void InstanceCullValidation::AddFrame( RDG::Builder& graph, const uint64_t frameIndex, const GpuScene& scene,
                                           std::span<const CullViewInput> views,
                                           std::span<const std::string> viewNames, const OcclusionCarry& carry,
                                           const RDG::BufferRef vsmDirtyMask )
    {
        // The pattern of VirtualShadowMapRenderer::ReportOverflow: the slot's previous frame has completed (the
        // frame start waited its fence), so it is read first, then handed this frame's words in its place.
        const uint32_t slots = std::max( 1u, EngineContext::GetInstance().GetMaxFramesInFlight() );
        if ( m_Slots.size() != slots )
        {
            m_Slots.clear();
            m_Slots.resize( slots );
        }
        Slot& slot = m_Slots[static_cast<std::size_t>( frameIndex % slots )];
        Report( slot );
        slot = Slot{};

        if ( views.size() != viewNames.size() || carry.ViewCount != views.size() || !carry.Visibility.IsValid() )
        {
            Refuse(
                 std::format( "{} views, {} names, the carry holds {} views (Visibility {}); nothing is read back",
                              views.size(), viewNames.size(), carry.ViewCount,
                              carry.Visibility.IsValid() ? "valid" : "invalid" ) );
            return;
        }
        if ( scene.AllInstances().size() < carry.InstanceCount )
        {
            Refuse(
                 std::format( "the cull ran over {} instance rows, the CPU table holds {}; nothing is read back",
                              carry.InstanceCount, scene.AllInstances().size() ) );
            return;
        }

        graph.Extract( carry.Visibility, slot.Visibility, RDG::Access::HostRead );
        if ( vsmDirtyMask.IsValid() )
            graph.Extract( vsmDirtyMask, slot.Mask, RDG::Access::HostRead );

        const std::span<const GpuScenePrimitive>   primitives  = scene.AllPrimitives();
        const std::span<const GpuSceneInstance>    instances   = scene.AllInstances();
        const std::span<const float>               sizes       = scene.LodScreenSizes();
        const std::span<const GpuSceneWorldBounds> worldBounds = scene.WorldBounds();
        slot.Primitives.assign( primitives.begin(), primitives.end() );
        slot.Instances.assign( instances.begin(), instances.begin() + carry.InstanceCount );
        slot.LodScreenSizes.assign( sizes.begin(), sizes.end() );
        slot.WorldBounds.assign( worldBounds.begin(), worldBounds.end() );
        slot.InstanceCount = carry.InstanceCount;
        slot.Views.reserve( views.size() );
        for ( std::size_t v = 0; v < views.size(); ++v )
            slot.Views.push_back(
                 View{ .Gpu = views[v].View, .Frustum = views[v].Frustum, .Name = viewNames[v] } );
    }

    void InstanceCullValidation::Report( const Slot& slot )
    {
        if ( !slot.Visibility.Physical )
            return; // the slot has not been handed a frame yet

        const std::span<const std::byte> bytes  = slot.Visibility.Physical->ReadForHost();
        const uint64_t                   needed = static_cast<uint64_t>( slot.Views.size() ) * slot.InstanceCount;
        if ( bytes.size() < needed * sizeof( uint32_t ) )
        {
            Refuse( std::format( "the Visibility readback holds {} bytes, {} views x {} instances need {}",
                                 bytes.size(), slot.Views.size(), slot.InstanceCount,
                                 needed * sizeof( uint32_t ) ) );
            return;
        }
        std::vector<uint32_t> words( needed );
        std::memcpy( words.data(), bytes.data(), needed * sizeof( uint32_t ) );

        std::vector<uint32_t> mask;
        if ( slot.Mask.Physical )
        {
            const std::span<const std::byte> maskBytes = slot.Mask.Physical->ReadForHost();
            if ( maskBytes.size() < kMaskWords * sizeof( uint32_t ) )
            {
                Refuse( std::format(
                     "the VSM dirty-mask readback holds {} bytes, {} levels need {}; the VSM levels "
                     "are compared against an empty mask",
                     maskBytes.size(), System::kVsmClipmapLevelCount, kMaskWords * sizeof( uint32_t ) ) );
            }
            else
            {
                mask.resize( kMaskWords );
                std::memcpy( mask.data(), maskBytes.data(), kMaskWords * sizeof( uint32_t ) );
            }
        }

        for ( std::size_t v = 0; v < slot.Views.size(); ++v )
        {
            const View& view  = slot.Views[v];
            uint32_t    gpuIn = 0, cpuIn = 0, mismatches = 0, occluded = 0, postVisible = 0;
            uint32_t    firstRow = 0, firstGpu = 0, firstCpu = 0;
            BadRows     bad;

            for ( uint32_t row = 0; row < slot.InstanceCount; ++row )
            {
                const uint32_t gpu = words[v * slot.InstanceCount + row];
                if ( ( gpu & CullVisibility::kInView ) != 0u )
                    ++gpuIn;
                if ( ( gpu & CullVisibility::kOccluded ) != 0u )
                    ++occluded;
                if ( ( gpu & CullVisibility::kPostVisible ) != 0u )
                    ++postVisible;

                // InstanceCull.shader CullMain (:347-395), rule for rule.
                const uint32_t expected = [&]() -> uint32_t
                {
                    const GpuSceneInstance& instance = slot.Instances[row];
                    if ( instance.Primitive == kInvalidIndex )
                        return 0u;
                    if ( instance.Primitive >= slot.Primitives.size() )
                    {
                        ++bad.Primitive;
                        return 0u;
                    }
                    const GpuScenePrimitive& primitive = slot.Primitives[instance.Primitive];
                    // A free instance row keeps its last primitive index; only the rows in the primitive's range
                    // live.
                    if ( row < primitive.FirstInstance ||
                         row >= primitive.FirstInstance + primitive.InstanceCount )
                        return 0u;
                    if ( ( view.Gpu.Flags & CullViewFlags::kShadowCaster ) != 0u &&
                         ( primitive.Flags & PrimitiveFlags::kCastsShadow ) == 0u )
                        return 0u;
                    const glm::mat4            world = UnpackAffine( primitive.LocalToWorld ) * UnpackAffine( instance.InstanceToPrimitive );
                    // InstanceCull.shader's cascade-cache layer split, per instance.
                    // GpuCull_CastsRestPose: a caster view's swaying instance past the shadow wind distance stands
                    // still there.
                    const bool restPose =
                         ( view.Gpu.Flags & CullViewFlags::kShadowCaster ) != 0u && primitive.Wind.z > 0.0f &&
                         ShadowWindCastsRestPose(
                              ShadowWindInstancePoint( world, glm::vec3( primitive.LocalBoundsCenter ) ),
                              glm::vec3( view.Gpu.LodOrigin ), view.Gpu.LodOrigin.w );
                    const bool movableCaster =
                         ( primitive.Flags & PrimitiveFlags::kMovableCaster ) != 0u && !restPose;
                    if ( ( ( view.Gpu.Flags & CullViewFlags::kCasterStaticLayer ) != 0u && movableCaster ) ||
                         ( ( view.Gpu.Flags & CullViewFlags::kCasterMovableLayer ) != 0u && !movableCaster ) )
                        return 0u;
                    const InstanceCullDistance cull  = ScaledByLens(
                         { instance.CullDistanceStart, instance.CullDistanceEnd }, view.Gpu.CullOrigin.w );
                    if ( !KeepsInstanceAtDistance( cull, row - primitive.FirstInstance, glm::vec3( world[3] ),
                                                   glm::vec3( view.Gpu.CullOrigin ) ) )
                        return 0u;

                    const glm::vec3          centre( primitive.LocalBoundsCenter );
                    const glm::vec3          extent( primitive.LocalBoundsExtent );
                    const Common::Math::AABB localBox{ centre - extent, centre + extent };
                    // GpuScene keeps centre = extent = 0 for a mesh with no bounds: "I do not know where this is"
                    // = draw.
                    bool               empty = false;
                    Common::Math::AABB box{};
                    if ( ( primitive.Flags & PrimitiveFlags::kWorldBoundsOverride ) != 0u &&
                         instance.WorldBoundsRow != kInvalidIndex )
                    {
                        if ( instance.WorldBoundsRow >= slot.WorldBounds.size() )
                        {
                            ++bad.WorldBounds;
                            return 0u;
                        }
                        const GpuSceneWorldBounds& pose = slot.WorldBounds[instance.WorldBoundsRow];
                        box = Common::Math::AABB{ glm::vec3( pose.Min ), glm::vec3( pose.Max ) };
                    }
                    else if ( centre == glm::vec3( 0.0f ) && extent == glm::vec3( 0.0f ) )
                    {
                        empty = true;
                    }
                    else
                    {
                        // WindExpandedBounds: the wind moves vertices horizontally by up to the row's pad.
                        const float pad = primitive.LocalBoundsCenter.w;
                        box             = Geometry::TransformBounds( world, localBox );
                        box.Min -= glm::vec3( pad, 0.0f, pad );
                        box.Max += glm::vec3( pad, 0.0f, pad );
                    }
                    if ( !empty )
                    {
                        bool inside = view.Frustum.Intersects( box );
                        if ( inside && ( view.Gpu.Flags & CullViewFlags::kVsmPageMask ) != 0u )
                            inside = CoversDrawnVsmCell( view.Gpu, box.Min, box.Max, mask );
                        if ( !inside )
                            return 0u;
                    }

                    // MeshRenderer::ComputeLOD's order: forced level, the renderer's LOD toggle, then the policy.
                    uint32_t lod = 0;
                    if ( primitive.ForcedLod >= 0 )
                    {
                        lod = static_cast<uint32_t>( primitive.ForcedLod );
                    }
                    else if ( ( view.Gpu.Flags & CullViewFlags::kLodDisabled ) != 0u || primitive.LodCount <= 1u ||
                              empty || primitive.FirstLodScreenSize == kInvalidIndex )
                    {
                        lod = 0;
                    }
                    else if ( static_cast<uint64_t>( primitive.FirstLodScreenSize ) + primitive.LodCount >
                              slot.LodScreenSizes.size() )
                    {
                        ++bad.LodSizes;
                        return 0u;
                    }
                    else
                    {
                        const Geometry::LODView lodView{ .Origin     = glm::vec3( view.Gpu.LodOrigin ),
                                                         .Projection = view.Gpu.LodProjection };
                        lod = Geometry::SelectLODFromBounds(
                             world, localBox, lodView,
                             std::span<const float>( slot.LodScreenSizes )
                                  .subspan( primitive.FirstLodScreenSize, primitive.LodCount ),
                             -1, primitive.LodBias );
                    }
                    return CullVisibility::kInView | std::min( lod, CullVisibility::kLodMask );
                }();

                if ( ( expected & CullVisibility::kInView ) != 0u )
                    ++cpuIn;
                if ( ( gpu & kComparedBits ) != expected )
                {
                    if ( mismatches == 0 )
                    {
                        firstRow = row;
                        firstGpu = gpu;
                        firstCpu = expected;
                    }
                    ++mismatches;
                }
            }

            const std::string first =
                 mismatches == 0 ? std::string( "none" )
                                 : std::format( "instance {} gpu {:#x} cpu {:#x}", firstRow, firstGpu, firstCpu );
            LOG_INFO(
                 "[GPUDriven] Cull validation {}: GPU {} in view, CPU {}, mismatches {} (first: {}), occluded {}, "
                 "post-visible {}, bad rows (primitive {}, world bounds {}, LOD sizes {})",
                 view.Name, gpuIn, cpuIn, mismatches, first, occluded, postVisible, bad.Primitive, bad.WorldBounds,
                 bad.LodSizes );
        }
    }
} // namespace Desert::Graphic::GpuDriven
