// BuildPool: ParseInvocation (BuildPool/Invocation.hpp) - what a compiler, linker or archiver command line does,
// read from the command line alone. Response files are expanded for reading only; the command that runs is
// always the one the build tool wrote.
#include "BuildPool/Invocation.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>

namespace buildpool
{
    namespace
    {
        constexpr int kMaxResponseDepth = 8;

        std::string Lower( std::string_view text )
        {
            std::string lower( text );
            for ( char& c : lower )
            {
                if ( c >= 'A' && c <= 'Z' )
                    c = static_cast<char>( c - 'A' + 'a' );
            }
            return lower;
        }

        std::string_view BaseName( std::string_view path )
        {
            const size_t slash = path.find_last_of( "/\\" );
            return slash == std::string_view::npos ? path : path.substr( slash + 1 );
        }

        // The file name without its directory and its last extension.
        std::string Stem( std::string_view path )
        {
            const std::string_view base = BaseName( path );
            const size_t           dot  = base.find_last_of( '.' );
            return std::string( dot == std::string_view::npos || dot == 0 ? base : base.substr( 0, dot ) );
        }

        std::string Extension( std::string_view path )
        {
            const std::string_view base = BaseName( path );
            const size_t           dot  = base.find_last_of( '.' );
            return dot == std::string_view::npos ? std::string() : Lower( base.substr( dot + 1 ) );
        }

        bool EndsWithSlash( std::string_view path )
        {
            return !path.empty() && ( path.back() == '/' || path.back() == '\\' );
        }

        // "-18", "-13.2": the version a toolchain appends to a driver name.
        bool IsVersionSuffix( std::string_view text )
        {
            if ( text.size() < 2 || text[0] != '-' )
                return false;
            bool digit = false;
            for ( char c : text.substr( 1 ) )
            {
                if ( c >= '0' && c <= '9' )
                    digit = true;
                else if ( c != '.' )
                    return false;
            }
            return digit;
        }

        // `name` is `driver`, "<target triple>-<driver>", either one with "-<version>" behind it.
        bool IsDriver( std::string_view name, std::string_view driver )
        {
            const size_t dash = name.find_last_of( '-' );
            if ( dash != std::string_view::npos && IsVersionSuffix( name.substr( dash ) ) )
                name = name.substr( 0, dash );
            if ( name == driver )
                return true;
            return name.size() > driver.size() && name.ends_with( driver ) &&
                   name[name.size() - driver.size() - 1] == '-';
        }

        // ---- response files
        // -------------------------------------------------------------------------------------
        void AppendUtf8( std::string& out, uint32_t code )
        {
            if ( code < 0x80 )
                out.push_back( static_cast<char>( code ) );
            else if ( code < 0x800 )
            {
                out.push_back( static_cast<char>( 0xC0 | ( code >> 6 ) ) );
                out.push_back( static_cast<char>( 0x80 | ( code & 0x3F ) ) );
            }
            else if ( code < 0x10000 )
            {
                out.push_back( static_cast<char>( 0xE0 | ( code >> 12 ) ) );
                out.push_back( static_cast<char>( 0x80 | ( ( code >> 6 ) & 0x3F ) ) );
                out.push_back( static_cast<char>( 0x80 | ( code & 0x3F ) ) );
            }
            else
            {
                out.push_back( static_cast<char>( 0xF0 | ( code >> 18 ) ) );
                out.push_back( static_cast<char>( 0x80 | ( ( code >> 12 ) & 0x3F ) ) );
                out.push_back( static_cast<char>( 0x80 | ( ( code >> 6 ) & 0x3F ) ) );
                out.push_back( static_cast<char>( 0x80 | ( code & 0x3F ) ) );
            }
        }

