#include "StringTableSource.hpp"

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/StringTableAsset.hpp>

#include <Engine/Localization/LocalizationService.hpp>

#include <Common/Core/Logger.hpp>

#include <filesystem>
#include <map>
#include <unordered_set>

namespace Desert::Assets
{
    namespace
    {
        // THE ONE PROJECT'S TABLE SOURCE: which file belongs to which language (from the registry, no file
        // opened), which languages have been asked for, and the live requests - the request handle is what
        // keeps a table's payload alive until its completion publishes it.
        struct StringTableSource
        {
            std::weak_ptr<AssetManager>                               Assets;
            std::map<std::string, std::vector<std::filesystem::path>> FilesByLanguage;
            std::unordered_set<std::string>                           RequestedLanguages;
            std::map<std::string, LoadRequest>                        Requests;
        };

        StringTableSource& Source()
        {
            static StringTableSource source;
            return source;
        }

        void RequestLanguage( const Localization::LocaleRow& language )
        {
            StringTableSource& source = Source();
            const std::string  tag( language.Tag );
            // Asked once per project: a language already requested is published (or refused, by name) and
            // stays in the lookup, so switching back to it is instant.
            if ( !source.RequestedLanguages.insert( tag ).second )
                return;
            const auto manager = source.Assets.lock();
            const auto files   = source.FilesByLanguage.find( tag );
            if ( !manager || files == source.FilesByLanguage.end() )
                return;
            for ( const std::filesystem::path& file : files->second )
            {
                auto shell = manager->CreateAsset<StringTableAsset>( file,
                                                                     /*loadAfterCreate=*/false );
                if ( !shell )
                {
                    LOG_ERROR( "[Localization] String table '{}' ({}) could not be created; its keys resolve as "
                               "missing",
                               file.generic_string(), tag );
                    continue;
                }
                const std::string id = file.generic_string();
                // clang-tidy's bugprone-exception-escape blames the closures' implicit copies, which
                // std::function needs; the bodies log and erase and throw nothing.
                // NOLINTBEGIN(bugprone-exception-escape)
                LoadRequest request = AsyncAssetLoader::Get().Request(
                     shell,
                     [id, tag]( const Asset<AssetBase>& loaded, const LoadOutcome outcome,
                                const std::string& error )
                     {
                         auto& localization = Localization::Localization::Get();
                         if ( outcome != LoadOutcome::Loaded )
                         {
                             LOG_ERROR(
                                  "[Localization] String table '{}' ({}) could not be read; its keys resolve "
                                  "as missing: {}",
                                  id, tag, error );
                         }
                         else if ( const auto published =
                                        std::static_pointer_cast<StringTableAsset>( loaded )->Publish();
                                   !published )
                         {
                             LOG_ERROR( "[Localization] {}", published.GetError() );
                         }
                         localization.TableSettled( tag );
                         Source().Requests.erase( id );
                     },
                     [id, tag]
                     {
                         Localization::Localization::Get().TableSettled( tag );
                         Source().Requests.erase( id );
                     } );
                // NOLINTEND(bugprone-exception-escape)
                if ( !request.IsValid() )
                    continue;
                Localization::Localization::Get().TableRequested( tag );
                source.Requests.insert_or_assign( id, std::move( request ) );
            }
        }
    } // namespace

    Common::BoolResultStr BeginStringTables( const std::weak_ptr<AssetManager>& assets )
    {
        EndStringTables();
        StringTableSource& source = Source();
        source.Assets             = assets;

        std::string refused;
        for ( const std::filesystem::path& file :
              ContentRegistry::FilesOfKind( Common::Content::ContentKind::StringTable ) )
        {
            const auto language = Localization::StringTableLanguageOf( file );
            if ( !language )
            {
                refused += ( refused.empty() ? "" : "; " ) + language.GetError();
                continue;
            }
            source.FilesByLanguage[std::string( language.GetValue()->Tag )].push_back( file );
        }

        std::vector<std::string> languages;
        languages.reserve( source.FilesByLanguage.size() );
        for ( const auto& [tag, files] : source.FilesByLanguage )
            languages.push_back( tag );
        auto& localization = Localization::Localization::Get();
        localization.BindTableSource( std::move( languages ), &RequestLanguage );
        // ONLY THE LANGUAGE ASKED FOR: a boot reads one language's files, not every translation the project
        // carries. Through SetLanguage, so a switch made before this stage (--language) is the one requested.
        if ( const auto set = localization.SetLanguage( localization.RequestedLanguage().Tag ); !set )
            return set;
        if ( !refused.empty() )
            return Common::MakeFormattedError<bool>( "{}", refused );
        return BOOLSUCCESS;
    }

    void EndStringTables()
    {
        StringTableSource& source = Source();
        for ( auto& [id, request] : source.Requests )
            request.Release();
        source.Requests.clear();
        source.RequestedLanguages.clear();
        source.FilesByLanguage.clear();
        source.Assets.reset();
        Localization::Localization::Get().BindTableSource( {}, nullptr );
    }
} // namespace Desert::Assets
