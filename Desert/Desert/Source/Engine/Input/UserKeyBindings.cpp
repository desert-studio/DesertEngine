#include <Engine/Input/UserKeyBindings.hpp>
#include <Engine/Input/InputKey.hpp>

#include <Common/Json/Json.hpp>
#include <Common/Settings/MachineSettings.hpp>

namespace Desert::Input
{
    std::filesystem::path UserKeyBindingsFile()
    {
        const std::filesystem::path& machine = Common::Settings::MachineSettings::File();
        return machine.empty() ? std::filesystem::path{} : machine.parent_path() / "input.json";
    }

    Common::ResultStr<UserKeyBindings> LoadUserKeyBindings( const std::filesystem::path& file )
    {
        std::error_code ec;
        if ( !std::filesystem::exists( file, ec ) )
            return Common::MakeSuccess( UserKeyBindings{} );
        auto read = Common::Json::ReadFile<UserKeyBindings>( file );
        if ( !read )
            return Common::MakeFormattedError<UserKeyBindings>( "key bindings '{}': {}", file.string(),
                                                                read.GetError() );
        UserKeyBindings bindings = read.GetValue();
        for ( const UserKeyOverride& o : bindings.Overrides )
            if ( !InputKeyFromName( o.Key ) || !InputKeyFromName( o.DefaultKey ) )
                return Common::MakeFormattedError<UserKeyBindings>(
                     "key bindings '{}': '{}' -> '{}' names a key this engine does not have", file.string(),
                     o.DefaultKey, o.Key );
        return Common::MakeSuccess( std::move( bindings ) );
    }

    Common::BoolResultStr SaveUserKeyBindings( const std::filesystem::path& file, const UserKeyBindings& bindings )
    {
        std::error_code ec;
        std::filesystem::create_directories( file.parent_path(), ec );
        return Common::Json::WriteFileAtomic( file, bindings );
    }
} // namespace Desert::Input
