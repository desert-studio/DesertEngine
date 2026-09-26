#include <Engine/Core/Serialize/ForeignKeys.hpp>

#include <string>
#include <utility>

namespace Desert::Core::Serialize
{
    KeyIsOurs NothingIsOurs()
    {
        return []( const std::string& ) { return false; };
    }

    Common::Json::TextDocument MergeObjects( const Common::Json::TextDocument& fresh,
                                             const Common::Json::TextDocument& source, const KeyIsOurs& ours )
    {
        return Common::Json::MergeCarried( fresh, source, Common::Json::CarryRule{ .RootKeyIsOurs = ours } );
    }

    Common::Json::TextDocument MergeSceneDocument( const Common::Json::TextDocument& fresh,
                                                   const Common::Json::TextDocument& source,
                                                   const KeyIsOurs&                  entityKeyIsOurs )
    {
        // The top level answers to NothingIsOurs (every top-level member SceneSerialized states it always
        // writes); `Entities` is the array whose records have identity - matched on `id`, first claimant
        // wins, as a duplicate id loads onto the first record (SceneStitchRules.hpp).
        return Common::Json::MergeCarried( fresh, source,
                                           Common::Json::CarryRule{ .RootKeyIsOurs   = NothingIsOurs(),
                                                                    .RecordArray     = "Entities",
                                                                    .RecordId        = "id",
                                                                    .RecordKeyIsOurs = entityKeyIsOurs } );
    }

    void CountForeignKeysAtLevel( const std::vector<std::string>& keys, const KeyIsOurs& ours,
                                  std::map<std::string, int>& into )
    {
        for ( const std::string& key : keys )
            if ( !ours( key ) )
                ++into[key];
    }

    std::string DescribeForeignKeys( const std::map<std::string, int>& counted )
    {
        std::string described;
        for ( const auto& [key, count] : counted )
        {
            if ( !described.empty() )
                described += ", ";
            described += key;
            if ( count > 1 )
                described += " (x" + std::to_string( count ) + ")";
        }
        return described;
    }
} // namespace Desert::Core::Serialize
