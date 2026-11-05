#include "CombineLUTsRenderer.hpp"

#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr uint32_t kGroupSize = 4; // must match LocalSize in CombineLUTs.shader
        static_assert( CombineLUTsRenderer::kLUTSize % kGroupSize == 0,
                       "the table is dispatched in whole groups, with no partial group to guard" );
    } // namespace

    Common::BoolResultStr CombineLUTsRenderer::Initialize()
    {
        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "CombineLUTs" );
        if ( !shader )
            return Common::MakeError( "CombineLUTsRenderer: missing compute shader 'CombineLUTs'" );
        const auto pipeline = ComputePipeline::Create( { .Shader = shader, .DebugName = "CombineLUTs" } );
        if ( !pipeline )
            return Common::MakeFormattedError<bool>( "CombineLUTsRenderer: {}", pipeline.GetError() );
        m_Pipeline = pipeline.GetValue();
        if ( const auto valid =
                  m_Layout.Validate( m_Pipeline->GetSpecification().Shader, CombineLUTsShaderParameters::kAll );
             !valid )
            return valid;
        return BOOLSUCCESS;
    }

    RDG::TextureDesc CombineLUTsRenderer::GetLUTDesc()
    {
        return RDG::TextureDesc{ .Size   = { kLUTSize, kLUTSize, kLUTSize },
                                 .Format = Core::Formats::ImageFormat::RGBA16F,
                                 .Dim    = RDG::TextureDim::Tex3D };
    }

    void CombineLUTsRenderer::DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef lut,
                                               RDG::BufferRef grade ) const
    {
        if ( !m_Pipeline )
            return; // Record refuses by name
        pass.Bindings( m_Layout.Get( m_Pipeline->GetSpecification().Shader ),
                       Renderer::GetPipelineRouteFill( *m_Pipeline ) )
             .Storage( CombineLUTsShaderParameters::ColorGradingLUT, lut, RDG::Access::StorageWrite, 0 )
             .Storage( CombineLUTsShaderParameters::GradeBuffer, grade, RDG::Access::StorageRead );
    }

    Common::BoolResultStr CombineLUTsRenderer::Record( const RDG::PassContext& context ) const
    {
        if ( !m_Pipeline )
            return Common::MakeError( "PostFX: CombineLUTs: the CombineLUTs pipeline is not initialised" );
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        constexpr uint32_t      groups = kLUTSize / kGroupSize;
        return Renderer::GetInstance().DispatchCompute( bindings, *m_Pipeline, groups, groups, groups );
    }
} // namespace Desert::Graphic::System
