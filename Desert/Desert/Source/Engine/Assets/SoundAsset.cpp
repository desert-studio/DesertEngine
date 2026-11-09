#include <Engine/Assets/SoundAsset.hpp>

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Json/Json.hpp>

namespace Desert::Assets
{

    SoundAsset::SoundAsset( const Common::Filepath& filepath ) : AssetBase( filepath, AssetTypeID::Sound )
    {
        if ( const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath );
             !identity.Guid.IsNull() )
        {
            m_Guid = identity.Guid;
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
        }
    }

    Common::ResultStr<Audio::SoundSource> SoundAsset::Resolve( const Common::Content::AssetGuid& guid )
    {
        if ( guid.IsNull() )
            return Common::MakeError<Audio::SoundSource>( "no sound: the null GUID names none" );
        return Resolve( Common::Content::HandleForGuid( guid ) );
    }

    Common::ResultStr<Audio::SoundSource> SoundAsset::Resolve( AssetHandle handle )
    {
        using Result = Audio::SoundSource;
        if ( static_cast<uint64_t>( handle ) == 0 )
            return Common::MakeError<Result>( "no sound: the slot is unset" );
        const Common::Utils::AssetRegistry&      registry = ContentRegistry::Get();
        const Common::Utils::AssetRegistryEntry* row      = registry.FindByHandle( handle );
        if ( row == nullptr )
            return Common::MakeFormattedError<Result>( "sound (handle {}) is in no registry row",
                                                       static_cast<uint64_t>( handle ) );
        auto followed = registry.FollowRedirectors( *row );
        if ( !followed )
            return Common::MakeFormattedError<Result>( "sound '{}': {}", row->Key, followed.GetError() );
        const std::filesystem::path desound = Common::AssetHandle::PathForStableKey( followed.GetValue()->Key );
        const auto                  parsed  = ReadFile( desound );
        if ( !parsed )
            return Common::MakeError<Result>( parsed.GetError() );
        const Parsed& sound = parsed.GetValue();
        return Common::MakeSuccess(
             Result{ ( desound.parent_path() / sound.Source ).string(), sound.Class, sound.Loading } );
    }

    Common::ResultStr<SoundAsset::Parsed> SoundAsset::ReadFile( const std::filesystem::path& desound )
    {
        const std::string text = ReadTextForIdentity( ContentRegistry::FileToOpen( desound ) );
        if ( text.empty() )
            return Common::MakeFormattedError<Parsed>( "sound '{}' is empty or could not be opened",
                                                       desound.string() );
        auto parsed = Parse( text );
        if ( !parsed )
            return Common::MakeFormattedError<Parsed>( "sound '{}': {}", desound.string(), parsed.GetError() );
        return parsed;
    }

    Common::BoolResultStr SoundAsset::LoadFromFile()
    {
        const std::filesystem::path file = ContentRegistry::FileToOpen( m_Metadata.Filepath );
        const std::string           text = ReadTextForIdentity( file );
        if ( text.empty() )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "sound '{}' is empty or could not be opened", file.string() );
        }
        auto parsed = Parse( text );
        if ( !parsed )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "sound '{}': {}", file.string(), parsed.GetError() );
        }
        m_Guid       = parsed.GetValue().Guid;
        m_SourceFile = m_Metadata.Filepath.parent_path() / parsed.GetValue().Source;
        m_Class      = parsed.GetValue().Class;
        m_Loading    = parsed.GetValue().Loading;
        m_Ready      = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SoundAsset::Unload()
    {
        m_Ready = false;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SoundAsset::Save( const Common::Filepath&           filepath,
                                            const Common::Content::AssetGuid& guid, std::string_view source,
                                            Audio::SoundClass soundClass, Audio::SoundLoading loading )
    {
        const auto text = Write( guid, source, soundClass, loading );
        if ( !text )
            return Common::MakeFormattedError<bool>( "sound '{}' was not written: {}", filepath.string(),
                                                     text.GetError() );
        const auto written = Common::Content::WriteCanonicalJsonFileAtomic( filepath, text.GetValue() );
        if ( !written )
            return Common::MakeFormattedError<bool>( "sound '{}' was not written: {}", filepath.string(),
                                                     written.GetError() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
