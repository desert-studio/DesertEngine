// MeshRenderer's part of the sun's virtual shadow map: the caster pipelines, the invalidation feed and "VSM:
// Raster". Ported from UE 5.8
// Engine/Source/Runtime/Renderer/Private/VirtualShadowMaps/VirtualShadowMapCacheManager.cpp (primitive add /
// remove / transform-update invalidation: the old and the new bounds of what changed, every frame for what moves
// by itself) and ShadowDepthRendering.cpp (the non-Nanite page raster: one attachment-less pass per clipmap level
// over the virtual resolution, the casters' depth clamped), adapted: our caster walk (BuildShadowCascadeDraws),
// RDG PassBuilder::RenderArea, and invalidation by a diff of caster signatures instead of scene primitive
// callbacks. Each level's casters are culled on the GPU against the pages drawn this frame before they are drawn
// ("VSM: Cull Casters", UE VirtualShadowMapBuildPerPageDrawCommands.usf CullPerPageDrawCommands): indirect draws
// with the surviving instance counts and an instance remap.
#include "MeshRendererInternal.hpp"

#include <Engine/Graphic/InstanceWind.hpp>
#include <Engine/Graphic/MeshDrawArguments.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/VirtualShadowMap.hpp>
#include <Engine/Graphic/VirtualGeometry/VirtualMeshTable.hpp>

#include <algorithm>
#include <bit>
#include <format>
#include <utility>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct MeshVirtualShadowShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmFrameData );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmPageTable );
        DESERT_SHADER_PARAMETER( StorageTexture, PhysicalPoolTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmRasterLevel );
        DESERT_SHADER_PARAMETER( StorageBuffer, VsmInstanceRemap );
    };
} // namespace

namespace Desert::Graphic::System
{
    using MeshRendererDetail::MeshDrawCommand;
    using MeshRendererDetail::SignatureHash;

    namespace
    {
        // The world box of @p local under @p transform (the box of its eight transformed corners), grown by @p pad
        // centimetres on every side.
        VsmInvalidationBox WorldBox( const Common::Math::AABB& local, const glm::mat4& transform, const float pad )
        {
            const glm::vec3 centre = glm::vec3( transform * glm::vec4( ( local.Min + local.Max ) * 0.5f, 1.0f ) );
            const glm::vec3 half   = ( local.Max - local.Min ) * 0.5f;
            glm::vec3       extent( pad );
            for ( glm::length_t axis = 0; axis < 3; ++axis )
                extent += glm::abs( glm::vec3( transform[axis] ) ) * half[axis];
            return { .Min = centre - extent, .Max = centre + extent };
        }

        void Grow( VsmInvalidationBox& into, const VsmInvalidationBox& box )
        {
            into.Min = glm::min( into.Min, box.Min );
            into.Max = glm::max( into.Max, box.Max );
        }

        // Every instance of @p transforms, each grown by how far the wind can push its vertices (WindBoundsPad).
        VsmInvalidationBox InstancesBox( const std::vector<glm::mat4>& transforms, const Common::Math::AABB& local,
                                         const InstanceWind& wind )
        {
            const float        pad = WindBoundsPad( wind );
            VsmInvalidationBox box = WorldBox( local, transforms.front(), pad );
            for ( const glm::mat4& transform : transforms )
                Grow( box, WorldBox( local, transform, pad ) );
            return box;
        }
        // A gathered caster's world box (MeshRenderer::GatherShadowCasters) as an invalidation box.
        VsmInvalidationBox GatheredBox( const Common::Math::AABB& world )
        {
            return { .Min = world.Min, .Max = world.Max };
        }
        VsmCullInstanceGpu CullInstance( const VsmInvalidationBox& box, const uint32_t draw, const uint32_t fetch )
        {
            return { .Min  = glm::vec4( box.Min, 0.0f ),
                     .Max  = glm::vec4( box.Max, 0.0f ),
                     .Draw = glm::uvec4( draw, fetch, 0u, 0u ) };
        }
    } // namespace

    Common::Math::AABB SkinnedPoseBounds( const Desert::SkinnedMesh& mesh, const glm::mat4& transform,
                                          const std::span<const glm::mat4> bones )
    {
        const auto&                       submeshes = mesh.GetSubmeshes();
        const float                       reach     = mesh.GetMorphReach();
        std::optional<VsmInvalidationBox> posed;
        for ( const Desert::SkinnedMesh::BoneBounds& bone : mesh.GetBoneBounds() )
        {
            // A palette shorter than the rig is refused at load (SkinnedMesh's bone index check).
            if ( bone.Bone >= bones.size() || bone.Submesh >= submeshes.size() )
                continue;
            const Common::Math::AABB grown{ .Min = bone.Box.Min - glm::vec3( reach ),
                                            .Max = bone.Box.Max + glm::vec3( reach ) };
            const VsmInvalidationBox box =
                 WorldBox( grown, transform * submeshes[bone.Submesh].Transform * bones[bone.Bone], 0.0f );
            if ( posed )
                Grow( *posed, box );
            else
                posed = box;
        }
        // No vertex carries a bone weight: nothing is skinned, the mesh draws its bind shape.
        if ( !posed )
            posed = WorldBox( Geometry::LocalBounds( submeshes ), transform, reach );
        return { .Min = posed->Min, .Max = posed->Max };
    }

