#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/compatibility.hpp>

namespace Desert::Assets::Serialization
{
    // EVERY SCALAR IN THIS FILE CARRIES AN INITIALISER. These structs are what reflect-cpp writes into a
    // `.anim`, so a field left indeterminate is not a runtime accident that the next assignment repairs — it
    // is bytes on disk that outlive the process. glm's vector/quaternion default constructors leave their
    // components indeterminate too, which is why the Value members are spelled out as well.
    // `Tick`, NOT `Time`, AND THE RENAME IS THE POINT. The field it replaces was a float in a unit the
    // format could not state: every shipped clip set `TicksPerSecond` to 1, so a "tick" was a second and
    // 210 of the corpus's 267 key times were fractional. A tick is now a whole number on the clip's own
    // `TickRate` grid, which makes "are these two keys at the same time" a `==` instead of a tolerance.
    //
    // The rename also does the work a version byte cannot do alone: a v0 file has `Time` and no `Tick`, so
    // reading it with DefaultIfMissing yields tick 0 for every key — which is why the loader REFUSES a v0
    // file outright instead of reading one.
    struct KeyPosition
    {
        int32_t   Tick  = 0;
        glm::vec3 Value = glm::vec3( 0.0f );
    };

    struct KeyRotation
    {
        int32_t   Tick  = 0;
        glm::quat Value = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
    };

    struct KeyScale
    {
        int32_t   Tick  = 0;
        glm::vec3 Value = glm::vec3( 1.0f );
    };

    // NO BONE INDEX. It was here, uninitialised, and the Sequencer wrote whatever the stack held into every
    // clip it saved; the loader then sized the track array from it. The bone NAME is what playback binds on,
    // it is what an animation and a character actually share, and an empty name is a refusal the loader can
    // see — which is the whole property an index of 0 could never have.
    struct ChannelData
    {
        std::string BoneName;

        std::vector<KeyPosition> Positions;
        std::vector<KeyRotation> Rotations;
        std::vector<KeyScale>    Scales;
    };

    // Animation notify / event: a named marker at a time (same unit as Duration / key times) that fires once
    // when playback crosses it — dispatched to the entity's scripts as OnAnimationNotify(name). Footstep,
    // hit-frame, "spawn VFX", etc.
    struct NotifyData
    {
        std::string Name;
        int32_t     Tick = 0;
    };

    // An exact rational rate, mirroring Animation::FrameRate. A SEPARATE STRUCT and not that type because
    // this one is the FILE: reflect-cpp writes these two fields, and a runtime type is free to grow
    // members that have no business on disk.
    struct FrameRateData
    {
        int32_t Numerator   = 24000;
        int32_t Denominator = 1;
    };

    /**
     * @brief The `.anim` file, and the ONE definition of it.
     *
     * IT HAS ITS OWN VERSION SEQUENCE, and that was a correction to this task's brief rather than a design
     * flourish. The scene carries `kSceneVersion`, but a `.anim` is not a scene: it has never had a version
     * field, `Tools/SceneMigrator` collected three extensions and none of them was this one, and a scene
     * names a clip BY NAME — so nothing in a `.desce` changes when a clip's time model does. Taking a scene
     * step for it would have sent every `.desce` in the repository through a migration that had nothing to
     * do with them, and stamped each one with a conversion that never happened to it.
     */
    struct AnimationAssetData
    {
        /**
         * @brief Schema generation of THIS file. ABSENT MEANS 0, AND 0 MEANS PRE-TICK — never "current".
         *
         * A plain `int` defaulting to 0 rather than an optional, because the two spellings differ exactly
         * where it matters: with `rfl::DefaultIfMissing`, a file written before this field existed reads
         * back as 0 and is therefore refused by the loader and converted by the migrator. Had the default
         * been the current generation, every legacy file would have claimed to be current and had its
         * float seconds read as ticks — an empty successful answer in a migrator, which is a defect class
         * this project has already paid for.
         */
        int Version = 0;

        std::string Name;

        /// The resolution `Tick` values are counted at. Stored per file so a `.anim` is self-describing:
        /// a future change of the project rate is then a difference two numbers can state, not an
        /// assumption every reader shares.
        FrameRateData TickRate;

        /// The grid an artist sees and snaps to. Separate from TickRate on purpose — see TimeModel.hpp.
        FrameRateData DisplayRate{ 30, 1 };

        /// Length of the clip in ticks on `TickRate`'s grid.
        int32_t DurationTicks = 0;

        uint64_t                 SkeletonSignature = 0; // 0 = "no rig claimed", as on AnimationClip
        std::vector<ChannelData> Channels;
        // New field — clips cooked before notifies existed load with rfl::DefaultIfMissing (empty list).
        std::vector<NotifyData>  Notifies;
    };

    /// The generation this build writes and the only one it reads. Its own sequence, starting at 1: the
    /// `.anim` format had no version before this step, and every file that predates it is generation 0.
    inline constexpr int kAnimationVersion = 1;
} // namespace Desert::Assets::Serialization