#include <Engine/Core/SceneUpgrade/Steps/V77TubeSourceLength.hpp>

#include <rflcpp/rfl/json.hpp>

#include <format>
#include <string>
#include <utility>

namespace Desert::Core
{
    std::size_t MigrateTubeSourceLengthV76ToV77( std::vector<Assets::EntityData>& entities )
    {
        std::size_t stated = 0;
        for ( auto& entity : entities )
        {
            for ( const char* block : { "PointLight", "SpotLight" } )
            {
                const auto payload = entity.Components.get( block );
                if ( !payload.has_value() )
                    continue;
                auto fields = payload.value().to_object();
                if ( !fields.has_value() || fields.value().get( "SourceLength" ).has_value() )
                    continue;
                fields.value()["SourceLength"] = rfl::Generic( 0.0 );
                entity.Components[block]       = rfl::Generic( std::move( fields.value() ) );
                ++stated;
            }
        }
        return stated;
    }

    StepOutcome RunTubeSourceLength( UpgradeContext& context )
    {
        StepOutcome outcome;
        outcome.Changed = MigrateTubeSourceLengthV76ToV77( context.Entities );
        outcome.Summary = std::format( "{} PointLight / SpotLight block(s) given SourceLength 0", outcome.Changed );
        return outcome;
    }
} // namespace Desert::Core
