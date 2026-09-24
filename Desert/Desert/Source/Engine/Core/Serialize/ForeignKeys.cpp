#include <Engine/Core/Serialize/ForeignKeys.hpp>

#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

namespace Desert::Core::Serialize
{
    namespace
    {
        // The id a scene record is matched on across a merge. An entity is the only thing in a .desce
        // whose array position is not its identity — everything else about a record can change, and
        // the loader mints an id for any record that arrives without one, so by the time a document
        // has been through a load every record has one.
        std::string RecordIdentity( const rfl::Generic::Object& record )
        {
            if ( const auto id = record.get( "id" ); id.has_value() )
            {
                if ( const auto asInt = id.value().to_int64(); asInt.has_value() )
                    return std::to_string( asInt.value() );
                if ( const auto asText = id.value().to_string(); asText.has_value() )
                    return asText.value();
            }
            return {};
        }
    } // namespace

    KeyIsOurs NothingIsOurs()
    {
        return []( const std::string& ) { return false; };
    }

    rfl::Generic::Object MergeObjects( rfl::Generic::Object fresh, const rfl::Generic::Object& source,
                                       const KeyIsOurs& ours )
    {
        // BUILT IN THE SOURCE'S ORDER, and that is what makes the round trip byte-stable rather than
        // merely loss-free. Appending the foreign keys after the fresh ones would move them every time
        // a file written by another build was saved, so "read it and write it back" would rewrite the
        // whole file and no diff over a .desce would mean anything. Keys the source does not have are
        // appended afterwards in the writer's own order, which is the order a file converges to once
        // this build has saved it once.
        rfl::Generic::Object merged;

        for ( const auto& [key, sourceValue] : source )
        {
            const auto inFresh = fresh.get( key );
            if ( !inFresh.has_value() )
            {
                // The fresh tree does not state this key. Either it is not ours at all — foreign,
                // and preserved — or it is ours and its absence is a DELETION the user performed.
                if ( !ours( key ) )
                    merged[key] = sourceValue;
                continue;
            }

            // Present in both. Where both sides are objects the disagreement is one level down, so it
            // is settled there; anything else is this build's value, because this build owns the key.
            const auto freshObject  = inFresh.value().to_object();
            const auto sourceObject = sourceValue.to_object();
            if ( freshObject.has_value() && sourceObject.has_value() )
                merged[key] = MergeObjects( freshObject.value(), sourceObject.value(), NothingIsOurs() );
            else
                merged[key] = inFresh.value();
        }

        for ( const auto& [key, freshValue] : fresh )
            if ( !source.get( key ).has_value() )
                merged[key] = freshValue;

        return merged;
    }

    rfl::Generic::Object MergeSceneDocument( rfl::Generic::Object fresh, const rfl::Generic::Object& source,
                                             const KeyIsOurs& entityKeyIsOurs )
    {
        // The entities first, because the generic merge one line below would replace the array
        // wholesale — an array has no keys to reconcile, and only this function knows that THIS array's
        // elements have identity.
        const auto freshEntities  = fresh.get( "Entities" );
        const auto sourceEntities = source.get( "Entities" );
        if ( freshEntities.has_value() && sourceEntities.has_value() )
        {
            const auto freshArray  = freshEntities.value().to_array();
            const auto sourceArray = sourceEntities.value().to_array();
            if ( freshArray.has_value() && sourceArray.has_value() )
            {
                // THE SOURCE RECORDS ARE INDEXED ONCE, BY IDENTITY. This used to scan the whole source
                // array for every fresh record, and `to_object()` returns the record BY VALUE, so every
                // step of the scan copied a record: N x N/2 object copies. A 50 179-record world spent
                // 419 s in the Play snapshot on it (Release, 2026-09-24), and Save Scene pays the same.
                // The first record to claim an identity keeps it, as the scan's `break` did.
                std::unordered_map<std::string, const rfl::Generic::Object*> sourceById;
                sourceById.reserve( sourceArray.value().size() );
                for ( const auto& sourceRecord : sourceArray.value() )
                {
                    const auto* sourceObject = std::get_if<rfl::Generic::Object>( &sourceRecord.variant() );
                    if ( sourceObject == nullptr )
                        continue;
                    if ( std::string identity = RecordIdentity( *sourceObject ); !identity.empty() )
                        sourceById.emplace( std::move( identity ), sourceObject );
                }

                rfl::Generic::Array merged;
                merged.reserve( freshArray.value().size() );

                for ( const auto& freshRecord : freshArray.value() )
                {
                    const auto freshObject = freshRecord.to_object();
                    if ( !freshObject.has_value() )
                    {
                        merged.push_back( freshRecord );
                        continue;
                    }

                    // A record the writer produced that the source does not have is a NEW entity and
                    // has nothing to be merged with. A record the source has that the writer did not
                    // produce is a DELETED entity and is not carried over — which is why this walks
                    // the fresh array and looks the source up, and not the other way round.
                    const std::string           identity = RecordIdentity( freshObject.value() );
                    const rfl::Generic::Object* match    = nullptr;
                    if ( !identity.empty() )
                        if ( const auto found = sourceById.find( identity ); found != sourceById.end() )
                            match = found->second;

                    merged.push_back( match == nullptr ? freshRecord
                                                       : rfl::Generic( MergeObjects( freshObject.value(), *match,
                                                                                     entityKeyIsOurs ) ) );
                }

                fresh["Entities"] = std::move( merged );
            }
        }

        // The top level. Every key this build states it has just stated, so nothing here is ours to
        // omit — an absent top-level key is foreign by definition.
        return MergeObjects( std::move( fresh ), source, NothingIsOurs() );
    }

    void CountForeignKeysAtLevel( const rfl::Generic::Object& source, const KeyIsOurs& ours,
                                  std::map<std::string, int>& into )
    {
        for ( const auto& [key, value] : source )
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
