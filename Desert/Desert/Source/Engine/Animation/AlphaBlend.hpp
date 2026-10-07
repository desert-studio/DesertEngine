#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <numbers>

// The shape a cross-fade's weight follows over its time (UE: EAlphaBlendOption / FAlphaBlend). ONE function
// for both readers of a transition's weight — the state machine's stack of active transitions and the
// Animator's stack of fading clips — so the two can never disagree about how far a fade has got.
namespace Desert::Animation
{
    /// Stored as int in the graph file (the AnimGraph convention); never renumber.
    enum class AlphaBlendOption : int
    {
        Linear         = 0,
        HermiteCubic   = 1, ///< smoothstep: 3t^2 - 2t^3
        Sinusoidal     = 2, ///< (1 - cos(pi t)) / 2
        QuadraticInOut = 3,
        CubicInOut     = 4,
        QuarticInOut   = 5,
        QuinticInOut   = 6,
        CircularIn     = 7,
        CircularOut    = 8,
        CircularInOut  = 9,
        ExpIn          = 10,
        ExpOut         = 11,
        ExpInOut       = 12,
    };

    inline constexpr int kAlphaBlendOptionCount = 13;

    /// The display name of `option`, for the transition inspector; "?" for a value outside the enum.
    [[nodiscard]] inline const char* AlphaBlendName( AlphaBlendOption option )
    {
        constexpr const char* names[kAlphaBlendOptionCount] = {
             "Linear",         "Hermite Cubic",  "Sinusoidal",  "Quadratic In Out", "Cubic In Out",
             "Quartic In Out", "Quintic In Out", "Circular In", "Circular Out",     "Circular In Out",
             "Exp In",         "Exp Out",        "Exp In Out" };
        const int i = static_cast<int>( option );
        return i >= 0 && i < kAlphaBlendOptionCount ? names[i] : "?";
    }

    /// The weight of the incoming pose at linear progress `t` (clamped to [0,1]) along `option`'s curve.
    /// Exactly 0 at 0 and exactly 1 at 1 for every option, so a fade starts on its source and ends on its
    /// target bit for bit. An unknown option is linear (a file from a newer build degrades to a plain fade;
    /// the inspector shows "?" for it).
    [[nodiscard]] inline float AlphaBlendCurve( AlphaBlendOption option, float t )
    {
        t = glm::clamp( t, 0.0F, 1.0F );
        if ( t <= 0.0F || t >= 1.0F )
            return t;
        const auto inOut = [t]( float power ) {
            return t < 0.5F ? 0.5F * std::pow( 2.0F * t, power )
                            : 1.0F - 0.5F * std::pow( 2.0F * ( 1.0F - t ), power );
        };
        switch ( option )
        {
            case AlphaBlendOption::Linear:
                return t;
            case AlphaBlendOption::HermiteCubic:
                return t * t * ( 3.0F - 2.0F * t );
            case AlphaBlendOption::Sinusoidal:
                return 0.5F * ( 1.0F - std::cos( std::numbers::pi_v<float> * t ) );
            case AlphaBlendOption::QuadraticInOut:
                return inOut( 2.0F );
            case AlphaBlendOption::CubicInOut:
                return inOut( 3.0F );
            case AlphaBlendOption::QuarticInOut:
                return inOut( 4.0F );
            case AlphaBlendOption::QuinticInOut:
                return inOut( 5.0F );
            case AlphaBlendOption::CircularIn:
                return 1.0F - std::sqrt( 1.0F - t * t );
            case AlphaBlendOption::CircularOut:
                return std::sqrt( 1.0F - ( 1.0F - t ) * ( 1.0F - t ) );
            case AlphaBlendOption::CircularInOut:
                return t < 0.5F ? 0.5F * ( 1.0F - std::sqrt( 1.0F - 4.0F * t * t ) )
                                : 0.5F * ( 1.0F + std::sqrt( 1.0F - 4.0F * ( 1.0F - t ) * ( 1.0F - t ) ) );
            case AlphaBlendOption::ExpIn:
                return std::pow( 2.0F, 10.0F * ( t - 1.0F ) ) * t; // exact 0 at 0, 1 at 1
            case AlphaBlendOption::ExpOut:
                return 1.0F - std::pow( 2.0F, -10.0F * t ) * ( 1.0F - t );
            case AlphaBlendOption::ExpInOut:
                return t < 0.5F
                            ? 0.5F * std::pow( 2.0F, 10.0F * ( 2.0F * t - 1.0F ) ) * ( 2.0F * t )
                            : 1.0F - 0.5F * std::pow( 2.0F, -10.0F * ( 2.0F * t - 1.0F ) ) * ( 2.0F - 2.0F * t );
        }
        return t;
    }

    /// Weights of a stack of fades over one base: `alphas[i]` is fade i's curved alpha (oldest first, the
    /// newest last — the pose it fades to is on top). Fade i blends from everything below it to its own
    /// pose, so its weight is alpha_i x the product of (1 - alpha_j) over every fade above it, and the base's
    /// is the product of every (1 - alpha_j). `out` gets base first, then each fade; they sum to 1.
    template <typename Alphas, typename Out>
    void FadeStackWeights( const Alphas& alphas, Out& out )
    {
        const size_t n = alphas.size();
        out.assign( n + 1, 0.0F );
        float above = 1.0F; // product of (1 - alpha) over the fades above the one being weighed
        for ( size_t i = n; i-- > 0; )
        {
            out[i + 1] = alphas[i] * above;
            above *= 1.0F - alphas[i];
        }
        out[0] = above;
    }
} // namespace Desert::Animation