        std::string Utf16LeToUtf8( std::string_view bytes )
        {
            std::string out;
            out.reserve( bytes.size() / 2 );
            const auto unit = [&]( size_t i )
            {
                return static_cast<uint32_t>( static_cast<uint8_t>( bytes[i] ) ) |
                       ( static_cast<uint32_t>( static_cast<uint8_t>( bytes[i + 1] ) ) << 8 );
            };
            for ( size_t i = 0; i + 1 < bytes.size(); i += 2 )
            {
                uint32_t code = unit( i );
                if ( code >= 0xD800 && code <= 0xDBFF && i + 3 < bytes.size() )
                {
                    const uint32_t low = unit( i + 2 );
                    if ( low >= 0xDC00 && low <= 0xDFFF )
                    {
                        code = 0x10000 + ( ( code - 0xD800 ) << 10 ) + ( low - 0xDC00 );
                        i += 2;
                    }
                }
                AppendUtf8( out, code );
            }
            return out;
        }

        Result<std::string> ReadResponseFile( std::string_view name )
        {
            const std::filesystem::path path( std::u8string( name.begin(), name.end() ) );
            std::ifstream               file( path, std::ios::binary );
            if ( !file )
                return Fail( "cannot read the response file " + std::string( name ) );
            std::string bytes( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
            if ( bytes.size() >= 2 && static_cast<uint8_t>( bytes[0] ) == 0xFF &&
                 static_cast<uint8_t>( bytes[1] ) == 0xFE )
                return Utf16LeToUtf8( std::string_view( bytes ).substr( 2 ) );
            if ( bytes.size() >= 3 && static_cast<uint8_t>( bytes[0] ) == 0xEF &&
                 static_cast<uint8_t>( bytes[1] ) == 0xBB && static_cast<uint8_t>( bytes[2] ) == 0xBF )
                return bytes.substr( 3 );
            return bytes;
        }

        bool IsBlank( char c )
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }

        // The CommandLineToArgvW rules: 2n backslashes + quote = n backslashes and a quote toggle, 2n+1
        // backslashes + quote = n backslashes and a literal quote, "" inside quotes = a literal quote.
        std::vector<std::string> SplitWindows( std::string_view text )
        {
            std::vector<std::string> words;
            size_t                   i = 0;
            while ( i < text.size() )
            {
                while ( i < text.size() && IsBlank( text[i] ) )
                    ++i;
                if ( i >= text.size() )
                    break;
                std::string word;
                bool        quoted = false;
                while ( i < text.size() && ( quoted || !IsBlank( text[i] ) ) )
                {
                    if ( text[i] == '\\' )
                    {
                        size_t slashes = 0;
                        while ( i < text.size() && text[i] == '\\' )
                        {
                            ++slashes;
                            ++i;
                        }
                        if ( i < text.size() && text[i] == '"' )
                        {
                            word.append( slashes / 2, '\\' );
                            if ( slashes % 2 == 1 )
                            {
                                word.push_back( '"' );
                                ++i;
                            }
                        }
                        else
                            word.append( slashes, '\\' );
                    }
                    else if ( text[i] == '"' )
                    {
                        if ( quoted && i + 1 < text.size() && text[i + 1] == '"' )
                        {
                            word.push_back( '"' );
                            i += 2;
                        }
                        else
                        {
                            quoted = !quoted;
                            ++i;
                        }
                    }
                    else
                        word.push_back( text[i++] );
                }
                words.push_back( std::move( word ) );
            }
            return words;
        }

        // GNU @file rules: blanks separate, single and double quotes group, a backslash takes the next character.
        std::vector<std::string> SplitGnu( std::string_view text )
        {
            std::vector<std::string> words;
            size_t                   i = 0;
            while ( i < text.size() )
            {
                while ( i < text.size() && IsBlank( text[i] ) )
                    ++i;
                if ( i >= text.size() )
                    break;
                std::string word;
                char        quote = 0;
                while ( i < text.size() && ( quote != 0 || !IsBlank( text[i] ) ) )
                {
                    const char c = text[i++];
                    if ( c == '\\' && i < text.size() )
                        word.push_back( text[i++] );
                    else if ( quote != 0 && c == quote )
                        quote = 0;
                    else if ( quote == 0 && ( c == '\'' || c == '"' ) )
                        quote = c;
                    else
                        word.push_back( c );
                }
                words.push_back( std::move( word ) );
            }
            return words;
        }

