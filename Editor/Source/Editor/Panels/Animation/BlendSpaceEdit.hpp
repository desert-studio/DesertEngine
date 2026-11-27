#pragma once

#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Graph/BlendSpace.hpp>

#include <span>
#include <string>

// ── A BLEND SPACE AS THE EDITOR SHOWS IT (UE BlendSpaceEditor + SBlendSpaceGridWidget) ─────────────────
//
// ONE spelling for the BlendSpace1D / BlendSpace2D node and a state that plays a blend space: the setup's
// widgets and the plot. The setup is the payload itself, so the plan (PlanPoseGraph) states what is wrong
// with an edit; the widgets refuse nothing of their own.
namespace Desert::Editor::Graph
{
    struct BlendSpaceEditResult
    {
        bool Changed = false; ///< the setup was edited this frame
    };

    /// Axis rows (Min, Max, Wrap, Smoothing type/time/damping/max speed), sample rows (clip combo, X, Y in 2D,
    /// RateScale, remove), Add Sample, Weight Speed, Ease In Out, Loop, Scale Axis.
    [[nodiscard]] BlendSpaceEditResult DrawBlendSpaceSetup( Animation::Graph::BlendSpaceSetup& setup,
                                                            std::span<const std::string>       clipNames );

    /// The plot: the axis box, the samples labelled by clip, the triangles (2D), the live filtered input point
    /// and each sample's current weight from @p run (nullptr = no preview running, the samples alone).
    void DrawBlendSpacePlot( const Animation::Graph::BlendSpaceSetup&  setup,
                             const Animation::Animator::BlendSpaceRun* run );
} // namespace Desert::Editor::Graph
