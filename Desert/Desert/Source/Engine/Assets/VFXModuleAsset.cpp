#include <Engine/Assets/VFXModuleAsset.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <algorithm>
#include <tuple>

namespace Desert::Assets
{
    VFXModuleAsset::VFXModuleAsset( const Common::Filepath& filepath )
         : AssetBase( filepath, AssetTypeID::VFXModule )
    {
        if ( const TextAssetIdentity identity =
                  ReadTextAssetIdentity( m_Metadata.Filepath, &ReadShaderHeaderGuid );
             !identity.Guid.IsNull() )
        {
            m_Guid = identity.Guid;
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
        }
    }

    Common::ResultStr<std::string> VFXModuleAsset::ReadText( const std::filesystem::path& file )
    {
        std::string text;
        if ( const auto packed =
                  Common::Utils::VFS::Exists( file ) ? Common::Utils::VFS::ReadFile( file ) : std::nullopt;
             packed.has_value() )
            text = packed.value();
        else if ( auto read = Common::Utils::FileSystem::ReadFileContent( file ); read )
            text = read.ExtractValue();
        if ( text.empty() )
            return Common::MakeFormattedError<std::string>( "VFX module '{}' is empty or could not be opened",
                                                            file.string() );
        return Common::MakeSuccess( std::move( text ) );
    }

    Common::BoolResultStr VFXModuleAsset::LoadFromFile()
    {
        const std::filesystem::path file = ContentRegistry::FileToOpen( m_Metadata.Filepath );
        auto                        text = ReadText( file );
        if ( !text )
        {
            m_Ready = false;
            return Common::MakeError<bool>( text.GetError() );
        }
        auto parsed = VFX::ParseVFXModule( text.GetValue(), file.string() );
        if ( !parsed )
        {
            m_Ready = false;
            return Common::MakeError<bool>( parsed.GetError() );
        }
        m_Definition = parsed.ExtractValue();
        m_Guid       = m_Definition.Guid;
        m_Ready      = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VFXModuleAsset::Unload()
    {
        m_Definition = VFX::VFXModuleDefinition{};
        m_Ready      = false;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VFXModuleAsset::Save( const Common::Filepath&         filepath,
                                                const VFX::VFXModuleDefinition& module )
    {
        if ( auto valid = VFX::ValidateVFXModule( module ); !valid )
            return Common::MakeFormattedError<bool>( "refusing to write VFX module '{}': {}", filepath.string(),
                                                     valid.GetError() );
        const std::string text = VFX::WriteVFXModule( module );
        std::error_code   ec;
        if ( filepath.has_parent_path() )
            std::filesystem::create_directories( filepath.parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError<bool>( "cannot create '{}' for VFX module: {}",
                                                     filepath.parent_path().string(), ec.message() );
        if ( auto ok = Common::Utils::FileSystem::WriteContentToFileAtomic( filepath, text ); !ok )
            return ok;
        // The registry records the write: VFXSystemService reissues every system that uses this module, so the
        // particle world recompiles it (ContentRegistry::WrittenSince).
        ContentRegistry::NoteFile( filepath );
        LOG_INFO( "[VFX] Module written: '{}' ({}).", filepath.string(), module.DisplayName );
        return BOOLSUCCESS;
    }

    Common::ResultStr<VFX::VFXModuleDefinition>
    RegistryVFXModuleResolver::Resolve( const Common::Content::AssetGuid& guid ) const
    {
        using Result                       = VFX::VFXModuleDefinition;
        const uint64_t              handle = static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) );
        const std::string           key    = ContentRegistry::KeyForHandle( handle );
        const std::filesystem::path file   = Common::AssetHandle::PathForStableKey( key );
        if ( ContentRegistry::KindForFile( file ) != Common::Content::ContentKind::VFXModule )
            return Common::MakeFormattedError<Result>(
                 "GUID {} names no VFX module in the content registry ('{}')",
                 Common::Content::AssetGuidToText( guid ), key );
        auto text = VFXModuleAsset::ReadText( ContentRegistry::FileToOpen( file ) );
        if ( !text )
            return Common::MakeError<Result>( text.GetError() );
        auto parsed = VFX::ParseVFXModule( text.GetValue(), file.string() );
        if ( !parsed )
            return Common::MakeError<Result>( parsed.GetError() );
        if ( parsed.GetValue().Guid != guid )
            return Common::MakeFormattedError<Result>(
                 "'{}' is registered under GUID {} but states {}", file.string(),
                 Common::Content::AssetGuidToText( guid ),
                 Common::Content::AssetGuidToText( parsed.GetValue().Guid ) );
        return parsed;
    }

    Common::ResultStr<std::vector<VFX::VFXModuleInfo>> ReadVFXModuleCatalog()
    {
        using Result = std::vector<VFX::VFXModuleInfo>;
        const RegistryVFXModuleResolver resolver;
        Result                          catalog;
        for ( const ContentRegistry::PickerRow& row : ContentRegistry::Rows( Common::Content::ContentKind::VFXModule ) )
        {
            if ( !row.Guid )
                return Common::MakeFormattedError<Result>( "VFX module '{}' states no GUID in its header", row.Key );
            auto module = resolver.Resolve( *row.Guid );
            if ( !module )
                return Common::MakeFormattedError<Result>( "VFX module '{}': {}", row.Key, module.GetError() );
            catalog.push_back( VFX::MakeModuleInfo( module.GetValue(), row.Key ) );
        }
        std::ranges::sort( catalog, []( const VFX::VFXModuleInfo& a, const VFX::VFXModuleInfo& b )
                           { return std::tie( a.Category, a.DisplayName ) < std::tie( b.Category, b.DisplayName ); } );
        return Common::MakeSuccess( std::move( catalog ) );
    }
} // namespace Desert::Assets
