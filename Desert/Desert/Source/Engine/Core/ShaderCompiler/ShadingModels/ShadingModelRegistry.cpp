#include <Engine/Core/ShaderCompiler/ShadingModels/ShadingModelRegistry.hpp>

#include <Engine/Core/ShaderCompiler/ShadingModels/ShaderRootShadingModels.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>

namespace Desert::Core::ShadingModels
{
    namespace
    {
        bool IsIdentChar( const char ch )
        {
            return std::isalnum( static_cast<unsigned char>( ch ) ) || ch == '_';
        }

        bool IsGlslIdentifier( const std::string_view name )
        {
            return !name.empty() && !std::isdigit( static_cast<unsigned char>( name[0] ) ) &&
                   std::ranges::all_of( name, IsIdentChar );
        }

        // Whether @p body names @p function as a whole word followed by '(' — a definition or a call; a body
        // that only calls one of the two is refused by the GLSL compiler, a body that names neither is here.
        bool NamesFunction( const std::string_view body, const std::string_view function )
        {
            for ( std::size_t at = body.find( function ); at != std::string_view::npos;
                  at             = body.find( function, at + 1 ) )
            {
                const std::size_t after = at + function.size();
                if ( at > 0 && IsIdentChar( body[at - 1] ) )
                    continue;
                if ( after < body.size() && IsIdentChar( body[after] ) )
                    continue;
                std::size_t next = after;
                while ( next < body.size() && std::isspace( static_cast<unsigned char>( body[next] ) ) )
                    ++next;
                if ( next < body.size() && body[next] == '(' )
                    return true;
            }
            return false;
        }

        std::string FileName( const ShadingModelManifest& m )
        {
            return m.SourcePath.generic_string();
        }

        std::string ReadText( const std::filesystem::path& path )
        {
            std::ifstream      in( path, std::ios::binary );
            std::ostringstream out;
            out << in.rdbuf();
            return out.str();
        }
    } // namespace

    // ------------------------------------------------------------------------------------------------------------
    std::string ShadingModelIndexDefine( const std::string_view modelName )
    {
        std::string snake;
        for ( std::size_t i = 0; i < modelName.size(); ++i )
        {
            const unsigned char ch = static_cast<unsigned char>( modelName[i] );
            if ( i > 0 && std::isupper( ch ) )
            {
                const unsigned char prev = static_cast<unsigned char>( modelName[i - 1] );
                const bool          nextLower =
                     i + 1 < modelName.size() && std::islower( static_cast<unsigned char>( modelName[i + 1] ) );
                // "DefaultLit" -> DEFAULT_LIT, "GGXCloth" -> GGX_CLOTH.
                if ( std::islower( prev ) || std::isdigit( prev ) || ( std::isupper( prev ) && nextLower ) )
                    snake.push_back( '_' );
            }
            snake.push_back( static_cast<char>( std::toupper( ch ) ) );
        }
        return std::format( "SHADING_MODEL_INDEX_{}", snake );
    }

    Common::ResultStr<std::vector<std::string>> ReadSurfaceOutputFields( const std::string_view surfaceTypesGlsl )
    {
        constexpr auto    kStruct = std::string_view( "struct SurfaceOutput" );
        const std::size_t start   = surfaceTypesGlsl.find( kStruct );
        const std::size_t open    = start == std::string_view::npos ? start : surfaceTypesGlsl.find( '{', start );
        const std::size_t close   = open == std::string_view::npos ? open : surfaceTypesGlsl.find( '}', open );
        if ( close == std::string_view::npos )
            return Common::MakeError<std::vector<std::string>>(
                 "Mesh/Surface/SurfaceTypes.glslh: no 'struct SurfaceOutput { ... }' — the field list of every "
                 "shading model's Inputs" );

        // `<type> <Name>;` per declaration, comments stripped.
        std::string        body;
        std::istringstream lines( std::string( surfaceTypesGlsl.substr( open + 1, close - open - 1 ) ) );
        for ( std::string line; std::getline( lines, line ); )
            body.append( line.substr( 0, line.find( "//" ) ) ).push_back( '\n' );

        std::vector<std::string> fields;
        std::istringstream       declarations( body );
        for ( std::string declaration; std::getline( declarations, declaration, ';' ); )
        {
            std::istringstream       words( declaration );
            std::vector<std::string> tokens{ std::istream_iterator<std::string>( words ), {} };
            if ( tokens.size() >= 2 )
                fields.push_back( tokens.back() );
        }
        if ( fields.empty() )
            return Common::MakeError<std::vector<std::string>>(
                 "Mesh/Surface/SurfaceTypes.glslh: struct SurfaceOutput declares no field" );
        return Common::MakeSuccess( std::move( fields ) );
    }

