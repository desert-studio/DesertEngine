#pragma once

// DEPENDENCY DECLARED BY THE AI-STATETREE CONTRACT (header only, implemented by the TAGS card) -
// docs/contracts/AI-STATETREE.md section 2.
//
// GAMEPLAY TAGS (UE GameplayTags module: FGameplayTag GameplayTagContainer.h, FGameplayTagContainer,
// UGameplayTagsManager). A tag is a dotted hierarchical name ("Perception.Sight.Gained") that the project
// REGISTERS (Config/GameplayTags.json, UE DefaultGameplayTags.ini) and the engine INTERNS: a GameplayTag value is
// an index into the one registry, so comparing, hashing and parent-matching never touch a string, and a tag
// nobody registered cannot be made (RequestTag refuses it by name - UE ErrorIfNotFound, here always on).
//
// Lead decision 10-22 (AI-STATETREE Q5): StateTree events, transition triggers and perception tags use this
// type, never strings. Until TAGS lands nothing that includes this header links a call to it.

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::GameplayTags
{
    /// Index of a registered tag; kNoTag = the empty tag (UE FGameplayTag::EmptyTag).
    using TagIndex                     = std::uint16_t;
    inline constexpr TagIndex kNoTag   = 0xFFFFu;
    inline constexpr TagIndex kMaxTags = 0xFFFEu;

    class GameplayTagRegistry;

    /// One interned tag. Trivially copyable, 2 bytes; equality is identity.
    class GameplayTag
    {
    public:
        constexpr GameplayTag() = default;

        [[nodiscard]] constexpr bool IsValid() const
        {
            return m_Index != kNoTag;
        }
        [[nodiscard]] constexpr TagIndex Index() const
        {
            return m_Index;
        }

        /// UE MatchesTag: true when this tag IS `other` or lies below it ("A.B.C" matches "A.B" and "A").
        [[nodiscard]] bool MatchesTag( GameplayTag other ) const;
        /// UE MatchesTagExact.
        [[nodiscard]] constexpr bool MatchesTagExact( GameplayTag other ) const
        {
            return IsValid() && m_Index == other.m_Index;
        }
        /// The direct parent ("A.B" for "A.B.C"); the empty tag for a root.
        [[nodiscard]] GameplayTag Parent() const;
        /// The full dotted name, owned by the registry.
        [[nodiscard]] std::string_view Name() const;

        friend constexpr bool operator==( GameplayTag, GameplayTag ) = default;

    private:
        friend class GameplayTagRegistry;
        constexpr explicit GameplayTag( TagIndex index ) : m_Index( index )
        {
        }
        TagIndex m_Index = kNoTag;
    };

    /// UE FGameplayTagContainer: a set of tags, with the parent-aware queries.
    class GameplayTagContainer
    {
    public:
        void AddTag( GameplayTag tag );
        void RemoveTag( GameplayTag tag );

        /// UE HasTag: some tag of the container matches `tag` (parent-aware).
        [[nodiscard]] bool HasTag( GameplayTag tag ) const;
        [[nodiscard]] bool HasTagExact( GameplayTag tag ) const;
        [[nodiscard]] bool HasAny( const GameplayTagContainer& other ) const;
        [[nodiscard]] bool HasAll( const GameplayTagContainer& other ) const;

        [[nodiscard]] std::span<const GameplayTag> Tags() const
        {
            return m_Tags;
        }

    private:
        std::vector<GameplayTag> m_Tags; // sorted by index, unique
    };

    /// The project's one tag table (UE UGameplayTagsManager). Built at startup from the project config; a tag
    /// table is append-only while the world runs, so a GameplayTag never dangles.
    class GameplayTagRegistry
    {
    public:
        static GameplayTagRegistry& Get();

        /// Reads Config/GameplayTags.json. Refuses, naming the row: a malformed name (empty segment, a character
        /// outside [A-Za-z0-9_]), a repeated name, more than kMaxTags tags. Every parent of a listed tag is
        /// registered implicitly (UE: "A.B.C" registers "A" and "A.B").
        Common::BoolResultStr Load( const std::filesystem::path& configPath );

        /// A native tag the engine itself needs (UE UE_DEFINE_GAMEPLAY_TAG): registered at startup, before Load.
        Common::ResultStr<GameplayTag> RegisterNative( std::string_view name, std::string_view comment );

        /// The registered tag named `name`; an error naming it otherwise (never a silent empty tag).
        [[nodiscard]] Common::ResultStr<GameplayTag> RequestTag( std::string_view name ) const;

        [[nodiscard]] std::string_view Name( GameplayTag tag ) const;
        [[nodiscard]] GameplayTag      Parent( GameplayTag tag ) const;
    };
} // namespace Desert::GameplayTags
