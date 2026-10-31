#include <Engine/Assets/VFXSystemAsset.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <filesystem>
#include <utility>

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
        m_Data   = std::make_shared<const Serialization::VFXSystemData>( parsed.ExtractValue() );
        m_OnDisk = m_Data;
        if ( m_Data->Header )
            if ( const auto guid = Common::Content::AssetGuidFromText( m_Data->Header->Guid ); guid )
                m_Guid = guid.GetValue();
        m_Ready = true;
        s_EditSerial.fetch_add( 1, std::memory_order_acq_rel );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VFXSystemAsset::Unload()
    {
        // An edit lives only here until it is saved: dropping it is losing the person's work, not freeing memory.
        if ( HasUnsavedEdits() )
            return Common::MakeFormattedError<bool>( "VFX system '{}' has unsaved edits; it stays loaded",
                                                     m_Metadata.Filepath.string() );
        m_Data   = std::make_shared<const Serialization::VFXSystemData>();
        m_OnDisk = m_Data;
        m_Ready  = false;
        return BOOLSUCCESS;
    }

    void VFXSystemAsset::Edit( Serialization::VFXSystemData data )
    {
        m_Data = std::make_shared<const Serialization::VFXSystemData>( std::move( data ) );
        s_EditSerial.fetch_add( 1, std::memory_order_acq_rel );
    }

    bool VFXSystemAsset::HasUnsavedEdits() const
    {
        return m_Ready && m_Data != m_OnDisk && *m_Data != *m_OnDisk;
    }

    Common::BoolResultStr VFXSystemAsset::SaveEdits()
    {
        if ( !m_Ready )
            return Common::MakeFormattedError<bool>( "VFX system '{}' is not loaded; there is nothing to save",
                                                     m_Metadata.Filepath.string() );
        if ( auto saved = Save( m_Metadata.Filepath, *m_Data ); !saved )
            return saved;
        m_OnDisk = m_Data;
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