    // ------------------------------------------------------------------------------------------------------------
    Common::ResultStr<ShadingModelRegistry> ShadingModelRegistry::Scan( const std::filesystem::path& shaderRoot )
    {
        const std::filesystem::path surfaceTypes = shaderRoot / "Mesh/Surface/SurfaceTypes.glslh";
        if ( !std::filesystem::is_regular_file( surfaceTypes ) )
            return Common::MakeError<ShadingModelRegistry>(
                 std::format( "{}: missing — it declares SurfaceOutput, the field list of shading-model Inputs",
                              surfaceTypes.generic_string() ) );
        const auto fields = ReadSurfaceOutputFields( ReadText( surfaceTypes ) );
        if ( !fields.IsSuccess() )
            return Common::MakeError<ShadingModelRegistry>( fields.GetError() );

        const std::filesystem::path directory = shaderRoot / kShadingModelDirectory;
        std::error_code             ec;
        if ( !std::filesystem::is_directory( directory, ec ) )
            return Common::MakeError<ShadingModelRegistry>(
                 std::format( "{}: missing — the shading models (Unlit and DefaultLit at least) live there",
                              directory.generic_string() ) );

        std::vector<std::filesystem::path> files;
        for ( const auto& entry : std::filesystem::directory_iterator( directory, ec ) )
            if ( entry.is_regular_file() && entry.path().extension() == kShadingModelExtension )
                files.push_back( entry.path() );
        if ( ec )
            return Common::MakeError<ShadingModelRegistry>(
                 std::format( "{}: cannot list ({})", directory.generic_string(), ec.message() ) );
        std::ranges::sort( files );

        std::vector<ShadingModelManifest> manifests;
        for ( const std::filesystem::path& file : files )
        {
            auto manifest = ParseShadingModelManifest( ReadText( file ), file );
            if ( !manifest.IsSuccess() )
                return Common::MakeError<ShadingModelRegistry>( manifest.GetError() );
            manifests.push_back( manifest.ExtractValue() );
        }
        return Build( std::move( manifests ), fields.GetValue() );
    }

    Common::ResultStr<ShadingModelRegistry>
    ShadingModelRegistry::Build( std::vector<ShadingModelManifest>  manifests,
                                 const std::span<const std::string> surfaceOutputFields )
    {
        if ( manifests.size() > kMaxShadingModels )
        {
            const std::string directory = manifests.front().SourcePath.parent_path().generic_string();
            return Common::MakeError<ShadingModelRegistry>(
                 std::format( "{}: {} shading models; the shading word's index carries at most {}",
                              directory.empty() ? std::string( kShadingModelDirectory ) : directory,
                              manifests.size(), kMaxShadingModels ) );
        }

        for ( std::size_t i = 0; i < manifests.size(); ++i )
        {
            const ShadingModelManifest& m = manifests[i];
            if ( !IsGlslIdentifier( m.Name ) )
                return Common::MakeError<ShadingModelRegistry>( std::format(
                     "{}: the model's name '{}' (the file stem) must be a GLSL identifier — the generated "
                     "include names <Name>_Evaluate",
                     FileName( m ), m.Name ) );
            for ( std::size_t j = 0; j < i; ++j )
            {
                if ( static_cast<std::uint64_t>( manifests[j].Guid ) == static_cast<std::uint64_t>( m.Guid ) )
                    return Common::MakeError<ShadingModelRegistry>(
                         std::format( "{} and {} carry the same Guid {} — a Guid is one model's identity",
                                      FileName( manifests[j] ), FileName( m ), m.Guid.ToString() ) );
                if ( manifests[j].Name == m.Name )
                    return Common::MakeError<ShadingModelRegistry>( std::format(
                         "{} and {} are both named '{}'", FileName( manifests[j] ), FileName( m ), m.Name ) );
            }
            for ( const std::string& input : m.Inputs )
                if ( std::ranges::find( surfaceOutputFields, input ) == surfaceOutputFields.end() )
                    return Common::MakeError<ShadingModelRegistry>(
                         std::format( "{}: Inputs field '{}' is not a field of SurfaceOutput "
                                      "(Mesh/Surface/SurfaceTypes.glslh)",
                                      FileName( m ), input ) );
            for ( const std::string_view function : { std::string_view( "Evaluate" ), "EvaluateAmbient" } )
                if ( !NamesFunction( m.Body, function ) )
                    return Common::MakeError<ShadingModelRegistry>(
                         std::format( "{}: the body does not define {} (ShadingModels/ShadingModelContract.glslh "
                                      "gives the signature)",
                                      FileName( m ), function ) );
        }

        for ( const std::uint64_t required : { kUnlitGuid, kDefaultLitGuid } )
            if ( std::ranges::none_of( manifests, [&]( const ShadingModelManifest& m )
                                       { return static_cast<std::uint64_t>( m.Guid ) == required; } ) )
                return Common::MakeError<ShadingModelRegistry>(
                     std::format( "no shading model carries Guid {} ({}) — the engine relies on it", required,
                                  required == kUnlitGuid ? "Unlit" : "DefaultLit" ) );

        // Unlit first, the rest by ascending Guid: the same files give the same indices everywhere.
        std::ranges::sort( manifests,
                           []( const ShadingModelManifest& a, const ShadingModelManifest& b )
                           {
                               const std::uint64_t ga = a.Guid, gb = b.Guid;
                               if ( ( ga == kUnlitGuid ) != ( gb == kUnlitGuid ) )
                                   return ga == kUnlitGuid;
                               return ga < gb;
                           } );

        ShadingModelRegistry registry;
        registry.m_Entries.reserve( manifests.size() );
        for ( std::size_t i = 0; i < manifests.size(); ++i )
            registry.m_Entries.push_back( { std::move( manifests[i] ), static_cast<std::uint8_t>( i ) } );
        return Common::MakeSuccess( std::move( registry ) );
    }

