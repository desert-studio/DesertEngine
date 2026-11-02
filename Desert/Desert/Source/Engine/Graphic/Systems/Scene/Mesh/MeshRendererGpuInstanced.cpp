#include "MeshRendererInternal.hpp"

#include <Engine/Graphic/Materials/Mesh/MaterialShadow.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>

#include <glm/gtc/packing.hpp>

#include <bit>
#include <algorithm>
#include <cmath>
#include <unordered_set>

// GPU-INSTANCED SECTIONS (GpuInstances.hpp): the landscape grass. The transforms and the instance count are the
// producer's compute output, so nothing here culls, sorts or uploads instances: each section is one indirect draw
// through its material's (Instanced x pass) cell. The cell is a COPY of the material (MaterialService::
// GetViewVariant, keyed per section and owned by this renderer, so it dies with the view) because a material owns one InstanceTransforms binding per frame and the
// CPU-batched ISMs of the same material fill the shared one.

namespace Desert::Graphic::System
{
    using namespace MeshRendererDetail;

    namespace
    {
        // View keys apart from the cascades' (0..kMaxCascades-1) and the cube faces' the masked casters use.
        constexpr uint32_t kGpuInstancedViewBase = 0x40000000u;

        const DataDrivenMaterial* SectionTemplate( const MaterialInstancePtr& instance )
        {
            return instance ? dynamic_cast<const DataDrivenMaterial*>( instance->GetParentMaterial() ) : nullptr;
        }

        // The LOD levels a GPU-instanced section is drawn in (MeshRenderer::GpuInstancedRenderData::Levels): a
        // blade strip's levels are its submeshes, drawn by one draw.
        uint32_t LevelsOf( const MeshRenderer::GpuInstancedRenderData& gpu )
        {
            return gpu.Path == MeshVertexPath::Instanced ? std::max( 1u, gpu.Levels ) : 1u;
        }

        // Where level @p level's records begin: one record per submesh per level (GpuInstances.hpp).
        uint64_t LevelArgsOffset( const MeshRenderer::GpuInstancedRenderData& gpu, uint32_t level )
        {
            return GpuInstanceArgsOffset( level * static_cast<uint32_t>( gpu.Mesh->GetSubmeshes().size() ) );
        }

        // GRASS-VIS: view keys of the post-occlusion draws' instances, apart from phase 1's of the same cell view.
        constexpr uint32_t kGpuInstancedPostViewOffset = 0x04000000u;

        // The permutation of a GPU-instanced section's cell for the list it draws: @p ids (the camera's or a
        // cascade's, GpuInstanceRefs::Ids) - MeshRenderer::SurfaceInstanceIdsVariant, the one spelling the cook
        // lists too; @p ids false (the full list, drawn directly): the default one. @p depthEqual: over the
        // G-buffer's SurfaceDepthEqualVariant. Built once (each is a ShaderVariant of heap strings).
        const Core::ShaderVariant& InstancePermutation( const bool ids, const bool depthEqual )
        {
            static const Core::ShaderVariant s_Plain{};
            static const Core::ShaderVariant s_Ids   = MeshRenderer::SurfaceInstanceIdsVariant();
            static const Core::ShaderVariant s_Equal = MeshRenderer::SurfaceDepthEqualVariant();
            static const Core::ShaderVariant s_EqualIds =
                 MeshRenderer::SurfaceInstanceIdsVariant( MeshRenderer::SurfaceDepthEqualVariant() );
            if ( depthEqual )
                return ids ? s_EqualIds : s_Equal;
            return ids ? s_Ids : s_Plain;
        }

        // The producer's buffers into a cascade's caster cell: the section's one Instances, @p ids (the drawn
        // list's ids, the kSurfaceInstanceIdsDefine cell's GpuInstanceIds; null: the full list, drawn directly),
        // and a blade's shape (MeshPathOwnBinding). The camera passes set them through their kept record's slots.
        void BindGpuInstancedBuffers( DataDrivenMaterial& cell, const MeshRenderer::GpuInstancedRenderData& gpu,
                                      const std::shared_ptr<ShaderResources::StorageBuffer>& ids )
        {
            if ( auto* sb = cell.Get<StorageBufferProperty>( kInstanceTransformsBlock ) )
                sb->SetBuffer( gpu.Instances );
            if ( ids )
                if ( auto* sb = cell.Get<StorageBufferProperty>( kGpuInstanceIdsBlock ) )
                    sb->SetBuffer( ids );
            if ( gpu.Path == MeshVertexPath::GrassBlade )
                if ( auto* sb = cell.Get<StorageBufferProperty>( kGrassBladeShapeBlock ) )
                    sb->SetBuffer( gpu.BladeShape );
        }