        Status Expand( std::span<const std::string> words, bool windowsRules, int depth,
                       std::vector<std::string>& out )
        {
            for ( const std::string& word : words )
            {
                if ( word.size() < 2 || word[0] != '@' )
                {
                    out.push_back( word );
                    continue;
                }
                if ( depth >= kMaxResponseDepth )
                    return Fail( "response files nested deeper than " + std::to_string( kMaxResponseDepth ) +
                                 " at " + word );
                auto text = ReadResponseFile( std::string_view( word ).substr( 1 ) );
                if ( !text )
                    return text.error();
                const std::vector<std::string> inner =
                     windowsRules ? SplitWindows( text.value() ) : SplitGnu( text.value() );
                if ( Status expanded = Expand( inner, windowsRules, depth + 1, out ); !expanded )
                    return expanded;
            }
            return Ok();
        }

        bool IsSourceExtension( std::string_view extension )
        {
            static constexpr std::array<std::string_view, 12> kSources = {
                 "c", "cc", "cpp", "cxx", "c++", "cp", "ixx", "cppm", "m", "mm", "s", "asm" };
            return std::find( kSources.begin(), kSources.end(), extension ) != kSources.end();
        }

        bool IsHeaderExtension( std::string_view extension )
        {
            static constexpr std::array<std::string_view, 5> kHeaders = { "h", "hh", "hpp", "hxx", "h++" };
            return std::find( kHeaders.begin(), kHeaders.end(), extension ) != kHeaders.end();
        }

        // The object a compile writes: /Fo (or -o) as written; a /Fo naming a directory gets the source's stem.
        std::string ObjectPath( const std::string& output, const std::string& source, std::string_view extension )
        {
            if ( output.empty() )
                return source.empty() ? std::string() : Stem( source ) + std::string( extension );
            if ( EndsWithSlash( output ) )
                return output + ( source.empty() ? std::string() : Stem( source ) + std::string( extension ) );
            return output;
        }

