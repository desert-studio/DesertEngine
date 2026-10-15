#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Core/PostProcessSettings.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/PostProcessing/FilmGrade.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <memory>
#include <optional>

namespace Desert::Graphic::System
{
    // UE's CombineLUTs (PostProcessCombineLUTs.cpp / .usf): the whole grade of a view - white balance, gamut
    // expansion, the colour correction of every range, the tone curve and the sRGB output device, FilmCombine in
    // Common/FilmToneMap.glslh - baked into one 32^3 table that the tonemap samples per pixel. The table is the
    // renderer's own persistent image, and it is baked by a compute node of the frame graph ("PostFX:
    // CombineLUTs") only in a frame whose grade packs differently from the one the table holds - UE's cached LUT,
    // which it too re-renders only when its settings hash changes.
    class CombineLUTsRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        // UE's LUT size (r.LUT.Size default 32).
        static constexpr uint32_t kLUTSize = 32;

        virtual Common::BoolResultStr Initialize() override;

        // The view's grade (Graphic::ResolveViewSettings), each frame from SceneRenderer::BeginScene.
        void SetGrade( const Core::PostProcessSettings& grade )
        {
            m_Grade = FilmGradeOf( grade );
        }
        [[nodiscard]] const FilmGradeParams& GetGrade() const
        {
            return m_Grade;
        }
        // The table the tonemap samples (u_ColorGradingLUT): rgba16f, kLUTSize^3, the device colour per texel.
        [[nodiscard]] const std::shared_ptr<Image3D>& GetLUTImage() const
        {
            return m_LUT;
        }
        // Whether this frame's graph must bake: the table was never baked, holds another grade, or the node that
        // last baked it faulted (OnBakeLost).
        [[nodiscard]] bool NeedsBake() const
        {
            return !m_Baked.has_value() || *m_Baked != m_Grade;
        }
        // The frame graph's history reset (FrameTextures::MarkHistory): a fault removed this frame's bake, so
        // what the table holds is not known and the next frame bakes again.
        void OnBakeLost()
        {
            m_Baked.reset();
        }

        // SETUP of "PostFX: CombineLUTs": block 0 - the table as a storage write and @p params (FilmGradeParams,
        // uploaded by the graph) as the shader's CombineLUTsBuffer.
        void DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef lut, RDG::BufferRef params ) const;
        // EXEC: one kLUTSize^3 dispatch; on success the table holds @p grade.
        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context,
                                                    const FilmGradeParams&  grade );

    private:
        std::shared_ptr<ComputePipeline> m_Pipeline;
        mutable ShaderBindingLayoutCache m_Layout; // the shader's layout, kept between frames
        std::shared_ptr<Image3D>         m_LUT;

        FilmGradeParams                m_Grade = FilmGradeOf( Core::PostProcessSettings{} );
        std::optional<FilmGradeParams> m_Baked; // what the table holds; nullopt = nothing known
    };
} // namespace Desert::Graphic::System