        // MeshRenderer::GpuInstancedDrawRecord's key: the section, the pass, phase 2, the G-buffer's EQUAL half,
        // and the forward prepass pairing (the companion-less Depth call is the deferred early-Z pass).
        uint64_t GpuInstancedDrawKey( const uint32_t section, const MeshPass pass, const bool postOcclusion,
                                      const bool prepassed, const bool paired )
        {
            uint64_t key = ( uint64_t{ section } << 32 ) | ( uint64_t{ static_cast<uint32_t>( pass ) } << 8 );
            if ( postOcclusion )
                key |= 1u;
            if ( prepassed )
                key |= 2u;
            if ( paired )
                key |= 4u;
            return key;
        }
    } // namespace

    MeshRenderer::~MeshRenderer()
    {
        // The section copies hold this view's buffers (the producers' instances, the Materials rows written
        // under the view): their instances first, then the copies, before the view's ledger check.
        m_GpuInstancedDraws.clear();
        m_GpuInstancedQueue.clear();
        if ( auto* materials = Runtime::ResourceRegistry::GetMaterialService() )
            materials->ReleaseViewVariants( this );
    }

    Core::ShaderVariant MeshRenderer::SurfaceDepthEqualVariant()
    {
        Core::ShaderVariant permutation;
        permutation.Defines.emplace_back( kSurfaceDepthEqualDefine );
        permutation.ShippedByCook = true;
        return permutation;
    }

    Core::ShaderVariant MeshRenderer::SurfaceInstanceIdsVariant( Core::ShaderVariant permutation )
    {
        permutation.Defines.emplace_back( kSurfaceInstanceIdsDefine );
        permutation.ShippedByCook = true;
        return permutation;
    }

