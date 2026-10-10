#pragma once

#include <UI/UITextSource.hpp>

namespace Desert::UI
{
    // The engine's text source: the process-wide Localization service (Localization/LocalizationService.hpp)
    // in the current language. Stateless -- every answer is the service's at the moment of asking.
    class LocalizationUITextSource final : public IUITextSource
    {
    public:
        [[nodiscard]] UIResolvedText Resolve( std::string_view authored, std::optional<double> count ) override;
        [[nodiscard]] std::string    FormatNumber( double value, int32_t fractionDigits ) override;
    };
} // namespace Desert::UI
