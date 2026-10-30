#include "MeshRendererInternal.hpp"

#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Geometry/StaticMesh.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/DisplacementCache.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>

#include <algorithm>
#include <bit>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Desert::Graphic::System
{
    void MeshRenderer::ApplyDisplacement( DisplacementCache& cache, const Geometry::LODView& view,
                                          const float viewHeightPixels, const uint64_t frame,
                                          const bool requestLevels )
    {
        DESERT_PROFILE_SCOPE( "MeshRenderer::ApplyDisplacement" );

        // BAKED displacement (§1g): the cooked dense mesh IS the geometry - no levels, no cache entry - drawn with
        // the displaced slots like a twin (no double relief). Captures substitute too: every view draws one surface.
        for ( StaticMeshRenderData& data : m_StaticQueue )
            if ( auto* const cooked = dynamic_cast<StaticMesh*>( data.Mesh );
                 cooked != nullptr && cooked->IsBakedDisplaced() )
                if ( const std::shared_ptr<StaticMesh> baked = cooked->GetBakedDisplaced() )
                {
                    data.Mesh = baked.get(); // held by the base mesh while its surface is the same
                    if ( data.MaterialSlots && data.MaterialSlots->Displaced )
                        data.MaterialSlots = data.MaterialSlots->Displaced;
                }
        for ( InstancedMeshRenderData& ism : m_InstancedQueue )
            if ( auto* const cooked = dynamic_cast<StaticMesh*>( ism.Mesh );
                 cooked != nullptr && cooked->IsBakedDisplaced() && ism.DisplacedMaterial )
                if ( const std::shared_ptr<StaticMesh> baked = cooked->GetBakedDisplaced() )
                {
                    ism.Mesh     = baked.get();
                    ism.Material = ism.DisplacedMaterial;
                }

        std::vector<std::optional<DisplacementMaterialRecord>> materials;
        for ( const StaticMeshRenderData& data : m_StaticQueue )
        {
            if ( !requestLevels )
                break;
            const auto* const cooked = dynamic_cast<const StaticMesh*>( data.Mesh );
            if ( cooked == nullptr || !cooked->GetDisplacementTopology() || !data.MaterialSlots ||
                 data.MaterialSlots->Slots.empty() )
                continue;
            const std::vector<Submesh>& submeshes = data.Mesh->GetSubmeshes();
            materials.assign( submeshes.size(), std::nullopt );
            for ( size_t s = 0; s < submeshes.size(); ++s )
            {
                const SurfaceSlot slot = SectionSurface(
                     data.MaterialSlots.get(),
                     static_cast<uint32_t>( SectionMaterialSlot( s, data.MaterialSlots->Slots.size() ) ) );
                if ( slot.Surface == nullptr )
                    continue;
                // The same predicate that picks the template's `.Parallax` cells: the material assigns its Height.
                const auto record =
                     ResolveDisplacementMaterial( slot.Surface->GetSchema(), slot.Surface->GetMaterialLayout(),
                                                  EffectiveRow( slot.Surface, slot.Instance ),
                                                  IsParallaxCell( slot.Surface->GetShaderName() ) );
                if ( !record.IsSuccess() )
                {
                    LOG_ERROR( "[Displacement] entity {} section {}: not displaced: {}", data.Entity, s,
                               record.GetError() );
                    continue;
                }
                materials[s] = record.GetValue();
            }
            // No section moves (no material assigns its Height): nothing to tessellate.
            if ( std::none_of( materials.begin(), materials.end(),
                               []( const std::optional<DisplacementMaterialRecord>& m ) { return m.has_value(); } ) )
                continue;
            cache.Request( data.Entity, data.MotionPart, *cooked, data.Transform, materials, view,
                           viewHeightPixels, frame );
        }
        // ISM members (§9.8): one request per instance - equal levels share one twin, so a field of stones costs
        // one geometry per distinct level combination. Entry part = (instance + 1) << 6 | section group, a
        // cache-only key (each section group of the component is its own record with its own material).
        const auto ismPart = []( size_t instance, uint64_t hidden )
        {
            const uint32_t group = static_cast<uint32_t>( std::countr_zero( ~hidden ) ) & 63u;
            return ( static_cast<uint32_t>( instance + 1 ) << 6u ) | group;
        };
        for ( const InstancedMeshRenderData& ism : m_InstancedQueue )
        {
            if ( !requestLevels )
                break;
            const auto* const cooked = dynamic_cast<const StaticMesh*>( ism.Mesh );
            if ( cooked == nullptr || !ism.DisplacedMaterial || !cooked->GetDisplacementTopology() )
                continue;
            auto* surface = dynamic_cast<DataDrivenMaterial*>( ism.Material->GetParentMaterial() );
            if ( surface == nullptr )
                continue;
            const auto record = ResolveDisplacementMaterial( surface->GetSchema(), surface->GetMaterialLayout(),
                                                             EffectiveRow( surface, ism.Material.get() ),
                                                             IsParallaxCell( surface->GetShaderName() ) );
            if ( !record.IsSuccess() )
            {
                LOG_ERROR( "[Displacement] ISM entity {}: not displaced: {}", ism.Entity, record.GetError() );
                continue;
            }
            if ( !record.GetValue() )
                continue;
            const StaticMesh& mesh = *cooked;
            materials.assign( mesh.GetSubmeshes().size(), std::nullopt );
            for ( size_t s = 0; s < materials.size(); ++s )
                if ( s >= 64 || ( ism.HiddenSubmeshes & ( 1ull << s ) ) == 0 )
                    materials[s] = record.GetValue();
            const std::vector<glm::mat4>& transforms = *ism.Transforms;
            for ( size_t i = 0; i < transforms.size(); ++i )
                cache.Request( ism.Entity, ismPart( i, ism.HiddenSubmeshes ), mesh, transforms[i], materials, view,
                               viewHeightPixels, frame );
        }

        // SKINNED records (§9.8): displaced over the skin cache's pose, so only while the skin cache runs (off, the
        // vertex stage skins and nothing can move the twin: the base draws, as with the cache's own Low level). The
        // entry key is the skin cache's: the n-th skinned record of the entity in queue order (KeyRecords).
        const bool skinnedDisplacement = IsSkinCacheEnabled();
        std::unordered_map<uint32_t, uint32_t> skinnedSlots;
        for ( const SkinnedMeshRenderData& data : m_SkinnedQueue )
        {
            const uint32_t slot = skinnedSlots[data.Entity]++;
            if ( !requestLevels || !skinnedDisplacement )
                continue;
            if ( data.Mesh == nullptr || !data.Mesh->GetDisplacementTopology() || !data.MaterialSlots ||
                 data.MaterialSlots->Slots.empty() || !data.MaterialSlots->Displaced )
                continue;
            const std::vector<Submesh>& submeshes = data.Mesh->GetSubmeshes();
            materials.assign( submeshes.size(), std::nullopt );
            for ( size_t s = 0; s < submeshes.size(); ++s )
            {
                const SurfaceSlot surface = SectionSurface(
                     data.MaterialSlots.get(),
                     static_cast<uint32_t>( SectionMaterialSlot( s, data.MaterialSlots->Slots.size() ) ) );
                if ( surface.Surface == nullptr )
                    continue;
                const auto record = ResolveDisplacementMaterial(
                     surface.Surface->GetSchema(), surface.Surface->GetMaterialLayout(),
                     EffectiveRow( surface.Surface, surface.Instance ),
                     IsParallaxCell( surface.Surface->GetShaderName() ) );
                if ( !record.IsSuccess() )
                {
                    LOG_ERROR( "[Displacement] skinned entity {} section {}: not displaced: {}", data.Entity, s,
                               record.GetError() );
                    continue;
                }
                materials[s] = record.GetValue();
            }
            if ( std::none_of( materials.begin(), materials.end(),
                               []( const std::optional<DisplacementMaterialRecord>& m ) { return m.has_value(); } ) )
                continue;
            cache.RequestSkinned( data.Entity, slot, *data.Mesh, data.Transform, data.BoneMatrices, materials, view,
                                  viewHeightPixels, frame );
        }

        if ( const Common::BoolResultStr displaced = cache.DisplaceFrame( frame ); !displaced )
            LOG_ERROR( "{}", displaced.GetError() );

        // The skinned twins: the record draws the twin (index buffer, submeshes, rest vertices) and the skin cache
        // skins its base (SkinSource), then writes the twin's pose; every section draws its displaced sibling.
        skinnedSlots.clear();
        for ( SkinnedMeshRenderData& data : m_SkinnedQueue )
        {
            const uint32_t slot = skinnedSlots[data.Entity]++;
            if ( !skinnedDisplacement || data.Mesh == nullptr || !data.MaterialSlots ||
                 !data.MaterialSlots->Displaced )
                continue;
            const std::shared_ptr<SkinnedMesh> twin = cache.FindSkinned( data.Entity, slot );
            if ( twin == nullptr )
                continue;
            const auto& slots     = data.MaterialSlots->Slots;
            const auto& displaced = data.MaterialSlots->Displaced->Slots;
            std::vector<SkinnedSection> sections = data.Sections;
            bool                        complete = displaced.size() == slots.size();
            for ( SkinnedSection& section : sections )
            {
                const auto it = std::find( slots.begin(), slots.end(), section.Instance );
                if ( !complete || it == slots.end() )
                {
                    complete = false;
                    break;
                }
                MaterialInstance* const   inst    = displaced[static_cast<size_t>( it - slots.begin() )];
                DataDrivenMaterial* const surface =
                     inst != nullptr ? dynamic_cast<DataDrivenMaterial*>( inst->GetParentMaterial() ) : nullptr;
                if ( surface == nullptr || MeshCellPath( surface->GetShaderName() ) != MeshVertexPath::Skinned )
                {
                    complete = false;
                    break;
                }
                section.Material = surface;
                section.Instance = inst;
            }
            if ( !complete )
            {
                // The twin would draw with the relief marched a second time, or with a static cell: refused by
                // name, the base surface draws (parallax) instead.
                LOG_ERROR( "[Displacement] skinned entity {}: a section has no skinned displaced material; the base "
                           "surface is drawn",
                           data.Entity );
                continue;
            }
            data.Sections   = std::move( sections );
            data.SkinSource = data.Mesh;
            data.Mesh       = twin.get(); // the cache holds the twin past this frame's draws (parked on release)
        }
        for ( StaticMeshRenderData& data : m_StaticQueue )
            if ( const std::shared_ptr<StaticMesh> twin = cache.Find( data.Entity, data.MotionPart ) )
            {
                data.Mesh = twin.get(); // the cache holds the twin past this frame's draws (EndFrame of the next)
                // No double relief: the displaced slots draw base cells (MaterialSlotBinding::Displaced).
                if ( data.MaterialSlots && data.MaterialSlots->Displaced )
                    data.MaterialSlots = data.MaterialSlots->Displaced;
            }

        // Split each displaced ISM record by the twin its instances draw: the base keeps the undisplaced ones,
        // every twin gets a record of its own with the displaced material, each instance keeping its GPU-scene
        // row. The split is cached (m_DisplacedSplits): re-derived only when the record names another transform
        // snapshot or the cache's twin generation moved, so a steady frame neither asks Find() per instance nor
        // allocates.
        if ( frame != m_DisplacedSplitFrame )
        {
            const uint64_t previous = m_DisplacedSplitFrame;
            std::erase_if( m_DisplacedSplits,
                           [previous]( const auto& held ) { return held.second.Frame != previous; } );
            m_DisplacedSplitFrame = frame;
        }
        const uint64_t generation = cache.GetTwinGeneration();
        m_DisplacedTwinRecords.clear();
        for ( InstancedMeshRenderData& ism : m_InstancedQueue )
        {
            if ( ism.Mesh == nullptr || !ism.DisplacedMaterial || !ism.Mesh->GetDisplacementTopology() ||
                 !ism.Transforms )
                continue;
            DisplacedSplit& split = m_DisplacedSplits[DisplacedSplitKey{ ism.Entity, ism.HiddenSubmeshes }];
            split.Frame           = frame;
            if ( split.Snapshot != ism.Transforms || split.TwinGeneration != generation )
            {
                split.Snapshot       = ism.Transforms;
                split.TwinGeneration = generation;
                split.Groups.clear();
                const std::vector<glm::mat4>&                                 transforms = *ism.Transforms;
                std::vector<std::pair<std::shared_ptr<StaticMesh>, uint32_t>> drawn;
                drawn.reserve( transforms.size() );
                bool anyTwin = false;
                for ( size_t i = 0; i < transforms.size(); ++i )
                {
                    drawn.emplace_back( cache.Find( ism.Entity, ismPart( i, ism.HiddenSubmeshes ) ),
                                        static_cast<uint32_t>( i ) );
                    anyTwin = anyTwin || drawn.back().first != nullptr;
                }
                if ( anyTwin )
                {
                    // Group by twin, in first-seen order (deterministic draw order).
                    for ( const auto& [twin, i] : drawn )
                        if ( std::none_of( split.Groups.begin(), split.Groups.end(),
                                           [&twin]( const DisplacedSplitGroup& g ) { return g.Twin == twin; } ) )
                            split.Groups.push_back( DisplacedSplitGroup{ .Twin = twin } );
                    for ( DisplacedSplitGroup& group : split.Groups )
                    {
                        auto subset = std::make_shared<std::vector<glm::mat4>>();
                        auto ids    = std::make_shared<std::vector<uint32_t>>();
                        for ( const auto& [t, i] : drawn )
                            if ( t == group.Twin )
                            {
                                subset->push_back( transforms[i] );
                                ids->push_back( i );
                            }
                        group.Transforms  = std::move( subset );
                        group.InstanceIds = std::move( ids );
                    }
                }
            }
            if ( split.Groups.empty() )
                continue; // no instance draws a twin: the record draws as queued
            const DisplacedSplitGroup* base = nullptr;
            for ( const DisplacedSplitGroup& group : split.Groups )
            {
                if ( group.Twin == nullptr )
                {
                    base = &group;
                    continue;
                }
                InstancedMeshRenderData& twin = m_DisplacedTwinRecords.emplace_back( ism );
                twin.Transforms               = group.Transforms;
                twin.InstanceIds              = group.InstanceIds;
                twin.Mesh                     = group.Twin.get(); // held by the cache past this frame's draws
                twin.Material                 = ism.DisplacedMaterial;
            }
            if ( base != nullptr )
            {
                ism.Transforms  = base->Transforms;
                ism.InstanceIds = base->InstanceIds;
            }
            else
                ism.Transforms.reset(); // every instance draws a twin: the record is dropped below
        }
        std::erase_if( m_InstancedQueue, []( const InstancedMeshRenderData& ism ) { return !ism.Transforms; } );
        for ( InstancedMeshRenderData& twin : m_DisplacedTwinRecords )
            m_InstancedQueue.push_back( std::move( twin ) );
        m_DisplacedTwinRecords.clear();
        // The twins replaced queued entries and the ISM records were split: the shadow views drawn from here on
        // read this frame's casters as they now stand.
        GatherShadowCasters();
    }
} // namespace Desert::Graphic::System