    MeshRenderer::GpuInstancedDrawRecord* MeshRenderer::GpuInstancedDrawOf( const GpuInstancedRenderData& gpu,
                                                                            const MeshPass                pass,
                                                                            GraphicsPipeline* const       carrier,
                                                                            const StaticPassCells* const  companion,
                                                                            const bool postOcclusion,
                                                                            const bool prepassed )
    {
        auto* const materials = Runtime::ResourceRegistry::GetMaterialService();
        if ( materials == nullptr )
            return nullptr;
        if ( materials->GetInvalidationVersion() != m_GpuInstancedStamp )
        {
            // The cells and the templates they copy may be in the graveyard: every record goes.
            m_GpuInstancedDraws.clear();
            m_GpuInstancedStamp = materials->GetInvalidationVersion();
        }
        const bool              cameraList       = gpu.CameraIds != nullptr && gpu.CameraRefs != nullptr;
        GraphicsPipeline* const companionCarrier = companion != nullptr ? companion->InstancedState : nullptr;
        const MeshPass          companionPass    = companion != nullptr ? companion->Pass : MeshPass::Forward;
        GpuInstancedDrawRecord& record =
             m_GpuInstancedDraws[GpuInstancedDrawKey( gpu.Key, pass, postOcclusion, prepassed, companion != nullptr )];

        // The revision (GpuInstancedDrawRecord): the producer's instance by OWNER (an expired one never equals a
        // live one, whatever address the new one took), and every input the resolve below reads apart from the
        // material service's own state, which its invalidation version covers.
        const bool sameSource = !record.Source.expired() && !record.Source.owner_before( gpu.Material ) &&
                                !gpu.Material.owner_before( record.Source );
        const bool current = sameSource && record.Path == gpu.Path && record.CameraList == cameraList &&
                             record.Carrier == carrier && record.CompanionCarrier == companionCarrier &&
                             record.CompanionPass == companionPass &&
                             record.EqualCarrier == m_InstancedGBufferEqualPipeline.get() &&
                             record.ForwardVirtual == m_ForwardVirtualLocal && record.EarlyZ == m_EarlyZPass;
        if ( current )
        {
            // A texture in flight: drawn from the frame its material is rebuilt bound (GRASS-SETTLE).
            if ( materials->AwaitsTextures( record.Parent ) )
                return nullptr;
            return &record;
        }

        const DataDrivenMaterial* const parent = SectionTemplate( gpu.Material );
        // Not resolved while a texture is in flight (the cell would copy the slot defaults): asked again next frame.
        if ( materials->AwaitsTextures( parent ) )
        {
            record = GpuInstancedDrawRecord{};
            return nullptr;
        }
        record                  = GpuInstancedDrawRecord{};
        record.Source           = gpu.Material;
        record.Path             = gpu.Path;
        record.CameraList       = cameraList;
        record.Carrier          = carrier;
        record.CompanionCarrier = companionCarrier;
        record.CompanionPass    = companionPass;
        record.EqualCarrier     = m_InstancedGBufferEqualPipeline.get();
        record.ForwardVirtual   = m_ForwardVirtualLocal;
        record.EarlyZ           = m_EarlyZPass;
        record.Parent           = parent;
        // The deferred early-Z pass (the companion-less Depth call, DeclareMaskedDepthDraws) draws the sections
        // this frame's EarlyZPass admits (EarlyZAdmits: the Masked ones, or under All every opaque one too). A
        // section it does not admit gets no cell in it (nothing compiles a Depth pipeline it never draws).
        const bool maskedPrepass = pass == MeshPass::Depth && companion == nullptr;
        record.Admitted          = !maskedPrepass || EarlyZAdmits( parent );
        if ( !record.Admitted )
            return &record;

        const bool owned    = parent != nullptr && materials->Owns( parent );
        const bool twoSided = gpu.Material != nullptr && gpu.Material->IsTwoSided();
        // One cell (and instance) per section x view pass.
        const auto viewOf = [&gpu]( const MeshPass of )
        { return kGpuInstancedViewBase + gpu.Key * kMeshPassCount + static_cast<uint32_t>( of ); };
        // This view's copy of the section's (path x pass) cell; the Forward one in its virtual-shadow
        // permutation (ForwardVirtualCell: a blade or instance under a local light reads the VSM).
        const auto cellOf = [&]( const MeshPass of ) -> DataDrivenMaterial*
        {
            if ( !owned )
                return nullptr;
            return ForwardVirtualCell( materials->GetViewVariant( parent, gpu.Path, of, viewOf( of ),
                                                                  InstancePermutation( cameraList, false ), this ),
                                       parent, gpu.Path, of, viewOf( of ), cameraList );
        };
        // The G-buffer cell of a section the masked prepass drew: the clip-less equal-depth permutation.
        const auto depthEqualCell = [&]() -> DataDrivenMaterial*
        {
            if ( !owned )
                return nullptr;
            return materials->GetViewVariant( parent, gpu.Path, MeshPass::GBuffer, viewOf( MeshPass::GBuffer ),
                                              InstancePermutation( cameraList, true ), this );
        };
        record.Cell = prepassed ? depthEqualCell() : cellOf( pass );
        if ( record.Cell == nullptr )
            return &record;
        // The resolve's full-screen triangle is never culled (its carrier is CullMode None).
        record.Pipeline = CullPermutation( CellPipeline( prepassed ? record.EqualCarrier : carrier, *record.Cell ),
                                           twoSided || pass == MeshPass::VisibilityResolve );
        if ( maskedPrepass )
        {
            // The masked prepass's G-buffer half (the equal-depth cell's pipeline), asked Ready every frame.
            DataDrivenMaterial* const equalCell = depthEqualCell();
            record.EqualRefused                 = equalCell == nullptr;
            if ( equalCell != nullptr )
                record.EqualPipeline =
                     CullPermutation( CellPipeline( record.EqualCarrier, *equalCell ), twoSided );
        }
        if ( companion != nullptr )
        {
            // The forward prepass pairing's other half: its cell in the companion's pass.
            DataDrivenMaterial* const other = cellOf( companion->Pass );
            if ( other != nullptr && companionCarrier != nullptr )
                record.CompanionPipeline = CullPermutation( CellPipeline( companionCarrier, *other ), twoSided );
        }
        // The cell's buffer slots, kept: the producer's buffers are set through them every frame.
        record.Instance       = record.Cell->CreateInstance( "GpuInstanced" );
        record.InstancesBlock = record.Cell->Get<StorageBufferProperty>( kInstanceTransformsBlock );
        if ( cameraList )
            record.IdsBlock = record.Cell->Get<StorageBufferProperty>( kGpuInstanceIdsBlock );
        if ( gpu.Path == MeshVertexPath::GrassBlade )
            record.BladeBlock = record.Cell->Get<StorageBufferProperty>( kGrassBladeShapeBlock );
        // Find: a surface template with no parameters declares no Materials row block (LendMaterialTable).
        record.MaterialBlock = record.Cell->Find<StorageBufferProperty>( kMaterialRowBlock );
        record.Cell->SetPushMatrix( glm::mat4( 1.0f ) ); // unused by the instanced vertex stage
        return &record;
    }

