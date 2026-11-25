#include <Water/Public/WaterRenderer.hpp>

// THE SHORE FIELD ON THE GPU: the bake's packed texels into the map the view's water samples. Only what a re-bake
// changed is uploaded; a bake over another grid replaces the map.

#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <span>

namespace
{
    struct WaterShoreShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterShoreUploadParams );
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterShoreUploadTexels );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterShoreTarget );
        static constexpr std::array kAll{ ::Desert::Graphic::RDG::Erase( WaterShoreUploadParams ),
                                          ::Desert::Graphic::RDG::Erase( WaterShoreUploadTexels ),
                                          ::Desert::Graphic::RDG::Erase( WaterShoreTarget ) };
    };
} // namespace

namespace Desert::Graphic::System
{
    namespace
    {
        // Programs/Water/WaterShoreUpload.shader WaterShoreUploadParams, std430.
        struct ShoreUploadParams
        {
            glm::ivec4 Rect{ 0 };
            glm::ivec4 Rows{ 0 };
        };
        static_assert( sizeof( ShoreUploadParams ) == 32 );

        // The map of a world whose bake is not there yet: one texel of the open sea.
        constexpr uint64_t kOpenSeaRevision = std::numeric_limits<uint64_t>::max();

        const std::array<uint64_t, 1>& OpenSeaTexel()
        {
            static const std::array<uint64_t, 1> texel{
                 Water::PackShoreTexel( Water::WaterShoreField::Texel{ .GroundCm        = -Water::kOpenSeaDepthCm,
                                                                       .ShoreDistanceCm = Water::kShoreFarCm,
                                                                       .ShoreDirX       = 0.0f,
                                                                       .ShoreDirZ       = 0.0f },
                                        0.0f ) };
            return texel;
        }
    } // namespace

    RDG::TextureRef WaterRenderer::AddShorePass( RDG::Builder& graph )
    {
        if ( !m_ShoreUploadTried )
        {
            m_ShoreUploadTried = true;
            m_ShoreUpload =
                 BuildWavePipeline( "WaterShoreUpload", m_ShoreUploadLayout, WaterShoreShaderParameters::kAll );
        }
        if ( m_ShoreUpload == nullptr )
            return {};
        const std::shared_ptr<const Water::WaterShoreField>& field = m_Walk.Shore;
        const uint64_t         revision = field != nullptr ? field->Revision : kOpenSeaRevision;
        const uint32_t         width    = field != nullptr ? field->Width : 1u;
        const uint32_t         height   = field != nullptr ? field->Height : 1u;
        const RDG::TextureDesc desc{ .Size = { width, height, 1 }, .Format = Core::Formats::ImageFormat::RGBA16F };
        const bool             held = m_ShoreImage.Physical != nullptr && m_ShoreImage.Desc.Size == desc.Size;
        if ( !held || m_ShoreHolds == nullptr )
            m_ShoreHolds = std::make_shared<std::atomic<uint64_t>>( 0 ); // another image: it holds nothing
        const RDG::TextureRef shore = held ? graph.RegisterExternal( m_ShoreImage, "Water.Shore" )
                                           : graph.CreateTexture( desc, "Water.Shore" );

        const uint64_t               holds = m_ShoreHolds->load( std::memory_order_acquire );
        Water::WaterShoreField::Rect rect{ 0u, 0u, width, height };
        if ( field != nullptr && field->BaseRevision != 0 && holds == field->BaseRevision )
            rect = field->Changed;
        if ( holds != revision && rect.Empty() )
            m_ShoreHolds->store( revision, std::memory_order_release ); // a re-bake that moved nothing
        else if ( holds != revision )
        {
            const std::span<const uint64_t> texels =
                 field != nullptr ? std::span<const uint64_t>( field->Packed )
                                         .subspan( size_t( rect.Z0 ) * width, size_t( rect.Z1 - rect.Z0 ) * width )
                                  : std::span<const uint64_t>( OpenSeaTexel() );
            const ShoreUploadParams params{ .Rect = glm::ivec4( rect.X0, rect.Z0, rect.X1, rect.Z1 ),
                                            .Rows = glm::ivec4( static_cast<int32_t>( width ), 0, 0, 0 ) };
            const RDG::BufferRef    paramsBuffer =
                 graph.CreateBuffer( RDG::BufferDesc{ sizeof( params ) }, "Water.ShoreUploadParams" );
            graph.QueueBufferUpload( paramsBuffer, std::as_bytes( std::span( &params, 1 ) ) );
            const RDG::BufferRef texelBuffer =
                 graph.CreateBuffer( RDG::BufferDesc{ texels.size_bytes() }, "Water.ShoreUploadTexels" );
            // Not copied: the pass below keeps the field, an immutable bake, until the graph has executed.
            graph.QueueStaticBufferUpload( texelBuffer, std::as_bytes( texels ) );
            using P = WaterShoreShaderParameters;
            graph.AddPass(
                 "Water: Shore Upload", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_ShoreUploadLayout.Get( m_ShoreUpload->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *m_ShoreUpload ) )
                          .Storage( P::WaterShoreUploadParams, paramsBuffer, RDG::Access::StorageRead )
                          .Storage( P::WaterShoreUploadTexels, texelBuffer, RDG::Access::StorageRead )
                          .Storage( P::WaterShoreTarget, shore, RDG::Access::StorageWrite, 0 );
                 },
                 [pipeline = m_ShoreUpload, field, rect, revision,
                  holdsNow = m_ShoreHolds]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     Common::BoolResultStr   done = Renderer::GetInstance().DispatchCompute(
                          bindings, *pipeline, ( rect.X1 - rect.X0 + 7u ) / 8u, ( rect.Z1 - rect.Z0 + 7u ) / 8u,
                          1u );
                     if ( done )
                         holdsNow->store( revision, std::memory_order_release );
                     return done;
                 } );
        }
        graph.Extract( shore, m_ShoreImage, RDG::Access::SampledGraphics,
                       { RDG::Access::StorageWrite, RDG::Access::SampledCompute } );
        return shore;
    }
} // namespace Desert::Graphic::System
