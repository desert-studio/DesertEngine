// MeshRenderer's surface-cache card capture (GI-SURFACE-CACHE-2): the frame's card instances and the G-buffer
// light-view draws that capture the cards SurfaceCacheScene picked (UE LumenSceneCardCapture.cpp).
#include "MeshRendererInternal.hpp"

#include <Engine/Graphic/Renderer.hpp>

#include <cstdint>
#include <format>
#include <unordered_map>

namespace Desert::Graphic::System
{
    namespace
    {
        uint64_t HashCombine( uint64_t seed, const uint64_t value )
        {
            return ( seed ^ value ) * 0x100000001B3ull;
        }
        uint64_t PointerBits( const void* p )
        {
            return static_cast<uint64_t>( reinterpret_cast<std::uintptr_t>( p ) );
        }
    } // namespace

    void MeshRenderer::GatherSurfaceCacheInstances( std::vector<SurfaceCache::SceneInstance>& out )
    {
        m_CardCaptureSources.clear();
        auto* const materials = Runtime::ResourceRegistry::GetMaterialService();
        // A key seen twice this frame (two parts submitted under one entity and part) gets a salt per repeat, so
        // neither replaces the other.
        std::unordered_map<uint64_t, uint32_t> repeats;
        // The surface's identity and whether its G-buffer cell draws this frame. A `.demat` surface is captured by
        // its own (path x GBuffer) cell - the scene G-buffer's program, textures and parameters - and only once
        // that cell's pipeline is Ready (the same readiness the scene's G-buffer draw asks, ChooseCellDraw); the
        // renderer-owned default surface by the pass's own cell, an engine pipeline.
        const auto surfaceOf = [&]( DataDrivenMaterial* surface, MaterialInstance* instance,
                                    const MeshVertexPath path, uint64_t& key, bool& ready )
        {
            key   = HashCombine( HashCombine( 0xCBF29CE484222325ull, PointerBits( surface ) ),
                                 PointerBits( instance ) );
            ready = true;
            if ( materials == nullptr || !materials->Owns( surface ) )
                return;
            DataDrivenMaterial* const cell = materials->GetVariant( surface, path, MeshPass::GBuffer );
            key                            = HashCombine( key, PointerBits( cell ) );
            GraphicsPipeline* const pass   = path == MeshVertexPath::Instanced ? m_InstancedGBufferPipeline.get()
                                                                               : m_StaticGBufferPipeline.get();
            const bool twoSided            = instance != nullptr ? instance->IsTwoSided() : surface->IsTwoSided();
            ready                          = cell != nullptr && pass != nullptr &&
                    ChooseCellDraw( pass, *cell, twoSided, nullptr ) == MaterialPipelineTracker::CellDraw::Own;
        };
        const auto add = [&]( uint64_t key, const Mesh* mesh,
                              std::shared_ptr<const Assets::MeshCardsBuildData> cards, const glm::mat4& world,
                              CardCaptureSource source )
        {
            const uint32_t repeat = repeats[key]++;
            if ( repeat > 0 )
                key = ( key * 0x100000001B3ull ) ^ ( 0x4000000000000000ull | repeat );
            SurfaceCache::SceneInstance instance{ .Key    = key,
                                                  .Mesh   = mesh,
                                                  .Cards  = std::move( cards ),
                                                  .World  = world,
                                                  .Source = static_cast<uint32_t>( m_CardCaptureSources.size() ) };
            surfaceOf( source.Surface, source.Instance,
                       source.Instanced ? MeshVertexPath::Instanced : MeshVertexPath::Static, instance.SurfaceKey,
                       instance.CaptureReady );
            out.push_back( std::move( instance ) );
            m_CardCaptureSources.push_back( source );
        };
        for ( const auto& data : m_StaticQueue )
        {
            if ( data.Mesh == nullptr || !data.MaterialSlots || data.MaterialSlots->Slots.empty() )
                continue;
            auto cards = data.Mesh->GetCards();
            if ( !cards || cards->Cards.empty() )
                continue;
            // Opaque surfaces only: glass transmits, its cards would hold what is behind it.
            const SurfaceSlot slot = FirstSurfaceSlot( data.MaterialSlots->Slots, MeshVertexPath::Static );
            if ( !slot || IsTranslucent( slot.Surface ) )
                continue;
            const uint64_t key = ( static_cast<uint64_t>( data.Entity ) << 24u ) | ( data.MotionPart & 0xFFFFFFu );
            add( key, data.Mesh, std::move( cards ), data.Transform,
                 { .Mesh      = data.Mesh,
                   .World     = data.Transform,
                   .Instance  = slot.Instance,
                   .Surface   = slot.Surface,
                   .Hidden    = data.HiddenSubmeshes,
                   .MotionRow = data.MotionRow } );
        }
        // Instanced static meshes and foliage: every instance is an instance of the Lumen scene (UE adds each ISM
        // instance's cards), keyed by its mesh, material and index - stable while the set is; a moved instance
        // keeps its texels.
        for ( const auto& ism : m_InstancedQueue )
        {
            if ( ism.Mesh == nullptr || !ism.Material || !ism.Transforms || ism.Transforms->empty() )
                continue;
            auto* const surface = dynamic_cast<DataDrivenMaterial*>( ism.Material->GetParentMaterial() );
            if ( surface == nullptr || IsTranslucent( surface ) )
                continue;
            const auto cards = ism.Mesh->GetCards();
            if ( !cards || cards->Cards.empty() )
                continue;
            const uint64_t batch = HashCombine( HashCombine( 0x84222325CBF29CE4ull, PointerBits( ism.Mesh ) ),
                                                PointerBits( ism.Material.get() ) );
            for ( std::size_t i = 0; i < ism.Transforms->size(); ++i )
            {
                const glm::mat4& world = ( *ism.Transforms )[i];
                // Bit 63 set: never an entity key (entity << 24 stays below it).
                const uint64_t key = 0x8000000000000000ull | ( HashCombine( batch, i ) >> 1u );
                add( key, ism.Mesh, cards, world,
                     { .Mesh      = ism.Mesh,
                       .World     = world,
                       .Instance  = ism.Material.get(),
                       .Surface   = surface,
                       .Instanced = true } );
            }
        }
    }

