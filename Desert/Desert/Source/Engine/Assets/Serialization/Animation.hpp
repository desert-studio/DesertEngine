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
    /**
     * @brief The shape of the segment a key is the LATER end of, plus the slopes that shape it.
     *
     * A6 / generation 2. `Interp` and `Mode` are stored as int for the same reason every enum in this
     * file is (stable, tolerant serialization); the two weight fields are RESERVED AND ZERO — weighted
     * tangents are a second evaluator, not a field, and the place for them exists so that adding them
     * later is a code change and not a migration (report 05 §969).
     *
     * EVERY KEY IN EVERY SHIPPED CLIP STATES THESE EXPLICITLY, and that is the condition the version step
     * was granted on rather than a nicety. `rfl::DefaultIfMissing` would happily invent them, and an
     * invented default is indistinguishable from an authored one for ever after — so the migration writes
     * them into the corpus and `Tests/Engine/AnimationClipCorpus` reads the files back to check that it
     * did. A version that changed only the number a file states about ITSELF, and nothing it says about
     * its contents, would be versioning for its own sake.
     */
    struct KeyShape
    {
        int   Interp       = 1; // KeyInterp::Linear — what every clip did before per-key interpolation
        int   Mode         = 0; // TangentMode::Auto
        float ArriveWeight = 0.0f;
        float LeaveWeight  = 0.0f;
    };

    struct KeyPosition
    {
        int32_t   Tick  = 0;
        glm::vec3 Value = glm::vec3( 0.0f );
        KeyShape  Shape;
        // Value units per SECOND, one per component — a tangent is a slope and a slope is a scalar. See
        // Engine/Animation/KeyInterpolation.hpp for why the unit is seconds and not ticks.
        glm::vec3 ArriveTangent = glm::vec3( 0.0f );
        glm::vec3 LeaveTangent  = glm::vec3( 0.0f );
    };

    /**
     * @brief A rotation key. IT CARRIES NO TANGENTS, and the reason is the maths rather than the schedule.
     *
     * A cubic through quaternions is not a rotation: the Bezier of four quaternions leaves the unit
     * sphere, and the curve that does not is `squad`, which builds its own control quaternions from the
     * neighbours and is a different construction with a different authoring surface. Storing tangent
     * fields here would be four numbers nothing could read — and the shape enum is still present, because
     * `Constant` and `Linear` are both meaningful for a rotation and holding a pose is exactly what
     * `Constant` is for.
     */
    struct KeyRotation
    {
        int32_t   Tick  = 0;
        glm::quat Value = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
        KeyShape  Shape;
    };

    struct KeyScale
    {
        int32_t   Tick  = 0;
        glm::vec3 Value = glm::vec3( 1.0f );
        KeyShape  Shape;
        glm::vec3 ArriveTangent = glm::vec3( 0.0f );
        glm::vec3 LeaveTangent  = glm::vec3( 0.0f );
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

    /// The generation this build writes and the only one it reads.
    ///
    ///   0 - no version field at all: key times are float SECONDS under a `TicksPerSecond` every shipped
    ///       clip set to 1, so the format's own unit was a fiction (A5)
    ///   1 - key times are integer TICKS on a rate the file states, with a display rate beside it (A5)
    ///   2 - a key states the SHAPE of the segment it ends and the slopes that shape it (A6)
    ///
    /// A number given by the teamlead, as the contract requires, and given on a condition: the step had
    /// to make the corpus SAY something new, not merely claim a newer number. See KeyShape.
    inline constexpr int kAnimationVersion = 2;
} // namespace Desert::Assets::Serialization