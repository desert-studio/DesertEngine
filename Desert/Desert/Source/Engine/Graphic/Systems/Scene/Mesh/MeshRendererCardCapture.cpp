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
                              const SurfaceSlot slot, const uint64_t hidden, const uint32_t motionRow )
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
                                              .Hidden   = hidden,
                                              .MotionRow = motionRow } );
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
            add( key, data.Mesh, std::move( cards ), data.Transform, slot, data.HiddenSubmeshes, data.MotionRow );
        }
        // Instanced meshes have no ObjectMotions row of their own (the view-pass static cell draws a world from one):
        // their capture needs the instanced G-buffer cell - REMAINDER GI-SURFACE-CACHE-2.
        
    }

    void MeshRenderer::DeclareCardCaptureDraws( RDG::PassBuilder&                             pass,
                                                const std::vector<SurfaceCache::CardCapture>& captures )
    {
        m_CardCaptureDraws.Clear();
        if ( !m_StaticGBufferPipeline || captures.empty() )
            return;
        // The view-pass G-buffer cell (world = the view's ObjectMotions row x the push transform; reversed-Z), one
        // dedicated material per capture: its camera block holds that card's camera.
        while ( m_CardCaptureMaterials.size() < captures.size() )
        {
            auto material = CreateCellMaterial( MeshVertexPath::Static, MeshPass::GBuffer );
            if ( !material )
            {
                m_CardCaptureDraws.Fail( "the G-buffer cell material for the card capture could not be created" );
                return;
            }
            m_CardCaptureInstances.push_back( material->CreateInstance() );
            m_CardCaptureMaterials.push_back( std::move( material ) );
        }
        for ( std::size_t i = 0; i < captures.size(); ++i )
        {
            const SurfaceCache::CardCapture& capture  = captures[i];
            const CardCaptureSource&         source   = m_CardCaptureSources[capture.Source];
            DataDrivenMaterial* const        material = m_CardCaptureMaterials[i].get();
            MaterialInstance* const          instance = m_CardCaptureInstances[i].get();
            // Its one row: the surface's effective parameters (the RSM's way - the textures are not captured).
            std::vector<glm::vec4> rows;
            AppendRow( rows, EffectiveRow( source.Surface, source.Instance ) );
            if ( auto* sb = material->Get<StorageBufferProperty>( "Materials" ) )
                sb->SetRawData( rows.data(), static_cast<uint32_t>( rows.size() * sizeof( glm::vec4 ) ) );
            // The card is not a view of its own (no jitter, no previous frame): its camera, in its material.
            const ViewFrame cardView =
                 MakeStillViewFrame( capture.View, capture.Projection, capture.Eye, m_WorldTimeSeconds );
            CaptureFrameState( &cardView ).ApplyTo( material );
            const SurfaceCache::AtlasRect rect = capture.CaptureRect;
            m_CardCaptureDraws.Add( { .Pipeline          = m_StaticGBufferPipeline.get(),
                                      .Mesh              = source.Mesh,
                                      .Transform         = source.World,
                                      .Material          = material->GetMaterialExecutor(),
                                      .HiddenSubmeshMask = source.Hidden,
                                      .BindState = [material, instance, row = source.MotionRow, rect]
                                      {
                                          // Its own rectangle of the capture atlas only (the pass cleared the
                                          // whole atlas: every capture of the frame is new).
                                          Renderer::GetInstance().SetViewportRegion(
                                               static_cast<int32_t>( rect.X ), static_cast<int32_t>( rect.Y ),
                                               rect.Width, rect.Height );
                                          material->SetPushMatrix( glm::mat4( 1.0f ) );
                                          material->SetMaterialIndex( 0u );
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