    Common::BoolResultStr MeshRenderer::EnsureVirtualShadowPipelines()
    {
        if ( m_VsmPipeline )
            return BOOLSUCCESS;
        if ( m_VsmPipelinesRefused )
            return Common::MakeError( *m_VsmPipelinesRefused );

        // The cascades' caster state (SetupShadowPass: no culling, the static vertex layout for the static and the
        // instanced cells) on a node with no attachment: no depth test or write - the pool's atomic max keeps the
        // occluder nearest the sun (Common/VirtualShadowRaster.glslh) - and depth clamp.
        const auto specFor = []( const MeshVertexPath path, std::string debugName,
                                 std::shared_ptr<Shader> shader ) -> GraphicsPipelineSpecification
        {
            GraphicsPipelineSpecification spec;
            spec.DebugName         = std::move( debugName );
            spec.Layout            = MeshVertexLayout( path == MeshVertexPath::Skinned ? MeshVertexPath::Skinned
                                                                                       : MeshVertexPath::Static );
            spec.Shader            = std::move( shader );
            spec.TargetLayout      = RenderTargetLayout{};
            spec.BlendEnable       = false;
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            spec.DepthClampEnable  = true;
            spec.CullMode          = CullMode::None;
            return spec;
        };
        const auto build =
             [this, &specFor]( const MeshVertexPath path,
                               std::string debugName ) -> Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
        {
            auto shader = DefaultSurfaceProgram( path, MeshPass::VirtualShadowDepth );
            if ( !shader )
                return Common::MakeError<std::shared_ptr<GraphicsPipeline>>(
                     std::format( "the default surface template has no ({} x {}) cell", MeshVertexPathName( path ),
                                  MeshPassName( MeshPass::VirtualShadowDepth ) ) );
            return m_SceneRenderer->GetPipelineCache().GetOrCreate(
                 specFor( path, std::move( debugName ), std::move( shader ) ) );
        };

        const auto staticPipeline = build( MeshVertexPath::Static, "VirtualShadowPipeline" );
        if ( !staticPipeline )
        {
            m_VsmPipelinesRefused =
                 std::format( "no mesh casts into the virtual shadow map: {}", staticPipeline.GetError() );
            return Common::MakeError( *m_VsmPipelinesRefused );
        }
        m_VsmPipeline = staticPipeline.GetValue();

        // The instanced and skinned casters are each left out on their own refusal, as the cascades' are.
        if ( const auto instanced = build( MeshVertexPath::Instanced, "VirtualShadowPipelineInstanced" ) )
        {
            m_VsmInstancedPipeline = instanced.GetValue();
        }
        else
        {
            LOG_ERROR( "[MeshRenderer] instanced meshes cast no virtual shadow: {}", instanced.GetError() );
        }
        if ( const auto skinned = build( MeshVertexPath::Skinned, "VirtualShadowPipelineSkinned" ) )
        {
            m_VsmSkinnedPipeline = skinned.GetValue();
        }
        else
        {
            LOG_ERROR( "[MeshRenderer] skinned meshes cast no virtual shadow: {}", skinned.GetError() );
        }
        return BOOLSUCCESS;
    }

