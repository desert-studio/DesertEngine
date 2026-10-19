#pragma once

#include <Common/Core/DevInstruments.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/Graphic/Materials/Debug/MaterialDebugLine.hpp>
#include <Engine/Graphic/Render2D/Render2D.hpp>
#include <Engine/Graphic/SystemRasterPass.hpp>

#include <array>
#include <memory>

namespace Desert::Core
{
    class DebugDrawWorld;
}

namespace Desert::Graphic
{
    class SceneRenderer;

    /// Draws one view's share of its scene's DebugDrawWorld (Core/DebugDraw.hpp): the segments over the lit scene
    /// and the strings over the picture. UE splits the same job between the line batcher's scene proxy (the
    /// lines, in the world's depth or the foreground group) and the viewport canvas (DrawDebugString); here both
    /// are passes of the view's frame, so the editor viewport and Play draw them by the same code.
    ///
    /// Owned by the SceneRenderer and only in development builds - a player's binary has no DebugDraw store to
    /// read (DESERT_DEV_INSTRUMENTS, as UE's ENABLE_DRAW_DEBUG).
    class DebugDrawRenderer
    {
    public:
        explicit DebugDrawRenderer( SceneRenderer& owner ) : m_Owner( owner )
        {
        }

        /// This frame's store, or nullptr outside a BeginScene (nothing is drawn then).
        void SetWorld( const Core::DebugDrawWorld* world )
        {
            m_World = world;
        }

        [[nodiscard]] bool HasLines() const;
        [[nodiscard]] bool HasStrings() const;

        /// The segments, depth-tested (DebugDrawDepth::World) then over everything (Foreground). Thickness 0 is
        /// a hairline; a thicker line is a camera-facing quad of that many centimetres.
        [[nodiscard]] SystemRasterPass LinesPass();
        /// The strings, at the screen point each world position projects to, with a one-pixel shadow (UE
        /// DrawDebugString's bDrawShadow).
        [[nodiscard]] SystemRasterPass StringsPass();

    private:
        // Batch index = depth * 2 + (thick ? 1 : 0): hairlines go through a Lines pipeline, thick lines as
        // triangles; each depth group has its own pipeline because depth testing is pipeline state.
        static constexpr size_t kBatchCount = 4;

        [[nodiscard]] bool                  EnsureLinePipelines();
        [[nodiscard]] Common::BoolResultStr EnsureText( const std::shared_ptr<Framebuffer>& target );
        [[nodiscard]] Common::BoolResultStr RecordStrings( float width, float height );

        SceneRenderer&                                              m_Owner;
        const Core::DebugDrawWorld*                                 m_World = nullptr;
        std::array<std::shared_ptr<GraphicsPipeline>, kBatchCount>  m_Pipelines;
        std::array<std::unique_ptr<MaterialDebugLine>, kBatchCount> m_Materials;
        bool                                                        m_LinePipelinesRefused = false;

        Render2D::Render2D m_Text;
        const Framebuffer* m_TextTarget = nullptr; // the target m_Text's pipeline was built against
    };
} // namespace Desert::Graphic
#endif // DESERT_DEV_INSTRUMENTS