        // ---- cl.exe and clang-cl
        // --------------------------------------------------------------------------------
        void ParseMsvc( std::span<const std::string> args, ToolKind tool, Invocation& invocation )
        {
            bool        compileOnly = false, preprocessStdout = false, preprocessFile = false, pchCreate = false;
            bool        sharedPdb = false, pdbName = false, forceSync = false;
            std::string objectOut, preprocessOut, pchOut, imageOut, source;
            size_t      inputs = 0;

            // Options whose value may be the next argument when written alone (/I dir, /Fo: path).
            static constexpr std::array<std::string_view, 9> kSeparate = { "I",  "D",  "U",      "FI",        "AI",
                                                                           "Tp", "Tc", "Xclang", "external:I" };

            for ( size_t i = 0; i < args.size(); ++i )
            {
                const std::string& arg = args[i];
                if ( arg.empty() )
                    continue;
                if ( arg[0] != '/' && arg[0] != '-' )
                {
                    ++inputs;
                    if ( source.empty() && IsSourceExtension( Extension( arg ) ) )
                        source = arg;
                    continue;
                }
                const std::string_view option = std::string_view( arg ).substr( 1 );
                if ( option == "link" )
                    break; // the rest belongs to the linker cl starts
                // A path option: "/FoX", "/Fo:X" or "/Fo:" followed by X.
                const auto pathValue = [&]( std::string_view prefix, std::string& into )
                {
                    if ( !option.starts_with( prefix ) )
                        return false;
                    std::string_view value = option.substr( prefix.size() );
                    if ( value == ":" )
                    {
                        if ( i + 1 < args.size() )
                            into = args[++i];
                        return true;
                    }
                    if ( value.starts_with( ':' ) )
                        value.remove_prefix( 1 );
                    into = std::string( value );
                    return true;
                };
                if ( std::find( kSeparate.begin(), kSeparate.end(), option ) != kSeparate.end() )
                {
                    if ( i + 1 < args.size() )
                    {
                        ++i;
                        if ( ( option == "Tp" || option == "Tc" ) && source.empty() )
                        {
                            source = args[i];
                            ++inputs;
                        }
                    }
                    continue;
                }
                if ( option == "c" )
                    compileOnly = true;
                else if ( option == "E" || option == "EP" )
                    preprocessStdout = true;
                else if ( option == "P" )
                    preprocessFile = true;
                else if ( option.starts_with( "Yc" ) )
                    pchCreate = true;
                else if ( option == "Zi" || option == "ZI" )
                    sharedPdb = true;
                else if ( option == "FS" )
                    forceSync = true;
                else if ( option.starts_with( "Fd" ) )
                    pdbName = true;
                else if ( pathValue( "Fo", objectOut ) || pathValue( "Fi", preprocessOut ) ||
                          pathValue( "Fp", pchOut ) || pathValue( "Fe", imageOut ) )
                    continue;
                else if ( ( option.starts_with( "Tp" ) || option.starts_with( "Tc" ) ) && option.size() > 2 )
                {
                    ++inputs;
                    if ( source.empty() )
                        source = std::string( option.substr( 2 ) );
                }
                else if ( tool == ToolKind::ClangCl && option == "o" && i + 1 < args.size() )
                    objectOut = args[++i];
            }

            if ( preprocessStdout || preprocessFile )
            {
                invocation.action             = ActionKind::Preprocess;
                invocation.preprocessToStdout = preprocessStdout;
                if ( !preprocessStdout )
                {
                    invocation.preprocessOutput = preprocessOut.empty() ? Stem( source ) + ".i"
                                                  : EndsWithSlash( preprocessOut )
                                                       ? preprocessOut + Stem( source ) + ".i"
                                                       : preprocessOut;
                    invocation.primaryOutput    = invocation.preprocessOutput;
                }
                // Output already written to stdout cannot be taken back: only a file preprocess runs again.
                invocation.restartable = !preprocessStdout;
                return;
            }
            if ( compileOnly )
            {
                invocation.action = pchCreate ? ActionKind::PchCreate : ActionKind::Compile;
                invocation.primaryOutput =
                     pchCreate && !pchOut.empty() ? pchOut : ObjectPath( objectOut, source, ".obj" );
                invocation.restartable = !( sharedPdb || ( pdbName && forceSync ) );
                return;
            }
            if ( inputs > 0 )
            {
                invocation.action        = ActionKind::Link;
                invocation.primaryOutput = ObjectPath( imageOut, source, ".exe" );
            }
        }