    MeshRenderer::VirtualShadowInvalidation MeshRenderer::CollectVirtualShadowInvalidation()
    {
        VirtualShadowInvalidation result;
        // The pages drawn last frame lack a masked caster whose pipeline was still compiling: drop them all.
        result.StaticCastersChanged = std::exchange( m_VsmCastersPending, false );
        // Every caster's box, LOD and kept instances are this frame's gather (GatherShadowCasters), the ones the
        // levels' draws cull by; gathered for other queues, no caster can be told apart: every page drops.
        if ( !ShadowCastersGathered() )
        {
            LOG_ERROR( "[Shadows] virtual shadow pages invalidated from casters gathered for other mesh queues (a "
                       "queue changed after MeshRenderer::GatherShadowCasters); every cached page drops." );
            result.StaticCastersChanged = true;
            return result;
        }
        const ShadowCasterSetup& gathered = m_ShadowCasterSetup;

        // THE CASTERS THAT STAND STILL, each by a signature of what it draws and where (CascadeStaticSignature's
        // terms, without a frustum: the map's pages outlive any one level's window). One whose signature is known
        // from last frame keeps its box, so a still forest is not re-boxed every frame.
        std::vector<ShadowCasterEntry> still;
        still.reserve( m_VsmStillCasters.size() );
        const auto addStill = [this, &still]( const uint64_t signature, const auto& boxOf )
        {
            const auto known = std::lower_bound( m_VsmStillCasters.begin(), m_VsmStillCasters.end(), signature,
                                                 []( const ShadowCasterEntry& entry, const uint64_t value )
                                                 { return entry.Signature < value; } );
            if ( known != m_VsmStillCasters.end() && known->Signature == signature )
            {
                still.push_back( *known );
                return;
            }
            const VsmInvalidationBox box = boxOf();
            still.push_back( { .Signature = signature, .Min = box.Min, .Max = box.Max } );
        };

        // A caster's rastered set enters its signature only when the raster draws some of it: "no raster" and
        // "raster, none of its submeshes" both mean the walk draws it whole, so they hash alike and a non-VG
        // caster's signature never moves with the carriers: a refusal / recovery redraws the moved casters' pages
        const auto addRasteredSet = []( SignatureHash& hash, const std::vector<uint64_t>& words )
        {
            if ( std::any_of( words.begin(), words.end(), []( const uint64_t word ) { return word != 0u; } ) )
                for ( const uint64_t word : words )
                    hash.Add( word );
        };
        for ( const auto& rd : m_StaticQueue )
        {
            if ( rd.Mesh == nullptr || !rd.CastShadows )
                continue;
            const auto index = static_cast<uint32_t>( &rd - m_StaticQueue.data() );
            SignatureHash hash;
            hash.Add( rd.Mesh->GetRenderRevision() );
            hash.Add( rd.Transform );
            hash.Add( rd.HiddenSubmeshes );
            hash.Add( rd.ForcedLOD );
            hash.Add( rd.LODBias );
            // The LOD it is drawn with follows the main camera: a change redraws its pages.
            hash.Add( std::min( ShadowStaticLod( index ), Geometry::MaxAvailableLOD( rd.Mesh->GetSubmeshes() ) ) );
            const SectionShading shading = ShadingOf( rd );
            hash.Add( rd.SectionSlot );
            hash.Add( shading.Surface );
            hash.Add( shading.Instance );
            // Its rastered set (VsmRasteredSubmeshes, the levels' walk rule): the clusters' depth is the cut's,
            // the walk's the mesh LOD's, so a submesh moving between them redraws its pages. The set is this
            // frame's clusterable submeshes under LAST frame's carrier outcome (this frame's cut and carriers are
            // made after the pages, from their dirty mask): a refused or returning cut or carrier redraws exactly
            // the pages of the casters it moved, one frame later (UE: a primitive's invalidation is applied to the
            // next frame's cache).
            if ( !m_VsmCarriedTwinsLastFrame.empty() )
            {
                VsmRasteredSubmeshes( rd.Mesh, rd.GpuPrimitive, rd.HiddenSubmeshes, m_VsmCarriedTwinsLastFrame,
                                      m_ClusterWalkScratch );
                addRasteredSet( hash, m_ClusterWalkScratch );
            }
            addStill( hash.Value,
                      [&gathered, index] { return GatheredBox( gathered.Statics[index].WorldBounds ); } );
        }
        for ( const auto& g : m_GenericQueue )
        {
            if ( g.Mesh == nullptr || !g.CastShadows )
                continue;
            const auto    index = static_cast<uint32_t>( &g - m_GenericQueue.data() );
            SignatureHash hash;
            hash.Add( g.Mesh->GetRenderRevision() );
            hash.Add( g.Transform );
            hash.Add( g.VisibleSubmeshMask );
            hash.Add( ShadowGenericLod( index ) );
            addStill( hash.Value,
                      [&gathered, index] { return GatheredBox( gathered.Generics[index].WorldBounds ); } );
        }

        // THE CASTERS THAT MOVE BY THEMSELVES (swaying foliage, skinned meshes): their box now; last frame's box
        // is added below, so the pages they left are redrawn too.
        std::vector<VsmInvalidationBox>                            moving;
        std::vector<std::shared_ptr<const std::vector<glm::mat4>>> snapshots;
        std::unordered_map<uint64_t, std::vector<uint32_t>>        ismStates;
        for ( const auto& ism : m_InstancedQueue )
        {
            if ( ism.Mesh == nullptr || !ism.CastShadows || !ism.Transforms || ism.Transforms->empty() )
                continue;
            const auto field = static_cast<uint32_t>( &ism - m_InstancedQueue.data() );
            // The instances the main camera's cull distance keeps, each with the box the wind can reach and its
            // level: the gather's (ShadowIsmInstances), the ones the levels draw.
            const ShadowIsmField&    kept  = ShadowIsmInstances( field );
            const Common::Math::AABB local = Geometry::LocalBounds( ism.Mesh->GetSubmeshes() );
            if ( ism.Wind.Sways() )
            {
                // The kept instances' boxes now (one not kept casts nothing; last frame's box, added below, covers
                // the pages one leaving the cull distance was drawn in).
                if ( kept.Count == 0 )
                    continue;
                VsmInvalidationBox box = GatheredBox( gathered.Instances[kept.First].WorldBounds );
                for ( uint32_t k = kept.First + 1; k < kept.First + kept.Count; ++k )
                    Grow( box, GatheredBox( gathered.Instances[k].WorldBounds ) );
                moving.push_back( box );
                continue;
            }
            // Keyed by its snapshot, as the cascades' cache is (m_VsmKeySnapshots keeps the address unique).
            SignatureHash hash;
            hash.Add( ism.Mesh->GetRenderRevision() );
            hash.Add( ism.Transforms.get() );
            hash.Add( ism.Transforms->size() );
            hash.Add( ism.Material.get() );
            hash.Add( ism.HiddenSubmeshes );
            // Its clustered set, as a static entry's (above): a submesh moving between the VSM cluster raster and
            // the levels' CPU walk redraws the field's pages.
            if ( !m_VsmCarriedTwinsLastFrame.empty() && field < m_FieldPrimitives.size() )
            {
                VsmRasteredSubmeshes( dynamic_cast<const Desert::StaticMesh*>( ism.Mesh ),
                                      m_FieldPrimitives[field], ism.HiddenSubmeshes, m_VsmCarriedTwinsLastFrame,
                                      m_ClusterWalkScratch );
                addRasteredSet( hash, m_ClusterWalkScratch );
            }
            // The box a field added or removed invalidates covers every instance, kept or not: it is made once per
            // signature and outlives the camera's cull distance, which may keep any of them later.
            addStill( hash.Value, [&ism, &local] { return InstancesBox( *ism.Transforms, local, ism.Wind ); } );
            snapshots.push_back( ism.Transforms );

            // What each instance casts follows the main camera (the gather's kept instances and levels): 0 for
            // one the cull distance drops, 1 + its level for one it keeps. An instance whose state changed redraws
            // the pages under it alone (UE: an instance's own invalidation, not its component's).
            const uint32_t        maxLevel = Geometry::MaxAvailableLOD( ism.Mesh->GetSubmeshes() );
            std::vector<uint32_t> states( ism.Transforms->size(), 0u );
            for ( uint32_t k = kept.First; k < kept.First + kept.Count; ++k )
                states[gathered.InstanceIndex[k]] = 1u + std::min( gathered.Instances[k].LodLevel, maxLevel );
            if ( const auto before = m_VsmIsmStates.find( hash.Value );
                 before != m_VsmIsmStates.end() && before->second.size() == states.size() )
            {
                for ( size_t i = 0; i < states.size(); ++i )
                    if ( before->second[i] != states[i] )
                        result.Boxes.push_back( WorldBox( local, ( *ism.Transforms )[i], 0.0f ) );
            }
            ismStates.insert_or_assign( hash.Value, std::move( states ) );
        }
        for ( std::size_t i = 0; i < m_SkinnedQueue.size(); ++i )
        {
            const SkinnedMeshRenderData& sd = m_SkinnedQueue[i];
            // The pose's box, not the bind pose's (the gather's SkinnedPoseBounds): a swung arm leaves the bind
            // bounds.
            if ( sd.Mesh != nullptr && sd.CastShadows && !sd.BoneMatrices.empty() )
                moving.push_back( GatheredBox( gathered.Skinned[i].WorldBounds ) );
        }

        // The casters that are not meshes (the terrain) say what they draw themselves.
        std::vector<ShadowCasterEntry> others;
        for ( const auto& weak : m_ShadowCasters )
            if ( const auto caster = weak.lock() )
                caster->AppendVirtualShadowEntries( others );
        for ( const ShadowCasterEntry& entry : others )
            addStill( entry.Signature,
                      [&entry] { return VsmInvalidationBox{ .Min = entry.Min, .Max = entry.Max }; } );

        // A caster added, removed or changed is a signature on one side only: its old box (it left) and its new
        // box (it came) are invalidated, and nothing else is.
        const auto bySignature = []( const ShadowCasterEntry& a, const ShadowCasterEntry& b )
        { return a.Signature < b.Signature; };
        std::sort( still.begin(), still.end(), bySignature );
        auto before = m_VsmStillCasters.begin();
        auto now    = still.begin();
        while ( before != m_VsmStillCasters.end() || now != still.end() )
        {
            if ( now == still.end() ||
                 ( before != m_VsmStillCasters.end() && before->Signature < now->Signature ) )
            {
                result.Boxes.push_back( { .Min = before->Min, .Max = before->Max } );
                ++before;
            }
            else if ( before == m_VsmStillCasters.end() || now->Signature < before->Signature )
            {
                result.Boxes.push_back( { .Min = now->Min, .Max = now->Max } );
                ++now;
            }
            else
            {
                ++before;
                ++now;
            }
        }
        result.Boxes.insert( result.Boxes.end(), m_VsmMovingBoxes.begin(), m_VsmMovingBoxes.end() );
        result.Boxes.insert( result.Boxes.end(), moving.begin(), moving.end() );

        m_VsmStillCasters = std::move( still );
        m_VsmMovingBoxes  = std::move( moving );
        m_VsmKeySnapshots = std::move( snapshots );
        m_VsmIsmStates    = std::move( ismStates );
        return result;
    }

