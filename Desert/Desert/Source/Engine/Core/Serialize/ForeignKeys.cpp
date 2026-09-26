#include <Engine/Core/Serialize/ForeignKeys.hpp>

#include <string>
#include <unordered_map>
#include <utility>

namespace Desert::Core::Serialize
{
    namespace
    {
        using Common::Json::Kind;
        using Common::Json::Node;
        using Common::Json::Object;
        using Common::Json::Value;

        // A record's identity as text: the decimal-string UUID every writer states, or the integer a
        // hand-written file may use. No id -> "", which matches nothing (such a record is written as fresh).
        std::string RecordIdentity( const Node& record )
        {
            const auto id = record.Find( "id" );
            if ( !id.has_value() )
                return {};
            if ( id->GetKind() == Kind::Integer )
                if ( const auto asInt = id->AsInteger(); asInt )
                    return std::to_string( asInt.GetValue() );
            if ( const auto asText = id->AsString(); asText )
                return asText.GetValue();
            return {};
        }

        // One level of the merge. The SOURCE's keys come first and in the source's order (that is what keeps a
        // file's bytes when nothing changed), then the keys only `fresh` states. `replacement`, when given,
        // is the value `replacedKey` takes wherever it sits - the entity array MergeSceneDocument built.
        Object MergeLevel( const Node& fresh, const Node& source, const KeyIsOurs& ours, std::string_view replacedKey,
                           const Value* replacement )
        {
            Object merged;
            source.ForEachMember(
                 [&]( std::string_view key, const Node& sourceValue )
                 {
                     const std::string name( key );
                     const auto        inFresh = fresh.Find( key );
                     if ( !inFresh.has_value() )
                     {
                         if ( !ours( name ) )
                             merged[name] = sourceValue.Raw();
                         return;
                     }
                     if ( replacement != nullptr && key == replacedKey )
                         merged[name] = *replacement;
                     else if ( inFresh->GetKind() == Kind::Object && sourceValue.GetKind() == Kind::Object )
                         merged[name] = MergeLevel( *inFresh, sourceValue, NothingIsOurs(), {}, nullptr );
                     else
                         merged[name] = inFresh->Raw();
                 } );

            fresh.ForEachMember(
                 [&]( std::string_view key, const Node& freshValue )
                 {
                     if ( source.Find( key ).has_value() )
                         return;
                     merged[std::string( key )] =
                          ( replacement != nullptr && key == replacedKey ) ? *replacement : freshValue.Raw();
                 } );
            return merged;
        }
    } // namespace

    KeyIsOurs NothingIsOurs()
    {
        return []( const std::string& ) { return false; };
    }

    Object MergeObjects( const Node& fresh, const Node& source, const KeyIsOurs& ours )
    {
        return MergeLevel( fresh, source, ours, {}, nullptr );
    }

    Object MergeSceneDocument( const Node& fresh, const Node& source, const KeyIsOurs& entityKeyIsOurs )
    {
        const auto freshEntities  = fresh.Find( "Entities" );
        const auto sourceEntities = source.Find( "Entities" );
        if ( !freshEntities.has_value() || !sourceEntities.has_value() || freshEntities->GetKind() != Kind::Array ||
             sourceEntities->GetKind() != Kind::Array )
            return MergeLevel( fresh, source, NothingIsOurs(), {}, nullptr );

        // By id, first claimant wins: a duplicate id loads onto the first record (SceneStitchRules.hpp), so
        // the first is the one the fresh record with that id came from.
        std::unordered_map<std::string, Node> sourceById;
        sourceEntities->ForEachElement(
             [&]( std::size_t, const Node& record )
             {
                 if ( record.GetKind() != Kind::Object )
                     return;
                 if ( std::string identity = RecordIdentity( record ); !identity.empty() )
                     sourceById.emplace( std::move( identity ), record );
             } );

        // A source record whose id is not in `fresh` is DROPPED - an entity the user deleted.
        Value::Array merged;
        freshEntities->ForEachElement(
             [&]( std::size_t, const Node& record )
             {
                 if ( record.GetKind() != Kind::Object )
                 {
                     merged.push_back( record.Raw() );
                     return;
                 }
                 const std::string identity = RecordIdentity( record );
                 const auto        match    = identity.empty() ? sourceById.end() : sourceById.find( identity );
                 if ( match == sourceById.end() )
                     merged.push_back( record.Raw() );
                 else
                     merged.push_back( Value( MergeLevel( record, match->second, entityKeyIsOurs, {}, nullptr ) ) );
             } );

        const Value entities( std::move( merged ) );
        return MergeLevel( fresh, source, NothingIsOurs(), "Entities", &entities );
    }

    void CountForeignKeysAtLevel( const Node& source, const KeyIsOurs& ours, std::map<std::string, int>& into )
    {
        source.ForEachMember(
             [&]( std::string_view key, const Node& )
             {
                 std::string name( key );
                 if ( !ours( name ) )
                     ++into[std::move( name )];
             } );
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
