#pragma once

/**
 * THE RIG AS A FILE — T5 gets an author, and therefore a scene gets a rig.
 *
 * Tier T5 is built: a control hierarchy (T5.1), a manipulator (T5.2), keying (T5.3) and a pipeline stage
 * (T5.4). Every one of them is proven by a suite and NONE of them is reachable from a scene, because a
 * `ControlRigStage` has to be constructed in C++ by somebody, and nobody does. This file is the half that
 * was missing: the thing a rigger saves, a component names, and the frame loads.
 *
 * ── THE DEFECT SHAPE THIS EXISTS TO AVOID ────────────────────────────────────────────────────────────
 *
 * `PreloadCloudLayouts` scanned a directory, registered what it found, and was called by nobody; the
 * feature was dead from the day it shipped and every format and panel test passed, because not one of them
 * ran the LAYER. Adding more rig capability before the rig has a way into a scene builds exactly that
 * again. So the proof this file owes is not "the bytes round-trip" — it is "a scene that names a rig
 * produces DIFFERENT skinning matrices from the same scene without one", which is what
 * `Tests/Engine/ControlRigAsset` measures.
 *
 * ── ITS OWN VERSION COUNTER, AND THE ARGUMENT IS THE PAYLOAD ─────────────────────────────────────────
 *
 * `PrefabData.hpp` states, in the field comment on its two integers, exactly WHY a `.deprefab` shares the
 * scene's number: "a prefab's payload is the scene's own EntityData, written by the same
 * ComponentRegistry, so a schema step that moves Core::kSceneVersion moves this file's format with it
 * whether anyone remembered prefabs or not". Not one clause of that is true here — a `.derig` carries no
 * `EntityData`, no component key and nothing `ComponentRegistry` writes — so borrowing `kSceneVersion`
 * would tie this format to steps that cannot touch it and bind every existing scene to a re-run of the
 * migrator for a change that is not in them.
 *
 * `.anim` (`kAnimationVersion`) is the precedent that fits: a distinct animation-domain payload with a
 * sequence of its own. `.detheme` and `.decloudtype` are the shape that fits: `FormatVersion` is an
 * OPTIONAL integer the file states about itself, an unknown value is REFUSED by name rather than read as
 * if it meant what it means here, and there is no migration in the runtime at all.
 *
 * AND THERE IS NOTHING TO MIGRATE. Generation 1 is the first generation: there exists no `.derig` written
 * by an earlier build, so the corpus conversion the contract requires around a version bump has an empty
 * input. The engine's scene and prefab corpora are untouched by this task — adding a component KEY has
 * never moved `kSceneVersion` (A3/2 `cf9ab2d0` added `TwoBoneIK` and did not), because an unknown key is
 * preserved by `Serialize/ForeignKeys.hpp` and an absent one defaults.
 *
 * ── EVERYTHING IS A NAME, AND THAT IS THE LOAD-BEARING DECISION ──────────────────────────────────────
 *
 * In memory a `ControlSpace` holds a bone INDEX and a `ControlBoneDrive` holds two indices, because that is
 * what a per-frame resolve wants. In the FILE they are names, and the conversion happens at load against
 * the skeleton the rig is being applied to.
 *
 * Storing the indices would be the "a middle link drops a property" defect with a file in the middle: both
 * ends look right — the rig saves, the rig loads — and a rig applied to a character with a different bone
 * order silently drives the wrong bones, or resolves its spaces to identity and puts every control at the
 * origin. `ControlHierarchy::GetStructureError` exists because that failure was foreseen; a name-keyed file
 * is what lets it be REPORTED instead of merely possible. It is also the same answer `TwoBoneIKData`
 * already gives (`EndBone` is a name) and the same one `AnimationClipBuild` gives (channels bind by bone
 * name, and the serialised index was deleted).
 */

#include <Engine/Animation/Rig/ControlHierarchy.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Animation
{
    class Skeleton;
    class ControlRigStage;
} // namespace Desert::Animation