    uint32_t MeshRenderer::CountVsmFallbackSubmeshes( const std::vector<uint8_t>& fallbackGroups ) const
    {
        // Per static queue entry (one per section material: Rules::SplitMeshSlotDraws) and per ISM / foliage field
        // (its m_FieldPrimitives row), each VG submesh it shows that the VirtualShadow table bins at lod 0 into a
        // fallback group - the lookup ClusterableSubmeshes reads its twin through, so clustered, fallback and
        // refused partition an entry's VG submeshes.
        uint32_t   count   = 0;
        const auto countOf = [&]( const Desert::StaticMesh* mesh, const GpuDriven::PrimitiveIndex primitive,
                                  const uint64_t hidden )
        {
            if ( mesh == nullptr )
                return;
            const auto* registration = mesh->GetVirtualMesh();
            if ( registration == nullptr || !*registration )
                return;
            const std::size_t submeshes = mesh->GetSubmeshes().size();
            const SubmeshMask own{ hidden };
            for ( const VirtualGeometry::VirtualMeshDagSpan& span :
                  VirtualGeometry::VirtualMeshTable::Get().DescribeUploaded( registration->Index() ) )
            {
                if ( span.Submesh >= submeshes || own.Hides( span.Submesh ) )
                    continue;
                const std::optional<uint32_t> group = m_DrawCommandCache->SubmeshGroup(
                     GpuDriven::MeshPass::VirtualShadow, primitive, span.Submesh );
                if ( group && *group < fallbackGroups.size() && fallbackGroups[*group] != 0u )
                    ++count;
            }
        };
        for ( const StaticMeshRenderData& rd : m_StaticQueue )
            if ( rd.CastShadows )
                countOf( rd.Mesh, rd.GpuPrimitive, rd.HiddenSubmeshes );
        for ( size_t field = 0; field < m_InstancedQueue.size() && field < m_FieldPrimitives.size(); ++field )
        {
            const InstancedMeshRenderData& ism = m_InstancedQueue[field];
            if ( ism.CastShadows && ism.Transforms && !ism.Transforms->empty() )
                countOf( dynamic_cast<const Desert::StaticMesh*>( ism.Mesh ), m_FieldPrimitives[field],
                         ism.HiddenSubmeshes );
        }
        return count;
    }

    void MeshRenderer::ClusterableSubmeshes( const GpuDriven::MeshPass pass, const Desert::StaticMesh* mesh,
                                             const GpuDriven::PrimitiveIndex primitive, const uint64_t hidden,
                                             std::vector<uint64_t>& words ) const
    {
        words.clear();
        if ( !m_DrawCommandCache || mesh == nullptr )
            return;
        const auto* registration = mesh->GetVirtualMesh();
        if ( registration == nullptr || !*registration )
            return;
        // No uploaded DAG rows: no candidate this frame (MakeVirtualGeometryCandidates reads the same spans), so
        // not a VG primitive - words stay empty.
        const std::vector<VirtualGeometry::VirtualMeshDagSpan> spans =
             VirtualGeometry::VirtualMeshTable::Get().DescribeUploaded( registration->Index() );
        if ( spans.empty() )
            return;
        const std::size_t submeshes = mesh->GetSubmeshes().size();
        words.assign( ( submeshes + 63u ) / 64u, uint64_t{ 0 } );
        const SubmeshMask own{ hidden }; // the entry's hidden sections
        for ( const VirtualGeometry::VirtualMeshDagSpan& span : spans )
        {
            if ( span.Submesh >= submeshes || own.Hides( span.Submesh ) )
                continue;
            // A submesh whose group has no twin (a fallback, a held refusal) is never rastered as clusters, so the
            // walk keeps it and its shadow is never lost.
            if ( m_DrawCommandCache->SubmeshVirtualGeometryPipeline( pass, primitive, span.Submesh ) != nullptr )
                words[span.Submesh / 64u] |= uint64_t{ 1 } << ( span.Submesh % 64u );
        }
    }

    void MeshRenderer::VsmRasteredSubmeshes( const Desert::StaticMesh*       mesh,
                                             const GpuDriven::PrimitiveIndex primitive, const uint64_t hidden,
                                             const std::span<const GraphicsPipeline* const> carriedTwins,
                                             std::vector<uint64_t>&                         words ) const
    {
        constexpr GpuDriven::MeshPass kPass = GpuDriven::MeshPass::VirtualShadow;
        ClusterableSubmeshes( kPass, mesh, primitive, hidden, words );
        for ( std::size_t word = 0; word < words.size(); ++word )
        {
            for ( uint64_t bits = words[word]; bits != 0u; bits &= bits - 1u )
            {
                const auto submesh = static_cast<uint32_t>( word * 64u + std::countr_zero( bits ) );
                // Set by ClusterableSubmeshes only for a group with a twin: carried or not is the whole question.
                const GraphicsPipeline* twin =
                     m_DrawCommandCache->SubmeshVirtualGeometryPipeline( kPass, primitive, submesh );
                if ( !std::binary_search( carriedTwins.begin(), carriedTwins.end(), twin ) )
                    words[word] &= ~( uint64_t{ 1 } << ( submesh % 64u ) );
            }
        }
    }

