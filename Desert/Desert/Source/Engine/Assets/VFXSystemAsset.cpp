#include <Engine/Assets/VFXSystemAsset.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <filesystem>

namespace Desert::Assets
{
    VFXSystemAsset::VFXSystemAsset( const Common::Filepath& filepath )
         : AssetBase( filepath, AssetTypeID::VFXSystem )
    {
        if ( const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath );
             !identity.Guid.IsNull() )
        {
            m_Guid = identity.Guid;
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
        }
    }

    Common::BoolResultStr VFXSystemAsset::LoadFromFile()
    {
        const std::filesystem::path file = ContentRegistry::FileToOpen( m_Metadata.Filepath );
        const std::string           path = file.string();

        std::string text;
        if ( const auto packed =
                  Common::Utils::VFS::Exists( file ) ? Common::Utils::VFS::ReadFile( file ) : std::nullopt;
             packed.has_value() )
            text = packed.value();
        else if ( auto read = Common::Utils::FileSystem::ReadFileContent( file ); read )
            text = read.ExtractValue();

        if ( text.empty() )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "VFX system '{}' is empty or could not be opened", path );
        }
        const std::filesystem::path registerPath = Common::Constants::Path::CurrentProjectRoot().ProjectDir /
                                                   "Config" / Serialization::kVFXCategoriesFileName;
        auto categories = Serialization::ReadVFXCategories( registerPath );
        if ( !categories )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "VFX system '{}': {}", path, categories.GetError() );
        }
        auto parsed = Serialization::ParseVFXSystem( text, categories.GetValue() );
        if ( !parsed )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "VFX system '{}' is not usable: {}", path,
                                                     parsed.GetError() );
        }
        m_Data = parsed.ExtractValue();
        if ( m_Data.Header )
            if ( const auto guid = Common::Content::AssetGuidFromText( m_Data.Header->Guid ); guid )
                m_Guid = guid.GetValue();
        m_Ready = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VFXSystemAsset::Unload()
    {
        m_Data  = Serialization::VFXSystemData{};
        m_Ready = false;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VFXSystemAsset::Save( const Common::Filepath&             filepath,
                                                const Serialization::VFXSystemData& data )
    {
        if ( auto ok = Serialization::SaveVFXSystemFile( filepath, data ); !ok )
            return ok;
        LOG_INFO( "[VFX] System written: '{}', {} emitter(s).", filepath.string(), data.Emitters.size() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