namespace Desert::Assets::Serialization
{
    /// The extension the Content Browser, the file dialog, the component's slot and the loader all agree
    /// on. One constant, because a second spelling of it is a slot that silently refuses a valid file.
    inline constexpr const char* kControlRigExtension = ".derig";

    /// Where rigs live, RELATIVE to the project's assets root — the same relative form every asset slot is
    /// serialized in, so a scene that names one is portable off this machine.
    inline constexpr const char* kControlRigAssetsRelativeDir = "Rigs/";

    /**
     * @brief The FILE layout's generation.
     *
     *   1 - controls with named parent spaces, and control -> bone drives by name (A12).
     *
     * See the file note for why this is its own sequence and not `Core::kSceneVersion`. An unknown value is
     * refused in BOTH directions rather than read as if it meant what it means here.
     */
    inline constexpr int32_t kControlRigVersion = 1;

    /// EVERY SCALAR HERE CARRIES AN INITIALISER, for `Serialization/Animation.hpp`'s reason: these structs
    /// are what reflect-cpp writes to disk, and a field left indeterminate is bytes that outlive the
    /// process. glm's default constructors leave their components indeterminate, so they are spelled out.
    struct RigTransformData
    {
        glm::vec3 Translation = glm::vec3( 0.0f );
        glm::quat Rotation    = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
        glm::vec3 Scale       = glm::vec3( 1.0f );

        [[nodiscard]] bool operator==( const RigTransformData& ) const = default;
    };

    /**
     * @brief One parent slot of one control: what it follows, by NAME, and how much.
     *
     * `Kind` is the spelling of `Animation::ControlSpaceKind` — "Component", "Bone" or "Control" — and a
     * STRING rather than an integer because this is the field a rigger hand-edits and a diff reads. An
     * unrecognised spelling is refused by name; it is never taken as Component, which would turn a typo
     * into a control silently parented to the mesh.
     *
     * `Target` is the bone name or the control name. It is IGNORED for "Component" and must be empty there:
     * a slot that names a bone and claims to follow the mesh is two statements about one fact, and the one
     * that loses is invisible.
     */
    struct ControlSpaceData
    {
        std::string Kind = "Component";
        std::string Target;
        float       Weight = 1.0f;

        [[nodiscard]] bool operator==( const ControlSpaceData& ) const = default;
    };

    /// One control: the rigger's offset, the animated pose, the shape name, and the parent slots.
    struct ControlElementData
    {
        std::string                   Name;
        std::string                   ShapeName;
        RigTransformData              Offset;
        RigTransformData              Pose;
        std::vector<ControlSpaceData> Parents;

        [[nodiscard]] bool operator==( const ControlElementData& ) const = default;
    };

    /// One output hop: this control's component-space transform BECOMES this bone's. Both by name; see the
    /// file note on why an index would be a defect with a file in the middle.
    struct ControlDriveData
    {
        std::string Control;
        std::string Bone;

        [[nodiscard]] bool operator==( const ControlDriveData& ) const = default;
    };

    /**
     * @brief One rig on disk, and in memory — the same struct, because there is nothing to convert.
     *
     * `Drives` is NOT optional in the sense that matters. `Animator::AttachRig` refuses a rig that drives no
     * bones, because such a rig is a pipeline stage that cannot change the pose and therefore passes every
     * assertion a working one passes. `ValidateControlRigData` refuses it HERE, at the file, so the refusal
     * names the file instead of arriving one layer later as "AttachRig said no".
     */
    struct ControlRigData
    {
        std::optional<int32_t>          FormatVersion;
        std::string                     Name;
        std::vector<ControlElementData> Controls;
        std::vector<ControlDriveData>   Drives;

        [[nodiscard]] bool operator==( const ControlRigData& ) const = default;
    };

    // ----------------------------------------------------------------------------------------------
    // Pure functions over the file form. No filesystem, no GPU, no globals.
    // ----------------------------------------------------------------------------------------------

