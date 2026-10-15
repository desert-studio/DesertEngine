// MeshRenderer's surface-cache card capture (GI-SURFACE-CACHE-2): the frame's card instances and the G-buffer
// light-view draws that capture the cards SurfaceCacheScene picked (UE LumenSceneCardCapture.cpp).
#include "MeshRendererInternal.hpp"

#include <Engine/Graphic/Renderer.hpp>

#include <unordered_map>

namespace Desert::Graphic::System
{
    void MeshRenderer::GatherSurfaceCacheInstances( std::vector<SurfaceCache::SceneInstance>& out )
    {
        m_CardCaptureSources.clear();
        // A key seen twice this frame (two parts submitted under one entity and part) gets a salt per repeat, so
        // neither replaces the other.
        std::unordered_map<uint64_t, uint32_t> repeats;
        const auto                             add = [&]( uint64_t key, const Mesh* mesh,
                              std::shared_ptr<const Assets::MeshCardsBuildData> cards, const glm::mat4& world,
                              const SurfaceSlot slot, const uint64_t hidden )
        {
            const uint32_t repeat = repeats[key]++;
            if ( repeat > 0 )
                key = ( key * 0x100000001B3ull ) ^ ( 0x4000000000000000ull | repeat );
            out.push_back( { .Key    = key,
                             .Mesh   = mesh,
                             .Cards  = std::move( cards ),
                             .World  = world,
                             .Source = static_cast<uint32_t>( m_CardCaptureSources.size() ) } );
            m_CardCaptureSources.push_back( { .Mesh     = mesh,
                                              .World    = world,
                                              .Instance = slot.Instance,
                                              .Surface  = slot.Surface,
                                              .Hidden   = hidden } );
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
            add( key, data.Mesh, std::move( cards ), data.Transform, slot, data.HiddenSubmeshes );
        }
        for ( uint32_t batch = 0; batch < m_InstancedQueue.size(); ++batch )
        {
            const auto& ism = m_InstancedQueue[batch];
            if ( ism.Mesh == nullptr || !ism.Transforms || !ism.Material )
                continue;
            auto cards = ism.Mesh->GetCards();
            if ( !cards || cards->Cards.empty() )
                continue;
            MaterialInstance* const instance = ism.Material.get();
            auto* const             surface  = dynamic_cast<DataDrivenMaterial*>( instance->GetParentMaterial() );
            if ( surface == nullptr || IsTranslucent( surface ) )
                continue;
            for ( uint32_t i = 0; i < ism.Transforms->size(); ++i )
                add( 0x8000000000000000ull | ( static_cast<uint64_t>( batch ) << 32u ) | i, ism.Mesh, cards,
                     ( *ism.Transforms )[i], SurfaceSlot{ .Instance = instance, .Surface = surface }, 0u );
        }
    }

    void MeshRenderer::DeclareCardCaptureDraws( RDG::PassBuilder&                             pass,
                                                const std::vector<SurfaceCache::CardCapture>& captures )
    {
        m_CardCaptureDraws.Clear();
        if ( !m_RSMPipeline || captures.empty() )
            return;
        if ( !m_CardCaptureMaterial )
        {
            // (Static x GBuffer), allocated from the cell by name like the RSM's: it binds against the light-view
            // permutation's pipeline unchanged (SetupGBufferPass says why).
            m_CardCaptureMaterial = CreateCellMaterial( MeshVertexPath::Static, MeshPass::GBuffer );
            if ( !m_CardCaptureMaterial )
            {
                m_CardCaptureDraws.Fail( "the G-buffer cell material for the card capture could not be created" );
                return;
            }
        }

        // The captured surfaces' effective rows, in the dedicated material's Materials buffer, written ONCE before
        // the block is declared (the RSM's order).
        std::vector<glm::vec4> rows;
        std::vector<uint32_t>  rowOf;
        rowOf.reserve( captures.size() );
        for ( const SurfaceCache::CardCapture& capture : captures )
        {
            const CardCaptureSource& source = m_CardCaptureSources[capture.Source];
            rowOf.push_back( AppendRow( rows, EffectiveRow( source.Surface, source.Instance ) ) );
        }
        if ( auto* sb = m_CardCaptureMaterial->Get<StorageBufferProperty>( "Materials" ) )
            sb->SetRawData( rows.data(), static_cast<uint32_t>( rows.size() * sizeof( glm::vec4 ) ) );

        while ( m_CardCaptureInstances.size() < captures.size() )
            m_CardCaptureInstances.push_back( m_CardCaptureMaterial->CreateInstance() );

        DataDrivenMaterial* const material = m_CardCaptureMaterial.get();
        for ( std::size_t i = 0; i < captures.size(); ++i )
        {
            const SurfaceCache::CardCapture& capture = captures[i];
            const CardCaptureSource&         source  = m_CardCaptureSources[capture.Source];
            // The card is not a view (no jitter, no previous frame): its camera block, in its own instance.
            const ViewFrame cardView =
                 MakeStillViewFrame( capture.View, capture.Projection, capture.Eye, m_WorldTimeSeconds );
            MaterialInstance* const instance = m_CardCaptureInstances[i].get();
            // A light view (UE: the card capture is not a view of the scene's GPUScene rows): the permutation it
            // records with declares no motion rows, so none are carried into the template that does.
            SceneFrameBinding cardFrame = CaptureFrameState( &cardView );
            cardFrame.ObjectMotions     = {};
            cardFrame.ObjectBones       = {};
            cardFrame.ApplyTo( instance );
            const SurfaceCache::AtlasRect rect = capture.CaptureRect;
            m_CardCaptureDraws.Add( { .Pipeline          = m_RSMPipeline.get(),
                                      .Mesh              = source.Mesh,
                                      .Transform         = source.World,
                                      .Material          = material->GetMaterialExecutor(),
                                      .HiddenSubmeshMask = source.Hidden,
                                      .BindState = [material, instance, world = source.World, row = rowOf[i], rect]
                                      {
                                          // Its own rectangle of the capture atlas only (the pass cleared the
                                          // whole atlas: every capture of the frame is new).
                                          Renderer::GetInstance().SetViewportRegion(
                                               static_cast<int32_t>( rect.X ), static_cast<int32_t>( rect.Y ),
                                               rect.Width, rect.Height );
                                          material->SetPushMatrix( world );
                                          material->SetMaterialIndex( row );
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