        // ---- clang, gcc as compiler or link driver
        // --------------------------------------------------------------
        void ParseGccLike( std::span<const std::string> args, Invocation& invocation )
        {
            bool compileOnly = false, preprocess = false, depsOnly = false, headerLanguage = false,
                 emitPch = false;
            std::string      output, source, header;
            size_t           inputs       = 0;
            std::string_view extensionOut = ".o";

            static constexpr std::array<std::string_view, 33> kSeparate = { "-I",
                                                                            "-D",
                                                                            "-U",
                                                                            "-include",
                                                                            "-imacros",
                                                                            "-isystem",
                                                                            "-iquote",
                                                                            "-idirafter",
                                                                            "-iprefix",
                                                                            "-iwithprefix",
                                                                            "-isysroot",
                                                                            "-MF",
                                                                            "-MT",
                                                                            "-MQ",
                                                                            "-arch",
                                                                            "-target",
                                                                            "-Xlinker",
                                                                            "-Xpreprocessor",
                                                                            "-Xassembler",
                                                                            "-F",
                                                                            "-iframework",
                                                                            "-L",
                                                                            "-framework",
                                                                            "-u",
                                                                            "-T",
                                                                            "-z",
                                                                            "-include-pch",
                                                                            "-mllvm",
                                                                            "--serialize-diagnostics",
                                                                            "-MJ",
                                                                            "-install_name",
                                                                            "-rpath",
                                                                            "-ivfsoverlay" };

            for ( size_t i = 0; i < args.size(); ++i )
            {
                const std::string& arg = args[i];
                if ( arg.empty() )
                    continue;
                if ( arg == "-" || arg[0] != '-' )
                {
                    ++inputs;
                    const std::string extension = Extension( arg );
                    if ( source.empty() && IsSourceExtension( extension ) )
                        source = arg;
                    else if ( header.empty() && IsHeaderExtension( extension ) )
                        header = arg;
                    continue;
                }
                if ( arg == "-c" )
                    compileOnly = true;
                else if ( arg == "-S" )
                {
                    compileOnly  = true;
                    extensionOut = ".s";
                }
                else if ( arg == "-E" )
                    preprocess = true;
                else if ( arg == "-M" || arg == "-MM" )
                    depsOnly = true;
                else if ( arg == "-o" )
                {
                    if ( i + 1 < args.size() )
                        output = args[++i];
                }
                else if ( arg.size() > 2 && arg.starts_with( "-o" ) && !arg.starts_with( "-objc" ) )
                    output = arg.substr( 2 );
                else if ( arg == "-x" )
                {
                    if ( i + 1 < args.size() )
                        headerLanguage = args[++i].ends_with( "-header" );
                }
                else if ( arg.starts_with( "-x" ) && arg.size() > 2 )
                    headerLanguage = arg.ends_with( "-header" );
                else if ( arg == "-Xclang" )
                {
                    if ( i + 1 < args.size() )
                        emitPch = emitPch || args[++i] == "-emit-pch";
                }
                else if ( arg.starts_with( "-Xarch_" ) )
                    ++i; // "-Xarch_<arch> <argument>"
                else if ( std::find( kSeparate.begin(), kSeparate.end(), std::string_view( arg ) ) !=
                          kSeparate.end() )
                    ++i;
            }

            if ( preprocess )
            {
                invocation.action             = ActionKind::Preprocess;
                invocation.preprocessToStdout = output.empty() || output == "-";
                if ( !invocation.preprocessToStdout )
                {
                    invocation.preprocessOutput = output;
                    invocation.primaryOutput    = output;
                }
                invocation.restartable = !invocation.preprocessToStdout;
                return;
            }
            if ( depsOnly )
                return; // a dependency scan writes no object: Other
            const bool pch = emitPch || headerLanguage || ( source.empty() && !header.empty() && compileOnly );
            if ( pch )
            {
                invocation.action = ActionKind::PchCreate;
                invocation.primaryOutput =
                     !output.empty() ? output : ( header.empty() ? source : header ) + ".gch";
                invocation.restartable = true;
                return;
            }
            if ( compileOnly )
            {
                invocation.action        = ActionKind::Compile;
                invocation.primaryOutput = ObjectPath( output, source, extensionOut );
                invocation.restartable   = true;
                return;
            }
            if ( inputs > 0 )
            {
                invocation.action        = ActionKind::Link;
                invocation.primaryOutput = output.empty() ? std::string( "a.out" ) : output;
            }
        }

        // ---- link.exe, lib.exe, lld-link
        // ------------------------------------------------------------------------
        void ParseMsvcLink( std::string_view linker, std::span<const std::string> args, Invocation& invocation )
        {
            const std::string name    = Lower( BaseName( linker ) );
            const bool        archive = name.starts_with( "lib" ) || name.starts_with( "llvm-lib" );
            bool              dll     = false;
            std::string       output, firstInput;
            for ( const std::string& arg : args )
            {
                if ( arg.empty() )
                    continue;
                if ( arg[0] == '/' || arg[0] == '-' )
                {
                    const std::string option = Lower( std::string_view( arg ).substr( 1 ) );
                    if ( option.starts_with( "out:" ) )
                        output = arg.substr( 5 );
                    else if ( option == "dll" )
                        dll = true;
                    continue;
                }
                if ( firstInput.empty() )
                    firstInput = arg;
            }
            invocation.action        = ActionKind::Link;
            invocation.primaryOutput = !output.empty()      ? output
                                       : firstInput.empty() ? std::string()
                                                            : Stem( firstInput ) + ( archive ? ".lib"
                                                                                     : dll   ? ".dll"
                                                                                             : ".exe" );
        }