    void MeshRenderer::BuildGpuInstancedDraws( MeshDrawList& list, GraphicsPipeline* instancedPipeline,
                                               const MeshPass pass, const SceneFrameBinding& frameState,
                                               const StaticPassCells* companion, const bool postOcclusion )
    {
        if ( m_GpuInstancedQueue.empty() || instancedPipeline == nullptr )
            return;

        // A section with a box is drawn only where the view's frustum meets it (GpuInstancedRenderData::WorldBounds).
        const auto*                        camera = m_SceneRenderer != nullptr ? m_SceneRenderer->GetMainCamera() : nullptr;
        const std::optional<Core::Frustum> frustum =
             camera != nullptr ? std::optional<Core::Frustum>( camera->GetFrustum() ) : std::nullopt;

        // The deferred early-Z pass is the companion-less Depth call; the forward depth prepass (Depth with a
        // companion) draws every section. The G-buffer draws a section the early-Z pass drew with an EQUAL depth
        // test, without writes and without the clip (m_InstancedGBufferEqualPipeline, its cell in
        // SurfaceDepthEqualVariant: UE's base pass after the early-Z pass).
        const bool maskedPrepass = pass == MeshPass::Depth && companion == nullptr;
        // GRASS-VIS: a blade section this frame draws through the visibility buffer (m_GrassVisibilitySections,
        // chosen by DeclareMaskedDepthDraws) is in the Visibility raster and its resolve and nowhere else of the
        // deferred geometry: not in the masked prepass, not in the G-buffer. Every other section is in neither.
        const bool visibilityPass = pass == MeshPass::Visibility || pass == MeshPass::VisibilityResolve;
        // Each phase's G-buffer draws EQUAL exactly the sections its own depth pass drew: phase 1's "Deferred:
        // Masked Depth Prepass" (m_MaskedPrepassed), phase 2's "Grass: Depth Prepass Post-Occlusion"
        // (m_MaskedPrepassedPost, the same records the G-buffer post pass draws).
        std::vector<uint32_t>& phasePrepassed = postOcclusion ? m_MaskedPrepassedPost : m_MaskedPrepassed;

        for ( const GpuInstancedRenderData& gpu : m_GpuInstancedQueue )
        {
            if ( gpu.WorldBounds && frustum && !frustum->Intersects( *gpu.WorldBounds ) )
                continue;
            // OCCLUSION PER VIEW (GRASS-VIS): the camera passes draw the camera's list when the producer made one
            // (GpuInstancedRenderData::CameraIds); the post-occlusion passes draw only phase 2's records of it.
            const bool cameraList = gpu.CameraIds != nullptr && gpu.CameraRefs != nullptr;
            if ( postOcclusion && ( !cameraList || gpu.PostRefs == nullptr ) )
                continue;
            const GpuInstanceRefs& refs     = cameraList ? *gpu.CameraRefs : *gpu.Refs;
            const RDG::BufferRef   drawArgs = postOcclusion ? gpu.PostRefs->Args : refs.Args;
            const bool             viaVisibility =
                 gpu.Path == MeshVertexPath::GrassBlade &&
                 std::find( m_GrassVisibilitySections.begin(), m_GrassVisibilitySections.end(), gpu.Key ) !=
                      m_GrassVisibilitySections.end();
            if ( visibilityPass && !viaVisibility )
                continue;
            if ( viaVisibility && ( maskedPrepass || pass == MeshPass::GBuffer ) )
                continue;
            const bool prepassed = pass == MeshPass::GBuffer &&
                                   std::find( phasePrepassed.begin(), phasePrepassed.end(), gpu.Key ) !=
                                        phasePrepassed.end();

            // The kept record (GpuInstancedDrawOf); null: a texture of its material is in flight.
            GpuInstancedDrawRecord* const record =
                 GpuInstancedDrawOf( gpu, pass, instancedPipeline, companion, postOcclusion, prepassed );
            if ( record == nullptr || !record->Admitted )
                continue;
            GpuInstancedDrawRecord& draw = *record;
            if ( draw.Cell == nullptr )
            {
                static std::unordered_set<const DataDrivenMaterial*> s_Refused;
                if ( s_Refused.insert( draw.Parent ).second )
                    LOG_ERROR( "[MeshRenderer] a GPU-instanced section's material has no ({} x {}) cell; the "
                               "section is not drawn (MaterialService named the cell it refused).",
                               MeshVertexPathName( gpu.Path ), MeshPassName( pass ) );
                continue;
            }
            if ( draw.Pipeline == nullptr || draw.Pipeline->GetReadiness() != PipelineReadiness::Ready )
                continue; // still in the driver: drawn from the frame it is Ready (RDG-PSO)
            if ( maskedPrepass )
            {
                // Paired like the forward prepass: the masked prepass draws a section only when its G-buffer half
                // (the equal-depth cell's pipeline) is Ready too, since that half shades exactly the texels whose
                // depth this pass wrote and nothing else would. Until then the G-buffer draws it with its own clip
                // and depth writes (not recorded in m_MaskedPrepassed).
                if ( draw.EqualRefused )
                {
                    static std::unordered_set<const DataDrivenMaterial*> s_RefusedEqual;
                    if ( s_RefusedEqual.insert( draw.Parent ).second )
                        LOG_ERROR(
                             "[MeshRenderer] a Masked GPU-instanced section's material has no ({} x GBuffer) "
                             "{} permutation; the section is not in the masked depth prepass and pays its "
                             "alpha-tested overdraw in the G-buffer.",
                             MeshVertexPathName( gpu.Path ), kSurfaceDepthEqualDefine );
                    continue;
                }
                if ( draw.EqualPipeline == nullptr || draw.EqualPipeline->GetReadiness() != PipelineReadiness::Ready )
                    continue;
            }
            if ( companion != nullptr )
            {
                // Paired with the forward depth prepass: the prepass draws a section only when its cell is Ready
                // in BOTH passes (and its textures are in) and records it; the base pass of the same frame draws
                // exactly the recorded ones.
                if ( pass == MeshPass::Forward && PrepassCurrent() )
                {
                    if ( !m_PrepassGpuSections.contains( static_cast<uint64_t>( gpu.Key ) ) )
                        continue;
                }
                else
                {
                    if ( draw.CompanionPipeline == nullptr ||
                         draw.CompanionPipeline->GetReadiness() != PipelineReadiness::Ready )
                        continue;
                    if ( pass == MeshPass::Depth )
                        m_PrepassGpuSections.insert( static_cast<uint64_t>( gpu.Key ) );
                }
            }

            // The producer's buffers through the kept slots: the section's one Instances, the camera list's ids
            // (the kSurfaceInstanceIdsDefine cell's GpuInstanceIds), a blade's shape (MeshPathOwnBinding). The
            // same buffer again is no write (StorageBufferProperty::SetBuffer).
            if ( draw.InstancesBlock != nullptr )
                draw.InstancesBlock->SetBuffer( gpu.Instances );
            if ( draw.IdsBlock != nullptr )
                draw.IdsBlock->SetBuffer( gpu.CameraIds );
            if ( draw.BladeBlock != nullptr )
                draw.BladeBlock->SetBuffer( gpu.BladeShape );
            // The section's row is the scene material table's (SyncGpuScene claims it every sync), read by the
            // cell of its template; the table is lent through the kept slot (LendMaterialTable).
            const std::optional<uint32_t> materialRow = FindMaterialRow( draw.Parent, gpu.Material.get() );
            if ( !materialRow )
                continue;
            if ( draw.MaterialBlock != nullptr )
            {
                if ( m_GpuScene == nullptr || m_GpuScene->GetMaterialsBuffer() == nullptr )
                {
                    if ( !LendMaterialTable( *draw.Cell ) ) // says why
                        continue;
                }
                else
                    draw.MaterialBlock->LendViewBuffer( m_GpuScene->GetMaterialsBuffer() );
            }
            const uint32_t row = *materialRow;
            frameState.ApplyTo( draw.Instance.get() );
            if ( maskedPrepass )
                phasePrepassed.push_back( gpu.Key );

            // THE RESOLVE (GRASS-VIS): one full-screen triangle per section - record 0 of the view's resolve table
            // (VirtualGeometry::VisibilityBuffer: {3, 1, 0, 0, 0}, the strip's first three indices, which
            // Vertex_GrassBlade.glslh's resolve branch places over the screen), every other submesh hidden. The
            // transforms are still the section's: the fragment stage rebuilds the blade its pixel's word names.
            if ( pass == MeshPass::VisibilityResolve )
            {
                if ( !m_GrassResolveArgs.IsValid() )
                    continue;
                list.Add( { .Pipeline           = draw.Pipeline,
                            .Mesh               = gpu.Mesh,
                            .Transform          = glm::mat4( 1.0f ),
                            .Material           = draw.Cell->GetMaterialExecutor(),
                            .HiddenSubmeshMask  = SubmeshMask( ~uint64_t{ 1 } ),
                            .IndirectArgs       = m_GrassResolveArgs,
                            .IndirectInstances  = refs.Instances,
                            .IndirectIds        = refs.Ids,
                            .IndirectArgsOffset = GpuInstanceArgsOffset( 0 ),
                            .Push               = DrawPushFields{}
                                         .MaterialIndex( row )
                                         .Wind( PackViewInstanceWind( gpu.Wind, gpu.PrevInstanceOffset ) )
                                         .PrimitiveFlags( 0u ) } );
                continue;
            }

            // One draw per LOD level the producer sorted its instances into, each reading its level's records.
            for ( uint32_t level = 0; level < LevelsOf( gpu ); ++level )
                list.Add( { .Pipeline           = draw.Pipeline,
                            .Mesh               = gpu.Mesh,
                            .Transform          = glm::mat4( 1.0f ),
                            .Material           = draw.Cell->GetMaterialExecutor(),
                            .HiddenSubmeshMask  = gpu.HiddenSubmeshes,
                            .IndirectArgs       = drawArgs,
                            .IndirectInstances  = refs.Instances,
                            .IndirectIds        = refs.Ids,
                            .IndirectArgsOffset = LevelArgsOffset( gpu, level ),
                            .Push               = DrawPushFields{}
                                         .MaterialIndex( row )
                                         .Wind( PackViewInstanceWind( gpu.Wind, gpu.PrevInstanceOffset ) )
                                         .PrimitiveFlags( 0u ) } );
        }
    }