    // ------------------------------------------------------------------------------------------------------------
    std::span<const ShadingModelEntry> ShadingModelRegistry::Entries() const
    {
        return m_Entries;
    }

    const ShadingModelEntry* ShadingModelRegistry::FindByGuid( const Common::UUID guid ) const
    {
        const auto it = std::ranges::find_if(
             m_Entries, [&]( const ShadingModelEntry& e )
             { return static_cast<std::uint64_t>( e.Manifest.Guid ) == static_cast<std::uint64_t>( guid ); } );
        return it == m_Entries.end() ? nullptr : &*it;
    }

    const ShadingModelEntry* ShadingModelRegistry::FindByName( const std::string_view name ) const
    {
        const auto it = std::ranges::find_if( m_Entries, [&]( const ShadingModelEntry& e )
                                              { return e.Manifest.Name == name; } );
        return it == m_Entries.end() ? nullptr : &*it;
    }

    std::string ShadingModelRegistry::IndexLayoutKey() const
    {
        std::string key;
        for ( const ShadingModelEntry& e : m_Entries )
            std::format_to( std::back_inserter( key ), "{}={};", e.Manifest.Guid.ToString(), e.Index );
        return key;
    }

    std::optional<std::string>
    ShadingModelRegistry::MissingInput( const ShadingModelEntry&           model,
                                        const std::span<const std::string> writtenFields )
    {
        const auto written = [&]( const std::string& field )
        { return std::ranges::find( writtenFields, field ) != writtenFields.end(); };
        for ( const std::string& input : model.Manifest.Inputs )
            if ( !written( input ) )
                return input;
        for ( const PayloadSlot& slot : model.Manifest.Payload )
            if ( !written( slot.Pin ) )
                return slot.Pin;
        return std::nullopt;
    }