        // ---- ar, libtool
        // ----------------------------------------------------------------------------------------
        void ParseArchiver( std::string_view name, std::span<const std::string> args, Invocation& invocation )
        {
            invocation.action = ActionKind::Link;
            if ( IsDriver( name, "libtool" ) )
            {
                for ( size_t i = 0; i + 1 < args.size(); ++i )
                {
                    if ( args[i] == "-o" )
                        invocation.primaryOutput = args[i + 1];
                }
                return;
            }
            // "ar <operation> [options] <archive> <members...>": the operation is the first word ("qc", "-rcs").
            bool operation = false;
            for ( const std::string& arg : args )
            {
                if ( arg.empty() )
                    continue;
                if ( !operation )
                {
                    operation = arg[0] != '-' ||
                                arg.find_first_not_of( "-abcdfiDlMNoOPqrsStTuUvVx" ) == std::string::npos;
                    continue;
                }
                if ( arg[0] == '-' )
                    continue;
                invocation.primaryOutput = arg;
                return;
            }
        }
    } // namespace

    Result<Invocation> ParseInvocation( std::span<const std::string> argv ) noexcept
    {
        if ( argv.empty() || argv[0].empty() )
            return Fail( "an empty command line" );

        Invocation        invocation;
        const std::string name = [&]
        {
            std::string base = Lower( BaseName( argv[0] ) );
            if ( base.size() > 4 && base.ends_with( ".exe" ) )
                base.resize( base.size() - 4 );
            return base;
        }();

        // "cmake -E vs_link_exe|vs_link_dll [cmake options] -- <linker> <arguments>"
        if ( IsDriver( name, "cmake" ) )
        {
            if ( argv.size() < 3 || argv[1] != "-E" || !argv[2].starts_with( "vs_link_" ) )
                return invocation; // another cmake command: Other
            const auto separator = std::find( argv.begin() + 3, argv.end(), std::string( "--" ) );
            if ( separator == argv.end() || separator + 1 == argv.end() )
                return Fail( "cmake -E " + argv[2] + " without \"-- <linker> ...\"" );
            std::vector<std::string>           linkerArgs;
            const std::span<const std::string> rest( separator + 2, argv.end() );
            if ( Status expanded = Expand( rest, true, 0, linkerArgs ); !expanded )
                return expanded.error();
            invocation.tool = ToolKind::MsvcLink;
            ParseMsvcLink( *( separator + 1 ), linkerArgs, invocation );
            return invocation;
        }

        if ( name == "clang-cl" || IsDriver( name, "clang-cl" ) )
            invocation.tool = ToolKind::ClangCl;
        else if ( name == "cl" )
            invocation.tool = ToolKind::Msvc;
        else if ( IsDriver( name, "link" ) || IsDriver( name, "lib" ) || IsDriver( name, "lld-link" ) ||
                  IsDriver( name, "llvm-lib" ) )
            invocation.tool = ToolKind::MsvcLink;
        else if ( IsDriver( name, "clang" ) || IsDriver( name, "clang++" ) || IsDriver( name, "gcc" ) ||
                  IsDriver( name, "g++" ) || IsDriver( name, "cc" ) || IsDriver( name, "c++" ) )
            invocation.tool = ToolKind::GccLike;
        else if ( IsDriver( name, "ar" ) || IsDriver( name, "libtool" ) )
            invocation.tool = ToolKind::Archiver;
        else
            return invocation; // Unknown tool: an Other action, admitted as such

        const bool windowsRules = invocation.tool == ToolKind::Msvc || invocation.tool == ToolKind::ClangCl ||
                                  invocation.tool == ToolKind::MsvcLink;
        std::vector<std::string> args;
        if ( Status expanded = Expand( argv.subspan( 1 ), windowsRules, 0, args ); !expanded )
            return expanded.error();

        switch ( invocation.tool )
        {
            case ToolKind::Msvc:
            case ToolKind::ClangCl:
                ParseMsvc( args, invocation.tool, invocation );
                break;
            case ToolKind::GccLike:
                ParseGccLike( args, invocation );
                break;
            case ToolKind::MsvcLink:
                ParseMsvcLink( argv[0], args, invocation );
                break;
            case ToolKind::Archiver:
                ParseArchiver( name, args, invocation );
                break;
            case ToolKind::Unknown:
                break;
        }
        return invocation;
    }
} // namespace buildpool