    bool MeshRenderer::GrassVisibilityReady( const GpuInstancedRenderData& gpu )
    {
        // The same records BuildGpuInstancedDraws draws the section with in the two passes (GpuInstancedDrawOf):
        // their cells, and their pipelines through the two carriers, all Ready.
        for ( const auto& [of, carrier] :
              { std::pair{ MeshPass::Visibility, m_InstancedVisibilityPipeline.get() },
                std::pair{ MeshPass::VisibilityResolve, m_InstancedVisibilityResolvePipeline.get() } } )
        {
            if ( carrier == nullptr )
                return false;
            const GpuInstancedDrawRecord* const record = GpuInstancedDrawOf( gpu, of, carrier, nullptr, false, false );
            if ( record == nullptr || record->Cell == nullptr || record->Pipeline == nullptr ||
                 record->Pipeline->GetReadiness() != PipelineReadiness::Ready )
                return false;
        }
        return true;
    }

    MeshRenderer::BladeCasterLod MeshRenderer::BladeShadowSet( const GpuInstancedRenderData& gpu,
                                                               const uint32_t                cascade ) const
    {
        // A segment of the blade per this many texels of its height: a finer curve than that is lost in the
        // map's own filtering (the PCF / PCSS footprint is several texels wide).
        constexpr float     kBladeShadowTexelsPerSegment = 4.0f;
        const GpuInstanceBladeShadowLod& lod             = gpu.ShadowLod;
        const float                      worldPerTexel   = GetCascadeWorldPerTexel( cascade );
        if ( gpu.Path != MeshVertexPath::GrassBlade || lod.Levels == 0u || worldPerTexel <= 0.0f )
            return {};
        const float texel  = worldPerTexel * std::exp2( static_cast<float>( std::clamp( m_FoliageShadowLodBias, 0, 3 ) ) );
        const auto  wanted = static_cast<uint32_t>(
             std::max( 1.0f, std::ceil( lod.Height / ( texel * kBladeShadowTexelsPerSegment ) ) ) );
        BladeCasterLod out{};
        // The strip's levels halve its segments down to one (GrassRenderer::BladeStrip): the coarsest that still
        // has the segments the cascade resolves.
        for ( uint32_t segments = std::max( 1u, lod.Segments ); out.Set + 1u < lod.Levels; ++out.Set )
        {
            const uint32_t next = std::max( 1u, segments / 2u );
            if ( next < wanted )
                break;
            segments = next;
        }
        out.ThinFrom = lod.PixelSpread > 0.0f && lod.StartCull > 0.0f ? texel / lod.PixelSpread : 0.0f;
        return out;
    }

