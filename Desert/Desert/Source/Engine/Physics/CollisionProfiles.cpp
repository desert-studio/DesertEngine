#include <Engine/Physics/CollisionProfiles.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <format>
#include <system_error>

namespace Desert::Physics
{
    namespace
    {
        std::optional<uint8_t> ChannelIndex( const std::vector<std::string>& channels, std::string_view name )
        {
            const auto found = std::find( channels.begin(), channels.end(), name );
            if ( found == channels.end() )
                return std::nullopt;
            return static_cast<uint8_t>( found - channels.begin() );
        }

        std::string Joined( const std::vector<std::string>& names )
        {
            std::string out;
            for ( const auto& name : names )
                out += ( out.empty() ? "" : ", " ) + name;
            return out;
        }

        bool TakesPartInPhysics( CollisionEnabled enabled )
        {
            return enabled == CollisionEnabled::PhysicsOnly || enabled == CollisionEnabled::QueryAndPhysics;
        }
    } // namespace

    Common::ResultStr<CollisionProfiles> CollisionProfiles::Build( const CollisionProfilesConfig& config )
    {
        using Result = CollisionProfiles;
        if ( config.Channels.empty() || config.Channels.size() > kMaxChannels )
            return Common::MakeFormattedError<Result>( "collision profiles: {} channels, a register has 1..{}",
                                                       config.Channels.size(), kMaxChannels );
        if ( config.Profiles.size() > kMaxProfiles )
            return Common::MakeFormattedError<Result>(
                 "collision profiles: {} profiles, a register has at most {}", config.Profiles.size(),
                 kMaxProfiles );

        CollisionProfiles              out;
        std::vector<CollisionResponse> defaults;
        for ( const CollisionChannelConfig& channel : config.Channels )
        {
            if ( channel.Name.empty() || ChannelIndex( out.m_Channels, channel.Name ).has_value() )
                return Common::MakeFormattedError<Result>( "collision channel '{}' is empty or repeated",
                                                           channel.Name );
            if ( !channel.DefaultResponse.has_value() )
                return Common::MakeFormattedError<Result>( "collision channel '{}' states no DefaultResponse",
                                                           channel.Name );
            out.m_Channels.push_back( channel.Name );
            defaults.push_back( *channel.DefaultResponse );
        }

        for ( const CollisionProfileConfig& row : config.Profiles )
        {
            const bool repeated = std::any_of( out.m_Profiles.begin(), out.m_Profiles.end(),
                                               [&]( const Profile& p ) { return p.Name == row.Name; } );
            if ( row.Name.empty() || repeated )
                return Common::MakeFormattedError<Result>( "collision profile '{}' is empty or repeated",
                                                           row.Name );
            if ( !row.Enabled.has_value() )
                return Common::MakeFormattedError<Result>( "collision profile '{}' states no Enabled", row.Name );
            const auto channel = ChannelIndex( out.m_Channels, row.ObjectChannel );
            if ( !channel )
                return Common::MakeFormattedError<Result>(
                     "collision profile '{}': ObjectChannel '{}' is no channel (channels: {})", row.Name,
                     row.ObjectChannel, Joined( out.m_Channels ) );

            Profile profile{ row.Name, *row.Enabled,           *channel,
                             defaults, row.GeneratesHitEvents, row.GeneratesOverlapEvents };
            for ( const CollisionResponseConfig& response : row.Responses )
            {
                const auto target = ChannelIndex( out.m_Channels, response.Channel );
                if ( !target || !response.Response.has_value() )
                    return Common::MakeFormattedError<Result>(
                         "collision profile '{}': response to '{}' names no channel or states no Response",
                         row.Name, response.Channel );
                profile.Responses[*target] = *response.Response;
            }
            out.m_Profiles.push_back( std::move( profile ) );
        }

        for ( const std::string_view required :
              { EngineProfiles::kBlockAll, EngineProfiles::kBlockAllDynamic, EngineProfiles::kPhysicsActor,
                EngineProfiles::kPawn, EngineProfiles::kDestructible } )
            if ( !out.Resolve( required ) )
                return Common::MakeFormattedError<Result>(
                     "collision profiles: the engine profile '{}' is missing — the engine gives it to the bodies "
                     "it creates itself",
                     required );
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::ResultStr<CollisionProfiles> CollisionProfiles::Read( const std::filesystem::path& path )
    {
        // Through the VFS as well as the disk: a packaged game reads the register out of its archive.
        if ( !Common::Utils::FileSystem::Exists( path ) )
            return Common::MakeFormattedError<CollisionProfiles>( "collision profile register '{}' does not exist",
                                                                  path.string() );
        auto read = Common::Json::ReadFile<CollisionProfilesConfig>( path );
        if ( !read )
            return Common::MakeFormattedError<CollisionProfiles>( "{}", read.GetError() );
        auto built = Build( read.GetValue() );
        if ( !built )
            return Common::MakeFormattedError<CollisionProfiles>( "'{}': {}", path.string(), built.GetError() );
        return built;
    }

    Common::ResultStr<CollisionProfileId> CollisionProfiles::Resolve( std::string_view name ) const
    {
        for ( std::size_t i = 0; i < m_Profiles.size(); ++i )
            if ( m_Profiles[i].Name == name )
                return Common::MakeSuccess( static_cast<CollisionProfileId>( i ) );
        std::vector<std::string> names;
        names.reserve( m_Profiles.size() );
        for ( const Profile& profile : m_Profiles )
            names.push_back( profile.Name );
        return Common::MakeFormattedError<CollisionProfileId>( "no collision profile '{}' (profiles: {})", name,
                                                               Joined( names ) );
    }

    CollisionResponse CollisionProfiles::PhysicsResponse( CollisionProfileId a, CollisionProfileId b ) const
    {
        const Profile& pa = m_Profiles[a];
        const Profile& pb = m_Profiles[b];
        if ( !TakesPartInPhysics( pa.Enabled ) || !TakesPartInPhysics( pb.Enabled ) )
            return CollisionResponse::Ignore;
        return std::min( pa.Responses[pb.Channel], pb.Responses[pa.Channel] );
    }

    bool CollisionProfiles::IsQueryable( CollisionProfileId id ) const
    {
        const CollisionEnabled enabled = m_Profiles[id].Enabled;
        return enabled == CollisionEnabled::QueryOnly || enabled == CollisionEnabled::QueryAndPhysics;
    }
} // namespace Desert::Physics
