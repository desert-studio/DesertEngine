#pragma once

#include <Engine/Animation/AnimationClip.hpp>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Animation
{
    class AnimationLibrary;

    // Hand-authored-in-code locomotion clips for the procedural humanoid mannequin
    // ([[procedural-character]] / Geometry::ProceduralCharacterFactory). Each clip is built once and cached,
    // carries the humanoid skeleton's signature, and animates the limb joints with sine cycles (every track
    // also stores a constant bind-local position key, or the bone would collapse to the parent origin).
    class ProceduralCharacterAnimations
    {
    public:
        static const AnimationClip& Idle();
        static const AnimationClip& Walk();
        static const AnimationClip& Run();
        static const AnimationClip& Jump(); // airborne hold pose (tucked legs, arms forward)

        // Registers all locomotion clips as in-memory AnimationAssets so they appear in the AnimationLibrary
        // (editor clip selector + AnimationECSSystem auto-play). Engine-level: a runtime/game calls this, not
        // just the editor — locomotion is gameplay, not an editor concern.
        //
        // CALLED FROM `Animation::PopulateLibrary` AND NOWHERE ELSE. It used to be called by the editor
        // layer directly and by nothing in the runtime, which is half of why a packaged game's library was
        // empty; the population point now owns both kinds of clip so neither host can have one without the
        // other. Returns how many were registered, because a count nobody can read is a count the caller
        // has to guess — and the whole refusal below it is about telling an empty half from a full one.
        //
        // The clips and the mesh are bound to the skeleton asset ProceduralCharacterFactory::HumanoidSkeletonFile
        // names, by the GUID its registry row states (so the registry must be gathered first); no row = 0 clips
        // and an error naming the file.
        [[nodiscard]] static size_t RegisterClips( Assets::AssetManager& assets, AnimationLibrary& library );
    };
} // namespace Desert::Animation
