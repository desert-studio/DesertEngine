#include "WorldPartitionSettings.hpp"

#include "EngineConfig.hpp"

#include <cmath>
#include <format>

namespace Desert::Project
{
    namespace
    {
        bool FiniteAtLeast( double value, double least )
        {
            return std::isfinite( value ) && value >= least;
        }

        // The rule Rules::StepResidency refuses a frame by (CheckResidencySettings), applied before the value is
        // written, so a project cannot save settings its Play would refuse.
        Common::BoolResultStr CheckWorldPartitionSettings( const WorldPartitionSettings& s )
        {
            const bool valid = FiniteAtLeast( s.ActivationBudgetMs, 0.0 ) && s.MaxConcurrentLoads > 0 &&
                               FiniteAtLeast( s.UnloadMargin, 0.0 ) && std::isfinite( s.RetryDelaySeconds ) &&
                               s.RetryDelaySeconds > 0.0 && FiniteAtLeast( s.RetryBackoff, 1.0 ) &&
                               FiniteAtLeast( s.MaxRetryDelaySeconds, s.RetryDelaySeconds ) &&
                               FiniteAtLeast( s.SlowStreamingRatio, 0.0 ) &&
                               FiniteAtLeast( s.SlowStreamingWarningFactor, 1.0 );
            if ( valid )
                return Common::MakeSuccess( true );
            return Common::MakeError<bool>( std::format(
                 "the World Partition settings were not saved: ActivationBudgetMs {} (>= 0), MaxConcurrentLoads "
                 "{} "
                 "(>= 1), UnloadMargin {} (>= 0), RetryDelaySeconds {} (> 0), RetryBackoff {} (>= 1), "
                 "MaxRetryDelaySeconds {} (>= RetryDelaySeconds), SlowStreamingRatio {} (>= 0), "
                 "SlowStreamingWarningFactor {} (>= 1)",
                 s.ActivationBudgetMs, s.MaxConcurrentLoads, s.UnloadMargin, s.RetryDelaySeconds, s.RetryBackoff,
                 s.MaxRetryDelaySeconds, s.SlowStreamingRatio, s.SlowStreamingWarningFactor ) );
        }
    } // namespace

    const WorldPartitionSettings& CurrentWorldPartitionSettings()
    {
        return CurrentEngineConfig().Effective.WorldPartition;
    }

    const WorldPartitionSettings& ProjectWorldPartitionSettings()
    {
        return CurrentEngineConfig().Project.WorldPartition;
    }

    Common::BoolResultStr SaveWorldPartitionSettings( const WorldPartitionSettings& toSave )
    {
        if ( auto valid = CheckWorldPartitionSettings( toSave ); !valid )
            return valid;
        EngineConfig project   = CurrentEngineConfig().Project;
        project.WorldPartition = toSave;
        return SaveEngineConfig( project );
    }
} // namespace Desert::Project
