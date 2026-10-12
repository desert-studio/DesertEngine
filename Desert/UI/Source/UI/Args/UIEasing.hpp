#pragma once

namespace Desert::UI
{
    // The easing curve a UI tween or screen transition authors. Its own header, not Components.hpp: the
    // CPU-side Timeline maps it onto EasingPreset (Timeline/Hosts.cpp) and must not reach the GPU headers
    // Components.hpp pulls in through Material.hpp.
    enum class UIEasing
    {
        Linear,
        QuadIn,
        QuadOut,
        QuadInOut,
        CubicIn,
        CubicOut,
        CubicInOut,
        BackOut, // overshoots then settles — the "pop" of a modal
        ElasticOut,
        BounceOut
    };
} // namespace Desert::UI