    void MeshRenderer::DeclareCardCaptureDraws( RDG::PassBuilder&                             pass,
                                                const std::vector<SurfaceCache::CardCapture>& captures )
    {
        m_CardCaptureDraws.Clear();
        if ( captures.empty() )
            return;
        auto* const materials = Runtime::ResourceRegistry::GetMaterialService();
        // The k-th capture of a surface in this frame draws through that surface's k-th view copy (its camera
        // block holds this card's camera); the renderer-owned surface through the k-th material of its cell's
        // pool.
        std::unordered_map<const DataDrivenMaterial*, uint32_t> viewsTaken;
        uint32_t                                                pooled[2] = { 0u, 0u };
        for ( const SurfaceCache::CardCapture& capture : captures )
        {
            const CardCaptureSource& source = m_CardCaptureSources[capture.Source];
            const MeshVertexPath     path = source.Instanced ? MeshVertexPath::Instanced : MeshVertexPath::Static;
            const uint32_t           cell = source.Instanced ? 1u : 0u;
            GraphicsPipeline* const  passState =
                 source.Instanced ? m_InstancedGBufferPipeline.get() : m_StaticGBufferPipeline.get();
            if ( passState == nullptr )
            {
                m_CardCaptureDraws.Fail( std::format( "the ({} x GBuffer) pipeline the card capture draws with is "
                                                      "missing",
                                                      MeshVertexPathName( path ) ) );
                return;
            }
            DataDrivenMaterial* material = nullptr;
            MaterialInstance*   instance = source.Instance;
            if ( materials != nullptr && materials->Owns( source.Surface ) )
            {
                const uint32_t view = viewsTaken[source.Surface]++;
                material            = materials->GetViewVariant( source.Surface, path, MeshPass::GBuffer, view );
                if ( material == nullptr )
                {
                    // Gathered as ready, so the cell exists: MaterialService named what refused the copy.
                    m_CardCaptureDraws.Fail(
                         std::format( "the ({} x GBuffer) view copy {} of a surface for the card "
                                      "capture was refused",
                                      MeshVertexPathName( path ), view ) );
                    return;
                }
            }
            else
            {
                auto& pool      = m_CardCaptureMaterials[cell];
                auto& instances = m_CardCaptureInstances[cell];
                if ( pooled[cell] == pool.size() )
                {
                    auto created = CreateCellMaterial( path, MeshPass::GBuffer );
                    if ( !created )
                    {
                        m_CardCaptureDraws.Fail( std::format(
                             "the ({} x GBuffer) cell material for the card capture could not be created",
                             MeshVertexPathName( path ) ) );
                        return;
                    }
                    instances.push_back( created->CreateInstance() );
                    pool.push_back( std::move( created ) );
                }
                material = pool[pooled[cell]].get();
                // The default surface's own instance stands for itself: its overrides are the slot's.
                instance = source.Instance != nullptr ? source.Instance : instances[pooled[cell]].get();
                ++pooled[cell];
            }

            // Its one row: the surface's effective parameters, as the scene's G-buffer draw packs them.
            std::vector<glm::vec4> rows;
            AppendRow( rows, EffectiveRow( source.Surface, source.Instance ) );
            if ( auto* sb = material->Get<StorageBufferProperty>( "Materials" ) )
                sb->SetRawData( rows.data(), static_cast<uint32_t>( rows.size() * sizeof( glm::vec4 ) ) );
            // An instance's one transform (the instanced cell reads its world from InstanceTransforms).
            if ( source.Instanced )
                if ( auto* sb = material->Get<StorageBufferProperty>( "InstanceTransforms" ) )
                    sb->SetRawData( &source.World, static_cast<uint32_t>( sizeof( glm::mat4 ) ) );
            // The card is not a view of its own (no jitter, no previous frame): its camera, in its material.
            const ViewFrame cardView =
                 MakeStillViewFrame( capture.View, capture.Projection, capture.Eye, m_WorldTimeSeconds );
            CaptureFrameState( &cardView ).ApplyTo( static_cast<Material*>( material ) );

            const bool twoSided = instance != nullptr ? instance->IsTwoSided() : material->IsTwoSided();
            GraphicsPipeline* const pipeline   = CullPermutation( CellPipeline( passState, *material ), twoSided );
            const SurfaceCache::AtlasRect rect = capture.CaptureRect;
            // The view-pass cells place the mesh themselves (static: the ObjectMotions row; instanced: the
            // instance transform): the push transform is the submesh's alone, as in the scene's G-buffer draw.
            m_CardCaptureDraws.Add( { .Pipeline          = pipeline,
                                      .Mesh              = source.Mesh,
                                      .Transform         = glm::mat4( 1.0f ),
                                      .Material          = material->GetMaterialExecutor(),
                                      .InstanceCount     = 1u,
                                      .FirstInstance     = 0u,
                                      .HiddenSubmeshMask = source.Hidden,
                                      .BindState         = [material, instance, row = source.MotionRow,
                                                    instanced = source.Instanced, rect]
                                      {
                                          // Its own rectangle of the capture atlas only (the pass cleared the
                                          // whole atlas: every capture of the frame is new).
                                          Renderer::GetInstance().SetViewportRegion(
                                               static_cast<int32_t>( rect.X ), static_cast<int32_t>( rect.Y ),
                                               rect.Width, rect.Height );
                                          material->SetPushMatrix( glm::mat4( 1.0f ) );
                                          material->SetMaterialIndex( 0u );
                                          if ( instanced )
                                              material->SetInstancedWind( InstanceWindPush{} ); // a still card
                                          else
                                              material->SetPrimitiveIndex( row );
                                          material->Bind( instance );
                                      } } );
        }
        m_CardCaptureDraws.Declare( pass, std::nullopt );
    }

    Common::BoolResultStr MeshRenderer::RenderCardCaptureManual( const RDG::PassContext& context ) const
    {
        return m_CardCaptureDraws.Record( context );
    }
} // namespace Desert::Graphic::System
