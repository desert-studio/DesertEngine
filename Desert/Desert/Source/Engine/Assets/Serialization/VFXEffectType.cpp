#include <Engine/Assets/Serialization/VFXEffectType.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Json/Json.hpp>

namespace Desert::Assets::Serialization
{
    Common::BoolResultStr ValidateVFXEffectTypeData( const VFXEffectTypeData& data )
    {
        if ( data.Levels.size() != kVFXEffectTypeLevelCount )
            return Common::MakeFormattedError<bool>(
                 "a VFX effect type states {} level(s); it states one per Effects level, {}", data.Levels.size(),
                 kVFXEffectTypeLevelCount );
        for ( std::size_t i = 0; i < data.Levels.size(); ++i )
        {
            const VFXEffectTypeLevel& l = data.Levels[i];
            if ( !( l.MaxDistance >= 0.0f ) )
                return Common::MakeFormattedError<bool>( "level {}: MaxDistance {} is negative", i,
                                                         l.MaxDistance );
            if ( !( l.SpawnCountScale >= 0.0f && l.SpawnCountScale <= 1.0f ) )
                return Common::MakeFormattedError<bool>( "level {}: SpawnCountScale {} is outside 0..1", i,
                                                         l.SpawnCountScale );
        }
        return BOOLSUCCESS;
    }

    Common::ResultStr<VFXEffectTypeData> ParseVFXEffectType( const std::string& text )
    {
        using Result = VFXEffectTypeData;
        if ( text.empty() )
            return Common::MakeFormattedError<Result>( "the file is empty" );

        if ( auto headed = Assets::RefuseTextWithoutHeader( text, kVFXEffectTypeVersion, std::nullopt ); !headed )
            return Common::MakeFormattedError<Result>( "VFX effect type {}", headed.GetError() );

        const auto parsed = Common::Json::Read<VFXEffectTypeData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<Result>( "{}", parsed.GetError() );
        VFXEffectTypeData data = parsed.GetValue();

        if ( auto header = Assets::CheckStatedHeader( data.Header, Common::Content::ContentKind::VFXEffectType,
                                                      Assets::kVFXEffectTypeSchemaTag, kVFXEffectTypeVersion,
                                                      VFXEffectTypeTextSubsystems() );
             !header )
            return Common::MakeFormattedError<Result>( "VFX effect type {}", header.GetError() );

        if ( auto valid = ValidateVFXEffectTypeData( data ); !valid )
            return Common::MakeFormattedError<Result>( "{}", valid.GetError() );

        // A type references no other asset, so a stated Dependency is an edge to nothing it names.
        if ( data.Header && !data.Header->Dependencies.empty() )
            return Common::MakeFormattedError<Result>(
                 "the header states {} Dependencies; a VFX effect type references no asset",
                 data.Header->Dependencies.size() );
        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteVFXEffectType( const VFXEffectTypeData& data )
    {
        VFXEffectTypeData out = data;
        out.Header            = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::VFXEffectType,
                                                         VFXEffectTypeTextSubsystems() );
        out.Header->Dependencies.clear();
        return Common::Json::Write( out );
    }

    Common::BoolResultStr SaveVFXEffectTypeFile( const std::filesystem::path& path, const VFXEffectTypeData& data )
    {
        if ( auto valid = ValidateVFXEffectTypeData( data ); !valid )
            return Common::MakeFormattedError<bool>( "refusing to write VFX effect type '{}': {}", path.string(),
                                                     valid.GetError() );
        std::error_code ec;
        if ( path.has_parent_path() )
            std::filesystem::create_directories( path.parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError<bool>( "cannot create '{}' for VFX effect type: {}",
                                                     path.parent_path().string(), ec.message() );
        return Common::Content::WriteCanonicalJsonFileAtomic( path, WriteVFXEffectType( data ) );
    }
} // namespace Desert::Assets::Serialization