    /**
     * @brief Rejects a rig the loader cannot honour, naming the row that is wrong.
     *
     * Pure, so a rig editor can refuse to save for the same reason the loader refuses to read, rather than
     * the two disagreeing about what is legal. Refuses: no controls; no drives (see `ControlRigData`); an
     * empty or duplicate control name; an unknown space kind; a "Component" slot that names a target; a
     * "Bone"/"Control" slot that names none; a "Control" slot naming a control this file does not define;
     * a parent cycle; an all-zero weight set; a non-finite number anywhere; an empty drive; a drive naming
     * a control this file does not define; and two drives on the same bone.
     *
     * THE CYCLE AND THE UNKNOWN-CONTROL CHECKS ARE HERE AS WELL AS IN `ControlHierarchy::Add`, and that is
     * not duplication of the rule but of the MOMENT: `Add` can only see what has been added so far, so a
     * file whose controls are written child-first would be refused by `Add` for naming an unknown parent
     * when the real answer is "this file is fine, read it in a different order". `BuildControlRig` below
     * relies on this function having ordered the file, which is why it is not optional.
     */
    NO_DISCARD Common::BoolResultStr ValidateControlRigData( const ControlRigData& data );

    /**
     * @brief Parses the text of a `.derig`, or says why it could not.
     *
     * Reads the version FIRST and on its own, for `ParseUITheme`'s reason: a file from another format fails
     * a full parse with a message about whichever field happens to be missing, which is true and useless.
     */
    NO_DISCARD Common::ResultStr<ControlRigData> ParseControlRig( const std::string& text );

    /// Serialises a rig back to the text `ParseControlRig` reads. Total over any @p data `Validate`
    /// accepts, and re-reads EQUAL — which is `operator==`, by value, not "the file is non-empty".
    NO_DISCARD std::string WriteControlRig( const ControlRigData& data );

    /// Reads and parses a `.derig` from disk. The filesystem half, kept out of `ParseControlRig` so the
    /// format's rules stay testable without a file.
    NO_DISCARD Common::ResultStr<ControlRigData> LoadControlRigFile( const std::filesystem::path& path );

    /// Writes @p data to @p path atomically, reporting whether the BYTES ARRIVED — `SaveClipToFile`'s
    /// reason: an unchecked `operator<<` hands back a success value before the flush has happened.
    NO_DISCARD Common::BoolResultStr SaveControlRigFile( const std::filesystem::path& path,
                                                         const ControlRigData&        data );

    // ----------------------------------------------------------------------------------------------
    // File form -> runtime form
    // ----------------------------------------------------------------------------------------------

    /**
     * @brief Build the pipeline stage this file describes, resolved against THIS skeleton.
     *
     * The one place a name becomes an index. Refuses — naming both the rig and the bone — when the file
     * names a bone this skeleton does not have, because the alternative is what the file note describes:
     * a space that resolves to identity and a control at the origin, with nothing said.
     *
     * The stage comes back with its drives already sorted into the skeleton's resolve order
     * (`ControlRigStage::SetDrives` does that), so `Animator::AttachRig` can accept it as it stands.
     */
    NO_DISCARD Common::BoolResultStr BuildControlRig( const ControlRigData&       data,
                                                      const Animation::Skeleton&  skeleton,
                                                      Animation::ControlRigStage& out );

    /// The exact mirror of `BuildControlRig`, and it lives beside it for `BuildAssetDataFromClip`'s reason:
    /// a format whose two directions are not testable together is a format whose round trip is an
    /// assumption. Needs the skeleton to turn the stage's bone indices back into names.
    NO_DISCARD Common::ResultStr<ControlRigData> BuildDataFromControlRig( const std::string&                name,
                                                                          const Animation::ControlRigStage& rig,
                                                                          const Animation::Skeleton& skeleton );
} // namespace Desert::Assets::Serialization
