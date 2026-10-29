#include <Engine/Assets/Serialization/WorldParameters.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Json/Json.hpp>

#include <cmath>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace Desert::Assets::Serialization
{
    Common::BoolResultStr ValidateWorldParametersData( const WorldParametersData& data )
    {
        std::unordered_set<std::string>           names;
        std::unordered_map<uint32_t, std::string> keys;
        for ( size_t i = 0; i < data.Parameters.size(); ++i )
        {
            const WorldParameterData& p = data.Parameters[i];
            if ( !IsWorldParameterName( p.Name ) )
                return Common::MakeFormattedError<bool>( "Parameters[{}] name '{}' is not an identifier", i,
                                                         p.Name );
            if ( !names.insert( p.Name ).second )
                return Common::MakeFormattedError<bool>( "Parameters[{}] name '{}' is used twice", i, p.Name );
            const uint32_t key = WorldParameterKey( p.Name );
            if ( key == 0u )
                return Common::MakeFormattedError<bool>( "Parameters[{}] name '{}' hashes to 0, the empty slot", i,
                                                         p.Name );
            if ( const auto [it, inserted] = keys.emplace( key, p.Name ); !inserted )
                return Common::MakeFormattedError<bool>(
                     "Parameters[{}] names '{}' and '{}' share the key {:#010x}", i, it->second, p.Name, key );
            for ( size_t c = 0; c < p.Default.size(); ++c )
                if ( !std::isfinite( p.Default[c] ) )
                    return Common::MakeFormattedError<bool>( "Parameters[{}] '{}' default[{}] {} is not finite", i,
                                                             p.Name, c, p.Default[c] );
            if ( p.Type == WorldParameterType::Scalar &&
                 ( p.Default[1] != 0.0f || p.Default[2] != 0.0f || p.Default[3] != 0.0f ) )
                return Common::MakeFormattedError<bool>(
                     "Parameters[{}] '{}' is a scalar but states yzw ({}, {}, {})", i, p.Name, p.Default[1],
                     p.Default[2], p.Default[3] );
            if ( ( p.Type == WorldParameterType::Texture ) == p.Texture.empty() )
                return Common::MakeFormattedError<bool>(
                     p.Texture.empty() ? "Parameters[{}] '{}' is a texture but names no Texture"
                                       : "Parameters[{}] '{}' names a Texture but is not a texture parameter",
                     i, p.Name );
        }
        return BOOLSUCCESS;
    }

    Common::ResultStr<WorldParametersData> ParseWorldParameters( const std::string& text )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<WorldParametersData>( "the file is empty" );

        if ( auto headed = Assets::RefuseTextWithoutHeader( text, kWorldParametersVersion, std::nullopt );
             !headed )
            return Common::MakeFormattedError<WorldParametersData>( "world parameters {}", headed.GetError() );

        const auto parsed = Common::Json::Read<WorldParametersData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<WorldParametersData>( "{}", parsed.GetError() );
        WorldParametersData data = parsed.GetValue();

        if ( auto header = Assets::CheckStatedHeader( data.Header, Common::Content::ContentKind::WorldParameters,
                                                      Assets::kWorldParametersSchemaTag, kWorldParametersVersion,
                                                      WorldParametersTextSubsystems() );
             !header )
            return Common::MakeFormattedError<WorldParametersData>( "world parameters {}", header.GetError() );

        if ( auto valid = ValidateWorldParametersData( data ); !valid )
            return Common::MakeFormattedError<WorldParametersData>( "{}", valid.GetError() );

        // A collection references no other asset, so a stated Dependency is an edge the registry would follow to
        // something this file never names.
        if ( data.Header && !data.Header->Dependencies.empty() )
            return Common::MakeFormattedError<WorldParametersData>(
                 "the header states {} Dependencies; a world parameter collection references no asset",
                 data.Header->Dependencies.size() );

        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteWorldParameters( const WorldParametersData& data )
    {
        WorldParametersData out = data;
        out.Header = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::WorldParameters,
                                              WorldParametersTextSubsystems() );
        out.Header->Dependencies.clear();
        return Common::Json::Write( out );
    }

    Common::BoolResultStr SaveWorldParametersFile( const std::filesystem::path& path,
                                                   const WorldParametersData&   data )
    {
        if ( auto valid = ValidateWorldParametersData( data ); !valid )
            return Common::MakeFormattedError<bool>( "refusing to write world parameters '{}': {}", path.string(),
                                                     valid.GetError() );
        std::error_code ec;
        if ( path.has_parent_path() )
            std::filesystem::create_directories( path.parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError<bool>( "cannot create '{}' for world parameters: {}",
                                                     path.parent_path().string(), ec.message() );
        return Common::Content::WriteCanonicalJsonFileAtomic( path, WriteWorldParameters( data ) );
    }
} // namespace Desert::Assets::Serialization
