// BeginWorldStreaming out of line: its signature names SceneSerialized by forward declaration so the
// executor header (reached by EditorLayer.hpp through WorldStreamer.hpp) does not include SceneFormat.hpp.
#include <Engine/Core/Serialize/WorldPartitionResidencyExecutor.hpp>

#include <Engine/Core/Serialize/SceneFormat.hpp>

namespace Desert::Core::Rules
{
    Common::ResultStr<std::optional<ResidencyExecutor>>
    BeginWorldStreaming( const SceneSerialized& scene, const AssetBoundsSource& bounds,
                         const ResidencySettings& settings, std::span<const StreamingSource> sources,
                         ResidencyWorld& world, const ScriptReferenceSource& scripts )
    {
        if ( !scene.WorldPartition.has_value() )
            return Common::MakeSuccess( std::optional<ResidencyExecutor>() );
        WorldPartitionPlan plan = PlanWorldPartition( scene.Entities, *scene.WorldPartition, bounds, scripts );
        // Play names what the planner refused exactly as the cook does (ReportPlanIssues).
        ReportPlanIssues( plan, scene.SceneName );
        auto begun = ResidencyExecutor::Begin( std::move( plan ), *scene.WorldPartition, scene.Entities, settings,
                                               sources, world );
        if ( !begun.IsSuccess() )
            return Common::MakeError<std::optional<ResidencyExecutor>>( "'" + scene.SceneName +
                                                                        "': " + begun.GetError() );
        return Common::MakeSuccess( std::optional<ResidencyExecutor>( begun.ExtractValue() ) );
    }
} // namespace Desert::Core::Rules
