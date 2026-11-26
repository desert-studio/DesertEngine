#include <Editor/LiveCoding/LiveCodingBuildIndex.hpp>

#include <Common/Json/Document.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined( _WIN32 )
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace Desert::Editor::LiveCoding
{
    namespace fs = std::filesystem;

    namespace
    {
        // ONE SPELLING PER FILE: absolute, lexically normal, forward slashes; compared without case on Windows,
        // whose file system does not keep it (compile_commands.json and a unity file may spell a drive
        // differently).
        fs::path Normal( const fs::path& path )
        {
            return fs::path( path.lexically_normal().generic_string() );
        }

        std::string KeyOf( const fs::path& path )
        {
            std::string key = path.lexically_normal().generic_string();
#if defined( _WIN32 )
            std::transform( key.begin(), key.end(), key.begin(),
                            []( const unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
#endif
            return key;
        }

        // A path a compile command names, relative to its "directory" when not absolute.
        fs::path Resolve( const fs::path& directory, const fs::path& path )
        {
            return Normal( path.is_absolute() ? path : directory / path );
        }

        std::uint32_t CurrentProcessId()
        {
#if defined( _WIN32 )
            return static_cast<std::uint32_t>( ::GetCurrentProcessId() );
#else
            return static_cast<std::uint32_t>( ::getpid() );
#endif
        }

        bool ProcessRuns( const std::uint32_t pid )
        {
#if defined( _WIN32 )
            const HANDLE process = ::OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid );
            if ( process == nullptr )
                return ::GetLastError() == ERROR_ACCESS_DENIED; // it runs, as another user
            DWORD      exitCode = 0;
            const BOOL read     = ::GetExitCodeProcess( process, &exitCode );
            ::CloseHandle( process );
            return read != FALSE && exitCode == STILL_ACTIVE;
#else
            return ::kill( static_cast<pid_t>( pid ), 0 ) == 0 || errno == EPERM;
#endif
        }

        Common::ResultStr<std::string> RequiredString( const Common::Json::Node& node, const std::string_view key )
        {
            Common::ResultStr<Common::Json::Node> member = node.Get( key );
            if ( !member )
                return Common::MakeError<std::string>( member.GetError() );
            return member.GetValue().AsString();
        }

        Common::ResultStr<Common::Json::Value> ReadJson( const fs::path& file, const std::string_view what )
        {
            std::error_code ec;
            if ( !fs::exists( file, ec ) )
                return Common::MakeFormattedError<Common::Json::Value>( "{} {} is missing", what, file.string() );
            Common::ResultStr<std::string> text = Common::Utils::FileSystem::ReadFileContent( file );
            if ( !text )
                return Common::MakeFormattedError<Common::Json::Value>( "{} {}: {}", what, file.string(),
                                                                        text.GetError() );
            Common::ResultStr<Common::Json::Value> parsed = Common::Json::Parse( text.GetValue() );
            if ( !parsed )
                return Common::MakeFormattedError<Common::Json::Value>( "{} {}: {}", what, file.string(),
                                                                        parsed.GetError() );
            return parsed;
        }

        // The CMake target whose object @p output is: ".../CMakeFiles/<Target>.dir/...". Empty when the output
        // lies in no target directory.
        std::string TargetOfOutput( const std::string& output )
        {
            const std::string generic = fs::path( output ).generic_string();
            const std::size_t files   = generic.find( "CMakeFiles/" );
            if ( files == std::string::npos )
                return {};
            const std::size_t begin = files + std::string_view( "CMakeFiles/" ).size();
            const std::size_t end   = generic.find( '/', begin );
            if ( end == std::string::npos )
                return {};
            const std::string_view directory( generic.data() + begin, end - begin );
            if ( !directory.ends_with( ".dir" ) )
                return {};
            return std::string( directory.substr( 0, directory.size() - std::string_view( ".dir" ).size() ) );
        }

        // One token of a Windows command line: [Begin, End) in the command, and its text without quotes.
        struct Token
        {
            std::size_t Begin = 0;
            std::size_t End   = 0;
            std::string Text;
        };

        std::vector<Token> Tokenise( const std::string& command )
        {
            std::vector<Token> tokens;
            std::size_t        i = 0;
            while ( i < command.size() )
            {
                while ( i < command.size() && ( command[i] == ' ' || command[i] == '\t' ) )
                    ++i;
                if ( i == command.size() )
                    break;
                Token token;
                token.Begin = i;
                bool quoted = false;
                while ( i < command.size() && ( quoted || ( command[i] != ' ' && command[i] != '\t' ) ) )
                {
                    if ( command[i] == '"' )
                        quoted = !quoted;
                    else
                        token.Text += command[i];
                    ++i;
                }
                token.End = i;
                tokens.push_back( std::move( token ) );
            }
            return tokens;
        }

        // Where a compile command names its translation unit and its object: the token that IS @p unit (any
        // spelling), else the one after -c; and the /Fo token.
        struct CommandSlots
        {
            Token Unit;
            Token Object;
        };

        std::optional<CommandSlots> LocateSlots( const std::string& command, const fs::path& directory,
                                                 const fs::path& unit )
        {
            const std::vector<Token> tokens  = Tokenise( command );
            const std::string        unitKey = KeyOf( unit );
            std::optional<Token>     unitToken;
            std::optional<Token>     objectToken;
            for ( std::size_t i = 0; i < tokens.size(); ++i )
            {
                const std::string& text = tokens[i].Text;
                if ( text.starts_with( "/Fo" ) || text.starts_with( "-Fo" ) )
                    objectToken = tokens[i];
                else if ( !unitToken && ( text == "-c" || text == "/c" ) && i + 1 < tokens.size() )
                    unitToken = tokens[i + 1];
                else if ( !text.empty() && text[0] != '/' && text[0] != '-' &&
                          KeyOf( Resolve( directory, fs::path( text ) ) ) == unitKey )
                    unitToken = tokens[i];
            }
            if ( !unitToken || !objectToken )
                return std::nullopt;
            return CommandSlots{ *unitToken, *objectToken };
        }

        // CMake's unity file: one `#include "<absolute path>"` line per member, in the batch's order.
        Common::ResultStr<std::vector<fs::path>> UnityMembers( const fs::path& unity )
        {
            Common::ResultStr<std::string> text = Common::Utils::FileSystem::ReadFileContent( unity );
            if ( !text )
                return Common::MakeFormattedError<std::vector<fs::path>>( "unity file {}: {}", unity.string(),
                                                                          text.GetError() );
            std::vector<fs::path>  members;
            const std::string_view all = text.GetValue();
            std::size_t            at  = 0;
            while ( at < all.size() )
            {
                std::size_t end = all.find( '\n', at );
                if ( end == std::string_view::npos )
                    end = all.size();
                std::string_view line               = all.substr( at, end - at );
                at                                  = end + 1;
                constexpr std::string_view kInclude = "#include \"";
                if ( !line.starts_with( kInclude ) )
                    continue;
                line.remove_prefix( kInclude.size() );
                const std::size_t close = line.find( '"' );
                if ( close == std::string_view::npos )
                    continue;
                members.push_back( Resolve( unity.parent_path(), fs::path( line.substr( 0, close ) ) ) );
            }
            if ( members.empty() )
                return Common::MakeFormattedError<std::vector<fs::path>>( "unity file {} includes no source",
                                                                          unity.string() );
            return Common::MakeSuccess( std::move( members ) );
        }

        bool IsUnityFile( const fs::path& file )
        {
            return file.parent_path().filename() == "Unity" && file.filename().string().starts_with( "unity_" );
        }

        bool IsHeader( const fs::path& file )
        {
            const std::string extension = file.extension().string();
            return extension == ".hpp" || extension == ".h" || extension == ".inl";
        }

        // <build>/LivePatch/<pid> of an editor that no longer runs: its objects describe a process that is gone.
        void RemoveDeadPatchRoots( const fs::path& livePatch, const std::uint32_t self )
        {
            std::error_code ec;
            for ( auto it = fs::directory_iterator( livePatch, ec ); !ec && it != fs::directory_iterator();
                  it.increment( ec ) )
            {
                const std::string name = it->path().filename().string();
                std::uint32_t     pid  = 0;
                const auto [end, parsed] = std::from_chars( name.data(), name.data() + name.size(), pid );
                if ( parsed != std::errc() || end != name.data() + name.size() )
                    continue;
                if ( pid != self && !ProcessRuns( pid ) )
                {
                    std::error_code removed;
                    fs::remove_all( it->path(), removed );
                }
            }
        }
    } // namespace

    Common::ResultStr<BuildIndex> BuildIndex::Load( const fs::path& manifest )
    {
        Common::ResultStr<Common::Json::Value> manifestJson = ReadJson( manifest, "the Live Coding manifest" );
        if ( !manifestJson )
            return Common::MakeError<BuildIndex>( manifestJson.GetError() );
        const Common::Json::Node root = Common::Json::Root( manifestJson.GetValue() );

        Common::ResultStr<std::string> buildDir        = RequiredString( root, "BuildDir" );
        Common::ResultStr<std::string> compileCommands = RequiredString( root, "CompileCommands" );
        if ( !buildDir || !compileCommands )
            return Common::MakeFormattedError<BuildIndex>(
                 "{}: {}", manifest.string(), !buildDir ? buildDir.GetError() : compileCommands.GetError() );

        BuildIndex  index;
        std::string refusal;
        if ( const std::optional<Common::Json::Node> roots = root.Find( "SourceRoots" ) )
            roots->ForEachElement(
                 [&]( std::size_t, const Common::Json::Node& element )
                 {
                     Common::ResultStr<std::string> path = element.AsString();
                     if ( path )
                         index.m_SourceRoots.push_back( Normal( fs::path( path.GetValue() ) ) );
                     else if ( refusal.empty() )
                         refusal = path.GetError();
                 } );
        if ( const std::optional<Common::Json::Node> images = root.Find( "Images" ) )
            images->ForEachElement(
                 [&]( std::size_t, const Common::Json::Node& element )
                 {
                     Common::ResultStr<std::string> target = RequiredString( element, "Target" );
                     Common::ResultStr<std::string> image  = RequiredString( element, "Image" );
                     Common::ResultStr<std::string> map    = RequiredString( element, "Map" );
                     if ( !target || !image || !map )
                     {
                         if ( refusal.empty() )
                             refusal = !target ? target.GetError() : !image ? image.GetError() : map.GetError();
                         return;
                     }
                     index.m_Images.push_back( ImageRecord{ .Target = target.GetValue(),
                                                            .Image  = Normal( fs::path( image.GetValue() ) ),
                                                            .Map    = Normal( fs::path( map.GetValue() ) ) } );
                 } );
        if ( !refusal.empty() )
            return Common::MakeFormattedError<BuildIndex>( "{}: {}", manifest.string(), refusal );
        if ( index.m_Images.empty() || index.m_SourceRoots.empty() )
            return Common::MakeFormattedError<BuildIndex>( "{} names no image or no source root",
                                                           manifest.string() );

        std::unordered_map<std::string, std::size_t> imageOfTarget;
        for ( std::size_t i = 0; i < index.m_Images.size(); ++i )
        {
            const ImageRecord& image = index.m_Images[i];
            std::error_code ec;
            if ( !fs::exists( image.Image, ec ) )
                return Common::MakeFormattedError<BuildIndex>( "{} is missing: the build did not link it",
                                                               image.Image.string() );
            if ( !fs::exists( image.Map, ec ) )
                return Common::MakeFormattedError<BuildIndex>( "{} is missing: {} was linked without /MAP",
                                                               image.Map.string(), image.Image.string() );
            imageOfTarget.emplace( image.Target, i );
        }

        Common::ResultStr<Common::Json::Value> commandsJson =
             ReadJson( fs::path( compileCommands.GetValue() ), "the compilation database" );
        if ( !commandsJson )
            return Common::MakeError<BuildIndex>( commandsJson.GetError() );

        // A TARGET'S OBJECTS ARE ITS OWN AND ITS SPLIT PARTS': _desert_split_objects names an OBJECT library
        // <owner>-<suffix> (the PCH holder <donor>-Pch too) whose objects the owner links. Anything else - a test,
        // a tool, Runtime - is not in the editor process and is left out.
        const auto imageOf = [&]( std::string target ) -> std::optional<std::size_t>
        {
            while ( !target.empty() )
            {
                if ( const auto found = imageOfTarget.find( target ); found != imageOfTarget.end() )
                    return found->second;
                const std::size_t dash = target.rfind( '-' );
                if ( dash == std::string::npos )
                    break;
                target.resize( dash );
            }
            return std::nullopt;
        };

        Common::Json::Root( commandsJson.GetValue() )
             .ForEachElement(
                  [&]( std::size_t, const Common::Json::Node& entry )
                  {
                      if ( !refusal.empty() )
                          return;
                      Common::ResultStr<std::string> directory = RequiredString( entry, "directory" );
                      Common::ResultStr<std::string> command   = RequiredString( entry, "command" );
                      Common::ResultStr<std::string> file      = RequiredString( entry, "file" );
                      Common::ResultStr<std::string> output    = RequiredString( entry, "output" );
                      if ( !directory || !command || !file || !output )
                      {
                          refusal = std::format( "{}: {}", compileCommands.GetValue(),
                                                 !directory ? directory.GetError()
                                                 : !command ? command.GetError()
                                                 : !file    ? file.GetError()
                                                            : output.GetError() );
                          return;
                      }
                      const std::optional<std::size_t> image = imageOf( TargetOfOutput( output.GetValue() ) );
                      if ( !image )
                          return;
                      const fs::path dir    = Normal( fs::path( directory.GetValue() ) );
                      const fs::path unit   = Resolve( dir, fs::path( file.GetValue() ) );
                      const fs::path object = Resolve( dir, fs::path( output.GetValue() ) );
                      if ( !LocateSlots( command.GetValue(), dir, unit ) )
                      {
                          refusal = std::format( "{}: the command of {} names no source or no /Fo to replace",
                                                 compileCommands.GetValue(), unit.string() );
                          return;
                      }
                      if ( !IsUnityFile( unit ) )
                      {
                          index.m_Sources.push_back( SourceRecord{ .Source      = unit,
                                                                   .BuiltObject = object,
                                                                   .UnitySource = {},
                                                                   .Command     = command.GetValue(),
                                                                   .Directory   = dir,
                                                                   .Image       = *image } );
                          return;
                      }
                      Common::ResultStr<std::vector<fs::path>> members = UnityMembers( unit );
                      if ( !members )
                      {
                          refusal = members.GetError();
                          return;
                      }
                      for ( const fs::path& member : members.GetValue() )
                          index.m_Sources.push_back( SourceRecord{ .Source      = member,
                                                                   .BuiltObject = object,
                                                                   .UnitySource = unit,
                                                                   .Command     = command.GetValue(),
                                                                   .Directory   = dir,
                                                                   .Image       = *image } );
                  } );
        if ( !refusal.empty() )
            return Common::MakeError<BuildIndex>( refusal );

        // THE MANIFEST AND THE DATABASE AGREE OR THE INDEX IS REFUSED: an image no compile command builds means
        // the two were written by different configurations.
        std::vector<bool> compiled( index.m_Images.size(), false );
        for ( const SourceRecord& source : index.m_Sources )
            compiled[source.Image] = true;
        for ( std::size_t i = 0; i < index.m_Images.size(); ++i )
            if ( !compiled[i] )
                return Common::MakeFormattedError<BuildIndex>(
                     "no entry of {} compiles into {} ({}): the manifest and the compilation database disagree",
                     compileCommands.GetValue(), index.m_Images[i].Target, index.m_Images[i].Image.string() );

        index.m_PatchedAt.assign( index.m_Sources.size(), fs::file_time_type::min() );

        const std::uint32_t self      = CurrentProcessId();
        const fs::path      livePatch = Normal( fs::path( buildDir.GetValue() ) ) / "LivePatch";
        RemoveDeadPatchRoots( livePatch, self );
        index.m_PatchRoot = livePatch / std::to_string( self );
        std::error_code created;
        fs::create_directories( index.m_PatchRoot, created );
        if ( created )
            return Common::MakeFormattedError<BuildIndex>( "cannot create {}: {}", index.m_PatchRoot.string(),
                                                           created.message() );
        return Common::MakeSuccess( std::move( index ) );
    }

    std::optional<std::size_t> BuildIndex::FindSource( const fs::path& source ) const
    {
        const std::string key = KeyOf( source );
        for ( std::size_t i = 0; i < m_Sources.size(); ++i )
            if ( KeyOf( m_Sources[i].Source ) == key )
                return i;
        return std::nullopt;
    }

    Common::ResultStr<std::vector<std::size_t>> BuildIndex::ChangedSources() const
    {
        using Result = std::vector<std::size_t>;
        std::vector<fs::file_time_type> imageAt( m_Images.size() );
        fs::file_time_type              newestImage = fs::file_time_type::min();
        for ( std::size_t i = 0; i < m_Images.size(); ++i )
        {
            std::error_code ec;
            imageAt[i] = fs::last_write_time( m_Images[i].Image, ec );
            if ( ec )
                return Common::MakeFormattedError<Result>( "cannot read the time of {}: {}",
                                                           m_Images[i].Image.string(), ec.message() );
            newestImage = std::max( newestImage, imageAt[i] );
        }

        // A HEADER NEWER THAN EVERY IMAGE IS IN NONE OF THEM. Against the newest image, not the oldest: a module
        // DLL that includes no edited header is not relinked by the next build, so the oldest image would refuse
        // every patch after any header edit until that DLL happened to relink.
        for ( const fs::path& root : m_SourceRoots )
        {
            std::error_code ec;
            for ( auto it =
                       fs::recursive_directory_iterator( root, fs::directory_options::skip_permission_denied, ec );
                  !ec && it != fs::recursive_directory_iterator(); it.increment( ec ) )
            {
                std::error_code kindError;
                if ( !it->is_regular_file( kindError ) || !IsHeader( it->path() ) )
                    continue;
                std::error_code          timeError;
                const fs::file_time_type at = fs::last_write_time( it->path(), timeError );
                if ( !timeError && at > newestImage )
                    return Common::MakeFormattedError<Result>( "header {} changed; a header edit needs a rebuild",
                                                               Normal( it->path() ).string() );
            }
            if ( ec )
                return Common::MakeFormattedError<Result>( "cannot scan {}: {}", root.string(), ec.message() );
        }

        std::unordered_map<std::string, fs::file_time_type> objectAt;
        Result                                              changed;
        for ( std::size_t s = 0; s < m_Sources.size(); ++s )
        {
            const SourceRecord& source = m_Sources[s];
            const std::string   objKey = source.BuiltObject.generic_string();
            auto                object = objectAt.find( objKey );
            if ( object == objectAt.end() )
            {
                std::error_code          ec;
                const fs::file_time_type at = fs::last_write_time( source.BuiltObject, ec );
                if ( ec )
                    return Common::MakeFormattedError<Result>(
                         "{} is gone: the build tree no longer describes the running images",
                         source.BuiltObject.string() );
                object = objectAt.emplace( objKey, at ).first;
            }
            if ( object->second > imageAt[source.Image] )
                return Common::MakeFormattedError<Result>(
                     "{} is newer than {}: a build ran while the editor held it", source.BuiltObject.string(),
                     m_Images[source.Image].Image.string() );
            std::error_code          ec;
            const fs::file_time_type at = fs::last_write_time( source.Source, ec );
            if ( ec )
                return Common::MakeFormattedError<Result>( "source {} is gone", source.Source.string() );
            if ( at > std::max( imageAt[source.Image], m_PatchedAt[s] ) )
                changed.push_back( s );
        }
        return Common::MakeSuccess( std::move( changed ) );
    }

    std::vector<std::size_t> BuildIndex::BatchOf( const fs::path& unitySource ) const
    {
        const std::string        key = KeyOf( unitySource );
        std::vector<std::size_t> batch;
        for ( std::size_t i = 0; i < m_Sources.size(); ++i )
            if ( !m_Sources[i].UnitySource.empty() && KeyOf( m_Sources[i].UnitySource ) == key )
                batch.push_back( i );
        return batch;
    }

    std::string BuildIndex::CommandFor( const std::size_t source, const fs::path& object ) const
    {
        const SourceRecord& record = m_Sources[source];
        const fs::path      unit   = record.UnitySource.empty() ? record.Source : record.UnitySource;
        // Load refused every command whose slots it could not find, so both are here.
        const std::optional<CommandSlots> slots      = LocateSlots( record.Command, record.Directory, unit );
        const std::string                 sourceText = std::format( "\"{}\"", record.Source.generic_string() );
        const std::string objectText = std::format( "/Fo\"{}\"", Normal( object ).generic_string() );

        // Splice from the back so the front offsets stay valid; every other byte is the build's own.
        std::string  command  = record.Command;
        const bool   unitLast = slots->Unit.Begin > slots->Object.Begin;
        const Token& later    = unitLast ? slots->Unit : slots->Object;
        const Token& earlier  = unitLast ? slots->Object : slots->Unit;
        command.replace( later.Begin, later.End - later.Begin, unitLast ? sourceText : objectText );
        command.replace( earlier.Begin, earlier.End - earlier.Begin, unitLast ? objectText : sourceText );
        return command;
    }

    void BuildIndex::MarkPatched( const std::span<const std::size_t>        sources,
                                  const std::span<const fs::file_time_type> at )
    {
        for ( std::size_t i = 0; i < sources.size() && i < at.size(); ++i )
            m_PatchedAt[sources[i]] = at[i];
    }
} // namespace Desert::Editor::LiveCoding
