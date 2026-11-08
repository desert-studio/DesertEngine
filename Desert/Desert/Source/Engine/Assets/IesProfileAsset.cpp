#include <Engine/Assets/IesProfileAsset.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

namespace Desert::Assets
{
    IesProfileAsset::IesProfileAsset( const Common::Filepath& filepath )
         : AssetBase( filepath, AssetTypeID::IesProfile )
    {
        if ( const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath );
             !identity.Guid.IsNull() )
        {
            m_Guid = identity.Guid;
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
        }
    }

    Common::ResultStr<IesProfileAsset::Parsed> IesProfileAsset::ReadFile( const std::filesystem::path& dies )
    {
        const std::filesystem::path file = ContentRegistry::FileToOpen( dies );

        std::string text;
        if ( const auto packed =
                  Common::Utils::VFS::Exists( file ) ? Common::Utils::VFS::ReadFile( file ) : std::nullopt;
             packed.has_value() )
        {
            text = packed.value();
        }
        else if ( auto read = Common::Utils::FileSystem::ReadFileContent( file ); read )
        {
            text = read.ExtractValue();
        }
        if ( text.empty() )
            return Common::MakeFormattedError<Parsed>( "IES profile '{}' is empty or could not be opened",
                                                       file.string() );

        auto parsed = Serialization::ParseIesProfileAsset( text );
        if ( !parsed )
            return Common::MakeFormattedError<Parsed>( "IES profile '{}' is not usable: {}", file.string(),
                                                       parsed.GetError() );
        Parsed out;
        out.Data = parsed.ExtractValue();
        if ( out.Data.Header )
            if ( const auto guid = Common::Content::AssetGuidFromText( out.Data.Header->Guid ); guid )
                out.Guid = guid.GetValue();
        if ( out.Guid.IsNull() )
            return Common::MakeFormattedError<Parsed>( "IES profile '{}' states no header GUID", file.string() );
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::BoolResultStr IesProfileAsset::LoadFromFile()
    {
        auto read = ReadFile( m_Metadata.Filepath );
        if ( !read )
        {
            m_Ready = false;
            return Common::MakeError<bool>( read.GetError() );
        }
        Parsed parsed = read.ExtractValue();
        m_Guid        = parsed.Guid;
        m_Data        = std::move( parsed.Data );
        m_Ready       = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr IesProfileAsset::Unload()
    {
        m_Data  = Serialization::IesProfileData{};
        m_Ready = false;
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