    std::optional<SubmeshMask> MeshRenderer::CpuWalkHidden( const ClusterWalkView& view, const Desert::Mesh* mesh,
                                                            const GpuDriven::PrimitiveIndex primitive,
                                                            const uint64_t                  hidden )
    {
        const std::size_t      submeshes = mesh != nullptr ? mesh->GetSubmeshes().size() : 0u;
        std::vector<uint64_t>& words     = m_ClusterWalkScratch;
        words.clear();
        if ( view.VirtualShadow )
        {
            // A VSM level issues no bins: without its raster the walk draws the record whole.
            if ( view.RasterCarriers == nullptr || !m_DrawCommandCache )
                return SubmeshMask{ hidden };
            // The main view's rule: hidden is only what the raster DRAWS - a group whose carrier was refused
            // (AddVirtualShadowRasterPasses) is walked. The same rule the page cache's signature keys on.
            VsmRasteredSubmeshes( dynamic_cast<const Desert::StaticMesh*>( mesh ), primitive, hidden,
                                  m_VsmCarriedTwins, words );
            if ( words.empty() )
                return SubmeshMask{ hidden }; // not a VG primitive
        }
        else
        {
            if ( !view.BinsIssued || !m_DrawCommandCache )
                return SubmeshMask{ hidden };
            const auto candidate = m_FrameClusterCandidates.find( primitive );
            if ( candidate == m_FrameClusterCandidates.end() )
            {
                // Not a VG primitive: its instances are in the view's bins.
                if ( m_DrawCommandCache->SubmeshesBinned( view.Pass, primitive, hidden ) )
                    return std::nullopt;
                return SubmeshMask{ hidden };
            }
            // A VG primitive: in no bin of this view. Hidden from the walk is only what the raster draws.
            words.assign( ( submeshes + 63u ) / 64u, uint64_t{ 0 } );
            if ( view.RasterCarriers != nullptr )
            {
                const std::vector<uint64_t>& taken = candidate->second;
                const SubmeshMask            own{ hidden };
                for ( std::size_t submesh = 0; submesh < submeshes; ++submesh )
                {
                    const std::size_t word = submesh / 64u;
                    const uint64_t    bit  = uint64_t{ 1 } << ( submesh % 64u );
                    if ( word >= taken.size() || ( taken[word] & bit ) == 0u || own.Hides( submesh ) )
                        continue;
                    const std::optional<uint32_t> group = m_DrawCommandCache->SubmeshGroup(
                         view.Pass, primitive, static_cast<uint32_t>( submesh ) );
                    if ( group && *group < view.RasterCarriers->size() &&
                         ( *view.RasterCarriers )[*group] != nullptr )
                        words[word] |= bit;
                }
            }
        }
        if ( words.empty() )
            words.push_back( 0u );
        const SubmeshMask low{ hidden | words.front() };
        // The words past submesh 63, without trailing zero words: equal sets intern to one element.
        std::size_t end = words.size();
        while ( end > 1u && words[end - 1u] == 0u )
            --end;
        SubmeshMask walk = low;
        if ( end > 1u )
        {
            const auto first    = words.begin() + 1;
            const auto last     = words.begin() + static_cast<std::ptrdiff_t>( end );
            const auto interned = m_ClusterWalkWords.emplace( first, last ).first;
            walk                = SubmeshMask{ low.Low, std::span<const uint64_t>( *interned ) };
        }
        for ( std::size_t submesh = 0; submesh < submeshes; ++submesh )
            if ( !walk.Hides( submesh ) )
                return walk;
        return std::nullopt; // every submesh it shows is drawn as clusters
    }

    void MeshRenderer::ResetVirtualShadowGeometry()
    {
        // Only a recorded raster's carrier outcome is what last frame's pages hold
        // (CollectVirtualShadowInvalidation).
        if ( std::exchange( m_VsmClusteredThisFrame, false ) )
            m_VsmCarriedTwinsLastFrame = std::move( m_VsmCarriedTwins );
        else
            m_VsmCarriedTwinsLastFrame.clear();
        m_VsmCarriedTwins.clear();
        m_VsmGeometry.reset();
        m_VsmRefusedGroups     = 0;
        m_VsmFallbackSubmeshes = 0;
        m_ClusterWalkWords.clear();
    }

    Common::ResultStr<std::vector<uint8_t>>
    MeshRenderer::SetVirtualShadowGeometry( MeshRendererDetail::VirtualGeometryIssue issue,
                                            const GpuDriven::DrawCommandBuffers&     upload,
                                            const GpuDriven::GpuSceneBuffers&        scene )
    {
        using Result                        = std::vector<uint8_t>;
        constexpr GpuDriven::MeshPass kPass = GpuDriven::MeshPass::VirtualShadow;
        m_VsmGeometry.reset();
        m_VsmRefusedGroups     = 0;
        m_VsmFallbackSubmeshes = 0;
        if ( !m_DrawCommandCache )
            return Common::MakeError<Result>(
                 "virtual shadow map clusters: no draw command cache to bin them by" );
        if ( auto pipelines = EnsureVirtualShadowPipelines(); !pipelines.IsSuccess() )
            return Common::MakeError<Result>( pipelines.GetError() );
        const GpuDriven::PassDrawTable& table = m_DrawCommandCache->Pass( kPass );
        if ( issue.GroupCount != table.Groups.size() )
            return Common::MakeError<Result>(
                 std::format( "virtual shadow map clusters: {} cluster bins for {} groups", issue.GroupCount,
                              table.Groups.size() ) );
        Result               drawn( table.Groups.size(), uint8_t{ 0 } );
        std::vector<uint8_t> fallback( table.Groups.size(), uint8_t{ 0 } );
        bool                 anyFallback = false;
        for ( size_t group = 0; group < table.Groups.size(); ++group )
        {
            const GpuDriven::IndirectGroup& indirect = table.Groups[group];
            if ( indirect.BinCount == 0u || indirect.Pipeline == nullptr )
                continue;
            if ( indirect.VirtualGeometryPipeline != nullptr )
            {
                drawn[group] = 1u;
                continue;
            }
            // Only a group of a path with the VG permutation (Static, Instanced) can hold clusters.
            const std::string& cell = indirect.Pipeline->GetSpecification().Shader->GetName();
            if ( const auto path = MeshCellPath( cell ); !path || !HasVirtualGeometryPermutation( *path ) )
                continue;
            // The template has no `.VirtualGeometry` permutation: the fallback (UE: no Nanite path -> the fallback
            // mesh), a known state - its submeshes are counted below, nothing is logged.
            if ( !DeclaresVirtualGeometryTwin( cell ) )
            {
                fallback[group] = 1u;
                anyFallback     = true;
                continue;
            }
            ++m_VsmRefusedGroups;
            if ( m_BinnedRefused.insert( cell + "|vsm-vg" ).second )
                LOG_WARN( "[MeshRenderer] '{}' has no `.VirtualGeometry` twin (no such cell, or it is compiling): "
                          "its virtual-geometry meshes cast into the virtual shadow map through the CPU walk",
                          cell );
        }
        if ( anyFallback )
            m_VsmFallbackSubmeshes = CountVsmFallbackSubmeshes( fallback );
        issue.GroupCarriers.assign( table.Groups.size(), nullptr );
        MeshRendererDetail::BinnedIssue binned;
        binned.Table             = &table;
        binned.Instances         = scene.Instances;
        binned.DrawSlots         = upload.Passes[static_cast<size_t>( kPass )].DrawSlots;
        binned.SubmeshTransforms = upload.SubmeshTransforms;
        binned.GroupCarriers.assign( table.Groups.size(), nullptr );
        binned.BinsIssued      = false;
        binned.VirtualGeometry = std::move( issue );
        m_VsmGeometry          = std::move( binned );
        return Common::MakeSuccess( std::move( drawn ) );
    }

