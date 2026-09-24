#pragma once

#include <Common/Core/UUID.hpp>
#include <Engine/Assets/Prefab/PrefabData.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <unordered_map>
#include <vector>

namespace Desert::Core::Rules
{
    // The DECISIONS the scene loader makes while turning a parsed .desce tree into entities, as a pure
    // function of that tree.
    //
    // WHY THIS IS NOT SIMPLY LEFT IN SceneSerializer::DeserializeFromJson. Because nothing could reach it
    // there. SceneSerializer.cpp includes Scene.hpp, Scene.hpp reaches the renderer, and no test project in
    // this repository compiles or links that file - which means the two-pass identity stitch, the code every
    // single scene load in the engine goes through, was invisible to a green sweep. A defect deliberately
    // planted in it left all 69 suites passing. The stitch is the part worth testing; creating entities and
    // handing payloads to the entity serializer is the part that fetches the arguments. Same split as
    // Engine/ECS/System/SystemRules.hpp, and for the same reason.
    //
    // WHY A PLAN AND NOT A CALLBACK. The loader needs THREE things out of this: which entities to create and
    // under which ids, which entity each record's payload lands on, and which entity is its parent. Returned
    // as data, all three can be asserted in a test without a Scene existing; handed to a callback, only the
    // side effects could be, and the side effects need a renderer.

    // "No entity". Used for a parent link that names an entity the file does not contain, which the loader
    // must leave unattached rather than attaching to entity zero.
    inline constexpr size_t kNoSlot = static_cast<size_t>( -1 );

    // THE TWO POLICY DECISIONS, stated once for every caller.
    //
    // These were not decisions anybody took: they were whatever container each of the four hand-written
    // copies of this stitch happened to reach for. The scene loader and PrefabAsset used
    // `unordered_map::insert` (first claimant keeps the id), PrefabFactory used `operator[]` (last writer
    // wins), and the editor's snapshot restore keyed idless records under `value_or(Null())`, so every
    // record without an id collided on the key 0 and two anonymous entities collapsed into one. Nothing
    // reached any of the four, so the divergence was free to persist. It is decided here instead, once:
    //
    // 1. A DUPLICATED ID IS NOT A LOAD FAILURE, AND THE FIRST CLAIMANT KEEPS THE IDENTITY. The record that
    //    comes second still gets an entity - it is a thing in the file and dropping it would lose geometry
    //    - but its payload is written onto the claimant, and StitchPlan::Shadowed counts it so the caller
    //    says so out loud instead of leaving a half-loaded scene to be found in the viewport.
    //    WHY NOT "LAST WINS": creation order is file order, so first-wins is the only rule under which the
    //    entity that answers an id is the one a reader of the file would point at, and it is the rule that
    //    does not depend on how far down the file the duplicate is. WHY NOT A HARD ERROR: these files are
    //    hand-merged (that is where duplicates come from), and refusing the whole scene over one bad record
    //    destroys the other several hundred. A named warning costs nothing and loses nothing.
    //
    // 2. A RECORD WITHOUT AN ID IS MINTED ONE - it is not corruption. The id is a LINK KEY, not a claim
    //    that the record is well-formed: EntitySerializer writes one for every entity it has ever
    //    serialized, so a record without one is a record nobody can point at, which is a legal (if lonely)
    //    thing to be. Minting makes that exactly what it is - a fresh identity no `parent` field names -
    //    and it is what makes the snapshot-restore defect unspellable: two anonymous records can no longer
    //    share a key, because they no longer share an id.
    //
    // Both decisions are the SAME for all callers. The only thing a caller chooses is PrefabRecordPolicy.

    // What a record carrying a PrefabPath is, which differs between the two kinds of file these records
    // live in - and is the one difference this function takes an argument for.
    enum class PrefabRecordPolicy
    {
        // A .desce record naming a prefab file. The entity comes out of THAT file, not this one, so it
        // cannot be created from the record and takes no part in the stitch: it is listed for the caller
        // to instantiate afterwards. It answers no parent link, because the loader registers the
        // instantiated root only after pass 2 - pinned deliberately, see the suite.
        InstantiatedLater,

