#pragma once

// The collision register the physics suites initialise a PhysicsWorld with (PHYS-A1): ONE spelling of "a
// register with the engine profiles", built in code through CollisionProfiles::Build — the suites test bodies,
// not the project's Config file, and the CollisionProfiles suite reads that file on its own. Every channel
// blocks by default, so BlockAll / PhysicsActor / Pawn / Destructible bodies meet exactly as they did before
// profiles existed. Besides the engine profiles it holds the ones the CollisionProfiles suite needs: NoCollision,
// QueryOnly, PhysicsOnly, IgnoreAll (simulated, ignores every channel) and Trigger (overlaps every channel).
// Included by relative path; header-only.

#include <Engine/Physics/CollisionProfiles.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace Desert::TestSupport
{
    inline Physics::CollisionProfilesConfig PhysicsTestProfilesConfig()
    {
        using Physics::CollisionEnabled;
        using Physics::CollisionResponse;
        Physics::CollisionProfilesConfig config;
        const std::vector<std::string>   channels = { "WorldStatic", "WorldDynamic", "Pawn", "PhysicsBody",
                                                      "Destructible" };
        for ( const std::string& channel : channels )
            config.Channels.push_back( { channel, CollisionResponse::Block } );

        const auto every = [&]( CollisionResponse response )
        {
            std::vector<Physics::CollisionResponseConfig> responses;
            for ( const std::string& channel : channels )
                responses.push_back( { channel, response } );
            return responses;
        };
        config.Profiles = {
             { "BlockAll", CollisionEnabled::QueryAndPhysics, "WorldStatic", {} },
             { "BlockAllDynamic", CollisionEnabled::QueryAndPhysics, "WorldDynamic", {} },
             { "PhysicsActor", CollisionEnabled::QueryAndPhysics, "PhysicsBody", {} },
             { "Pawn", CollisionEnabled::QueryAndPhysics, "Pawn", {} },
             { "Destructible", CollisionEnabled::QueryAndPhysics, "Destructible", {} },
             { "NoCollision", CollisionEnabled::NoCollision, "WorldStatic", every( CollisionResponse::Ignore ) },
             { "QueryOnly", CollisionEnabled::QueryOnly, "WorldStatic", {} },
             { "PhysicsOnly", CollisionEnabled::PhysicsOnly, "WorldStatic", {} },
             { "IgnoreAll", CollisionEnabled::QueryAndPhysics, "WorldDynamic",
               every( CollisionResponse::Ignore ) },
             { "Trigger", CollisionEnabled::QueryAndPhysics, "WorldDynamic", every( CollisionResponse::Overlap ) },
        };
        return config;
    }

    /// The register above, built. A refusal fails the calling test with its message.
    inline Physics::CollisionProfiles PhysicsTestProfiles()
    {
        auto built = Physics::CollisionProfiles::Build( PhysicsTestProfilesConfig() );
        if ( !built )
        {
            ADD_FAILURE() << "the physics test register is refused: " << built.GetError();
            return {};
        }
        return built.ExtractValue();
    }

    /// @p name in @p world's register; kNoProfile (which every Create refuses) after an ADD_FAILURE.
    inline Physics::CollisionProfileId ProfileId( const Physics::PhysicsWorld& world, std::string_view name )
    {
        auto id = world.GetCollisionProfiles().Resolve( name );
        if ( !id )
        {
            ADD_FAILURE() << id.GetError();
            return Physics::kNoProfile;
        }
        return id.GetValue();
    }
} // namespace Desert::TestSupport
