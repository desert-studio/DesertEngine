#include <Engine/UI/Ecs/LocalizationUITextSource.hpp>

#include <Engine/Localization/LocaleFormat.hpp>
#include <Engine/Localization/LocalizationService.hpp>

namespace Desert::UI
{
    UIResolvedText LocalizationUITextSource::Resolve( std::string_view authored, std::optional<double> count )
    {
        Localization::FormatArguments args;
        args.Count          = count;
        auto resolved       = Localization::Localization::Get().Resolve( authored, args );
        const bool literal  = resolved.Outcome == Localization::Localization::Outcome::Literal;
        return UIResolvedText{ .Text = std::move( resolved.Text ), .Literal = literal };
    }

    std::string LocalizationUITextSource::FormatNumber( double value, int32_t fractionDigits )
    {
        return Localization::FormatNumber( Localization::Localization::Get().Language(), value, fractionDigits );
    }
} // namespace Desert::UI