        // An EDITOR SNAPSHOT record that carries a prefab link. Here the record IS an entity of this
        // capture - one whose children were captured alongside it, because CaptureSubtree walks the LIVE
        // subtree - so it is created and stitched like every other record, and it is still listed,
        // because whoever wants the nested body instantiated needs to know which slot to hang it off.
        //
        // A `.deprefab` USED TO USE THIS AND NO LONGER DOES (Ю19). While a nested prefab was COPIED into
        // the outer file, the nesting record really was an entity of that file. Now that it is a LINK,
        // creating it produced an extra entity between the outer prefab and the nested body: the record's
        // entity, with no components on it, and the instantiated root hanging underneath. The tree
        // therefore gained one level every time a prefab was captured and re-instantiated - unbounded,
        // one per "Apply Instance Changes to Prefab" - while looking identical on screen, because a UI
        // element with no UILayout is laid out as its parent. InstantiatedLater is what the scene loader
        // has always used for the same situation, and it is now what a prefab body uses too.
        CreatedInPlace,
    };

    // One entity the loader must create, in creation order.
    struct PlannedEntity
    {
        size_t       Record = 0;       // index into the record span handed in
        Common::UUID Id;               // the id the entity is created under
        bool         IdMinted = false; // the record carried no `id` and one was minted for it
    };

    // Where one record's payload goes once the entities exist.
    struct PlannedLoad
    {
        size_t Record = 0;       // index into the record span handed in
        size_t Target = 0;       // index into StitchPlan::Created - the entity that receives the payload
        size_t Parent = kNoSlot; // index into StitchPlan::Created, or kNoSlot when the record names none
    };

    // One record that names a prefab file. Slot is where the stitch put it under CreatedInPlace, and
    // kNoSlot under InstantiatedLater - so the caller reads the answer instead of re-deriving it from the
    // policy it passed in, which is the two-places-that-must-agree shape this whole header exists to kill.
    //
    // Parent is what the instantiated body HANGS OFF under InstantiatedLater, where there is no Slot to
    // hang it off: the created slot this record's `parent` id names, or kNoSlot when the file names none
    // (a prefab cut from an entity that had a parent keeps that id, and it belongs to no record here -
    // which is the normal spelling of "this is a root of the instance"). It is resolved HERE, with the
    // same id map that resolves every other parent link, because the alternative is each caller walking
    // the records again with its own copy of the rule - and four such copies is what this file replaced.
    struct PlannedPrefab
    {
        size_t Record = 0;
        size_t Slot   = kNoSlot;
        size_t Parent = kNoSlot;
    };

    struct StitchPlan
    {
        std::vector<PlannedEntity> Created;       // pass 1, in sibling order (siblingIndex, then file order)
        std::vector<PlannedLoad>   Loads;         // pass 2, same order and same length as Created
        std::vector<PlannedPrefab> PrefabRecords; // records carrying a PrefabPath, in sibling order

        size_t Minted            = 0; // entities whose id was invented because the file did not name one
        size_t Shadowed          = 0; // records whose id was already claimed - their payload lands on the CLAIMANT
        size_t UnresolvedParents = 0; // records naming a parent no stitched record answers to
    };

