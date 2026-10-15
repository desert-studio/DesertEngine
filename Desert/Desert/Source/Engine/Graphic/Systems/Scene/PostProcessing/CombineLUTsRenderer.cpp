// Ported from UE 5.8 Engine/Source/Runtime/Renderer/Private/PostProcess/PostProcessCombineLUTs.cpp
// (AddCombineLUTPass: the 32^3 volume, the compute dispatch, re-rendering only on a changed settings hash),
// adapted: the table is this system's persistent Image3D baked by a node of our frame graph, and the cache key is
// the packed grade itself.
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

        const Core::Formats::Image3DSpecification spec{
             .Tag        = "ColorGrading.LUT",
             .Width      = kLUTSize,
             .Height     = kLUTSize,
             .Depth      = kLUTSize,
             .Format     = Core::Formats::ImageFormat::RGBA16F,
             .Data       = Core::Formats::EmptyPixelData{},
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
        };
        m_LUT = Image3D::Create( spec );
        if ( !m_LUT )
            return Common::MakeFormattedError<bool>( "CombineLUTsRenderer: the {}^3 RGBA16F table was not created",
                                                     kLUTSize );
        m_Baked.reset();
        return BOOLSUCCESS;
    }

    void CombineLUTsRenderer::DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef lut,
                                               RDG::BufferRef params ) const
    {
        if ( !m_Pipeline )
            return; // Record refuses by name
        pass.Bindings( m_Layout.Get( m_Pipeline->GetSpecification().Shader ),
                       Renderer::GetPipelineRouteFill( *m_Pipeline ) )
             .Storage( "u_ColorGradingLUT", lut, RDG::Access::StorageWrite )
             .Storage( "CombineLUTsBuffer", params, RDG::Access::StorageRead );
    }

    Common::BoolResultStr CombineLUTsRenderer::Record( const RDG::PassContext& context,
                                                       const FilmGradeParams&  grade )
    {
        if ( !m_Pipeline )
            return Common::MakeError( "PostFX: CombineLUTs: the CombineLUTs pipeline is not initialised" );
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        constexpr uint32_t      groups = kLUTSize / kGroupSize;
        Common::BoolResultStr   dispatched =
             Renderer::GetInstance().DispatchCompute( bindings, *m_Pipeline, groups, groups, groups );
        if ( dispatched )
            m_Baked = grade;
        return dispatched;
    }
} // namespace Desert::Graphic::System