    Common::BoolResultStr MeshRenderer::AddVirtualShadowRasterPasses( RDG::Builder&                   graph,
                                                                      const VirtualShadowMapFrame&    frame,
                                                                      const VirtualShadowMapRenderer& map )
    {
        // This frame's cluster issue (SetVirtualShadowGeometry) is consumed by this call whatever it returns: the
        // next frame's walk draws every caster unless that frame's cut is set again.
        struct ConsumeIssue
        {
            std::optional<MeshRendererDetail::BinnedIssue>& Issue;
            ~ConsumeIssue()
            {
                Issue.reset();
            }
        };
        const ConsumeIssue consumed{ m_VsmGeometry };
        if ( auto pipelines = EnsureVirtualShadowPipelines(); !pipelines.IsSuccess() )
            return pipelines;

        // The sun's clipmap levels, when the frame has a sun (a night scene draws only its local lights).
        if ( frame.HasSun )
        {
            for ( uint32_t level = 0; level < kVsmClipmapLevelCount; ++level )
            {
                const uint32_t view = kVsmViewBase + level;
                // No caster bias: the virtual shadow map offsets its receivers along their normals instead
                // (kVsmNormalBiasTexels, "VSM: Project"), so the casters draw with the level's own matrix.
                m_CascadeCasterVP[view]   = frame.LevelViewProjection[level];
                m_CascadeCasterBias[view] = CascadeCasterBias{};
                if ( !EnsureShadowViewMaterials( view ) )
                    return Common::MakeError(
                         std::format( "virtual shadow map level {} has no caster materials", level ) );
            }

            // The clusters of every level (SetVirtualShadowGeometry): one carrier per group with a twin. The level
            // comes from the cluster (its cut view), the matrix and the page table from the level (the cell's
            // vertex and fragment stages), so the carrier's view is any VSM one: level 0's. Made, and the raster's
            // draw list set, BEFORE the walks below: the walks hide exactly the submeshes whose group carrier is
            // held here (VirtualShadowWalkView -> CpuWalkHidden, the main view's rule), so nothing refused after
            // them can lose a caster. A refused carrier leaves its group null - its submeshes are walked (UE: a
            // primitive the cluster path cannot draw is drawn by its own mesh), counted in VsmRefusedGroups and
            // logged once per cell; a refused raster list drops the whole cluster issue, so the walks draw every
            // caster.
            if ( m_VsmGeometry )
            {
                MeshRendererDetail::VirtualGeometryIssue& vg = *m_VsmGeometry->VirtualGeometry;
                const SceneFrameBinding frameState = CaptureFrameState( m_SceneRenderer->GetViewFrame() );
                for ( size_t group = 0; group < m_VsmGeometry->Table->Groups.size(); ++group )
                {
                    const GraphicsPipeline* twin = m_VsmGeometry->Table->Groups[group].VirtualGeometryPipeline;
                    if ( twin == nullptr )
                        continue;
                    std::string error;
                    vg.GroupCarriers[group] =
                         PrepareBinnedCarrier( twin->GetSpecification().Shader, kVsmViewBase, frameState, error );
                    if ( vg.GroupCarriers[group] != nullptr )
                        continue;
                    ++m_VsmRefusedGroups;
                    if ( const std::string& cell = twin->GetSpecification().Shader->GetName();
                         m_BinnedRefused.insert( cell + "|vsm-vg-carrier" ).second )
                        LOG_WARN( "[MeshRenderer] virtual shadow map clusters of '{}' have no carrier ({}): its "
                                  "virtual-geometry meshes cast through the CPU walk",
                                  cell, error );
                }
                m_VsmGeometryDraws.Clear();
                m_VsmGeometryDraws.SetBinned( *m_VsmGeometry );
                if ( m_VsmGeometryDraws.GetError() )
                {
                    for ( const MaterialExecutor* carrier : vg.GroupCarriers )
                        m_VsmRefusedGroups += carrier != nullptr ? 1u : 0u;
                    if ( m_BinnedRefused.insert( "|vsm-vg-raster" ).second )
                        LOG_ERROR(
                             "[MeshRenderer] virtual shadow map clusters refused ({}): every caster is drawn by "
                             "the CPU walk",
                             *m_VsmGeometryDraws.GetError() );
                    m_VsmGeometryDraws.Clear();
                    m_VsmGeometry.reset();
                }
            }
            // The carrier outcome the levels' walks and next frame's page signatures read (VsmRasteredSubmeshes):
            // the twins carried under an accepted list; a refused list (m_VsmGeometry dropped above) carries none.
            m_VsmCarriedTwins.clear();
            if ( m_VsmGeometry )
            {
                const MeshRendererDetail::VirtualGeometryIssue& vg = *m_VsmGeometry->VirtualGeometry;
                for ( size_t group = 0; group < vg.GroupCarriers.size(); ++group )
                    if ( vg.GroupCarriers[group] != nullptr )
                        m_VsmCarriedTwins.push_back( m_VsmGeometry->Table->Groups[group].VirtualGeometryPipeline );
                std::sort( m_VsmCarriedTwins.begin(), m_VsmCarriedTwins.end() );
                m_VsmCarriedTwins.erase( std::unique( m_VsmCarriedTwins.begin(), m_VsmCarriedTwins.end() ),
                                         m_VsmCarriedTwins.end() );
            }

            for ( uint32_t level = 0; level < kVsmClipmapLevelCount; ++level )
            {
                if ( auto added = AddVirtualShadowRasterNode(
                          graph, frame, map, level, kVsmViewBase + level, kVsmVirtualResolution,
                          std::format( "VSM: Raster L{}", level ), m_VsmDraws[level], m_VsmCasters[level] );
                     !added.IsSuccess() )
                    return added;
            }

            // THE CLUSTERS OF EVERY LEVEL IN ONE PASS (UE: one Nanite raster for all the VSM views): each draw
            // instance is a cluster whose cut view is its level; the `.VirtualGeometry` VSM cell takes the level's
            // matrix (VsmFrameData Levels[]) in its vertex stage and the level's page table in its fragment stage.
            // The list was set before the walks (above); without the issue the walks drew every caster.
            if ( m_VsmGeometry )
            {
                m_VsmClusteredThisFrame = true;
                const VirtualShadowBlockParameters clusterParameters =
                     [&frame]( RDG::BindingBlockBuilder& block, const RDG::ShaderBindingLayout& )
                {
                    block.Storage( MeshVirtualShadowShaderParameters::VsmFrameData, frame.FrameData,
                                   RDG::Access::StorageRead )
                         .Storage( MeshVirtualShadowShaderParameters::VsmPageTable, frame.PageTable,
                                   RDG::Access::StorageRead )
                         .Storage( MeshVirtualShadowShaderParameters::PhysicalPoolTexture, frame.PhysicalPool,
                                   RDG::Access::StorageWrite );
                };
                graph.AddPass(
                     "VSM: Raster VirtualGeometry", RDG::PassFlags::Raster,
                     [this, &clusterParameters]( RDG::PassBuilder& pass )
                     {
                         pass.RenderArea( kVsmVirtualResolution, kVsmVirtualResolution );
                         m_VsmGeometryDraws.Declare( pass, std::nullopt, clusterParameters );
                     },
                     [this]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         DESERT_PROFILE_PASS( "VSM: Raster VirtualGeometry" );
                         return m_VsmGeometryDraws.Record( context );
                     } );
            }
        }

