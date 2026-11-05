#pragma once

#include <Common/Core/DevInstruments.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/Core/DebugDraw.hpp>
#include <Engine/Graphic/Materials/Debug/MaterialDebugLine.hpp>
#include <Engine/Graphic/Render2D/Render2D.hpp>
#include <Engine/Graphic/SystemRasterPass.hpp>

#include <array>
#include <cstdint>
#include <memory>

namespace Desert::Graphic
{
    class SceneRenderer;

    /// Draws one view's share of its scene's DebugDrawWorld (Core/DebugDraw.hpp) over the TONEMAPPED final image
    /// (SceneRenderer::AddFrameDebugDraw): the segments and points tested against the scene's depth, the strings
    /// over the picture. UE splits the same job between the line batcher's scene proxy (the lines and points,
    /// composited with the editor primitives after the tonemapper, in the world's depth or the foreground group)
    /// and the viewport canvas (DrawDebugString); here both are passes of the view's frame, so the editor
    /// viewport and Play draw them by the same code.
    ///
    /// Owned by the SceneRenderer and only in development builds - a player's binary has no DebugDraw store to
    /// read (DESERT_DEV_INSTRUMENTS, as UE's ENABLE_DRAW_DEBUG).
    class DebugDrawRenderer
    {
    public:
        explicit DebugDrawRenderer( SceneRenderer& owner ) : m_Owner( owner )
        {
        }

        /// This frame's items, COPIED (UE: the line batcher's proxy owns its batched lines): BeginScene hands
        /// over the scene's store on the game thread, and the passes read the copy on the render task while the
        /// next update ages the store and draws into it. nullptr outside a BeginScene (nothing is drawn then).
        /// Copy-assigned, so the copy keeps its capacity from frame to frame.
        void SetWorld( const Core::DebugDrawWorld* world )
        {
            if ( world != nullptr )
                m_Captured = *world;
            m_World = world != nullptr ? &m_Captured : nullptr;
        }

        [[nodiscard]] bool HasLines() const;
        [[nodiscard]] bool HasStrings() const;

        /// The segments and points, depth-tested (DebugDrawDepth::World) then over everything (Foreground), on
        /// the final image (ViewTargetLayouts DisplayOverlayLayout). Thickness 0 is a hairline; a thicker line
        /// is a camera-facing quad of that many centimetres. A point is a square of its size in PIXELS at the
        /// node's render area (UE DrawDebugPoint), at its centre's depth.
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
        Core::DebugDrawWorld                                        m_Captured; // this frame's copy (SetWorld)
        const Core::DebugDrawWorld*                                 m_World = nullptr;
        std::array<std::shared_ptr<GraphicsPipeline>, kBatchCount>  m_Pipelines;
        std::array<std::unique_ptr<MaterialDebugLine>, kBatchCount> m_Materials;
        // The vertices of each batch LinesPass's Declare uploaded this frame (0 = the batch does not draw).
        std::array<uint32_t, kBatchCount>                           m_BatchVertices{};
        bool                                                        m_LinePipelinesRefused = false;

        Render2D::Render2D m_Text;
        const Framebuffer* m_TextTarget = nullptr; // the target m_Text's pipeline was built against
    };
} // namespace Desert::Graphic
#endif // DESERT_DEV_INSTRUMENTS