    bool MeshRenderer::BuildGpuInstancedShadowDraws( const uint32_t c, const uint32_t shadowView,
                                                     const ShadowCasterLayer layer, MeshDrawList& list )
    {
        auto* materials = Runtime::ResourceRegistry::GetMaterialService();
        if ( materials == nullptr )
            return false;
        bool pending = false;
        // The cascade's caster box (as BuildShadowCascadeDraws culls the static casters): a section with a box casts
        // only into the cascades it meets.
        const Core::Frustum cascadeFrustum( m_CascadeCasterVP[shadowView], glm::mat4( 1.0f ) );
        for ( const GpuInstancedRenderData& gpu : m_GpuInstancedQueue )
        {
            if ( !gpu.CastShadows )
                continue;
            if ( gpu.WorldBounds && !cascadeFrustum.Intersects( *gpu.WorldBounds ) )
                continue;
            // WHO SWAYS IN THE SHADOWS, for a still section (GpuInstancedRenderData::Still, the grass), per instance
            // in its vertex stage (CasterWindLayer::*, Mesh/Surface/CasterWindLayer.glslh; the GPU cull's
            // GpuCull_CastsRestPose, the CPU walk's ShadowIsmInstances): a swaying one casts its instances past the
            // shadow wind distance in their rest pose, into the static layer, and its nearer ones swaying, over the
            // cache; one that does not sway is all static. A section whose instances move by themselves is drawn
            // whole over the cache, as before the cache knew them.
            const bool still = gpu.Still.has_value();
            const bool sways = gpu.Wind.Sways();
            uint32_t   split = 0;
            if ( layer == ShadowCasterLayer::Static )
            {
                if ( !still )
                    continue;
                split = sways ? CasterWindLayer::kRule | CasterWindLayer::kDropNear : 0u;
            }
            else if ( layer == ShadowCasterLayer::Movable )
            {
                if ( still && !sways )
                    continue;
                split = still ? CasterWindLayer::kRule | CasterWindLayer::kDropFar : 0u;
            }
            else
                split = still && sways ? CasterWindLayer::kRule : 0u;
            // Only a static layer keeps what it drew: a still section it leaves out for now stales it.
            const bool keeps = layer == ShadowCasterLayer::Static;

            const DataDrivenMaterial* parent = SectionTemplate( gpu.Material );
            if ( materials->AwaitsTextures( parent ) )
            {
                pending = pending || keeps;
                continue; // casts from the frame it draws: a masked caster would cut the default alpha
            }
            // One caster cell per (section x cascade x layer): the static layer and the movers over it are drawn in
            // the same frame from light cameras of their own (the cascade's and its movable view's).
            const uint32_t layerBase = layer == ShadowCasterLayer::Movable ? 0x08000000u : 0x0A000000u;
            const uint32_t view      = kGpuInstancedViewBase + layerBase + gpu.Key * kCascadeViews + c;
            // Its own (Instanced x ShadowDepth) cell, masked or not: the shared instanced caster's transforms are
            // the CPU-batched ISMs'.
            // The cascade's own list when the producer keeps one for it (GPU-COST-21: the instances inside its
            // caster box, UE's per-shadow-view instance cull), read through its ids (GPU-COST-22), else the full
            // list (GRASS-VIS): either way the light sees what the camera's depth hides.
            const bool          ownList = gpu.CascadeRefs != nullptr && ( gpu.CascadeMask & ( 1u << c ) ) != 0u;
            DataDrivenMaterial* caster =
                 parent != nullptr && materials->Owns( parent )
                      ? materials->GetViewVariant( parent, gpu.Path, MeshPass::ShadowDepth, view,
                                                   InstancePermutation( ownList, false ), this )
                      : nullptr;
            if ( caster == nullptr )
            {
                static std::unordered_set<const DataDrivenMaterial*> s_Refused;
                if ( s_Refused.insert( parent ).second )
                    LOG_ERROR( "[Shadows] a GPU-instanced section's material has no ({} x ShadowDepth) cell; "
                               "the section casts no shadow (MaterialService named the cell it refused).",
                               MeshVertexPathName( gpu.Path ) );
                continue;
            }
            GraphicsPipeline* pipeline = MaskedCasterPipeline( *caster, gpu.Path, MeshPass::ShadowDepth );
            if ( pipeline != nullptr && pipeline->GetReadiness() == PipelineReadiness::Compiling )
                pending = pending || keeps;
            if ( pipeline == nullptr || pipeline->GetReadiness() != PipelineReadiness::Ready )
                continue;

            WriteLightCamera( *caster, glm::mat4( 1.0f ), m_CascadeCasterVP[shadowView], m_WorldTimeSeconds,
                              m_CascadeCasterBias[shadowView], m_ParallaxQuality, CasterWindOrigin() );
            BindGpuInstancedBuffers( *caster, gpu, ownList ? gpu.CascadeIds : nullptr );
            const GpuInstanceRefs& refs = ownList ? *gpu.CascadeRefs : *gpu.Refs;
            const uint64_t         listBytes =
                 ownList ? static_cast<uint64_t>( std::popcount( gpu.CascadeMask & ( ( 1u << c ) - 1u ) ) ) *
                                gpu.CascadeArgBytes
                                 : 0u;
            // The section's row is the scene material table's (SyncGpuScene), read by its masked caster cell.
            const std::optional<uint32_t> materialRow = FindMaterialRow( parent, gpu.Material.get() );
            if ( !materialRow || !LendMaterialTable( *caster ) )
                continue;
            const uint32_t row = *materialRow;

            // A light view's B.w is the split (InstanceWindPush; no previous instance in a light view), and a
            // blade's B.z the cascade's thinning (BladeShadowSet: half x the distance it thins from, half y the
            // generator's StartCull through the lens; 0 = every instance, Vertex_GrassBlade.glslh).
            InstanceWindPush     wind   = PackInstanceWind( gpu.Wind );
            const BladeCasterLod shadow = BladeShadowSet( gpu, c );
            wind.B.w                    = std::bit_cast<float>( split );
            wind.B.z                    = std::bit_cast<float>(
                 shadow.ThinFrom > 0.0f ? glm::packHalf2x16( glm::vec2( std::min( shadow.ThinFrom, 65000.0f ),
                                                                         std::min( gpu.ShadowLod.StartCull, 65000.0f ) ) )
                                                         : 0u );
            for ( uint32_t level = 0; level < LevelsOf( gpu ); ++level )
                list.Add(
                     { .Pipeline          = pipeline,
                       .Mesh              = gpu.Mesh,
                       .Transform         = glm::mat4( 1.0f ),
                       .Material          = caster->GetMaterialExecutor(),
                       .HiddenSubmeshMask = gpu.HiddenSubmeshes,
                       .IndirectArgs      = refs.Args,
                       .IndirectInstances = refs.Instances,
                       .IndirectIds       = refs.Ids,
                       // A blade's levels are one draw; its set is the cascade's shadow LOD; the cascade's
                       // own list is its copy of the records.
                       .IndirectArgsOffset = listBytes + LevelArgsOffset( gpu, level + shadow.Set ),
                       .Push =
                            DrawPushFields{}.Transform( glm::mat4( 1.0f ) ).MaterialIndex( row ).Wind( wind ) } );
        }
        return pending;
    }
} // namespace Desert::Graphic::System
