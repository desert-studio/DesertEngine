#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Core/PostProcessSettings.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/PostProcessing/FilmGrade.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <array>
#include <memory>

namespace Desert::Graphic::System
{
    // The CombineLUTs shader's parameters: the one spelling the pass binds and Initialize checks.
    struct CombineLUTsShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, ColorGradingLUT );
        DESERT_SHADER_PARAMETER( StorageBuffer, GradeBuffer );
        static constexpr std::array kAll{ RDG::Erase( ColorGradingLUT ), RDG::Erase( GradeBuffer ) };
    };

    // UE's CombineLUTs (PostProcessCombineLUTs.usf, AddCombineLUTPass): the view's whole grade - white balance,
    // gamut expansion, colour grading, the tone curve (Core::TonemapOperator) and the output device - baked into
    // one 32^3 table the tonemap samples (SceneComposite.shader). One compute node per frame and output target
    // ("PostFX: CombineLUTs", SceneRendererFramePostFX.cpp): the table is a transient of the frame graph, as UE's
    // is a per-view RDG texture; 32^3 texels cost a few microseconds. The SDR output and the HDR output (and the
    // HDR rendition of a capture) each get the table baked at their headroom - the LUT feeds both.
    class CombineLUTsRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        static constexpr uint32_t kLUTSize = 32; // UE r.LUT.Size

        Common::BoolResultStr Initialize() override;

        // The table's frame-graph desc: kLUTSize^3 RGBA16F, written as storage, sampled by the tonemap.
        [[nodiscard]] static RDG::TextureDesc GetLUTDesc();

        // The view's grade (Graphic::ResolveViewSettings), refreshed each frame by SceneRenderer::BeginScene.
        void SetGrade( const Core::PostProcessSettings& grade )
        {
            m_Grade = FilmGradeOf( grade, 1.0f );
        }
        // The grade as one table bakes it, at the output's @p headroom (1 = SDR).
        [[nodiscard]] FilmGradeParams GradeFor( float headroom ) const
        {
            FilmGradeParams grade = m_Grade;
            grade.Output.x        = headroom;
            return grade;
        }

        // Declares the bake: @p lut written whole, @p grade (one FilmGradeParams, uploaded by the frame) read.
        void DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef lut, RDG::BufferRef grade ) const;
        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context ) const;

    private:
        std::shared_ptr<ComputePipeline> m_Pipeline;
        mutable ShaderBindingLayoutCache m_Layout; // the shader's layout, kept between frames
        FilmGradeParams                  m_Grade = FilmGradeOf( Core::PostProcessSettings{}, 1.0f );
    };
} // namespace Desert::Graphic::System
