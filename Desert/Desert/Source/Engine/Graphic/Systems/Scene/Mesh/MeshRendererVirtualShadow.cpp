// MeshRenderer's part of the sun's virtual shadow map: the caster pipelines, the invalidation feed and "VSM:
// Raster". Ported from UE 5.8
// Engine/Source/Runtime/Renderer/Private/VirtualShadowMaps/VirtualShadowMapCacheManager.cpp (primitive add /
// remove / transform-update invalidation: the old and the new bounds of what changed, every frame for what moves
// by itself) and ShadowDepthRendering.cpp (the non-Nanite page raster: one attachment-less pass per clipmap level
// over the virtual resolution, the casters' depth clamped), adapted: our caster walk (BuildShadowCascadeDraws),
// RDG PassBuilder::RenderArea, and invalidation by a diff of caster signatures instead of scene primitive
// callbacks.
#include "MeshRendererInternal.hpp"

#include <Engine/Graphic/Systems/Scene/Shadows/VirtualShadowMap.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace Desert::Graphic::System
{
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
    } // namespace

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

        for ( const auto& rd : m_StaticQueue )
        {
            if ( rd.Mesh == nullptr || !rd.CastShadows )
                continue;
            SignatureHash hash;
            hash.Add( rd.Mesh );
            hash.Add( rd.Transform );
            hash.Add( rd.HiddenSubmeshes );
            hash.Add( rd.ForcedLOD );
            hash.Add( rd.LODBias );
            const SectionShading shading = ShadingOf( rd );
            hash.Add( rd.SectionSlot );
            hash.Add( shading.Surface );
            hash.Add( shading.Instance );
            addStill( hash.Value,
                      [&rd] {
                          return WorldBox( Geometry::LocalBounds( rd.Mesh->GetSubmeshes() ), rd.Transform, 0.0f );
                      } );
        }
        for ( const auto& g : m_GenericQueue )
        {
            if ( g.Mesh == nullptr || !g.CastShadows )
                continue;
            SignatureHash hash;
            hash.Add( g.Mesh );
            hash.Add( g.Transform );
            addStill( hash.Value, [&g]
                      { return WorldBox( Geometry::LocalBounds( g.Mesh->GetSubmeshes() ), g.Transform, 0.0f ); } );
        }

        // THE CASTERS THAT MOVE BY THEMSELVES (swaying foliage, skinned meshes): their box now; last frame's box
        // is added below, so the pages they left are redrawn too.
        std::vector<VsmInvalidationBox>                            moving;
        std::vector<std::shared_ptr<const std::vector<glm::mat4>>> snapshots;
        for ( const auto& ism : m_InstancedQueue )
        {
            if ( ism.Mesh == nullptr || !ism.CastShadows || !ism.Transforms || ism.Transforms->empty() )
                continue;
            const Common::Math::AABB local = Geometry::LocalBounds( ism.Mesh->GetSubmeshes() );
            if ( ism.Wind.Sways() )
            {
                moving.push_back( InstancesBox( *ism.Transforms, local, ism.Wind ) );
                continue;
            }
            // Keyed by its snapshot, as the cascades' cache is (m_VsmKeySnapshots keeps the address unique).
            SignatureHash hash;
            hash.Add( ism.Mesh );
            hash.Add( ism.Transforms.get() );
            hash.Add( ism.Transforms->size() );
            hash.Add( ism.Material.get() );
            hash.Add( ism.HiddenSubmeshes );
            addStill( hash.Value, [&ism, &local] { return InstancesBox( *ism.Transforms, local, ism.Wind ); } );
            snapshots.push_back( ism.Transforms );
        }
        for ( const auto& sd : m_SkinnedQueue )
        {
            if ( sd.Mesh != nullptr && sd.CastShadows && !sd.BoneMatrices.empty() )
                moving.push_back(
                     WorldBox( Geometry::LocalBounds( sd.Mesh->GetSubmeshes() ), sd.Transform, 0.0f ) );
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
        return result;
    }

    Common::BoolResultStr MeshRenderer::AddVirtualShadowRasterPasses( RDG::Builder&                graph,
                                                                      const VirtualShadowMapFrame& frame )
    {
        if ( auto pipelines = EnsureVirtualShadowPipelines(); !pipelines.IsSuccess() )
            return pipelines;

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

            // The pass parameters of every block the level's node declares (the mesh cells' and the non-mesh
            // casters'): every VirtualShadowDepth program includes Common/VirtualShadowRaster.glslh.
            const VirtualShadowBlockParameters parameters =
                 [&frame, level]( RDG::BindingBlockBuilder& block, const RDG::ShaderBindingLayout& )
            {
                block.Storage( "VsmFrameData", frame.FrameData, RDG::Access::StorageRead )
                     .Storage( "VsmPageTable", frame.PageTable, RDG::Access::StorageRead )
                     .Storage( "u_PhysicalPool", frame.PhysicalPool, RDG::Access::StorageWrite )
                     .Storage( "VsmRasterLevel", frame.RasterLevel[level], RDG::Access::StorageRead );
            };
            graph.AddPass(
                 std::format( "VSM: Raster L{}", level ), RDG::PassFlags::Raster,
                 [this, level, view, &parameters]( RDG::PassBuilder& pass )
                 {
                     pass.RenderArea( kVsmVirtualResolution, kVsmVirtualResolution );
                     MeshDrawList& draws = m_VsmDraws[level];
                     draws.Clear();
                     m_VsmCasters[level].clear();
                     BuildShadowCascadeDraws( view, draws, ShadowCasterLayer::All );
                     draws.Declare( pass, std::nullopt, parameters );
                     // The skin cache's streams its skinned casters fetch (after "SkinCache: Skin" wrote them).
                     for ( const RDG::BufferRef& streams : m_SkinCacheReads )
                         pass.Read( streams, RDG::Access::StorageRead );
                     // The non-mesh casters declare their blocks after the list's.
                     uint32_t nextBlock = draws.BlockCount();
                     for ( const auto& weak : m_ShadowCasters )
                     {
                         if ( const auto caster = weak.lock() )
                         {
                             const uint32_t declared = caster->DeclareVirtualShadow( pass, parameters );
                             m_VsmCasters[level].emplace_back( weak, nextBlock );
                             nextBlock += declared;
                         }
                     }
                 },
                 [this, level, view]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "VSM: Raster" );
                     if ( auto drawn = m_VsmDraws[level].Record( context ); !drawn.IsSuccess() )
                         return drawn;
                     for ( const auto& [weak, firstBlock] : m_VsmCasters[level] )
                     {
                         const auto caster = weak.lock();
                         if ( !caster )
                             continue; // gone since the setup: its draws went with it
                         if ( auto cast =
                                   caster->RecordVirtualShadow( context, firstBlock, m_CascadeCasterVP[view] );
                              !cast.IsSuccess() )
                             return cast;
                     }
                     return BOOLSUCCESS;
                 } );
        }
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::System