    // ------------------------------------------------------------------------------------------------------------
    std::string ShadingModelRegistry::GenerateGlsl() const
    {
        std::string out;
        auto        emit = std::back_inserter( out );
        std::format_to(
             emit,
             "// GENERATED by ShadingModelRegistry (Engine/Core/ShaderCompiler/ShadingModels) from {}/*{} "
             "— never edited, never committed.\n"
             "#ifndef DESERT_SHADING_MODELS_GENERATED_GLSLH\n"
             "#define DESERT_SHADING_MODELS_GENERATED_GLSLH\n\n"
             "#include <{}>\n\n",
             kShadingModelDirectory, kShadingModelExtension, kContractInclude );

        for ( const ShadingModelEntry& e : m_Entries )
            std::format_to( emit, "#define {} {}\n", ShadingModelIndexDefine( e.Manifest.Name ), e.Index );

        // Each body as written, its two functions renamed by the preprocessor (token-exact: EvaluateAmbient is
        // not touched by the Evaluate macro).
        for ( const ShadingModelEntry& e : m_Entries )
            std::format_to( emit,
                            "\n// ---- {} (Guid {}, index {}) ----\n"
                            "#define Evaluate {}_Evaluate\n"
                            "#define EvaluateAmbient {}_EvaluateAmbient\n"
                            "{}\n"
                            "#undef Evaluate\n"
                            "#undef EvaluateAmbient\n",
                            FileName( e.Manifest ), e.Manifest.Guid.ToString(), e.Index, e.Manifest.Name,
                            e.Manifest.Name, e.Manifest.Body );

        const auto dispatch =
             [&]( const std::string_view signature, const std::string_view suffix, const std::string_view args )
        {
            std::format_to( emit, "\n{}\n{{\n    switch ( index )\n    {{\n", signature );
            for ( const ShadingModelEntry& e : m_Entries )
                std::format_to( emit, "        case {}: return {}_{}( {} );\n",
                                ShadingModelIndexDefine( e.Manifest.Name ), e.Manifest.Name, suffix, args );
            // Every index the G-buffer can carry is a case above; GLSL still needs a return after the switch.
            std::format_to( emit, "    }}\n    return vec3( 0.0 );\n}}\n" );
        };
        dispatch( "vec3 DesertEvaluateShadingModel( int index, DesertLight L, DesertSurface S, DesertPayload P )",
                  "Evaluate", "L, S, P" );
        dispatch( "vec3 DesertEvaluateShadingModelAmbient( int index, DesertAmbient A, DesertSurface S, "
                  "DesertPayload P )",
                  "EvaluateAmbient", "A, S, P" );

        // The shading word (ShadingModelContract.glslh). The word is written and read as a magnitude: its sign
        // is not one of these fields and is left to the pass.
        out += R"(
uint DesertShadingWordField( uint word, int firstBit, int bits )
{
    return ( word >> uint( firstBit ) ) & ( ( 1u << uint( bits ) ) - 1u );
}

float DesertQuantizePayloadFloat( float value )
{
    const float steps = float( ( 1u << uint( DESERT_SHADING_WORD_PAYLOAD_BITS ) ) - 1u );
    return round( clamp( value, 0.0, 1.0 ) * steps ) / steps;
}

DesertPayload DesertQuantizePayload( DesertPayload P )
{
    DesertPayload q;
    q.CustomData0 = DesertQuantizePayloadFloat( P.CustomData0 );
    q.CustomData1 = DesertQuantizePayloadFloat( P.CustomData1 );
    return q;
}

float DesertPackShadingWord( int index, float sampledTextureCount, DesertPayload P )
{
    const uint indexMax   = ( 1u << uint( DESERT_SHADING_WORD_INDEX_BITS ) ) - 1u;
    const uint textureMax = ( 1u << uint( DESERT_SHADING_WORD_TEXTURES_BITS ) ) - 1u;
    const uint payloadMax = ( 1u << uint( DESERT_SHADING_WORD_PAYLOAD_BITS ) ) - 1u;
    uint word = uint( clamp( index, 0, int( indexMax ) ) ) << uint( DESERT_SHADING_WORD_INDEX_FIRST_BIT );
    word |= min( uint( round( max( sampledTextureCount, 0.0 ) ) ), textureMax )
            << uint( DESERT_SHADING_WORD_TEXTURES_FIRST_BIT );
    word |= uint( round( clamp( P.CustomData0, 0.0, 1.0 ) * float( payloadMax ) ) )
            << uint( DESERT_SHADING_WORD_PAYLOAD0_FIRST_BIT );
    word |= uint( round( clamp( P.CustomData1, 0.0, 1.0 ) * float( payloadMax ) ) )
            << uint( DESERT_SHADING_WORD_PAYLOAD1_FIRST_BIT );
    return float( word );
}

uint DesertShadingWordBits( float shadingWord )
{
    return uint( round( abs( shadingWord ) ) );
}

int DesertShadingModelIndex( float shadingWord )
{
    return int( DesertShadingWordField( DesertShadingWordBits( shadingWord ), DESERT_SHADING_WORD_INDEX_FIRST_BIT,
                                        DESERT_SHADING_WORD_INDEX_BITS ) );
}

float DesertSampledTextureCount( float shadingWord )
{
    return float( DesertShadingWordField( DesertShadingWordBits( shadingWord ),
                                          DESERT_SHADING_WORD_TEXTURES_FIRST_BIT, DESERT_SHADING_WORD_TEXTURES_BITS ) );
}

DesertPayload DesertUnpackPayload( float shadingWord )
{
    const uint  word  = DesertShadingWordBits( shadingWord );
    const float steps = float( ( 1u << uint( DESERT_SHADING_WORD_PAYLOAD_BITS ) ) - 1u );
    DesertPayload P;
    P.CustomData0 = float( DesertShadingWordField( word, DESERT_SHADING_WORD_PAYLOAD0_FIRST_BIT,
                                                   DESERT_SHADING_WORD_PAYLOAD_BITS ) ) / steps;
    P.CustomData1 = float( DesertShadingWordField( word, DESERT_SHADING_WORD_PAYLOAD1_FIRST_BIT,
                                                   DESERT_SHADING_WORD_PAYLOAD_BITS ) ) / steps;
    return P;
}

#endif
)";
        return out;
    }
} // namespace Desert::Core::ShadingModels