        // THE LOCAL LIGHTS' DRAWN MIPS: each a shadow view of its own past this frame's atlas rows (its own caster
        // materials: two nodes through one material would both read whichever upload came last), drawn with the
        // mip's matrix over its ResolutionPages * kVsmPageSize square.
        const auto firstView = static_cast<uint32_t>( kLocalShadowViewBase + m_LocalShadowRows.size() );
        for ( uint32_t view = firstView; view < m_VirtualShadowViews.size(); ++view )
            m_VirtualShadowViews[view] = 0u;
        if ( frame.LocalRasters.empty() )
            return BOOLSUCCESS;
        EnsureShadowViewCount( firstView + static_cast<uint32_t>( frame.LocalRasters.size() ) );
        while ( m_VsmLocalDraws.size() < frame.LocalRasters.size() )
            m_VsmLocalDraws.push_back( std::make_unique<VsmLocalRasterDraws>() );
        for ( size_t k = 0; k < frame.LocalRasters.size(); ++k )
        {
            const VsmLocalRaster& raster = frame.LocalRasters[k];
            const uint32_t        view   = firstView + static_cast<uint32_t>( k );
            m_VirtualShadowViews[view]   = 1u;
            m_CascadeCasterVP[view]      = raster.ViewProjection;
            m_CascadeCasterBias[view]    = CascadeCasterBias{};
            if ( !EnsureShadowViewMaterials( view ) )
                return Common::MakeError( std::format(
                     "virtual shadow map local light {} view {} mip {} has no caster materials", raster.Light,
                     raster.View, raster.Mip ) );
            VsmLocalRasterDraws& entry = *m_VsmLocalDraws[k];
            if ( auto added = AddVirtualShadowRasterNode(
                      graph, frame, map, raster.Level, view, raster.ResolutionPages * kVsmPageSize,
                      std::format( "VSM: Raster Local {} V{} M{}", raster.Light, raster.View, raster.Mip ),
                      entry.Draws, entry.Casters );
                 !added.IsSuccess() )
                return added;
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr MeshRenderer::AddVirtualShadowRasterNode( RDG::Builder&                   graph,
                                                                    const VirtualShadowMapFrame&    frame,
                                                                    const VirtualShadowMapRenderer& map,
                                                                    const uint32_t level, const uint32_t view,
                                                                    const uint32_t resolution, std::string name,
                                                                    MeshDrawList&                draws,
                                                                    std::vector<VsmCasterEntry>& casters )
    {
        draws.Clear();
        BuildShadowCascadeDraws( view, draws, ShadowCasterLayer::All );
        if ( draws.GetError() )
            return Common::MakeError(
                 std::format( "virtual shadow map level {}: {}", level, *draws.GetError() ) );

        // THE LEVEL'S CULL (UE CullPerPageDrawCommands): every draw's argument records (instance counts 0) and
        // every instance it may draw with its world box; "VSM: Cull Casters" keeps the instances over a page
        // drawn this frame. Mesh draws first, in list order, then the non-mesh casters' draws.
        VsmCasterCull         cull;
        std::vector<uint32_t> firstRecords;
        firstRecords.reserve( draws.GetCommands().size() );
        for ( const MeshDrawCommand& command : draws.GetCommands() )
        {
            const auto firstRecord = static_cast<uint32_t>( cull.Arguments.size() / kMeshDrawArgumentWords );
            firstRecords.push_back( firstRecord );
            if ( command.Instances.empty() && command.InstanceCount != 1u )
                return Common::MakeError( std::format(
                     "virtual shadow map level {}: a draw of {} instances names no instance matrices", level,
                     command.InstanceCount ) );
            const auto records = AppendMeshDrawArguments( *command.Mesh, command.HiddenSubmeshMask,
                                                          command.LodLevel, cull.RemapSize, cull.Arguments );
            if ( !records )
                return Common::MakeError(
                     std::format( "virtual shadow map level {}: {}", level, records.GetError() ) );
            if ( records.GetValue() == 0u )
                continue; // every submesh hidden: the walk draws nothing
            const auto draw = static_cast<uint32_t>( cull.Draws.size() );
            cull.Draws.push_back(
                 { .FirstRecord = firstRecord, .Records = records.GetValue(), .RemapBase = cull.RemapSize } );
            const Common::Math::AABB local = Geometry::LocalBounds( command.Mesh->GetSubmeshes() );
            if ( command.Instances.empty() )
            {
                VsmInvalidationBox box = WorldBox( local, command.Transform, command.BoundsPad );
                if ( command.WorldBounds )
                    box = { .Min = command.WorldBounds->Min, .Max = command.WorldBounds->Max };
                cull.Instances.push_back( CullInstance( box, draw, command.FirstInstance ) );
                cull.RemapSize += 1u;
                continue;
            }
            for ( size_t i = 0; i < command.Instances.size(); ++i )
                cull.Instances.push_back(
                     CullInstance( WorldBox( local, command.Instances[i], command.BoundsPad ), draw,
                                   command.FirstInstance + static_cast<uint32_t>( i ) ) );
            cull.RemapSize += static_cast<uint32_t>( command.Instances.size() );
        }
        casters.clear();
        std::vector<VirtualShadowCasterDraw> casterDraws;
        for ( const auto& weak : m_ShadowCasters )
        {
            const auto caster = weak.lock();
            if ( !caster )
                continue;
            casterDraws.clear();
            caster->AppendVirtualShadowDraws( casterDraws );
            casters.push_back(
                 { .Caster      = weak,
                   .FirstRecord = static_cast<uint32_t>( cull.Arguments.size() / kMeshDrawArgumentWords ) } );
            for ( const VirtualShadowCasterDraw& casterDraw : casterDraws )
            {
                const auto draw = static_cast<uint32_t>( cull.Draws.size() );
                cull.Draws.push_back(
                     { .FirstRecord = static_cast<uint32_t>( cull.Arguments.size() / kMeshDrawArgumentWords ),
                       .Records     = 1u,
                       .RemapBase   = cull.RemapSize } );
                std::array<uint32_t, kMeshDrawArgumentWords> arguments = casterDraw.Arguments;
                arguments[kMeshDrawInstanceWord]                       = 0u;
                cull.Arguments.insert( cull.Arguments.end(), arguments.begin(), arguments.end() );
                cull.Instances.push_back( CullInstance(
                     VsmInvalidationBox{ .Min = casterDraw.Min, .Max = casterDraw.Max }, draw, 0u ) );
                cull.RemapSize += 1u;
            }
        }
        // Nothing casts into this level on the CPU: no node (its clusters are "VSM: Raster VirtualGeometry"'s).
        if ( cull.Instances.empty() )
            return BOOLSUCCESS;

        const auto culled = map.AddCasterCulling( graph, frame, level, cull );
        if ( !culled )
            return Common::MakeError( culled.GetError() );
        const RDG::BufferRef arguments = culled.GetValue().Arguments;
        const RDG::BufferRef remap     = culled.GetValue().InstanceRemap;
        draws.SetIndirect( arguments, std::move( firstRecords ) );

        // The pass parameters of every block the level's node declares (the mesh cells' and the non-mesh
        // casters'): every VirtualShadowDepth program includes Common/VirtualShadowRaster.glslh; the instanced
        // cells also fetch through the level's instance remap (Vertex_Instanced.glslh).
        const VirtualShadowBlockParameters parameters =
             [&frame, level, remap]( RDG::BindingBlockBuilder& block, const RDG::ShaderBindingLayout& layout )
        {
            block.Storage( MeshVirtualShadowShaderParameters::VsmFrameData, frame.FrameData,
                           RDG::Access::StorageRead )
                 .Storage( MeshVirtualShadowShaderParameters::VsmPageTable, frame.PageTable,
                           RDG::Access::StorageRead )
                 .Storage( MeshVirtualShadowShaderParameters::PhysicalPoolTexture, frame.PhysicalPool,
                           RDG::Access::StorageWrite )
                 .Storage( MeshVirtualShadowShaderParameters::VsmRasterLevel, frame.RasterLevel[level],
                           RDG::Access::StorageRead );
            if ( remap.IsValid() &&
                 std::any_of( layout.Slots.begin(), layout.Slots.end(),
                              []( const RDG::ShaderSlot& slot ) { return slot.Name == "VsmInstanceRemap"; } ) )
                block.Storage( MeshVirtualShadowShaderParameters::VsmInstanceRemap, remap,
                               RDG::Access::StorageRead );
        };
        graph.AddPass(
             std::move( name ), RDG::PassFlags::Raster,
             [this, &draws, &casters, resolution, arguments, &parameters]( RDG::PassBuilder& pass )
             {
                 pass.RenderArea( resolution, resolution );
                 MeshDrawList& list = draws;
                 list.Declare( pass, std::nullopt, parameters );
                 if ( arguments.IsValid() )
                     pass.Read( arguments, RDG::Access::IndirectArgs );
                 // The skin cache's streams its skinned casters fetch (after "SkinCache: Skin" wrote them).
                 for ( const RDG::BufferRef& streams : m_SkinCacheReads )
                     pass.Read( streams, RDG::Access::StorageRead );
                 // The non-mesh casters declare their blocks after the list's.
                 uint32_t nextBlock = list.BlockCount();
                 for ( VsmCasterEntry& entry : casters )
                 {
                     if ( const auto caster = entry.Caster.lock() )
                     {
                         entry.FirstBlock = nextBlock;
                         nextBlock += caster->DeclareVirtualShadow( pass, parameters );
                     }
                 }
             },
             [this, &draws, &casters, view, arguments]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 DESERT_PROFILE_PASS( "VSM: Raster" );
                 if ( auto drawn = draws.Record( context ); !drawn.IsSuccess() )
                     return drawn;
                 for ( const VsmCasterEntry& entry : casters )
                 {
                     const auto caster = entry.Caster.lock();
                     if ( !caster )
                         continue; // gone since the setup: its draws went with it
                     if ( auto cast =
                               caster->RecordVirtualShadow( context, entry.FirstBlock, m_CascadeCasterVP[view],
                                                            arguments, entry.FirstRecord );
                          !cast.IsSuccess() )
                         return cast;
                 }
                 return BOOLSUCCESS;
             } );
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::System
