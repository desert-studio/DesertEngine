#include <Engine/Core/SceneUpgrade/Steps/Light1Rotation.hpp>

#include <Engine/Geometry/LightTravel.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>
#include <unordered_map>

#include <format>
#include <string>
#include <utility>

namespace Desert::Core
{
    SunRotationReport MigrateLightRotationV0ToV1( std::vector<Assets::EntityData>& entities )
    {
        // The old engine's gate (Rules::kSunDirectionEpsilon before light v1): below it a Translation was no
        // direction and the light emitted nothing.
        constexpr float kOldDirectionEpsilon = 1e-4f;

        std::unordered_map<Common::UUID, const Assets::EntityData*> byId;
        for ( const auto& entity : entities )
            if ( entity.id.has_value() )
                byId.emplace( *entity.id, &entity );

        // The world rotation of a record as the file states it: its own Rotation under its parents'. Bounded
        // by the record count, so a parent cycle in a damaged file ends instead of spinning.
        const auto worldRotation = [&]( std::optional<Common::UUID> id )
        {
            glm::quat world = glm::quat( 1.0f, glm::vec3( 0.0f ) );
            for ( std::size_t depth = 0; id.has_value() && depth <= entities.size(); ++depth )
            {
                const auto found = byId.find( *id );
                if ( found == byId.end() )
                    break;
                world = glm::quat( found->second->Rotation.value_or( glm::vec3( 0.0f ) ) ) * world;
                id    = found->second->parent;
            }
            return world;
        };

        const auto isLight = []( Common::Json::KeyedValues& components )
        { return components.get( "DirectionLight" ).has_value(); };

        SunRotationReport report;
        for ( auto& entity : entities )
        {
            if ( isLight( entity.Components ) )
            {
                const glm::vec3 travel = entity.Translation.value_or( glm::vec3( 0.0f ) );
                if ( glm::length( travel ) > kOldDirectionEpsilon )
                {
                    const glm::quat parent = worldRotation( entity.parent );
                    entity.Rotation =
                         glm::eulerAngles( glm::inverse( parent ) * Geometry::RotationForTravel( travel ) );
                    entity.Translation = glm::vec3( 0.0f );
                    ++report.Aimed;
                }
                else
                {
                    ++report.Undirected;
                }
            }

            if ( !entity.PrefabOverrides )
                continue;
            for ( auto& override_ : *entity.PrefabOverrides )
            {
                if ( !isLight( override_.Components ) || !override_.Translation.has_value() ||
                     glm::length( *override_.Translation ) <= kOldDirectionEpsilon )
                    continue;
                override_.Rotation = glm::eulerAngles( Geometry::RotationForTravel( *override_.Translation ) );
                override_.Translation = glm::vec3( 0.0f );
                ++report.Overrides;
            }
        }
        return report;
    }

    StepOutcome RunLightRotation( UpgradeContext& context )
    {
        const SunRotationReport report = MigrateLightRotationV0ToV1( context.Entities );
        StepOutcome             outcome;
        outcome.Changed = report.Aimed + report.Overrides;
        outcome.Summary = std::format( "{} directional light(s) aimed by Rotation, {} override(s) with an identity "
                                       "parent, {} without a direction kept their Rotation",
                                       report.Aimed, report.Overrides, report.Undirected );
        return outcome;
    }
} // namespace Desert::Core
