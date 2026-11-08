#include "MeshRendererInternal.hpp"

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Graphic/Materials/Mesh/MaterialShadow.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

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

        // A section's buckets (GpuInstancedRenderData::PrepassBuckets): a mesh's LOD levels (one draw of a range of
        // them, MeshDrawCommand::IndirectLevels), a blade's strip levels (the strip's submeshes, one draw).
        uint32_t BucketsOf( const MeshRenderer::GpuInstancedRenderData& gpu )
        {
            if ( gpu.Path == MeshVertexPath::GrassBlade )
                return static_cast<uint32_t>( gpu.Mesh->GetSubmeshes().size() );
            return LevelsOf( gpu );
        }

        // A blade strip's draw of buckets [first, end) of its @p buckets: the section's hidden submeshes and every
        // strip level outside the range (submesh b = bucket b).
        uint64_t BucketsHidden( const uint64_t hidden, const uint32_t buckets, const uint32_t first,
                                const uint32_t end )
        {
            uint64_t outside = 0;
            for ( uint32_t b = 0; b < std::min( buckets, 64u ); ++b )
                if ( b < first || b >= end )
                    outside |= uint64_t{ 1 } << b;
            return hidden | outside;
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

    Core::ShaderVariant MeshRenderer::SurfaceSceneInstancesVariant( Core::ShaderVariant permutation )
    {
        permutation.Defines.emplace_back( kSurfaceSceneInstancesDefine );
        permutation.ShippedByCook = true;
        return permutation;
    }

    Core::ShaderVariant MeshRenderer::SurfaceDrawRowsVariant( Core::ShaderVariant permutation )
    {
        permutation.Defines.emplace_back( Core::Preprocess::kSurfaceDrawRowsDefine );
        permutation.ShippedByCook = true;
        return permutation;
    }

    namespace
    {
        // InstancePermutation, in the compacted bins' permutation when @p drawRows (MeshRenderer::DrawBinsOf).
        const Core::ShaderVariant& InstanceDrawPermutation( const bool ids, const bool depthEqual,
                                                            const bool drawRows )
        {
            if ( !drawRows )
                return InstancePermutation( ids, depthEqual );
            static const Core::ShaderVariant s_Rows    = MeshRenderer::SurfaceDrawRowsVariant();
            static const Core::ShaderVariant s_RowsIds =
                 MeshRenderer::SurfaceDrawRowsVariant( MeshRenderer::SurfaceInstanceIdsVariant() );
            static const Core::ShaderVariant s_RowsEqual =
                 MeshRenderer::SurfaceDrawRowsVariant( MeshRenderer::SurfaceDepthEqualVariant() );
            static const Core::ShaderVariant s_RowsEqualIds = MeshRenderer::SurfaceDrawRowsVariant(
                 MeshRenderer::SurfaceInstanceIdsVariant( MeshRenderer::SurfaceDepthEqualVariant() ) );
            if ( depthEqual )
                return ids ? s_RowsEqualIds : s_RowsEqual;
            return ids ? s_RowsIds : s_Rows;
        }

        // A producer buffer's device address (a compacted bin's draw row); 0 without one.
        uint64_t AddressOf( const std::shared_ptr<ShaderResources::StorageBuffer>& buffer )
        {
            if ( buffer == nullptr )
                return 0u;
            const Common::ResultStr<uint64_t> address = buffer->GetDeviceAddress();
            if ( !address )
                return 0u;
            return address.GetValue();
        }

        // GRASS-GPU-DRIVEN-DRAWS: the vertex paths a compacted bin draws (MeshRenderer::kDrawRowsPaths, the cells
        // the cook lists in SurfaceDrawRowsVariant).
        bool BinnablePath( const MeshVertexPath path )
        {
            return std::ranges::find( MeshRenderer::kDrawRowsPaths, path ) !=
                   std::ranges::end( MeshRenderer::kDrawRowsPaths );
        }

        // THE GATHER RUNS OF ONE SECTION'S DRAW (GRASS-GPU-DRIVEN-DRAWS; GpuDrawCompactEntry), in the per-variety
        // path's draw order (Renderer::RenderMeshIndirect, MeshDrawArgumentsOrder::Submesh): @p groups LOD levels
        // from @p base (the device address of the first drawn level's record of submesh 0), @p submeshes records
        // per level, the submeshes in @p hidden not drawn. Of one level, each run of adjacent drawn submeshes is
        // one run at a record's stride (a blade strip's levels); of several, each drawn submesh is one run of its
        // record of every level at a level's stride (a mesh's LOD groups). @p emit( records, count, stride )
        // false stops the walk (a capacity), and so does this.
        template <typename Emit>
        bool ForEachRecordRun( const uint64_t base, const uint32_t groups, const uint32_t submeshes,
                               const uint64_t hidden, Emit&& emit ) noexcept
        {
            const uint32_t count      = std::min( submeshes, 64u );
            const uint64_t groupBytes = kGpuInstanceArgsRecordBytes * uint64_t{ submeshes };
            const auto     shown      = [hidden]( const uint32_t s ) { return ( ( hidden >> s ) & 1u ) == 0u; };
            uint32_t       s          = 0u;
            while ( s < count )
            {
                if ( !shown( s ) )
                {
                    ++s;
                    continue;
                }
                uint32_t runEnd = s + 1u;
                while ( groups == 1u && runEnd < count && shown( runEnd ) )
                    ++runEnd;
                const uint64_t records = base + kGpuInstanceArgsRecordBytes * uint64_t{ s };
                bool           kept    = false;
                if ( groups == 1u )
                    kept = emit( records, runEnd - s, static_cast<uint32_t>( kGpuInstanceArgsRecordBytes ) );
                else
                    kept = emit( records, groups, static_cast<uint32_t>( groupBytes ) );
                if ( !kept )
                    return false;
                s = runEnd;
            }
            return true;
        }
    } // namespace

    GpuDrawBinFrame* MeshRenderer::DrawBinsOf( const GpuInstancedRenderData& gpu, const MeshPass pass,
                                               const bool postOcclusion ) const
    {
        // The passes a bin draws in are the cooked ones (kDrawRowsPasses; the cascades' is CascadeBinsOf's).
        const bool rowsPass = pass != MeshPass::ShadowDepth &&
                              std::ranges::find( kDrawRowsPasses, pass ) != std::ranges::end( kDrawRowsPasses );
        if ( m_GrassPerVarietyDraws || !BinnablePath( gpu.Path ) || !rowsPass ||
             ( pass == MeshPass::Forward && m_ForwardVirtualLocal ) )
            return nullptr;
        // A mesh section's visibility raster reads its rows by address (GpuInstanceDrawRow::MeshVisibility): one
        // without them (the prepass pair this frame) is drawn by its own draws.
        if ( gpu.Path == MeshVertexPath::Instanced && pass == MeshPass::Visibility &&
             AddressOf( gpu.MeshVisibility ) == 0u )
            return nullptr;
        const auto& capabilities = EngineContext::GetInstance().GetCapabilities();
        if ( !capabilities.SupportsDrawIndirectCount || !capabilities.SupportsShaderDrawParameters )
            return nullptr;
        const bool             cameraList = gpu.CameraIds != nullptr && gpu.CameraRefs != nullptr;
        const GpuInstanceRefs* refs       = cameraList ? gpu.CameraRefs : gpu.Refs;
        if ( postOcclusion )
            refs = gpu.PostRefs;
        const size_t           phase = postOcclusion ? 1u : 0u;
        GpuDrawBinFrame* const bins  = gpu.DrawBins[phase];
        if ( refs == nullptr || refs->ArgsAddress == 0u || bins == nullptr || !bins->Open )
            return nullptr;
        // A mesh section bins only through its camera ids: the cook lists the Instanced rows cells for the id-list
        // materials (MaterialProgramVariants), the set its own ids cell comes from, so a material has both.
        if ( gpu.Path == MeshVertexPath::Instanced && !cameraList )
            return nullptr;
        // THE SECTION AS A WHOLE (UE InstanceCulling: a batch that does not fit falls back in every pass): its
        // verdict is decided at its first question of the frame and kept for every list of the phase, and an
        // admitted one reserves its slots of all of them (DrawBinSlots x kDrawBinCameraLists; its rows and entries
        // are never more than its slots), so no list bins it while a later one lacks the room. Refused, it takes
        // its own draws in every pass of the phase: past the table (Overflow, the producer grows it, bounded) or
        // for a buffer with no device address (its own reason, no growth).
        for ( const GpuDrawBinFrame::SectionVerdict& asked : bins->Verdicts )
            if ( asked.Key == gpu.Key )
                return asked.Admitted ? bins : nullptr;
        bool admitted = false;
        {
            const bool addressed = AddressOf( gpu.Instances ) != 0u &&
                                   ( !cameraList || AddressOf( gpu.CameraIds ) != 0u ) &&
                                   ( gpu.Path != MeshVertexPath::GrassBlade || AddressOf( gpu.BladeShape ) != 0u );
            const uint32_t reserve = DrawBinSlots( gpu ) * kDrawBinCameraLists;
            if ( !addressed )
            {
                static bool s_SaidAddress = false;
                if ( !s_SaidAddress )
                {
                    s_SaidAddress = true;
                    LOG_ERROR( "[MeshRenderer] a grass section's instances, ids or shape have no device address; "
                               "it draws per variety in every pass this frame." );
                }
            }
            else if ( reserve != 0u && bins->Reserved + reserve > bins->EntryCapacity )
            {
                bins->Overflow         = true; // the producer grows the table at its next frame
                static bool s_SaidFull = false;
                if ( !s_SaidFull )
                {
                    s_SaidFull = true;
                    LOG_ERROR( "[MeshRenderer] a grass section's draws do not fit its phase's compacted bins; it "
                               "draws per variety in every pass this frame." );
                }
            }
            else if ( reserve != 0u )
            {
                bins->Reserved += reserve;
                admitted = true;
            }
        }
        bins->Verdicts.push_back( { .Key = gpu.Key, .Admitted = admitted } );
        if ( !admitted )
            return nullptr;
        return bins;
    }

    uint32_t MeshRenderer::DrawBinSlots( const GpuInstancedRenderData& gpu )
    {
        // ForEachRecordRun over every level: a list's draw of buckets [first, end), and the G-buffer's two halves
        // together, never take more.
        uint32_t slots = 0u;
        ForEachRecordRun( 0u, LevelsOf( gpu ), static_cast<uint32_t>( gpu.Mesh->GetSubmeshes().size() ),
                          gpu.HiddenSubmeshes,
                          [&slots]( const uint64_t, const uint32_t count, const uint32_t ) -> bool
                          {
                              slots += count;
                              return true;
                          } );
        return slots;
    }

    GpuDrawBinFrame* MeshRenderer::CascadeBinsOf( const GpuInstancedRenderData& gpu ) const
    {
        if ( m_GrassPerVarietyDraws || !BinnablePath( gpu.Path ) )
            return nullptr;
        const auto& capabilities = EngineContext::GetInstance().GetCapabilities();
        if ( !capabilities.SupportsDrawIndirectCount || !capabilities.SupportsShaderDrawParameters )
            return nullptr;
        GpuDrawBinFrame* const bins = gpu.DrawBins[kGpuDrawBinCascadesPhase];
        if ( bins == nullptr || !bins->Open )
            return nullptr;
        return bins;
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
        // THE SPLIT (GpuInstancedDrawRecord::PrepassBuckets): its own revision, the section's buckets and the
        // producer's count of them the early-Z pair draws. No cell depends on it, so a lens that moves the small
        // buckets (a FOV change) re-derives only the split, never the cells and instances below.
        const uint32_t buckets     = BucketsOf( gpu );
        const uint32_t pairBuckets = std::min( gpu.PrepassBuckets, buckets );
        const auto     reviseSplit = [&]( GpuInstancedDrawRecord& of )
        {
            of.Buckets        = buckets;
            of.PrepassBuckets = pairBuckets;
        };

        // RENDER-JOB-CPU-4: the cells are the compacted bins' permutation while the section draws in them.
        const bool drawRows = DrawBinsOf( gpu, pass, postOcclusion ) != nullptr;
        // The revision (GpuInstancedDrawRecord): the producer's instance by OWNER (an expired one never equals a
        // live one, whatever address the new one took), and every input the resolve below reads apart from the
        // material service's own state, which its invalidation version covers.
        const bool sameSource = !record.Source.expired() && !record.Source.owner_before( gpu.Material ) &&
                                !gpu.Material.owner_before( record.Source );
        const bool current = sameSource && record.Path == gpu.Path && record.CameraList == cameraList &&
                             record.Carrier == carrier && record.CompanionCarrier == companionCarrier &&
                             record.CompanionPass == companionPass &&
                             record.EqualCarrier == m_InstancedGBufferEqualPipeline.get() &&
                             record.ForwardVirtual == m_ForwardVirtualLocal && record.EarlyZ == m_EarlyZPass &&
                             record.DrawRows == drawRows;
        // A REFUSAL BY A TEMPLATE THE SERVICE NO LONGER OWNS IS NOT KEPT. A material re-registered over its cell
        // (MaterialService::Register parks the old one) leaves the producer holding the old built material until
        // it re-resolves; a resolve between the two (an invalidation-version bump: a texture or load completion)
        // found `owned` false and the record kept that null cell as current for as long as the source stayed,
        // which is the cold-run "(GrassBlade x Depth) cell" refusal that MaterialService never named. Asked again
        // each frame until the producer's template is the service's (or the source changes).
        const bool staleRefusal =
             current && record.Admitted && record.Cell == nullptr && !materials->Owns( record.Parent );
        if ( current && !staleRefusal )
        {
            // A texture in flight: drawn from the frame its material is rebuilt bound (GRASS-SETTLE).
            if ( materials->AwaitsTextures( record.Parent ) )
                return nullptr;
            if ( record.Buckets != buckets || record.PrepassBuckets != pairBuckets )
                reviseSplit( record );
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
        record.DrawRows         = drawRows;
        record.Parent           = parent;
        reviseSplit( record );
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
            const bool                 rows        = DrawBinsOf( gpu, of, postOcclusion ) != nullptr;
            const Core::ShaderVariant& permutation = InstanceDrawPermutation( cameraList, false, rows );
            return ForwardVirtualCell( materials->GetViewVariant( parent, gpu.Path, of, viewOf( of ), permutation,
                                                                  this ),
                                       parent, gpu.Path, of, viewOf( of ), cameraList );
        };
        // The G-buffer cell of a section the masked prepass drew: the clip-less equal-depth permutation.
        const auto depthEqualCell = [&]() -> DataDrivenMaterial*
        {
            if ( !owned )
                return nullptr;
            const bool rows = DrawBinsOf( gpu, MeshPass::GBuffer, postOcclusion ) != nullptr;
            return materials->GetViewVariant( parent, gpu.Path, MeshPass::GBuffer, viewOf( MeshPass::GBuffer ),
                                              InstanceDrawPermutation( cameraList, true, rows ), this );
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
        // A bins' cell reads the transforms, the ids and the shape by address from its draw row: no slots.
        if ( !drawRows )
        {
            record.InstancesBlock = record.Cell->Get<StorageBufferProperty>( kInstanceTransformsBlock );
            if ( cameraList )
                record.IdsBlock = record.Cell->Get<StorageBufferProperty>( kGpuInstanceIdsBlock );
            if ( gpu.Path == MeshVertexPath::GrassBlade )
                record.BladeBlock = record.Cell->Get<StorageBufferProperty>( kGrassBladeShapeBlock );
        }
        // GRASS-VIS-MESH-1: the Instanced visibility cells read the section's rows (a bins' cell by address).
        if ( !drawRows && gpu.Path == MeshVertexPath::Instanced &&
             ( pass == MeshPass::Visibility || pass == MeshPass::VisibilityResolve ) )
            record.MeshRowsBlock = record.Cell->Get<StorageBufferProperty>( kGrassMeshVisibilityBlock );
        // Find: a surface template with no parameters declares no Materials row block (LendMaterialTable).
        record.MaterialBlock = record.Cell->Find<StorageBufferProperty>( kMaterialRowBlock );
        record.Cell->SetPushMatrix( glm::mat4( 1.0f ) ); // unused by the instanced vertex stage
        return &record;
    }

    void MeshRenderer::BuildGpuInstancedDraws( MeshDrawList& list, GraphicsPipeline* instancedPipeline,
                                               const MeshPass pass, const SceneFrameBinding& frameState,
                                               const StaticPassCells* companion, const bool postOcclusion,
                                               const std::optional<MeshVertexPath> onlyPath )
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
        std::vector<MaskedPrepassedSection>& phasePrepassed =
             postOcclusion ? m_MaskedPrepassedPost : m_MaskedPrepassed;
        // The producer's buffers through a record's kept slots: the section's one Instances, the camera list's ids
        // (the kSurfaceInstanceIdsDefine cell's GpuInstanceIds), a blade's shape (MeshPathOwnBinding); the same
        // buffer again is no write (StorageBufferProperty::SetBuffer). The section's row is the scene material
        // table's (SyncGpuScene claims it every sync), read by the cell of its template; the table is lent through
        // the kept slot (LendMaterialTable). Empty: the section is not drawn through @p of this frame.
        const auto bindRecord = [this]( GpuInstancedDrawRecord&       of,
                                        const GpuInstancedRenderData& section ) -> std::optional<uint32_t>
        {
            if ( of.InstancesBlock != nullptr )
                of.InstancesBlock->SetBuffer( section.Instances );
            if ( of.IdsBlock != nullptr )
                of.IdsBlock->SetBuffer( section.CameraIds );
            if ( of.BladeBlock != nullptr )
                of.BladeBlock->SetBuffer( section.BladeShape );
            if ( of.MeshRowsBlock != nullptr )
                of.MeshRowsBlock->SetBuffer( section.MeshVisibility );
            const std::optional<uint32_t> materialRow = FindMaterialRow( of.Parent, section.Material.get() );
            if ( !materialRow )
                return std::nullopt;
            if ( of.MaterialBlock != nullptr )
            {
                if ( m_GpuScene == nullptr || m_GpuScene->GetMaterialsBuffer() == nullptr )
                {
                    if ( !LendMaterialTable( *of.Cell ) ) // says why
                        return std::nullopt;
                }
                else
                    of.MaterialBlock->LendViewBuffer( m_GpuScene->GetMaterialsBuffer() );
            }
            return materialRow;
        };
        // The masked prepass's question for a section whose small buckets would leave the pair: is the record the
        // G-buffer of this phase draws them through (the section's own, not EQUAL: the G-buffer's carrier, no
        // companion - the deferred G-buffer's Rest half pairs with nothing) Ready this frame.
        const auto ownGBufferReady = [this, postOcclusion]( const GpuInstancedRenderData& section )
        {
            if ( !m_InstancedGBufferPipeline )
                return false;
            const GpuInstancedDrawRecord* const own = GpuInstancedDrawOf(
                 section, MeshPass::GBuffer, m_InstancedGBufferPipeline.get(), nullptr, postOcclusion, false );
            return own != nullptr && own->Admitted && own->Cell != nullptr && own->Pipeline != nullptr &&
                   own->Pipeline->GetReadiness() == PipelineReadiness::Ready;
        };

        // RENDER-JOB-CPU-4, THE COMPACTED BINS (GpuDrawBinFrame; UE FInstanceCullingContext + merged mesh draw
        // commands): a section whose record is the bins' (GpuInstancedDrawRecord::DrawRows) adds its runs of drawn
        // records to the bin of its (pipeline, template, mesh) instead of drawing them itself, in this list's walk
        // order; after the walk each bin is ONE RenderMeshIndirectCount over its slots, the list's bins laid out
        // end to end after the phase table's earlier lists'. The gather (the producer's "Grass: Draw Gather")
        // copies the records into the slots before the draws run.
        struct ListBin
        {
            const GpuInstancedDrawRecord* By    = nullptr; // the bin's first section's: pipeline, cell executor
            Desert::Mesh*                 Mesh  = nullptr;
            GpuDrawBinFrame*              Table = nullptr;
            uint32_t                      Index = 0; // the table's bin
            uint32_t                      Slots = 0;
            std::vector<uint32_t>         Entries{}; // into Table->Entries, FirstSlot bin-local until laid out
        };
        std::vector<ListBin> listBins;
        uint32_t             listSlots = 0; // the slots this list's bins asked so far

        // THE WALK ORDER. Phase 1's masked depth prepass walks the sections NEAREST FIRST (m_PrepassSectionOrder;
        // UE sorts its depth pass so the occluders come first, and DrawRows bins draw their slots in walk order):
        // the alpha-tested texels a near section keeps are in the depth before a far section behind them runs its
        // clip, whose fragments the depth test then rejects before the shader. The depth is the nearest surviving
        // fragment whatever the order (CloserOrEqual), so the G-buffer's EQUAL test sees the same texels. Every
        // other list walks the queue in its own order. Only this serial call (DeclareMaskedDepthDraws) writes the
        // scratch.
        const bool frontToBack = maskedPrepass && !postOcclusion && camera != nullptr;
        if ( frontToBack )
        {
            const glm::vec3 eye = camera->GetPosition();
            m_PrepassSectionOrder.clear();
            m_PrepassSectionOrder.reserve( m_GpuInstancedQueue.size() );
            for ( std::size_t k = 0; k < m_GpuInstancedQueue.size(); ++k )
            {
                float distanceSq = 0.0f;
                if ( const std::optional<Common::Math::AABB>& box = m_GpuInstancedQueue[k].WorldBounds; box )
                {
                    const glm::vec3 nearest = glm::clamp( eye, box->Min, box->Max );
                    const glm::vec3 offset  = nearest - eye;
                    distanceSq              = glm::dot( offset, offset );
                }
                m_PrepassSectionOrder.push_back(
                     { .DistanceSq = distanceSq, .Queue = static_cast<uint32_t>( k ) } );
            }
            // Equal distances (sections without a box, boxes holding the eye) keep the queue's order: the index is
            // the tie-break, so the walk is deterministic without std::stable_sort's temporary buffer.
            std::sort( m_PrepassSectionOrder.begin(), m_PrepassSectionOrder.end(),
                       []( const PrepassSectionOrder& a, const PrepassSectionOrder& b )
                       {
                           if ( a.DistanceSq != b.DistanceSq )
                               return a.DistanceSq < b.DistanceSq;
                           return a.Queue < b.Queue;
                       } );
        }
        const auto queueAt = [this, frontToBack]( const std::size_t walk ) -> std::size_t
        {
            if ( frontToBack )
                return m_PrepassSectionOrder[walk].Queue;
            return walk;
        };
        // The view cull four sections at a time (Frustum::IntersectsEach4: Intersects bit for bit, a section
        // without bounds kept), asked at every walk % 4 == 0; the walk's step i reads bit i % 4. No frustum: every
        // bit set.
        const auto boundsOf = [this, &queueAt]( const std::size_t walk ) -> const Common::Math::AABB*
        {
            const std::optional<Common::Math::AABB>& bounds = m_GpuInstancedQueue[queueAt( walk )].WorldBounds;
            return bounds ? &*bounds : nullptr;
        };
        uint32_t inView4 = 0xFu;
        for ( std::size_t walk = 0; walk < m_GpuInstancedQueue.size(); ++walk )
        {
            const GpuInstancedRenderData& gpu = m_GpuInstancedQueue[queueAt( walk )];
            if ( frustum && ( walk & 3u ) == 0u )
                inView4 = frustum->IntersectsEach4( walk, m_GpuInstancedQueue.size(), boundsOf );
            if ( ( ( inView4 >> ( walk & 3u ) ) & 1u ) == 0u )
                continue;
            if ( onlyPath && gpu.Path != *onlyPath )
                continue;
            // OCCLUSION PER VIEW (GRASS-VIS): the camera passes draw the camera's list when the producer made one
            // (GpuInstancedRenderData::CameraIds); the post-occlusion passes draw only phase 2's records of it.
            const bool cameraList = gpu.CameraIds != nullptr && gpu.CameraRefs != nullptr;
            if ( postOcclusion && ( !cameraList || gpu.PostRefs == nullptr ) )
                continue;
            const GpuInstanceRefs& refs     = cameraList ? *gpu.CameraRefs : *gpu.Refs;
            const RDG::BufferRef   drawArgs = postOcclusion ? gpu.PostRefs->Args : refs.Args;
            const bool             viaVisibility =
                 std::find( m_GrassVisibilitySections.begin(), m_GrassVisibilitySections.end(), gpu.Key ) !=
                      m_GrassVisibilitySections.end();
            if ( visibilityPass && !viaVisibility )
                continue;
            if ( viaVisibility && ( maskedPrepass || pass == MeshPass::GBuffer ) )
                continue;
            auto recorded = phasePrepassed.end();
            if ( pass == MeshPass::GBuffer )
                recorded = std::find_if( phasePrepassed.begin(), phasePrepassed.end(),
                                         [&gpu]( const MaskedPrepassedSection& section )
                                         { return section.Key == gpu.Key; } );
            const bool prepassed = recorded != phasePrepassed.end();

            // The kept record (GpuInstancedDrawOf); null: a texture of its material is in flight.
            GpuInstancedDrawRecord* const record =
                 GpuInstancedDrawOf( gpu, pass, instancedPipeline, companion, postOcclusion, prepassed );
            if ( record == nullptr || !record->Admitted )
                continue;
            GpuInstancedDrawRecord& draw = *record;
            // THE SMALL BUCKETS (GpuInstancedDrawRecord::PrepassBuckets, the record's split): how many of the
            // section's buckets the early-Z pair draws this frame - the prepass the first ones, the G-buffer those
            // EQUAL and the rest through the section's own G-buffer record. The G-buffer takes the count the
            // prepass recorded (cut back to every bucket while that record was not Ready).
            uint32_t prepassBuckets = draw.Buckets;
            if ( prepassed )
                prepassBuckets = std::min( recorded->Buckets, draw.Buckets );
            if ( draw.Cell == nullptr )
            {
                static std::unordered_set<const DataDrivenMaterial*> s_Refused;
                auto* const materials = Runtime::ResourceRegistry::GetMaterialService();
                const bool  owned     = materials != nullptr && materials->Owns( draw.Parent );
                if ( !owned )
                {
                    // Not a refusal: the producer's template is a cell the service replaced (GpuInstancedDrawOf's
                    // staleRefusal), re-resolved every frame until the producer hands the current one.
                    static std::unordered_set<const DataDrivenMaterial*> s_Stale;
                    if ( s_Stale.insert( draw.Parent ).second )
                    {
                        LOG_WARN( "[MeshRenderer] a GPU-instanced section's ({} x {}) template is no longer a "
                                  "MaterialService cell (replaced while its producer held it); the section is not "
                                  "drawn until the producer hands the current one.",
                                  MeshVertexPathName( gpu.Path ), MeshPassName( pass ) );
                    }
                    continue;
                }
                if ( s_Refused.insert( draw.Parent ).second )
                {
                    LOG_ERROR( "[MeshRenderer] a GPU-instanced section's material has no ({} x {}) cell; the "
                               "section is not drawn (MaterialService named the cell it refused).",
                               MeshVertexPathName( gpu.Path ), MeshPassName( pass ) );
                }
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
                // The small buckets leave the pair only for a G-buffer that can draw them itself: the section's
                // own G-buffer record (the one the G-buffer draws it with when this pass did not: the same
                // carrier, no companion, the same phase) Ready this frame, decided here and recorded, so the
                // G-buffer draws exactly what this pass left it. Until then every bucket stays in the pair.
                // (GpuInstancedDrawOf drops every record only when the material service's invalidation version
                // moves, which no lookup of this loop does: `draw` stays this section's.)
                prepassBuckets = draw.PrepassBuckets;
                if ( prepassBuckets < draw.Buckets && !ownGBufferReady( gpu ) )
                    prepassBuckets = draw.Buckets;
                if ( prepassBuckets == 0u )
                    continue; // every bucket small: the G-buffer draws the section as one this pass never drew
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

            const std::optional<uint32_t> materialRow = bindRecord( draw, gpu );
            if ( !materialRow )
                continue;
            const uint32_t row = *materialRow;
            frameState.ApplyTo( draw.Instance.get() );
            if ( maskedPrepass )
                phasePrepassed.push_back( { .Key = gpu.Key, .Buckets = prepassBuckets } );

            // THE RESOLVE (GRASS-VIS): one full-screen triangle per section - record 0 of the view's resolve table
            // (VirtualGeometry::VisibilityBuffer: {3, 1, 0, 0, 0}, the strip's first three indices, which
            // Vertex_GrassBlade.glslh's resolve branch places over the screen), every other submesh hidden. The
            // transforms are still the section's: the fragment stage rebuilds the blade its pixel's word names.
            if ( pass == MeshPass::VisibilityResolve )
            {
                if ( !m_GrassResolveArgs.IsValid() )
                    continue;
                // The producer's resolve mesh (GRASS-VIS-MESH-1: a mesh section's triangle is fetched, not drawn).
                Desert::Mesh* const resolveMesh = gpu.ResolveMesh != nullptr ? gpu.ResolveMesh : gpu.Mesh;
                list.Add( { .Pipeline           = draw.Pipeline,
                            .Mesh               = resolveMesh,
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

            // The section's compacted bins (DrawBinsOf) and its row in them, written once per list for every
            // record of the section that draws in them (the producer's buffers, the material row, the wind: what
            // bindRecord binds and addBuckets pushes per variety).
            GpuDrawBinFrame* const sectionBins = draw.DrawRows ? DrawBinsOf( gpu, pass, postOcclusion ) : nullptr;
            int64_t                sectionRow  = -1;
            const GpuInstanceRefs& argsRefs    = postOcclusion ? *gpu.PostRefs : refs;
            // The record runs of @p by's draw (ForEachRecordRun: @p groups levels from @p base, the submeshes in
            // @p hidden not drawn), each one entry of the section's bin; false: past the slots DrawBinsOf
            // reserved for the section (a walk that disagrees with DrawBinSlots).
            const auto binBuckets = [&]( const GpuInstancedDrawRecord& by, const uint64_t base,
                                         const uint32_t groups, const uint64_t hidden ) -> bool
            {
                GpuDrawBinFrame& table = *sectionBins;
                if ( sectionRow < 0 )
                {
                    if ( table.Rows.size() >= table.EntryCapacity )
                        return false;
                    GpuInstanceDrawRow drawRow{};
                    drawRow.Instances       = AddressOf( gpu.Instances );
                    drawRow.Ids             = cameraList ? AddressOf( gpu.CameraIds ) : uint64_t{ 0 };
                    drawRow.BladeShape      = AddressOf( gpu.BladeShape );
                    // Only the visibility raster reads a mesh's rows: no other pass resolves the view's copy.
                    if ( pass == MeshPass::Visibility )
                        drawRow.MeshVisibility = AddressOf( gpu.MeshVisibility );
                    drawRow.MaterialRow = row;
                    const auto wind     = PackViewInstanceWind( gpu.Wind, gpu.PrevInstanceOffset );
                    drawRow.WindA       = wind.A;
                    drawRow.WindB       = wind.B;
                    // The addresses were asked when DrawBinsOf admitted the section this frame.
                    sectionRow = static_cast<int64_t>( table.Rows.size() );
                    table.Rows.push_back( drawRow );
                }
                ListBin* bin = nullptr;
                for ( ListBin& candidate : listBins )
                    if ( candidate.Table == &table && candidate.By->Pipeline == by.Pipeline &&
                         candidate.By->Parent == by.Parent && candidate.Mesh == gpu.Mesh )
                        bin = &candidate;
                if ( bin == nullptr )
                {
                    if ( table.Bins >= table.BinCapacity )
                        return false;
                    listBins.push_back( { .By = &by, .Mesh = gpu.Mesh, .Table = &table, .Index = table.Bins } );
                    ++table.Bins;
                    table.BinReads.emplace_back();
                    table.BinReads.back().push_back( table.Refs.DrawRows ); // the slots, read by address
                    bin = &listBins.back();
                }
                std::vector<RDG::BufferRef>& reads = table.BinReads[bin->Index];
                reads.push_back( refs.Instances ); // the transforms and the ids, read by address from the row
                if ( refs.Ids.IsValid() )
                    reads.push_back( refs.Ids );
                const uint64_t rowAddress =
                     table.RowsAddress + static_cast<uint64_t>( sectionRow ) * sizeof( GpuInstanceDrawRow );
                const auto submeshes = static_cast<uint32_t>( gpu.Mesh->GetSubmeshes().size() );
                return ForEachRecordRun(
                     base, groups, submeshes, hidden,
                     [&]( const uint64_t records, const uint32_t count, const uint32_t stride ) -> bool
                     {
                         if ( table.Entries.size() >= table.EntryCapacity ||
                              table.Slots + listSlots + count > table.SlotCapacity )
                             return false;
                         GpuDrawCompactEntry entry{};
                         entry.Records   = records;
                         entry.DrawRow   = rowAddress;
                         entry.Count     = count;
                         entry.Stride    = stride;
                         entry.Bin       = bin->Index;
                         entry.FirstSlot = bin->Slots;
                         bin->Slots += count;
                         listSlots += count;
                         bin->Entries.push_back( static_cast<uint32_t>( table.Entries.size() ) );
                         table.Entries.push_back( entry );
                         return true;
                     } );
            };

            // The draws of buckets [first, end) through @p by: a mesh's ONE draw of the LOD levels the producer sorted
            // its instances into, [first, end) - each drawn submesh one multi-draw over its record of every level
            // (RENDER-CPU-COST-1, MeshDrawCommand::IndirectLevels; UE: one indirect draw per mesh batch, not a CPU
            // call per level); a blade's one draw of the strip, its records the strip levels (submesh b = bucket b),
            // the levels outside the range hidden (BucketsHidden).
            const auto addBuckets = [&]( const GpuInstancedDrawRecord& by, const uint32_t first, const uint32_t end )
            {
                const auto add = [&]( const uint64_t hidden, const uint32_t level, const uint32_t levels )
                {
                    list.Add( { .Pipeline           = by.Pipeline,
                                .Mesh               = gpu.Mesh,
                                .Transform          = glm::mat4( 1.0f ),
                                .Material           = by.Cell->GetMaterialExecutor(),
                                .HiddenSubmeshMask  = hidden,
                                .LodLevel           = level,
                                .IndirectArgs       = drawArgs,
                                .IndirectInstances  = refs.Instances,
                                .IndirectIds        = refs.Ids,
                                .IndirectArgsOffset = argsRefs.ArgsOffset + LevelArgsOffset( gpu, level ),
                                .IndirectLevels     = levels,
                                .Push               = DrawPushFields{}
                                             .MaterialIndex( row )
                                             .Wind( PackViewInstanceWind( gpu.Wind, gpu.PrevInstanceOffset ) )
                                             .PrimitiveFlags( 0u ) } );
                };
                if ( first >= end )
                    return;
                const bool blade = gpu.Path == MeshVertexPath::GrassBlade;
                if ( !by.DrawRows )
                {
                    if ( blade )
                        add( BucketsHidden( gpu.HiddenSubmeshes, by.Buckets, first, end ), 0u, 1u );
                    else
                        add( gpu.HiddenSubmeshes, first, end - first );
                    return;
                }
                {
                    // The bins' cell draws only in a bin (it reads its section by the draw's row): a blade's strip
                    // levels [first, end) of its one record set, a mesh's LOD levels [first, end) of each drawn
                    // submesh (GRASS-GPU-DRIVEN-DRAWS).
                    uint64_t hidden = gpu.HiddenSubmeshes;
                    uint32_t groups = end - first;
                    uint64_t base   = argsRefs.ArgsAddress + LevelArgsOffset( gpu, first );
                    if ( blade )
                    {
                        hidden = BucketsHidden( gpu.HiddenSubmeshes, by.Buckets, first, end );
                        groups = 1u;
                        base   = argsRefs.ArgsAddress + LevelArgsOffset( gpu, 0u );
                    }
                    if ( sectionBins != nullptr && binBuckets( by, base, groups, hidden ) )
                        return;
                    // Past what DrawBinsOf reserved for it: a walk that disagrees with DrawBinSlots, never a full
                    // table (that refuses the section as a whole before any list bins it).
                    if ( sectionBins != nullptr )
                        sectionBins->Overflow = true;
                    static bool s_Said = false;
                    if ( !s_Said )
                    {
                        s_Said = true;
                        LOG_ERROR( "[MeshRenderer] a grass section's draws passed the bin slots reserved for it "
                                   "(DrawBinSlots); this list does not draw it this frame." );
                    }
                    return;
                }
            };
            addBuckets( draw, 0u, prepassBuckets );
            if ( !prepassed || prepassBuckets >= draw.Buckets )
                continue;
            // THE SMALL BUCKETS the prepass left this frame (it found this record Ready): the section's own
            // G-buffer record, its clip and depth writes - the texels and depth the pair would have left.
            GpuInstancedDrawRecord* const own =
                 GpuInstancedDrawOf( gpu, pass, instancedPipeline, companion, postOcclusion, false );
            if ( own == nullptr || own->Cell == nullptr || own->Pipeline == nullptr || !bindRecord( *own, gpu ) )
                continue;
            frameState.ApplyTo( own->Instance.get() );
            addBuckets( *own, prepassBuckets, draw.Buckets );
        }

        // THE BINS' DRAWS: each bin's slots after the table's earlier ones, one indirect-count draw per bin (the
        // count the gather sums; the empty records are empty draws of the GPU's front end).
        for ( ListBin& bin : listBins )
        {
            if ( bin.Slots == 0u )
                continue;
            GpuDrawBinFrame& table     = *bin.Table;
            const uint32_t   firstSlot = table.Slots;
            table.Slots += bin.Slots;
            for ( const uint32_t e : bin.Entries )
                table.Entries[e].FirstSlot += firstSlot;
            const std::vector<RDG::BufferRef>& reads    = table.BinReads[bin.Index];
            const uint64_t                     rowSlots =
                 table.SlotsAddress + static_cast<uint64_t>( firstSlot ) * kGpuDrawBinRowBytes;
            list.Add( { .Pipeline            = bin.By->Pipeline,
                        .Mesh                = bin.Mesh,
                        .Transform           = glm::mat4( 1.0f ),
                        .Material            = bin.By->Cell->GetMaterialExecutor(),
                        .HiddenSubmeshMask   = SubmeshMask( ~uint64_t{ 1 } ),
                        .IndirectArgs        = table.Refs.Commands,
                        .IndirectArgsOffset  = static_cast<uint64_t>( firstSlot ) * kGpuDrawBinCommandBytes,
                        .IndirectLevels      = 1u,
                        .IndirectCount       = table.Refs.Counts,
                        .IndirectCountOffset = static_cast<uint64_t>( bin.Index ) * kGpuDrawBinCountBytes,
                        .IndirectMaxDraws    = bin.Slots,
                        .AddressedReads      = std::span<const RDG::BufferRef>( reads.data(), reads.size() ),
                        .Push                = DrawPushFields{}.DrawRows( rowSlots ).PrimitiveFlags( 0u ) } );
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
        // The cascade's caster box (as BuildShadowCascadeDraws culls the static casters): a section with a box
        // casts only into the cascades it meets. Open on the sun's side, and drawn with the view's pancaked caster
        // states (CasterPipelinesOf): a section nearer the sun than the level's depth range lands on its start.
        const Core::Frustum   cascadeFrustum = CascadeCasterFrustum( m_CascadeCasterVP[shadowView] );
        const CasterPipelines casterStates   = CasterPipelinesOf( shadowView );
        // The rule's measure in this cascade (ShadowWindDistanceOf): 0 where no receiver near the camera samples
        // it, every instance then casting its rest pose into the static layer and none left for the movable one.
        const glm::vec4 viewWind  = CasterWindOrigin( shadowView );
        const bool      viewSways = viewWind.w > 0.0f;
        // GRASS-SHADOW-BINS-1, THE CASCADE'S BINS (UE FInstanceCullingContext per shadow view: one indirect args
        // buffer per mesh draw command bin, the view's constants from one view uniform): a blade section whose
        // cascades' table is Open (CascadeBinsOf) adds its parts' records to the bin of its (template, strip mesh,
        // ids) instead of drawing them; the bin's cell carries this list's light camera (the cascade's
        // view-projection, bias, the rule's origin and distance), each part's draw row the section's words (its
        // buffers, material row, wind with the split and the thinning, the thinning's origin, the ring's inner
        // distance). After the walk each bin is ONE RenderMeshIndirectCount. A section the bins refuse this frame
        // (a capacity, an address, the bin's pipeline not Ready yet) casts per section, as before.
        struct CasterBin
        {
            const DataDrivenMaterial* Parent     = nullptr;
            Desert::Mesh*             Mesh       = nullptr;
            bool                      ThroughIds = false;
            DataDrivenMaterial*       Cell       = nullptr;
            GraphicsPipeline*         Pipeline   = nullptr;
            GpuDrawBinFrame*          Table      = nullptr;
            uint32_t                  Index      = 0;
            uint32_t                  Slots      = 0;
            std::vector<uint32_t>     Entries{};
        };
        std::vector<CasterBin> casterBins;
        uint32_t               listSlots       = 0;
        uint32_t               binnedSections  = 0;
        uint32_t               perSectionDraws = 0;
        // One bin cell per (template x cascade x layer): the static layer and the movers over it draw from light
        // cameras of their own (the cascade's and its movable view's), as the per-section cells do.
        const uint32_t binLayer = layer == ShadowCasterLayer::Movable ? kCascadeViews : 0u;
        const uint32_t binView  = kGpuInstancedViewBase + 0x0C000000u + binLayer + c;
        // THE SECTION'S CASTER WORDS (GRASS-BINS-TAIL-1; UE: an FMeshDrawCommand built once, issued by every pass
        // that draws it): one routine for what the cascade's bin row and the per-section draw both read - the list
        // its records are in, their offset (a cascade's own list after the earlier cascades' copies, then the
        // cascade's shadow LOD set), the material row, the thinning's origin and the wind with the split.
        struct SectionCaster
        {
            const GpuInstanceRefs*  Refs       = nullptr;
            uint64_t                ArgsOffset = 0;
            std::optional<uint32_t> MaterialRow{};
            glm::vec4               ThinOrigin{ 0.0f };
            InstanceWindPush        Wind{};     // the records' part
            InstanceWindPush        RestWind{}; // the rest part's twins: the rest pose's wind (Strength 0)
        };
        const auto sectionCasterOf = [&]( const GpuInstancedRenderData& gpu, const DataDrivenMaterial* parent,
                                          const bool ownList, const bool eyeThinned,
                                          const uint32_t split ) -> SectionCaster
        {
            SectionCaster setup{};
            setup.Refs         = gpu.Refs;
            uint64_t listBytes = 0u;
            if ( ownList )
            {
                setup.Refs = gpu.CascadeRefs;
                listBytes  = static_cast<uint64_t>( std::popcount( gpu.CascadeMask & ( ( 1u << c ) - 1u ) ) ) *
                            gpu.CascadeArgBytes;
            }
            const BladeCasterLod shadow = BladeShadowSet( gpu, c );
            setup.ArgsOffset            = listBytes + LevelArgsOffset( gpu, shadow.Set );
            setup.MaterialRow           = FindMaterialRow( parent, gpu.Material.get() );
            // A still blade section that does not sway (@p eyeThinned) reads the origin only for the cascade's
            // thinning (Vertex_GrassBlade.glslh; split 0, no wind rule): the generator's own eye (ThinOrigin, the
            // field's eye - CSM-SCROLL-3), so the blades its static layer keeps hold while the eye stays in a tile
            // and the cached cascade scrolls. A swaying one keeps the rule's origin (its cascade does not scroll).
            setup.ThinOrigin = viewWind;
            if ( eyeThinned && gpu.ShadowLod.Levels != 0u )
                setup.ThinOrigin = glm::vec4( gpu.ShadowLod.ThinOrigin, viewWind.w );
            // A light view's B.w is the split (InstanceWindPush; no previous instance in a light view), and a
            // blade's B.z the cascade's thinning (BladeShadowSet: half x the distance it thins from, half y the
            // generator's StartCull through the lens; 0 = every instance, Vertex_GrassBlade.glslh).
            setup.Wind        = PackInstanceWind( gpu.Wind );
            setup.Wind.B.w    = std::bit_cast<float>( split );
            uint32_t thinWord = 0u;
            if ( shadow.ThinFrom > 0.0f )
                thinWord = glm::packHalf2x16( glm::vec2( std::min( shadow.ThinFrom, 65000.0f ),
                                                         std::min( gpu.ShadowLod.StartCull, 65000.0f ) ) );
            setup.Wind.B.z   = std::bit_cast<float>( thinWord );
            setup.RestWind   = setup.Wind;
            setup.RestWind.A = glm::vec4( 0.0f );
            return setup;
        };
        // The cascade cull four sections at a time (Frustum::IntersectsEach4: Intersects bit for bit, a section
        // without bounds kept), asked at every i % 4 == 0; section i reads bit i % 4.
        const auto casterBoundsOf = [this]( const std::size_t k ) -> const Common::Math::AABB*
        {
            const std::optional<Common::Math::AABB>& bounds = m_GpuInstancedQueue[k].WorldBounds;
            return bounds ? &*bounds : nullptr;
        };
        uint32_t inCascade4 = 0u;
        for ( std::size_t queueIndex = 0; queueIndex < m_GpuInstancedQueue.size(); ++queueIndex )
        {
            const GpuInstancedRenderData& gpu = m_GpuInstancedQueue[queueIndex];
            if ( ( queueIndex & 3u ) == 0u )
                inCascade4 =
                     cascadeFrustum.IntersectsEach4( queueIndex, m_GpuInstancedQueue.size(), casterBoundsOf );
            if ( !gpu.CastShadows )
                continue;
            if ( ( ( inCascade4 >> ( queueIndex & 3u ) ) & 1u ) == 0u )
                continue;
            // WHO SWAYS IN THE SHADOWS, for a still section (GpuInstancedRenderData::Still, the grass), per instance
            // in its vertex stage (CasterWindLayer::*, Mesh/Surface/CasterWindLayer.glslh; the GPU cull's
            // GpuCull_CastsRestPose, the CPU walk's ShadowIsmInstances): a swaying one casts its instances past the
            // shadow wind distance in their rest pose, into the static layer, and its nearer ones swaying, over the
            // cache; one that does not sway is all static. A section whose instances move by themselves is drawn
            // whole over the cache, as before the cache knew them.
            const bool still = gpu.Still.has_value();
            const bool sways = gpu.Wind.Sways();
            // THE CASTER'S REACH (ShadowWindInnerOf, GPU-FRAME-6): in a far cascade the instances nearer than the
            // ring's inner distance cast their rest pose too - their shadows fall on receivers a tighter cascade
            // serves - so only the ring [inner, wind distance) sways over the cache; a ring past the wind distance
            // leaves no mover. The vertex stage reads it from the light camera (ViewFrame::CasterWindRing).
            const float inner     = still && sways ? ShadowWindInnerOf( shadowView, gpu.Still->Reach ) : 0.0f;
            const bool  ringSways = viewSways && inner < viewWind.w;
            uint32_t    split     = 0;
            if ( layer == ShadowCasterLayer::Static )
            {
                if ( !still )
                    continue;
                split = sways ? CasterWindLayer::kRule | CasterWindLayer::kDropNear : 0u;
            }
            else if ( layer == ShadowCasterLayer::Movable )
            {
                if ( still && !( sways && ringSways ) )
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
            // EACH LAYER ITS OWN PART (GRASS-SHADOW-1; UE: a cached shadow's static and movable casters are separate
            // draw lists): a list that holds its movers apart from its rest-pose casters (CascadeRestShift, split by
            // the same rule with the same measures) gives the static layer only the rest part, the movable layer
            // only the movers and a cascade without the cache both, so no vertex stage runs for an instance its
            // layer would throw away and the per-instance rule leaves it: the rest part casts with the rest pose's
            // wind (Strength 0), the movers with the section's.
            const bool parted       = ownList && gpu.CascadeRestShift != 0u;
            const bool drawsRecords = !parted || layer != ShadowCasterLayer::Static;
            const bool drawsTwins   = parted && layer != ShadowCasterLayer::Movable;
            if ( parted )
                split = 0u;
            // GRASS-VIEW-LAW-1: a cascade without a list of its own draws the full list - through its ids when the
            // producer's full list holds them (FullIds).
            const std::shared_ptr<ShaderResources::StorageBuffer>& listIds =
                 ownList ? gpu.CascadeIds : gpu.FullIds;
            const bool          throughIds = listIds != nullptr;
            // The section's caster words, once: the bin row and the per-section draw below both read them.
            const SectionCaster setup = sectionCasterOf( gpu, parent, ownList, still && !sways, split );

            // THE SECTION IN THE CASCADE'S BIN (GRASS-SHADOW-BINS-1): the words the per-section path below writes
            // into its own cell's light camera and push, written into the part's draw row instead.
            const auto binCasts = [&]( GpuDrawBinFrame& table ) -> bool
            {
                const GpuInstanceRefs& partRefs = *setup.Refs;
                if ( partRefs.ArgsAddress == 0u || !setup.MaterialRow || parent == nullptr ||
                     !materials->Owns( parent ) )
                    return false;
                GpuInstanceDrawRow drawRow{};
                drawRow.Instances   = AddressOf( gpu.Instances );
                drawRow.Ids         = throughIds ? AddressOf( listIds ) : uint64_t{ 0 };
                drawRow.BladeShape  = AddressOf( gpu.BladeShape );
                drawRow.MaterialRow = *setup.MaterialRow;
                drawRow.Caster      = glm::vec4( glm::vec3( setup.ThinOrigin ), inner );
                const bool shapeMissing = gpu.Path == MeshVertexPath::GrassBlade && drawRow.BladeShape == 0u;
                if ( drawRow.Instances == 0u || shapeMissing || ( throughIds && drawRow.Ids == 0u ) )
                    return false;
                // A mesh section bins only through ids (the cooked Instanced rows cells, DrawBinsOf).
                if ( gpu.Path == MeshVertexPath::Instanced && !throughIds )
                    return false;
                // The drawn records of the cascade's set and their runs (ForEachRecordRun): a blade's strip levels
                // (submesh b = record b), a mesh's LOD levels of each drawn submesh (GRASS-GPU-DRIVEN-DRAWS).
                const uint64_t hidden    = gpu.HiddenSubmeshes;
                const auto     submeshes = static_cast<uint32_t>( gpu.Mesh->GetSubmeshes().size() );
                const uint32_t groups    = LevelsOf( gpu );
                uint32_t       drawn = 0u, runs = 0u;
                ForEachRecordRun( 0u, groups, submeshes, hidden,
                                  [&]( const uint64_t, const uint32_t count, const uint32_t ) -> bool
                                  {
                                      drawn += count;
                                      ++runs;
                                      return true;
                                  } );
                const uint32_t parts = ( drawsRecords ? 1u : 0u ) + ( drawsTwins ? 1u : 0u );
                if ( parts == 0u || drawn == 0u )
                    return true; // nothing of it casts here, as its per-section draws would draw nothing
                CasterBin* bin = nullptr;
                for ( CasterBin& candidate : casterBins )
                    if ( candidate.Parent == parent && candidate.Mesh == gpu.Mesh &&
                         candidate.ThroughIds == throughIds )
                        bin = &candidate;
                if ( table.Rows.size() + parts > table.EntryCapacity ||
                     table.Entries.size() + parts * runs > table.EntryCapacity ||
                     table.Slots + listSlots + parts * drawn > table.SlotCapacity ||
                     ( bin == nullptr && table.Bins >= table.BinCapacity ) )
                {
                    table.Overflow = true; // the producer grows the table at its next frame
                    return false;
                }
                if ( bin == nullptr )
                {
                    DataDrivenMaterial* const cell =
                         materials->GetViewVariant( parent, gpu.Path, MeshPass::ShadowDepth, binView,
                                                    InstanceDrawPermutation( throughIds, false, true ), this );
                    if ( cell == nullptr )
                        return false;
                    GraphicsPipeline* const binPipeline = MaskedCasterPipeline( *cell, gpu.Path, casterStates );
                    if ( binPipeline == nullptr || binPipeline->GetReadiness() != PipelineReadiness::Ready )
                        return false; // still in the driver: the section casts per section until it is Ready
                    // The list's light camera, once per bin: the rule's origin and distance are the cascade's; the
                    // ring and the thinning's origin are each draw's (its row's Caster).
                    WriteLightCamera( *cell, glm::mat4( 1.0f ), m_CascadeCasterVP[shadowView], m_WorldTimeSeconds,
                                      m_CascadeCasterBias[shadowView], m_ParallaxQuality, m_SceneWind, viewWind,
                                      glm::vec4( 0.0f ) );
                    if ( !LendMaterialTable( *cell ) )
                        return false;
                    casterBins.push_back( { .Parent     = parent,
                                            .Mesh       = gpu.Mesh,
                                            .ThroughIds = throughIds,
                                            .Cell       = cell,
                                            .Pipeline   = binPipeline,
                                            .Table      = &table,
                                            .Index      = table.Bins } );
                    ++table.Bins;
                    table.BinReads.emplace_back();
                    table.BinReads.back().push_back( table.Refs.DrawRows ); // the slots, read by address
                    bin = &casterBins.back();
                }
                std::vector<RDG::BufferRef>& reads = table.BinReads[bin->Index];
                reads.push_back( partRefs.Instances ); // the transforms and the ids, read by address from the row
                if ( partRefs.Ids.IsValid() )
                    reads.push_back( partRefs.Ids );
                const uint64_t base       = partRefs.ArgsAddress + setup.ArgsOffset;
                const auto     addPartRow = [&]( const uint64_t shift, const InstanceWindPush& wind )
                {
                    drawRow.WindA = wind.A;
                    drawRow.WindB = wind.B;
                    const uint64_t rowAddress =
                         table.RowsAddress + table.Rows.size() * sizeof( GpuInstanceDrawRow );
                    table.Rows.push_back( drawRow );
                    ForEachRecordRun(
                         base + shift, groups, submeshes, hidden,
                         [&]( const uint64_t records, const uint32_t count, const uint32_t stride ) -> bool
                         {
                             GpuDrawCompactEntry entry{};
                             entry.Records   = records;
                             entry.DrawRow   = rowAddress;
                             entry.Count     = count;
                             entry.Stride    = stride;
                             entry.Bin       = bin->Index;
                             entry.FirstSlot = bin->Slots;
                             bin->Slots += count;
                             listSlots += count;
                             bin->Entries.push_back( static_cast<uint32_t>( table.Entries.size() ) );
                             table.Entries.push_back( entry );
                             return true;
                         } );
                };
                if ( drawsRecords )
                    addPartRow( 0u, setup.Wind );
                if ( drawsTwins )
                    addPartRow( gpu.CascadeRestShift, setup.RestWind ); // the rest pose's wind (Strength 0)
                return true;
            };
            if ( GpuDrawBinFrame* const cascadeBins = CascadeBinsOf( gpu ); cascadeBins != nullptr )
                if ( binCasts( *cascadeBins ) )
                {
                    ++binnedSections;
                    continue;
                }

            DataDrivenMaterial* caster =
                 parent != nullptr && materials->Owns( parent )
                      ? materials->GetViewVariant( parent, gpu.Path, MeshPass::ShadowDepth, view,
                                                   InstancePermutation( throughIds, false ), this )
                      : nullptr;
            if ( caster == nullptr )
            {
                if ( parent == nullptr || materials == nullptr || !materials->Owns( parent ) )
                {
                    // Not a refusal: the producer's template is a cell the service replaced (the same stale
                    // template the G-buffer pass names), re-resolved every frame until the producer hands the
                    // current one.
                    static std::unordered_set<const DataDrivenMaterial*> s_Stale;
                    if ( s_Stale.insert( parent ).second )
                    {
                        LOG_WARN( "[Shadows] a GPU-instanced section's ({} x ShadowDepth) template is no longer a "
                                  "MaterialService cell (replaced while its producer held it); the section casts "
                                  "no shadow until the producer hands the current one.",
                                  MeshVertexPathName( gpu.Path ) );
                    }
                    continue;
                }
                static std::unordered_set<const DataDrivenMaterial*> s_Refused;
                if ( s_Refused.insert( parent ).second )
                    LOG_ERROR( "[Shadows] a GPU-instanced section's material has no ({} x ShadowDepth) cell; "
                               "the section casts no shadow (MaterialService named the cell it refused).",
                               MeshVertexPathName( gpu.Path ) );
                continue;
            }
            GraphicsPipeline* pipeline = MaskedCasterPipeline( *caster, gpu.Path, casterStates );
            if ( pipeline != nullptr && pipeline->GetReadiness() == PipelineReadiness::Compiling )
                pending = pending || keeps;
            if ( pipeline == nullptr || pipeline->GetReadiness() != PipelineReadiness::Ready )
                continue;

            // The section's own light camera: the thinning's origin and the ring's inner distance
            // (sectionCasterOf).
            WriteLightCamera( *caster, glm::mat4( 1.0f ), m_CascadeCasterVP[shadowView], m_WorldTimeSeconds,
                              m_CascadeCasterBias[shadowView], m_ParallaxQuality, m_SceneWind, setup.ThinOrigin,
                              glm::vec4( inner, 0.0f, 0.0f, 0.0f ) );
            BindGpuInstancedBuffers( *caster, gpu, listIds );
            const GpuInstanceRefs& refs = *setup.Refs;
            // The section's row is the scene material table's (SyncGpuScene), read by its masked caster cell.
            if ( !setup.MaterialRow || !LendMaterialTable( *caster ) )
                continue;
            const uint32_t row = *setup.MaterialRow;
            // RENDER-CPU-COST-1: the section's LOD levels are one draw per part (MeshDrawCommand::IndirectLevels),
            // each drawn submesh one multi-draw over its record of every level from the cascade's set.
            const auto addPart = [&]( const uint64_t shift, const InstanceWindPush& push )
            {
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
                       // own list is its copy of the records, its rest part their twins.
                       .IndirectArgsOffset = refs.ArgsOffset + setup.ArgsOffset + shift,
                       .IndirectLevels     = LevelsOf( gpu ),
                       .Push =
                            DrawPushFields{}.Transform( glm::mat4( 1.0f ) ).MaterialIndex( row ).Wind( push ) } );
            };
            if ( drawsRecords )
                addPart( 0u, setup.Wind );
            if ( drawsTwins )
                addPart( gpu.CascadeRestShift, setup.RestWind );
            perSectionDraws += ( drawsRecords ? 1u : 0u ) + ( drawsTwins ? 1u : 0u );
        }

        // THE CASCADE'S BINS' DRAWS (GRASS-SHADOW-BINS-1): each bin's slots after the table's earlier lists', one
        // indirect-count draw per bin (the count the gather sums).
        uint32_t binDraws = 0u;
        for ( CasterBin& bin : casterBins )
        {
            if ( bin.Slots == 0u )
                continue;
            GpuDrawBinFrame& table     = *bin.Table;
            const uint32_t   firstSlot = table.Slots;
            table.Slots += bin.Slots;
            for ( const uint32_t e : bin.Entries )
                table.Entries[e].FirstSlot += firstSlot;
            const std::vector<RDG::BufferRef>& reads = table.BinReads[bin.Index];
            const uint64_t                     rowSlots =
                 table.SlotsAddress + static_cast<uint64_t>( firstSlot ) * kGpuDrawBinRowBytes;
            list.Add( { .Pipeline            = bin.Pipeline,
                        .Mesh                = bin.Mesh,
                        .Transform           = glm::mat4( 1.0f ),
                        .Material            = bin.Cell->GetMaterialExecutor(),
                        .HiddenSubmeshMask   = SubmeshMask( ~uint64_t{ 1 } ),
                        .IndirectArgs        = table.Refs.Commands,
                        .IndirectArgsOffset  = static_cast<uint64_t>( firstSlot ) * kGpuDrawBinCommandBytes,
                        .IndirectLevels      = 1u,
                        .IndirectCount       = table.Refs.Counts,
                        .IndirectCountOffset = static_cast<uint64_t>( bin.Index ) * kGpuDrawBinCountBytes,
                        .IndirectMaxDraws    = bin.Slots,
                        .AddressedReads      = std::span<const RDG::BufferRef>( reads.data(), reads.size() ),
                        .Push                = DrawPushFields{}.DrawRows( rowSlots ).PrimitiveFlags( 0u ) } );
            ++binDraws;
        }
        // The count line (said when a list's draws change): the lead greps "[Shadows] grass casters".
        const uint32_t listKey   = ( static_cast<uint32_t>( layer ) << 8u ) | shadowView;
        const uint64_t words     = ( uint64_t{ binDraws } << 32u ) | perSectionDraws;
        const auto said = m_GrassCasterBinsSaid.try_emplace( listKey, ~uint64_t{ 0 } ).first;
        if ( said->second != words )
        {
            said->second = words;
            LOG_INFO( "[Shadows] grass casters, view {} layer {}: {} indirect-count draws (bins) for {} sections, "
                      "{} per-section draws",
                      shadowView, static_cast<uint32_t>( layer ), binDraws, binnedSections, perSectionDraws );
        }
        return pending;
    }
} // namespace Desert::Graphic::System
