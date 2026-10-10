#include <Engine/Libraries/LocalizationLibrary.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Localization/LocalizationService.hpp>

namespace Desert::Libraries
{
    namespace
    {
        using Reflection::FieldType;
        using Reflection::Value;

        const Localization::LocaleRow& CurrentLanguage()
        {
            return Localization::Localization::Get().Language();
        }

        std::optional<double> NumberOf( const Value& value )
        {
            switch ( value.Type() )
            {
                case FieldType::Double:
                    return *value.Get<double>();
                case FieldType::Float:
                    return static_cast<double>( *value.Get<float>() );
                case FieldType::Int:
                    return static_cast<double>( *value.Get<std::int64_t>() );
                case FieldType::UInt:
                    return static_cast<double>( *value.Get<std::uint64_t>() );
                default:
                    return std::nullopt;
            }
        }

        /// The argument record of text/plural. A field of a kind it cannot use is ignored — a record is open, and
        /// an engine that errored on a stray field would make every call site fragile; a gender NAME it does not
        /// know is a typo in a fixed vocabulary and is logged.
        Localization::FormatArguments Arguments( const Value::Map& fields )
        {
            Localization::FormatArguments args;
            for ( std::size_t i = 0; i < fields.Size(); ++i )
            {
                const std::string&          name   = fields.Keys[i];
                const Value&                value  = fields.Values[i];
                const std::optional<double> number = NumberOf( value );
                const std::string*          text   = value.Get<std::string>();
                if ( name == "count" )
                {
                    if ( number )
                        args.Count = *number;
                }
                else if ( name == "digits" )
                {
                    if ( number )
                        args.CountFractionDigits = static_cast<int>( *number );
                }
                else if ( name == "gender" )
                {
                    if ( text == nullptr )
                        continue;
                    if ( const auto parsed = Localization::GenderFromName( *text ) )
                        args.Subject = *parsed;
                    else
                        LOG_ERROR( "[Localization] script passed gender '{}', which is not one of masculine, "
                                   "feminine, neuter, common — the string will be resolved without a gender",
                                   *text );
                }
                else if ( text != nullptr )
                    args.Named.emplace_back( name, *text );
                else if ( number )
                    // Formatted for the current locale on the way in, so a script cannot put a C-locale "1234.5"
                    // into a French sentence.
                    args.Named.emplace_back( name, Localization::FormatNumber( CurrentLanguage(), *number, 0 ) );
            }
            return args;
        }
    } // namespace

    std::string LocalizationLibrary::Text( const std::string& key, const Value::Map& args )
    {
        return Localization::Localization::Get().Format( key, Arguments( args ) ).Text;
    }

    std::string LocalizationLibrary::Plural( const std::string& key, double count, const Value::Map& args )
    {
        Localization::FormatArguments arguments = Arguments( args );
        arguments.Count                         = count;
        return Localization::Localization::Get().Format( key, arguments ).Text;
    }

    std::string LocalizationLibrary::Number( double value, int digits )
    {
        return Localization::FormatNumber( CurrentLanguage(), value, digits );
    }

    std::string LocalizationLibrary::Money( double amount, const std::string& code )
    {
        const Localization::CurrencyRow* currency = Localization::FindCurrency( code );
        if ( currency == nullptr )
        {
            // The CODE is returned beside the raw amount rather than a plausible-looking sum in the wrong
            // currency: a price shown in the wrong money is the one formatting mistake a player will act on.
            LOG_ERROR( "[Localization] currency '{}' is not one this build knows", code );
            return Localization::FormatNumber( CurrentLanguage(), amount, 2 ) + " " + code;
        }
        return Localization::FormatCurrency( CurrentLanguage(), amount, *currency );
    }

    std::string LocalizationLibrary::Date( int year, int month, int day )
    {
        const auto formatted =
             Localization::FormatDate( CurrentLanguage(), Localization::CalendarDate{ year, month, day } );
        if ( !formatted )
        {
            LOG_ERROR( "[Localization] {}", formatted.GetError() );
            return {};
        }
        return formatted.GetValue();
    }

    std::string LocalizationLibrary::Language()
    {
        return std::string( CurrentLanguage().Tag );
    }

    bool LocalizationLibrary::SetLanguage( const std::string& tag )
    {
        // false AND logged rather than raised: a language a project does not ship is a content problem, and a
        // script that asks for one should be able to fall back rather than die.
        const auto set = Localization::Localization::Get().SetLanguage( tag );
        if ( !set )
            LOG_ERROR( "[Localization] {}", set.GetError() );
        return set.IsSuccess();
    }

    std::vector<LanguageInfo> LocalizationLibrary::Languages()
    {
        std::vector<LanguageInfo> out;
        for ( const Localization::LocaleRow* row : Localization::Localization::Get().AvailableLanguages() )
            out.push_back( LanguageInfo{ std::string( row->Tag ), std::string( row->Endonym ) } );
        return out;
    }
} // namespace Desert::Libraries
