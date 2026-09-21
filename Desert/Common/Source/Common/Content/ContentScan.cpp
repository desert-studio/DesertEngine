#include <Common/Content/ContentScan.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>

namespace Common::Content
{
    namespace
    {
        std::string LowerExtension( const std::filesystem::path& file )
        {
            std::string ext = file.extension().string();
            std::transform( ext.begin(), ext.end(), ext.begin(),
                            []( unsigned char c ) { return static_cast<char>( ::tolower( c ) ); } );
            return ext;
        }

        // The remedy, spelled once. Every disagreement below ends with it, because a gate that names a
        // problem without naming the command that fixes it is a gate people learn to disable.
        constexpr const char* kRemedy =
             " Fix: `cd Editor && ../build/Bin/Debug/AssetRegistryTool cook Desert.deproj`, then commit "
             "Editor/Cooked/AssetRegistry.dreg.";
    } // namespace

    std::map<std::string, ContentFile> ScanContentRoots()
    {
        std::map<std::string, ContentFile> found;

        for ( std::size_t i = 0; i < CONTENT_KIND_COUNT; ++i )
        {
            const auto            kind = static_cast<ContentKind>( i );
            const ContentKindSpec spec = KindSpec( kind );

            // Through `ListFilesRecursive` and not a bare iterator, because that primitive is the one
            // that also sees what a mounted `.dpak` holds — a packaged game's content directories do
            // not exist on disk at all. The font and icon services each hand-rolled the disk half once,
            // and a packaged game scanned nothing.
            for ( const std::filesystem::path& candidate : Utils::FileSystem::ListFilesRecursive( *spec.Root ) )
            {
                if ( LowerExtension( candidate ) != spec.Extension )
                    continue;

                const std::string key = AssetHandle::StableKeyForPath( candidate );
                if ( key.empty() )
                    continue;

                found.emplace( key, ContentFile{ kind, Utils::FileSystem::GetFileSize( candidate ) } );
            }
        }
        return found;
    }

    std::vector<RegistryDisagreement> CompareWithDisk( const Utils::AssetRegistry&               registry,
                                                       const std::map<std::string, ContentFile>& onDisk )
    {
        std::vector<RegistryDisagreement> problems;

        for ( const auto& [key, file] : onDisk )
        {
            const std::string kindName( KindName( file.Kind ) );

            const Utils::AssetRegistryEntry* row = registry.FindByKey( key );
            if ( !row )
            {
                problems.push_back(
                     { RegistryDisagreement::Kind::MissingRow, key,
                       "'" + key + "' (" + kindName +
                            ") is on disk and has no row in the cooked asset registry. Since the boot "
                            "stopped walking the content roots this file DOES NOT REACH THE ENGINE: no "
                            "preload, no picker, and not one byte of it in a packaged build — while the "
                            "editor session that added it looked entirely normal." +
                            kRemedy } );
                continue;
            }

            if ( row->Kind != kindName )
            {
                problems.push_back( { RegistryDisagreement::Kind::WrongKind, key,
                                      "'" + key + "' is recorded as a '" + row->Kind + "' and is a '" +
                                           kindName + "' on disk, so the loader would build it with the "
                                                      "wrong asset class." +
                                           kRemedy } );
            }

            if ( row->Size != file.Size )
            {
                problems.push_back(
                     { RegistryDisagreement::Kind::StaleSize, key,
                       "'" + key + "' is recorded at " + std::to_string( row->Size ) + " bytes and is " +
                            std::to_string( file.Size ) +
                            " on disk, so the registry predates an edit. For a `.tex` or a `.demat` that "
                            "means the identity column may name a number the file no longer carries, "
                            "which is a reference resolving to nothing." +
                            kRemedy } );
            }
        }

        for ( const Utils::AssetRegistryEntry& row : registry.Entries() )
        {
            if ( onDisk.find( row.Key ) != onDisk.end() )
                continue;

            problems.push_back(
                 { RegistryDisagreement::Kind::OrphanRow, row.Key,
                   "the registry has a row for '" + row.Key +
                        "' and no such file is on disk, so the loader will try to read it once per boot "
                        "for ever and log a failure naming a file the reader cannot find." +
                        kRemedy } );
        }

        return problems;
    }
} // namespace Common::Content
