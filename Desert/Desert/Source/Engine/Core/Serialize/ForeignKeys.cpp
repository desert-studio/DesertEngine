#include <Engine/Core/Serialize/ForeignKeys.hpp>

#include <Engine/Core/TextTemplate.hpp>

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
        Common::Json::CarryRule rule;
        rule.RootKeyIsOurs = ours;
        return Common::Json::MergeCarried( fresh, source, rule );
    }

    Common::Json::TextDocument MergeSceneDocument( const Common::Json::TextDocument&    fresh,
                                                   const Common::Json::TextDocument&    source,
                                                   const KeyIsOurs&                     entityKeyIsOurs,
                                                   const Common::Json::MemberKeyIsOurs& componentKeyIsOurs )
    {
        // The top level answers to NothingIsOurs (every top-level member SceneSerialized states it always
        // writes); `Entities` is the array whose records have identity - matched on `id`, first claimant
        // wins, as a duplicate id loads onto the first record (SceneStitchRules.hpp).
        return Common::Json::MergeCarried(
             fresh, source,
             Common::Json::CarryRule{ .RootKeyIsOurs         = NothingIsOurs(),
                                      .RecordArray           = "Entities",
                                      .RecordId              = "id",
                                      .RecordKeyIsOurs       = entityKeyIsOurs,
                                      .RecordMemberKeyIsOurs = componentKeyIsOurs } );
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
        static const TextTemplate kDescribed(
             "ForeignKeys", "{% for k in keys %}{% if not loop.first %}, {% endif %}{{ k.key }}"
                            "{% if k.count > 1 %} (x{{ k.count }}){% endif %}{% endfor %}" );
        Common::Json::Value::Array keys;
        for ( const auto& [key, count] : counted )
            keys.emplace_back( Common::Json::ObjectBuilder().Set( "key", key ).Set( "count", count ).Build() );
        return kDescribed.RenderOrLog( Common::Json::Value(
             Common::Json::ObjectBuilder().Set( "keys", Common::Json::Value( std::move( keys ) ) ).Build() ) );
    }
} // namespace Desert::Core::Serialize
