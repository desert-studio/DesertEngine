#include "VFXSystemService.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/RegistryDiscovery.hpp>

#include <Common/Content/TextAssetHeader.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <set>
#include <string>

namespace Desert::Runtime
{
    void VFXSystemService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_Assets = assets;
    }

    void VFXSystemService::Adopt( const Assets::AssetHandle&                   handle,
                                  const Assets::Asset<Assets::VFXSystemAsset>& asset )
    {
        m_Ready[handle] = Ready{ asset->Snapshot(), asset->Snapshot() };
    }

    void VFXSystemService::Request( const Assets::AssetHandle& handle )
    {
        if ( m_Ready.contains( handle ) || m_Failed.contains( handle ) || m_Requests.contains( handle ) )
            return;

        const auto fail = [this, handle]( const std::string& why )
        {
            LOG_ERROR( "[VFX] System {} cannot be read: {}", static_cast<uint64_t>( handle ), why );
            m_Failed[handle] = why;
        };

        auto created = Assets::CreateFromRegistryRow<Assets::VFXSystemAsset>(
             m_Assets, handle, Common::Content::ContentKind::VFXSystem );
        if ( !created )
        {
            fail( created.GetError() );
            return;
        }
        if ( created.GetValue()->IsReadyForUse() )
        {
            Adopt( handle, created.GetValue() );
            return;
        }
        m_Requests[handle] = Assets::AsyncAssetLoader::Get().Request(
             created.GetValue(),
             [this, handle, fail]( const Assets::Asset<Assets::AssetBase>& loaded,
                                   const Assets::LoadOutcome outcome, const std::string& error )
             {
                 m_Requests.erase( handle );
                 if ( outcome == Assets::LoadOutcome::Loaded )
                     Adopt( handle, std::static_pointer_cast<Assets::VFXSystemAsset>( loaded ) );
                 else
                     fail( error );
             },
             [this, handle] { m_Requests.erase( handle ); } );
    }

    void VFXSystemService::ReissueOnModuleWrites()
    {
        if ( Assets::ContentRegistry::WriteSerial() == m_ModuleSerial )
            return;
        const auto written =
             Assets::ContentRegistry::WrittenSince( Common::Content::ContentKind::VFXModule, m_ModuleSerial );
        m_ModuleSerial = written.Serial;
        std::set<std::string> modules;
        for ( const auto& row : written.Rows )
            if ( row.Guid )
                modules.insert( Common::Content::AssetGuidToText( *row.Guid ) );
        if ( modules.empty() )
            return;
        for ( auto& [handle, ready] : m_Ready )
        {
            System& system = ready.Issued;
            if ( !system || !system->Header )
                continue;
            const auto& dependencies = system->Header->Dependencies;
            if ( std::any_of( dependencies.begin(), dependencies.end(),
                              [&]( const std::string& guid ) { return modules.contains( guid ); } ) )
            {
                LOG_INFO( "[VFX] System {} recompiles: a module it uses was written.",
                          static_cast<uint64_t>( handle ) );
                system = std::make_shared<const Assets::Serialization::VFXSystemData>( *system );
            }
        }
    }

    void VFXSystemService::FollowAssetEdits()
    {
        const uint64_t serial = Assets::VFXSystemAsset::EditSerial();
        if ( serial == m_EditSerial )
            return;
        m_EditSerial         = serial;
        const auto assetsPtr = m_Assets.lock();
        if ( !assetsPtr )
            return;
        for ( auto& [handle, ready] : m_Ready )
        {
            const auto asset = assetsPtr->FindByHandle<Assets::VFXSystemAsset>( handle );
            // Not held, or unloaded by the eviction sweep: the snapshot handed out stays what plays.
            if ( !asset || !asset->IsReadyForUse() || asset->Snapshot() == ready.Adopted )
                continue;
            ready = Ready{ asset->Snapshot(), asset->Snapshot() };
        }
    }

    Common::ResultStr<VFXSystemService::System> VFXSystemService::Get( const Assets::AssetHandle& handle )
    {
        FollowAssetEdits();
        ReissueOnModuleWrites();
        Request( handle );
        if ( const auto failed = m_Failed.find( handle ); failed != m_Failed.end() )
            return Common::MakeError<System>( failed->second );
        const auto found = m_Ready.find( handle );
        if ( found == m_Ready.end() )
            return Common::MakeSuccess( System{} );
        return Common::MakeSuccess( found->second.Issued );
    }

    Common::ResultStr<Assets::Asset<Assets::VFXEffectTypeAsset>>
    VFXSystemService::EffectType( const std::string& name )
    {
        using Result         = Assets::Asset<Assets::VFXEffectTypeAsset>;
        const auto assetsPtr = m_Assets.lock();
        if ( !assetsPtr )
            return Common::MakeFormattedError<Result>( "effect type '{}': no asset manager is bound", name );

        Result named;
        for ( const auto& [handle, held] : assetsPtr->FindAllByType<Assets::VFXEffectTypeAsset>() )
            if ( held && held->TypeName() == name )
                named = held;
        if ( !named )
        {
            // Not held yet: the content registry has a row for every `.dfxtype` under the VFX root.
            std::filesystem::path file;
            for ( const std::filesystem::path& row :
                  Assets::ContentRegistry::FilesOfKind( Common::Content::ContentKind::VFXEffectType ) )
            {
                if ( row.stem().string() != name )
                    continue;
                if ( !file.empty() )
                    return Common::MakeFormattedError<Result>( "two effect types are named '{}' ({} and {})", name,
                                                               file.generic_string(), row.generic_string() );
                file = row;
            }
            if ( file.empty() )
                return Common::MakeFormattedError<Result>(
                     "no effect type is named '{}' (no {} file of that stem in the content registry)", name,
                     Assets::Serialization::kVFXEffectTypeExtension );
            named = assetsPtr->CreateAsset<Assets::VFXEffectTypeAsset>( file );
            if ( !named )
                return Common::MakeFormattedError<Result>( "effect type '{}' ({}) could not be registered", name,
                                                           file.generic_string() );
        }
        if ( !named->IsReadyForUse() )
            if ( auto loaded = named->LoadFromFile(); !loaded )
                return Common::MakeFormattedError<Result>( "{}", loaded.GetError() );
        return Common::MakeSuccess( named );
    }

    void VFXSystemService::Clear()
    {
        m_ModuleSerial = 0;
        m_EditSerial   = 0;
        m_Requests.clear();
        m_Ready.clear();
        m_Failed.clear();
    }
} // namespace Desert::Runtime
