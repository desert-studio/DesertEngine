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
#include <string_view>

namespace Desert::Animation::Timeline
{
    enum class BindingKind : uint8_t;
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

        /**
         * @brief The binding GUID of an object, derived and not minted: the same input gives the same 128
         * bits on every machine and every run.
         *
         * A binding's GUID only has to be unique INSIDE its sequence, and a sequence binds an object once
         * (one Bone binding per bone of one parent, TrackEditing's find-or-create; one Entity binding per
         * entity), so the input is already an identity there. Deriving it makes generated content
         * deterministic the way UE's DDC keys are: the importer, the procedural clip generator and the
         * migrator write the same bytes for the same input, so a regenerated `.anim` diffs empty. Two FNV-1a
         * 64 passes (different offset bases) over the kind byte, the parent (bones in a level sequence) and
         * the locator's bytes; never null. The value is PERSISTED in every `.anim` and `.dseq`: changing the
         * derivation is a format change.
         *
         * There is one spelling per kind and no `(kind, locator)` form: a bone is an identity only together
         * with the entity it belongs to (two characters of one level sequence both have a `head`).
         */
        [[nodiscard]] static BindingGuid ForEntity( std::string_view uuidText ) noexcept;
        /// The master binding (Kind Sequence, no locator).
        [[nodiscard]] static BindingGuid ForMaster() noexcept;
        /// Bone @p bone under the Entity binding @p parent; @p parent null = a clip's bone, which has no
        /// parent — and then the bytes are exactly the ones every `.anim` already holds (no migration).
        [[nodiscard]] static BindingGuid ForBone( const BindingGuid& parent, std::string_view bone ) noexcept;

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

    namespace Detail
    {
        /// FNV-1a 64 over the kind byte, then @p prefix's bytes, then @p locator's — the one derivation.
        [[nodiscard]] inline BindingGuid DeriveGuid( const BindingKind kind, const std::string_view prefix,
                                                     const std::string_view locator ) noexcept
        {
            const auto fold = [&]( uint64_t hash ) noexcept
            {
                constexpr uint64_t kPrime = 0x100000001b3ULL;
                hash                      = ( hash ^ static_cast<uint8_t>( kind ) ) * kPrime;
                for ( const char c : prefix )
                    hash = ( hash ^ static_cast<unsigned char>( c ) ) * kPrime;
                for ( const char c : locator )
                    hash = ( hash ^ static_cast<unsigned char>( c ) ) * kPrime;
                return hash;
            };
            BindingGuid guid{
                 Common::Content::AssetGuid{ fold( 0xcbf29ce484222325ULL ), fold( 0x84222325cbf29ce4ULL ) } };
            if ( guid.IsNull() )
                guid.Value.Lo = 1;
            return guid;
        }
    } // namespace Detail

    inline BindingGuid BindingGuid::ForEntity( const std::string_view uuidText ) noexcept
    {
        return Detail::DeriveGuid( BindingKind::Entity, {}, uuidText );
    }
    inline BindingGuid BindingGuid::ForMaster() noexcept
    {
        return Detail::DeriveGuid( BindingKind::Sequence, {}, {} );
    }
    inline BindingGuid BindingGuid::ForBone( const BindingGuid& parent, const std::string_view bone ) noexcept
    {
        if ( parent.IsNull() )
        {
            return Detail::DeriveGuid( BindingKind::Bone, {}, bone );
        }
        // The parent's 128 bits, then '/', then the bone: `head` under Hero and under Twin differ.
        char prefix[17];
        for ( int i = 0; i < 8; ++i )
        {
            prefix[i]     = static_cast<char>( ( parent.Value.Hi >> ( 8 * i ) ) & 0xFFU );
            prefix[8 + i] = static_cast<char>( ( parent.Value.Lo >> ( 8 * i ) ) & 0xFFU );
        }
        prefix[16] = '/';
        return Detail::DeriveGuid( BindingKind::Bone, std::string_view( prefix, sizeof( prefix ) ), bone );
    }

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
