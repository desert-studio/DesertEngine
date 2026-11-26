#include <Engine/Graphic/Rhi/RhiSelection.hpp>

#include <charconv>
#include <format>
#include <string_view>

namespace Desert::Graphic
{
    namespace
    {
        std::string_view SourceName( const RhiSource source )
        {
            switch ( source )
            {
                case RhiSource::CommandLine:
                    return "the command line";
                case RhiSource::MachineSettings:
                    return "machine.json";
                case RhiSource::Default:
                    return "the default";
            }
            return "an unknown source";
        }

        std::optional<uint32_t> ParseUnsigned( const std::string_view text, const int base )
        {
            uint32_t value = 0;
            if ( text.empty() )
                return std::nullopt;
            const auto [end, error] = std::from_chars( text.data(), text.data() + text.size(), value, base );
            if ( error != std::errc() || end != text.data() + text.size() )
                return std::nullopt;
            return value;
        }
    } // namespace

    std::string Describe( const AdapterPreference& adapter )
    {
        switch ( adapter.By )
        {
            case AdapterPreference::Kind::Auto:
                return "auto";
            case AdapterPreference::Kind::Index:
                return std::format( "#{}", adapter.Index );
            case AdapterPreference::Kind::Vendor:
                return std::format( "vendor 0x{:04X}", adapter.VendorId );
            case AdapterPreference::Kind::Name:
                return std::format( "name '{}'", adapter.NameSubstring );
        }
        return "unknown";
    }

    Common::ResultStr<AdapterPreference> ParseAdapterPreference( const std::string_view text )
    {
        AdapterPreference preference;
        if ( text == "auto" )
            return Common::MakeSuccess( preference );
        if ( text == "nvidia" || text == "amd" || text == "intel" )
        {
            preference.By       = AdapterPreference::Kind::Vendor;
            preference.VendorId = text == "nvidia" ? 0x10DEu : text == "amd" ? 0x1002u : 0x8086u;
            return Common::MakeSuccess( preference );
        }
        constexpr std::string_view kVendor = "vendor:0x";
        if ( text.starts_with( kVendor ) )
        {
            const auto vendor = ParseUnsigned( text.substr( kVendor.size() ), 16 );
            if ( !vendor || *vendor == 0 )
                return Common::MakeFormattedError<AdapterPreference>(
                     "adapter '{}': 'vendor:0x' takes a non-zero hexadecimal PCI vendor id", text );
            preference.By       = AdapterPreference::Kind::Vendor;
            preference.VendorId = *vendor;
            return Common::MakeSuccess( preference );
        }
        constexpr std::string_view kName = "name:";
        if ( text.starts_with( kName ) )
        {
            if ( text.size() == kName.size() )
                return Common::MakeFormattedError<AdapterPreference>( "adapter '{}': 'name:' takes a text", text );
            preference.By            = AdapterPreference::Kind::Name;
            preference.NameSubstring = std::string( text.substr( kName.size() ) );
            return Common::MakeSuccess( preference );
        }
        if ( const auto index = ParseUnsigned( text, 10 ) )
        {
            preference.By    = AdapterPreference::Kind::Index;
            preference.Index = *index;
            return Common::MakeSuccess( preference );
        }
        return Common::MakeFormattedError<AdapterPreference>(
             "adapter '{}' is not an adapter preference; known: auto, <index>, nvidia, amd, intel, "
             "vendor:0x<hex>, name:<text>",
             text );
    }

    Common::ResultStr<RhiSelection> ResolveRhiSelection( const RhiSelectionInputs& in )
    {
        RhiSelection selection;

        // The backend: the first preference stated, in precedence order; a stated name that is not a backend is
        // refused where it was stated, never skipped for the next one down.
        const std::optional<std::string>* kindText = nullptr;
        if ( in.CommandLineRhi )
        {
            kindText           = &in.CommandLineRhi;
            selection.KindFrom = RhiSource::CommandLine;
        }
        else if ( in.MachineRhi )
        {
            kindText           = &in.MachineRhi;
            selection.KindFrom = RhiSource::MachineSettings;
        }
        if ( kindText != nullptr )
        {
            const auto kind = ParseRhiKind( **kindText );
            if ( !kind )
                return Common::MakeFormattedError<RhiSelection>(
                     "rendering backend '{}' from {} is not a backend; known: vulkan, d3d12", **kindText,
                     SourceName( selection.KindFrom ) );
            selection.Kind = *kind;
        }
#if !defined( DESERT_PLATFORM_WINDOWS )
        if ( selection.Kind == RhiKind::D3D12 )
            return Common::MakeFormattedError<RhiSelection>(
                 "rendering backend d3d12 from {} exists only on Windows", SourceName( selection.KindFrom ) );
#endif

        const std::optional<std::string>* adapterText = nullptr;
        if ( in.CommandLineAdapter )
        {
            adapterText           = &in.CommandLineAdapter;
            selection.AdapterFrom = RhiSource::CommandLine;
        }
        else if ( in.MachineAdapter )
        {
            adapterText           = &in.MachineAdapter;
            selection.AdapterFrom = RhiSource::MachineSettings;
        }
        if ( adapterText != nullptr )
        {
            auto adapter = ParseAdapterPreference( **adapterText );
            if ( !adapter )
                return Common::MakeFormattedError<RhiSelection>( "{} (from {})", adapter.GetError(),
                                                                 SourceName( selection.AdapterFrom ) );
            selection.Adapter = adapter.ExtractValue();
        }

        if ( in.CommandLineDeviceProfile )
        {
            auto profile = FindDeviceProfile( *in.CommandLineDeviceProfile );
            if ( !profile )
                return Common::MakeError<RhiSelection>( profile.GetError() );
            selection.Profile = profile.ExtractValue();
        }
        return Common::MakeSuccess( std::move( selection ) );
    }

    std::string Describe( const RhiSelection& selection )
    {
        return std::format( "{} from {}, adapter {} from {}, device profile {}", ToString( selection.Kind ),
                            SourceName( selection.KindFrom ), Describe( selection.Adapter ),
                            SourceName( selection.AdapterFrom ),
                            selection.Profile.Name.empty() ? std::string( "none" ) : selection.Profile.Name );
    }
} // namespace Desert::Graphic
