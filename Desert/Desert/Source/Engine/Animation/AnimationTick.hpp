#pragma once

namespace Desert::Animation
{
    /**
     * @brief How far one AnimationComponent advances this frame — UE's `bUpdateAnimationInEditor` rule.
     *
     * A world that ticks gameplay (Play, or a paused world's one stepped frame) advances every component by
     * the gameplay time. A world that does not tick gameplay advances NOTHING, except in the editor's own
     * world (`editorSeconds` > 0 only there, pushed by Scene through System::SetEditorTick), where a
     * component that asked for it with `UpdateAnimationInEditor` previews at the world's preview step (0 while
     * the viewport's Realtime is off, as in UE).
     *
     * IT USED TO BE "the gameplay timestep, or a wall clock when that is zero" for EVERY component, read
     * inside AnimationECSSystem: every character in an edited level walked on the spot while the author
     * placed it, a paused game kept animating, and there was no way to say "hold still". One rule, here,
     * so the system and its test read the same one.
     */
    [[nodiscard]] constexpr float AnimationAdvanceSeconds( float gameplaySeconds, float editorSeconds,
                                                           bool updateAnimationInEditor )
    {
        if ( gameplaySeconds > 0.0f )
            return gameplaySeconds;
        return updateAnimationInEditor ? editorSeconds : 0.0f;
    }
} // namespace Desert::Animation
