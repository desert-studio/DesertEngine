#pragma once

/**
 * THE TIMELINE CORE (ANIM-UNIFY, step 0): WHAT A TRACK IS ABOUT.
 *
 * The core is one chain, UE MovieScene's, and every file in this folder is one link of it:
 *
 *     Sequence ─owns→ Binding (an object, by GUID)        Binding.hpp
 *              ─owns→ Track   (binding GUID + property)   Track.hpp
 *                       ─owns→ Section (range, blend, weight)   Section.hpp
 *                                ─owns→ Channel (keys: ONE key model, ScalarKey)   Channel.hpp
 *     Player    — time only: position, rate, loop.        Player.hpp
 *     Evaluator — time → values → the HOST's resolver applies them.   Evaluator.hpp
 *     Hosts     — AnimationClip (.anim), UI animation, LevelSequence (.dseq): each IS a Sequence with a
 *                 restricted binding set, and the migration of each old format is a function in Hosts.hpp.
 *
 * LAYERS ARE NOT HERE. A layered character is several sequences feeding AnimGraph nodes
 * (Graph/LayeredBlendPerBone.hpp, Graph/LinkedAnimLayer.hpp); the core blends SECTIONS of one track, which
 * is a statement about one value, never about two poses.
 *
 * ── THIS FILE ─────────────────────────────────────────────────────────────────────────────────────────
 *
 * A BINDING IS AN IDENTITY PLUS A LOCATOR, AND THE TWO ARE DIFFERENT QUESTIONS (UE: FGuid + binding
 * reference). The GUID is what tracks point at; it is minted once and survives a rename. The locator is
 * how the HOST finds the live object this frame — a bone name, an entity UUID, a widget's entity UUID.
 * Renaming a bone rewrites one locator and no track, which is the whole reason the GUID exists: the bone
 * NAME used to be the only binding key (AnimationClip.hpp, BoneTrack) and a rename meant rewriting every
 * track that spoke it.
 *
 * The core never resolves a locator itself. It does not link the ECS, the skeleton or the UI, and it
 * must not: resolution is the host's (Evaluator.hpp, `ITimelineHost::Resolve`).
 */

#include <Common/Content/AssetEnvelope.hpp>

#include <cstdint>
#include <string>

namespace Desert::Animation::Timeline
{
    /**
     * @brief A binding's identity inside ONE sequence. 128 bits, minted once, then only copied.
     *
     * A distinct type around `AssetGuid` rather than an alias: the compiler must refuse a binding GUID
     * where an asset GUID is expected (an Animation section holds both, side by side). The generator is
     * the asset one — one source of randomness for every GUID in the engine.
     */
    struct BindingGuid
    {
        Common::Content::AssetGuid Value;

        [[nodiscard]] static BindingGuid Generate()
        {
            return BindingGuid{ Common::Content::AssetGuid::Generate() };
        }
        [[nodiscard]] bool IsNull() const
        {
            return Value.IsNull();
        }
        bool operator==( const BindingGuid& ) const = default;
    };

    /**
     * @brief What kind of object the locator names. STORED AS AN INTEGER: append only.
     *
     * `Sequence` is not an object: it is the null binding a sequence-level ("master") track uses — Camera
     * Cut, Event, a clip's named float curves. Spelled as a kind and not as "an empty GUID means master",
     * because a null GUID is also what a forgotten initialiser looks like.
     */
    enum class BindingKind : uint8_t
    {
        Sequence = 0, ///< no object; the track belongs to the sequence itself
        Bone     = 1, ///< locator = bone name, resolved against the skeleton the host plays on
        Entity   = 2, ///< locator = entity UUID as text (LevelSequence possessable)
        Widget   = 3, ///< locator = the UI element's entity UUID as text (UI animation)
    };

    [[nodiscard]] const char* ToString( BindingKind kind );

    /**
     * @brief One bound object of a sequence.
     *
     * INVARIANTS (checked by `Validate( const Sequence& )`):
     *   * `Guid` is non-null and unique within the sequence;
     *   * `Kind == Sequence` iff `Locator` is empty — a master binding names nothing, an object binding
     *     names something;
     *   * `Parent`, when non-null, is another binding of the same sequence (a bone under the entity whose
     *     skeleton it belongs to, in a LevelSequence). No cycles.
     *
     * SERIALIZATION: GUID as 32 hex (AssetGuidToText), kind as its integer, locator and label as text.
     */
    struct Binding
    {
        BindingGuid Guid;
        BindingKind Kind = BindingKind::Sequence;
        std::string Locator;
        /// What the Sequencer's outliner shows. NOT a key: two bindings may share a label.
        std::string Label;
        BindingGuid Parent;
    };
} // namespace Desert::Animation::Timeline
