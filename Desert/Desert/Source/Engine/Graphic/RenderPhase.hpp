#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace Desert::Graphic
{
    // Opaque phase identifier. Built-in phases are spaced by 100 so user phases
    // can be inserted between them without collisions.
    using RenderPhaseID = uint32_t;

    namespace RenderPhase
    {
        constexpr RenderPhaseID None         = 0;
        constexpr RenderPhaseID DepthPrePass = 100;
        constexpr RenderPhaseID Sky          = 200;
        constexpr RenderPhaseID Geometry     = 300;
        constexpr RenderPhaseID Outline      = 400;
        constexpr RenderPhaseID Decals       = 500;
        constexpr RenderPhaseID Lighting     = 600;
        constexpr RenderPhaseID Transparency = 700;
        constexpr RenderPhaseID PostProcess  = 800;
        constexpr RenderPhaseID Overlay      = 900;
        constexpr RenderPhaseID UI           = 1000;
        constexpr RenderPhaseID Debug        = 1100;

        // User-defined phases must start at k_UserBase.
        constexpr RenderPhaseID k_UserBase   = 10000;

        // Canonical declaration order used as tie-breaker seed in topological sort.
        inline constexpr RenderPhaseID k_BuiltinOrder[] = {
            DepthPrePass, Sky, Geometry, Outline, Decals,
            Lighting, Transparency, PostProcess, Overlay, UI, Debug
        };
        inline constexpr std::size_t k_BuiltinCount = std::size( k_BuiltinOrder );

        // --- THE PHASES THE MAIN GRAPH LOOP DOES NOT DRAW (Ю18) ---------------------------------
        //
        // WHAT THIS IS. Three phases are sorted by the graph like every other and then deliberately
        // SKIPPED by the loop that executes it; each has its own pass afterwards
        // (SceneRenderer::ExecuteTransparency / ExecuteUI / ExecuteDebugOverlay). Both halves of that
        // arrangement are load-bearing:
        //
        //   * they must run AFTER the deferred lighting composite, or lit geometry paints over them —
        //     that is the particle top-down defect, and a UI canvas has exactly the same exposure;
        //   * they must open their render pass as a LOAD rather than a CLEAR, or they wipe the depth
        //     the next overlay tests against — that is the grid-through-meshes regression.
        //
        // WHY IT IS A REGISTER AND NOT THREE COMPARISONS. It WAS three comparisons plus a fourth
        // `a || b || c` in the loop, i.e. four expressions of one policy that nothing forced to agree,
        // and the consequence of them disagreeing is invisible in every test this project has: the
        // canvas would be drawn in the wrong place in a Vulkan command buffer, which no device-free
        // suite can observe and which `RenderGraphSort` cannot see either — that suite asserts the
        // ORDER the graph produces, and this list is precisely the set of phases whose order the
        // executor then ignores. Same shape as Engine/Core/ViewBudget.hpp: several callers
        // needing one answer, written out separately a screen apart.
        //
        // Desert/Tests/Engine/UICanvasOverScene asserts the membership this file declares.
        inline constexpr RenderPhaseID k_DeferredOverlayPhases[] = { Transparency, UI, Debug };

        // Is @p id drawn after the composite by its own pass rather than by the main graph loop?
        [[nodiscard]] constexpr bool IsDeferredOverlay( RenderPhaseID id )
        {
            return std::ranges::any_of( k_DeferredOverlayPhases,
                                        [id]( RenderPhaseID phase ) { return phase == id; } );
        }
    }

    // Returns the human-readable name for a phase ID.
    // Delegates to RenderPhaseRegistry, so user-registered phases are supported.
    std::string_view RenderPhaseToString( RenderPhaseID id );

} // namespace Desert::Graphic
