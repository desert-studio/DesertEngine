#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Json/Json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Physics
{
    /**
     * @brief What one body does with another of a given channel (UE ECollisionResponse). Ordered: the
     * response of a PAIR is the lesser of the two bodies' responses to each other's channel, as in UE.
     */
    enum class CollisionResponse : uint8_t
    {
        Ignore,  ///< No contact at all — the pair is never handed to the narrow phase.
        Overlap, ///< Contacts are found and not solved: the bodies pass through each other (a sensor contact).
        Block,   ///< Contacts are found and solved.
    };

    /// Which halves of collision a profile takes part in (UE ECollisionEnabled).
    enum class CollisionEnabled : uint8_t
    {
        NoCollision,     ///< Neither simulated against nor found by a query.
        QueryOnly,       ///< Found by ray casts; never simulated against.
        PhysicsOnly,     ///< Simulated against; never found by a query.
        QueryAndPhysics, ///< Both.
    };

    /// One object channel of the project (UE: an ECollisionChannel row of DefaultChannelResponses).
    struct CollisionChannelConfig
    {
        std::string Name;
        /// What every profile does with this channel unless it states otherwise. Required.
        std::optional<CollisionResponse> DefaultResponse;
    };

    /// A profile's own answer for one channel (UE FResponseChannel).
    struct CollisionResponseConfig
    {
        std::string                      Channel;
        std::optional<CollisionResponse> Response;
    };

    /// One named profile (UE FCollisionResponseTemplate): the channel a body of this profile IS, whether it
    /// collides at all, and its responses to the channels it treats differently from their defaults.
    struct CollisionProfileConfig
    {
        std::string                          Name;
        std::optional<CollisionEnabled>      Enabled;
        std::string                          ObjectChannel;
        std::vector<CollisionResponseConfig> Responses;
    };

    /// The project's `Config/CollisionProfiles.json` (UE: [/Script/Engine.CollisionProfile] in DefaultEngine.ini).
    struct CollisionProfilesConfig
    {
        std::vector<CollisionChannelConfig> Channels;
        std::vector<CollisionProfileConfig> Profiles;
    };
    DESERT_JSON_STRUCT( CollisionProfilesConfig, "CollisionProfiles", 1 )

    /// The register's file name inside the project's Config directory.
    inline constexpr const char* kCollisionProfilesFileName = "CollisionProfiles.json";

    /// The profiles the ENGINE gives bodies it creates itself (UE: UCollisionProfile's engine profile names).
    /// A register without any of them is refused by name: the engine has no other answer for those bodies.
    namespace EngineProfiles
    {
        inline constexpr std::string_view kBlockAll        = "BlockAll";        ///< landscape heightfields
        inline constexpr std::string_view kBlockAllDynamic = "BlockAllDynamic"; ///< kinematic bodies (migration)
        inline constexpr std::string_view kPhysicsActor    = "PhysicsActor";    ///< simulated bodies (migration)
        inline constexpr std::string_view kPawn            = "Pawn";            ///< character capsules
        inline constexpr std::string_view kDestructible    = "Destructible";    ///< fractured geometry
    } // namespace EngineProfiles

    /// Index of a profile in a CollisionProfiles register.
    using CollisionProfileId                         = uint16_t;
    inline constexpr CollisionProfileId kNoProfile   = 0xFFFFu;
    inline constexpr std::size_t        kMaxChannels = 32u;     ///< UE's ECC_MAX
    inline constexpr std::size_t        kMaxProfiles = 0x7FFFu; ///< two Jolt object layers per profile, 16-bit

    /**
     * @brief The validated register: channels, and each profile's response to every channel resolved.
     * Built only through Build/Read, so a value of this type is always whole.
     */
    class CollisionProfiles
    {
    public:
        /// Refuses, naming the row: an empty or repeated channel or profile name, a missing DefaultResponse or
        /// Enabled, an ObjectChannel or response Channel that is no channel, more than kMaxChannels channels
        /// or kMaxProfiles profiles, and a register without one of the EngineProfiles.
        static Common::ResultStr<CollisionProfiles> Build( const CollisionProfilesConfig& config );

        /// Reads and builds @p path. A missing file is an error naming the path — there is no built-in register.
        static Common::ResultStr<CollisionProfiles> Read( const std::filesystem::path& path );

        /// The profile named @p name; an error naming it and the register's profiles otherwise.
        [[nodiscard]] Common::ResultStr<CollisionProfileId> Resolve( std::string_view name ) const;

        /// The pair's response: the lesser of a's response to b's channel and b's response to a's channel.
        /// Ignore when either profile does not take part in physics.
        [[nodiscard]] CollisionResponse PhysicsResponse( CollisionProfileId a, CollisionProfileId b ) const;

        /// A ray cast may find a body of this profile.
        [[nodiscard]] bool IsQueryable( CollisionProfileId id ) const;

        [[nodiscard]] std::size_t ProfileCount() const
        {
            return m_Profiles.size();
        }
        [[nodiscard]] const std::string& ProfileName( CollisionProfileId id ) const
        {
            return m_Profiles[id].Name;
        }

    private:
        struct Profile
        {
            std::string                    Name;
            CollisionEnabled               Enabled = CollisionEnabled::NoCollision;
            uint8_t                        Channel = 0;
            std::vector<CollisionResponse> Responses; ///< one per channel
        };
        std::vector<std::string> m_Channels;
        std::vector<Profile>     m_Profiles;
    };
} // namespace Desert::Physics