    // Plans the identity stitch for one parsed scene file. PURE - no GPU, no filesystem, no global state.
    //
    // @p mint is called ONCE for each record that carries no `id`, in file order, and must return a fresh
    // identity (the loader passes Common::UUID::Generate; a test passes a counter so the plan is
    // reproducible).
    //
    // WHY THE ID IS REMEMBERED AND NOT RECOMPUTED. The two passes used to call `id.value_or( Generate() )`
    // independently, so an entity saved without an `id` was minted TWO different ids: pass 2 looked it up
    // under the second, missed, and skipped it. The entity existed in the scene carrying nothing but a tag,
    // and no parent link pointing at it ever resolved. Here the id is decided once, in Created, and pass 2
    // reads it back rather than deriving it a second time - the defect is not fixed, it is unspellable.
    //
    // DUPLICATED IDS AND IDLESS RECORDS: see "THE TWO POLICY DECISIONS" above. First claimant keeps the
    // identity and the shadowed record is counted; a record with no id is minted one.
    //
    // @p prefabs says what a PrefabPath record is to this caller - the ONE thing that legitimately differs
    // between a scene file and a prefab body. Stated explicitly at every call site on purpose: there is no
    // default, because a default is how the four copies of this loop came to disagree in the first place.
    template <typename MintFn>
    inline StitchPlan PlanSceneStitch( std::span<const Assets::EntityData> records, MintFn&& mint,
                                       PrefabRecordPolicy prefabs )
    {
        StitchPlan plan;
        plan.Created.reserve( records.size() );
        plan.Loads.reserve( records.size() );

        // Slot the map kept for each created entity's id: its own, unless another record claimed that id
        // first. Captured at insert time so pass 2 never has to look an id up twice.
        std::vector<size_t> winner;
        winner.reserve( records.size() );

        // THE ORDER RECORDS ARE VISITED IN IS THE SIBLING ORDER. Since scene v25 the file is sorted by id,
        // so a record's position says nothing; `siblingIndex` does. Visiting in that order makes every
        // later step - creation, the attach in pass 2, the prefab pass - put siblings where the file
        // says, because each of them appends. A record without one sorts last, in file order.
        std::vector<size_t> visit( records.size() );
        std::iota( visit.begin(), visit.end(), size_t{ 0 } );
        std::stable_sort( visit.begin(), visit.end(),
                          [&]( size_t a, size_t b )
                          {
                              return records[a].siblingIndex.value_or( std::numeric_limits<uint32_t>::max() ) <
                                     records[b].siblingIndex.value_or( std::numeric_limits<uint32_t>::max() );
                          } );

        std::unordered_map<Common::UUID, size_t> byId;

        // Pass 1 - decide identities.
        for ( const size_t record : visit )
        {
            const Assets::EntityData& data = records[record];
            const bool                isPrefab = data.PrefabPath.has_value();
            if ( isPrefab && prefabs == PrefabRecordPolicy::InstantiatedLater )
            {
                plan.PrefabRecords.push_back( PlannedPrefab{ record, kNoSlot } );
                continue;
            }
            if ( isPrefab )
                plan.PrefabRecords.push_back( PlannedPrefab{ record, plan.Created.size() } );

            PlannedEntity created;
            created.Record   = record;
            created.IdMinted = !data.id.has_value();
            created.Id       = created.IdMinted ? mint() : *data.id;
            if ( created.IdMinted )
                ++plan.Minted;

            const auto inserted = byId.insert( { created.Id, plan.Created.size() } );
            winner.push_back( inserted.first->second );
            plan.Created.push_back( created );
        }

        // Where each PREFAB record's body hangs. Resolved after pass 1 because byId is only complete
        // then, and a nested prefab's parent is routinely a record that comes AFTER it in the file.
        for ( PlannedPrefab& prefab : plan.PrefabRecords )
        {
            const Assets::EntityData& data = records[prefab.Record];
            if ( data.parent.has_value() && !data.parent->IsNull() )
            {
                const auto found = byId.find( *data.parent );
                if ( found != byId.end() )
                {
                    prefab.Parent = found->second;
                }
            }
        }

        // Pass 2 - decide where each payload lands and what it hangs off.
        for ( size_t slot = 0; slot < plan.Created.size(); ++slot )
        {
            PlannedLoad load;
            load.Record = plan.Created[slot].Record;
            load.Target = winner[slot];
            if ( load.Target != slot )
                ++plan.Shadowed;

            const Assets::EntityData& data = records[load.Record];
            if ( data.parent.has_value() && !data.parent->IsNull() )
            {
                const auto found = byId.find( *data.parent );
                if ( found != byId.end() )
                    load.Parent = found->second;
                else
                    ++plan.UnresolvedParents;
            }

            plan.Loads.push_back( load );
        }

        return plan;
    }

} // namespace Desert::Core::Rules
