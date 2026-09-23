#pragma once

// WHICH CELL EACH THING IN A PARTITIONED WORLD BELONGS TO, AND THE ONE CUT THAT IS REFUSED.
//
// ── WHAT THIS IS NOT ──────────────────────────────────────────────────────────────────────────────
//
// It is not streaming. There is no residency here, no coarse layer, no root set that moves with the
// camera, and no spatial index: `PROGRAMME.md` §8 names those as three separate absences and this file
// is the first of them only — making the scene FORMAT able to express a partition, and making the
// partitioner refuse the cut that would make a partition incoherent whatever the runtime later does.
// A cell is a set of records here and nothing more.
//
// ── THE UNIT IS THE COMPOSITE, NOT THE ENTITY (owner decision, 2026-09-23) ────────────────────────
//
// The question "what happens to a reference that crosses a cell boundary" was posed as a choice
// between UE (the cell GROWS to contain the referent) and Unity ECS (the reference NULLS). Both
// answers were rejected as the question, because the thing that decides is not what the reference
// does, it is WHAT KIND OF RELATION it is:
//
//   * CONTAINMENT — a parent and its child, a weapon in a hand, a player inside a vehicle. These are
//     parts of ONE WHOLE. A whole that is half-loaded is incoherent no matter what the reference does:
//     nulling it drops the weapon through the floor, growing the cell loads the weapon and calls the
//     result a cell. So the composite — not the entity — is what a cell holds, and a composite is
//     never divided.
//
//   * OBSERVATION — "that building over there". It nulls, and the reader handles null. This is not a
//     new rule: `AttachmentSystem` already takes exactly that path for a target it cannot find, and
//     `SocketAttachmentComponent::Target` is a `UUID` with a `Null()` for the purpose.
//
// ── WHY IT IS AN INVARIANT AND NOT A CELL THAT GROWS ──────────────────────────────────────────────
//
// UE's answer costs something specific and it is worth naming, because it is the reason we did not
// take it: when a cell grows to swallow whatever its contents point at, the set of bytes resident at
// a given camera position stops being a function of the grid and becomes a function of the content
// graph. Residency becomes DERIVED and UNPREDICTABLE — you cannot look at a world and say what it
// costs to stand there — and the very next decision in this programme ("a budget that can be exceeded
// is not a budget") needs exactly that predictability.
//
// So the composite is assigned WHOLE, to the cell its anchor sits in, and the cost of that is bounded
// on purpose:
//
//   > A CELL'S CONTENTS LIE INSIDE THAT CELL'S BOUNDS EXPANDED BY AT MOST ONE CELL SIZE.
//
// That statement is the whole design, and `PlanWorldPartition` below is what makes it true rather than
// hoped for. It holds because assignment is by composite (so nothing is ever divided) AND because a
// composite whose members reach further than one cell size from its anchor is REFUSED (so the overhang
// has a number). Take either half away and the sentence is false.
//
// ── THE REFUSAL ───────────────────────────────────────────────────────────────────────────────────
//
// It names both entities — the composite's anchor and the member that reaches too far — the entity the
// member hangs off, WHICH RELATION it hangs by, how far it reaches and what the cell size is. A
// refusal nobody can read is a refusal nobody will fix; `DescribeRefusal` is the sentence.
//
// The author has three real answers to it and all three are edits to the world, not to this file:
// raise `CellSize`, break the composite up (if the relation was observation wearing containment's
// clothes, say so by deleting the parent link), or move the piece closer.
//
// ── WHAT COUNTS AS CONTAINMENT IS DERIVED, AND AN UNKNOWN REFERENCE IS RED ────────────────────────
//
// Two relations express containment in this engine today, and a census over every component in the
// tree found no third: the parent/child hierarchy (`RelationshipComponent`, on disk the entity
// record's `parent` meta field) and socket attachment (`SocketAttachmentComponent::Target`). The
// danger is not that the list is wrong today, it is that it is silently wrong LATER — a component
// added next month with a `UUID` in it would be containment nobody told this file about, and the
// partition would start cutting through composites with everything still green.
//
// `FindUnregisteredEntityReferences` is what stops that. It does not trust the list: it walks the
// component payloads of the records themselves, finds every value that is an id another record in the
// file claims, and reports any whose `{component key, field}` is not a row of `kEntityReferences`
// below. So the list is a REGISTER — one named row per known reference, each classified — and its
// completeness is checked against the data instead of against somebody's memory.
//
// ── PURE ──────────────────────────────────────────────────────────────────────────────────────────
//
// In: parsed records and one number. Out: a plan. No Scene, no renderer, no filesystem, no globals —
// same split as `SceneStitchRules.hpp` and for the same reason: `SceneSerializer.cpp` reaches the
// renderer through `Scene.hpp`, so anything left inside it cannot be reached by any suite in this
// repository. Desert/Tests/Engine/WorldPartition is what reaches this.

#include <Common/Core/UUID.hpp>
#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

// The transform composition below, and NOT Engine/ECS/Components.hpp for it. TransformComponent's
// GetTransform() is the three lines this file needs, but that header carries entt, the reflection
// macros and ninety-odd component definitions — including it here would make this file exactly as
// unreachable by a test as SceneSerializer.cpp is, which is the thing the split exists to prevent.
// ComposeLocal below states the same composition and the suite pins the two against each other.
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm> // std::max — MSVC does not get it transitively (scripts/CI/StandardIncludes.py)
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Desert::Core::Rules
{
    // "No record". Same spelling and the same purpose as SceneStitchRules' kNoSlot.
    inline constexpr std::size_t kNoRecord = static_cast<std::size_t>( -1 );

    // WHICH SQUARE OF THE GRID. Two-dimensional: X and Z, unbounded in Y — see
    // WorldPartitionSerialized for why the vertical axis is not divided.
    struct CellCoord
    {
        std::int32_t X = 0;
        std::int32_t Z = 0;

        friend bool operator==( const CellCoord& a, const CellCoord& b )
        {
            return a.X == b.X && a.Z == b.Z;
        }
        friend bool operator!=( const CellCoord& a, const CellCoord& b )
        {
            return !( a == b );
        }
    };

    // The cell a world position falls in. `std::floor` and not a cast: a cast truncates towards zero,
    // which makes the two cells either side of the origin the SAME cell and twice as wide as every
    // other one. Half the worlds anyone authors are centred on the origin.
    [[nodiscard]] inline CellCoord CellOf( float worldX, float worldZ, float cellSize )
    {
        CellCoord cell;
        cell.X = static_cast<std::int32_t>( std::floor( worldX / cellSize ) );
        cell.Z = static_cast<std::int32_t>( std::floor( worldZ / cellSize ) );
        return cell;
    }

    // WHAT ONE ENTITY REFERENCE MEANS. The only two answers, and the decision the owner took on
    // 2026-09-23 is which of the two a given reference is — not what the partitioner does about it.
    enum class ReferenceKind
    {
        // Parts of one whole. The partitioner keeps them in one cell, whole, or refuses.
        Containment,
        // "That thing over there". Nulls across a boundary; the reader handles null, as
        // AttachmentSystem already does for a target it cannot find.
        Observation,
    };

    // ONE KNOWN ENTITY→ENTITY REFERENCE, by the names it has ON DISK.
    //
    // A ROW PER REFERENCE AND NOT A COUNT. A gate that pins "there are two entity references in this
    // engine" is satisfied by editing the number; a gate that pins these rows and DERIVES the count
    // from them is not. `FindUnregisteredEntityReferences` supplies the other half — the rows are
    // checked against the records, so a reference nobody added a row for is found in the data.
    //
    // `std::string_view` AND NOT `const char*`. The rows are string literals with static storage, so a
    // raw pointer here would answer neither of the two ownership questions in its own type and would owe
    // a row in pointer_ownership_register.hpp for an answer that is not interesting — the view says
    // "borrowed, and I do not free it" in the type instead, which is the answer.
    struct EntityReferenceRow
    {
        std::string_view ComponentKey; // ComponentRegistry key, i.e. the key of the block in the .desce
        std::string_view Field;        // field inside that block holding the referenced id
        ReferenceKind    Kind;
    };

    // THE REGISTER. Every reference from one entity of a scene to another, as the file spells it.
    //
    // The entity record's `parent` meta field is NOT here and cannot be: it is not a component block,
    // it is structural — `EntitySerializer.cpp:24-32` writes it from `RelationshipComponent` — so it
    // is read directly by the planner below. It is containment, and it is the reason "containment
    // cannot be split" had to be INTRODUCED rather than observed: a parent link is written as the
    // parent's UUID, so the hierarchy is perfectly expressible across a load boundary today.
    inline constexpr EntityReferenceRow kEntityReferences[] = {
         // The weapon is IN the hand: AttachmentSystem writes this entity's transform from a bone of
         // the target, every frame. Half of that pair loaded is a weapon lying at the origin.
         { "SocketAttachment", "Target", ReferenceKind::Containment },
         // Who fired the bullet, kept so a projectile can skip self-hits. The bullet is not part of
         // the shooter; a null owner costs one redundant hit test.
         { "Projectile", "Owner", ReferenceKind::Observation },
    };

    // REFERENCES THAT NAME THEIR TARGET BY A NAME INSTEAD OF AN ID — the BLIND SPOT of the discovery
    // function below, written down rather than left to be found.
    //
    // `FindUnregisteredEntityReferences` recognises a reference by its SHAPE: a value that reads as the
    // decimal id of another record in the same file. Two references in this engine do not have that
    // shape — they carry a human-authored name and are resolved by SEARCHING the scene for the entity
    // whose component carries it. No amount of walking the payloads finds those, so they are listed
    // here by hand, and this table existing is the only reason "the census is complete" is a statement
    // anybody can check.
    //
    // BOTH ARE OBSERVATION, and the line that decides it is `UIOverlay::OverlayByName`
    // (Engine/UI/UIOverlay.cpp:418) — no overlay carrying the name is an ERROR RETURNED to the caller,
    // which opens nothing. The reader already handles absence, which is what observation means, and it
    // is the same shape `AttachmentSystem` uses for a socket target it cannot find. Neither is a part
    // of the other: a button is not made incoherent by the panel it opens being elsewhere.
    //
    // AND THE STANDING RISK, because it is not zero: the overlay lookup searches the WHOLE scene
    // (Components.hpp:1992, "names are looked up across the scene"), so a name link is not confined to
    // one canvas the way the hierarchy would confine it. If a name-addressed reference is ever
    // CONTAINMENT, this partitioner cannot enforce it without an index from name to record — and that
    // index is the work that row would cost.
    struct NamedEntityReferenceRow
    {
        std::string_view ComponentKey;       // the block holding the name
        std::string_view Field;              // the field holding it
        std::string_view TargetComponentKey; // the block on the target whose `Name` it matches
        ReferenceKind    Kind;
    };

    inline constexpr NamedEntityReferenceRow kEntityReferencesByName[] = {
         { "UIOverlayTrigger", "Overlay", "UIOverlay", ReferenceKind::Observation },
         { "UIScreenStack", "InitialScreen", "UIScreen", ReferenceKind::Observation },
    };

    // How a member of a composite hangs off the rest of it. Two, because the census found two.
    enum class Containment
    {
        Hierarchy,        // the entity record's `parent` meta field (RelationshipComponent)
        SocketAttachment, // SocketAttachmentComponent::Target
    };

    [[nodiscard]] inline const char* NameOf( Containment relation )
    {
        return relation == Containment::Hierarchy ? "hierarchy (parent)" : "socket attachment";
    }

    // One containment edge of the file, as record indices.
    struct ContainmentEdge
    {
        std::size_t  Holder   = kNoRecord; // the parent / the socket target
        std::size_t  Member   = kNoRecord; // the child / the socketed entity
        Containment  Relation = Containment::Hierarchy;
        Common::UUID HolderId;
    };

    // A containment reference naming an id no record in this file claims.
    //
    // NOT A REFUSAL. A dangling parent is already a counted warning at load (`StitchPlan::
    // UnresolvedParents`) and a dangling socket target is already a silent `continue` in
    // `AttachmentSystem` — neither is a cut this partitioner is making, so refusing to partition a
    // world over one would be this task punishing a defect that belongs to somebody else. It is
    // reported so that "the composite is smaller than you think" is visible rather than inferred.
    struct DanglingContainment
    {
        std::size_t  Member   = kNoRecord;
        Containment  Relation = Containment::Hierarchy;
        Common::UUID NamedId;
    };

    // ONE COMPOSITE AND THE ONE CELL IT IS IN. Members are record indices in file order and always
    // include the anchor.
    struct PlannedComposite
    {
        std::size_t              Anchor = kNoRecord; // whose position decides the cell
        CellCoord                Cell;
        std::vector<std::size_t> Members;
    };

    // THE REFUSAL: a composite that does not fit in a cell, so there is no cell that can hold it whole
    // and no promise left about how far its contents overhang.
    struct OversizedComposite
    {
        std::size_t Anchor       = kNoRecord; // the composite's anchor record
        std::size_t Member       = kNoRecord; // the member that reaches too far
        std::size_t MemberHolder = kNoRecord; // what that member hangs off (kNoRecord if it is dangling)
        Containment Relation     = Containment::Hierarchy; // by which relation it hangs
        float       Reach        = 0.0f;                   // furthest of |dx| and |dz| from the anchor
        float       CellSize     = 0.0f;
    };

    struct WorldPartitionPlan
    {
        std::vector<PlannedComposite>    Composites;
        std::vector<ContainmentEdge>     Containment;
        std::vector<DanglingContainment> Dangling;

        // RECORDS THIS FILE CANNOT PLACE, AND THEY ARE COUNTED RATHER THAN SILENTLY PUT AT THE ORIGIN.
        //
        // A `.desce` record naming a prefab file carries NO transform: SceneSerializer strips the
        // instance's translation, rotation and scale out of the record (SceneSerializer.cpp:112) and
        // re-states them as override records addressed by ids that only the `.deprefab` can resolve.
        // So the one thing a partitioner needs from such a record - where it is - is in another file,
        // and a pure function of THIS file cannot have it. They are planned at the origin, which is
        // wrong, and listed here, which is what makes it a stated limitation instead of a wrong answer
        // nobody asked about. Four such records exist in the repository today, in two scenes.
        //
        // Closing it is not this task: `PROGRAMME.md` already names the same gap from the other end -
        // a prefab has no bounds of its own, they have to be computed at import and stored in the
        // asset - and until that exists, placing an instance means opening its file.
        std::vector<std::size_t> UnplacedPrefabInstances;

        // Empty means the world partitions. Non-empty means it does NOT, and each entry is a sentence
        // DescribeRefusal can write out. The plan's Composites are still filled in either case, so a
        // caller can show the author what the partition WOULD have been beside what stopped it.
        std::vector<OversizedComposite> Refusals;

        [[nodiscard]] bool Refused() const
        {
            return !Refusals.empty();
        }
    };

    namespace Detail
    {
        // An id as the file spells it inside a component block: a DECIMAL STRING, because a 64-bit id
        // does not survive JSON's double (`AuthoredComponentIO.hpp:150-152` writes it, and that is the
        // same reason `SceneSettings::SplashSprite` lost handles above 2^53 before it was fixed).
        [[nodiscard]] inline bool ParseIdString( const std::string& text, Common::UUID& out )
        {
            if ( text.empty() || text.size() > 20 )
                return false;
            std::uint64_t value = 0;
            for ( const char character : text )
            {
                if ( character < '0' || character > '9' )
                    return false;
                const std::uint64_t digit = static_cast<std::uint64_t>( character - '0' );
                if ( value > ( ~std::uint64_t( 0 ) - digit ) / 10 )
                    return false;
                value = value * 10 + digit;
            }
            out = Common::UUID( value );
            return true;
        }

        // The id a named field of a component block holds, if it holds one.
        [[nodiscard]] inline bool ReadReference( const rfl::Generic::Object& block, std::string_view field,
                                                 Common::UUID& out )
        {
            const auto value = block.get( std::string( field ) );
            if ( !value.has_value() )
                return false;
            const auto text = value.value().to_string();
            if ( !text.has_value() )
                return false;
            return ParseIdString( text.value(), out );
        }

        // ONE RECORD'S LOCAL MATRIX, and it must stay identical to ECS::TransformComponent::
        // GetTransform() (Engine/ECS/Components.hpp:645-649) — translate * toMat4(quat(euler)) *
        // scale — because that is what the loader builds out of these same three fields. The suite
        // asserts the agreement on a rotated, scaled, offset case rather than trusting this comment.
        //
        // An absent field is its default, exactly as the loader treats it: EntitySerializer only
        // writes a transform field the entity has, and an entity created without one sits at zero
        // with unit scale.
        [[nodiscard]] inline glm::mat4 ComposeLocal( const Assets::EntityData& record )
        {
            const glm::vec3 translation = record.Translation.value_or( glm::vec3( 0.0f ) );
            const glm::vec3 rotation    = record.Rotation.value_or( glm::vec3( 0.0f ) );
            const glm::vec3 scale       = record.Scale.value_or( glm::vec3( 1.0f ) );
            return glm::translate( glm::mat4( 1.0f ), translation ) * glm::toMat4( glm::quat( rotation ) ) *
                   glm::scale( glm::mat4( 1.0f ), scale );
        }
    } // namespace Detail

    // EVERY ENTITY REFERENCE IN THESE RECORDS THAT `kEntityReferences` DOES NOT HAVE A ROW FOR.
    //
    // This is the half of the census that cannot be satisfied by editing a list, and the defect it
    // exists for is the one that would survive this whole task: a component added later that holds a
    // `UUID`, classified by nobody, cut through by the partitioner in silence. It walks the component
    // payloads as DATA and asks of every string value "is this an id some other record in this file
    // claims?" — so a new reference is found by the shape of what is written, not by being remembered.
    //
    // FALSE POSITIVES ARE POSSIBLE AND ARE THE SAFE DIRECTION. A string field that happens to read as
    // the decimal id of another record — a bone named "2010" on a scene that has an entity 2010 —
    // lands here and is one added row away from quiet. The opposite mistake is the one that ships.
    struct UnregisteredReference
    {
        std::size_t  Record = kNoRecord;
        std::string  ComponentKey;
        std::string  Field;
        Common::UUID NamedId;
    };

    [[nodiscard]] inline std::vector<UnregisteredReference>
    FindUnregisteredEntityReferences( std::span<const Assets::EntityData> records )
    {
        std::unordered_map<Common::UUID, std::size_t> byId;
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            if ( records[record].id.has_value() && !records[record].id->IsNull() )
                byId.emplace( *records[record].id, record );
        }

        std::vector<UnregisteredReference> found;
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            for ( const auto& [componentKey, payload] : records[record].Components )
            {
                const auto block = payload.to_object();
                if ( !block.has_value() )
                    continue;

                for ( const auto& [field, value] : block.value() )
                {
                    const auto text = value.to_string();
                    if ( !text.has_value() )
                        continue;

                    Common::UUID named;
                    if ( !Detail::ParseIdString( text.value(), named ) || named.IsNull() )
                        continue;
                    if ( byId.find( named ) == byId.end() )
                        continue;

                    bool registered = false;
                    for ( const EntityReferenceRow& row : kEntityReferences )
                    {
                        if ( componentKey == row.ComponentKey && field == row.Field )
                        {
                            registered = true;
                            break;
                        }
                    }
                    if ( !registered )
                        found.push_back( UnregisteredReference{ record, componentKey, field, named } );
                }
            }
        }
        return found;
    }

    // THE PARTITION, as a pure function of the parsed records and the world's one authored number.
    //
    // THE THREE STEPS, and why in this order:
    //
    //   1. WORLD POSITIONS, composed down the hierarchy. A record's `Translation` is LOCAL — the
    //      loader hands it straight to `TransformComponent`, which `Entity::GetWorldTransform`
    //      composes with its parent's — so partitioning on the stated number alone would put every
    //      child of a moved parent in the wrong cell, and the deeper the tree the further wrong.
    //      Rotation and scale are composed too, because a parent rotated ninety degrees moves its
    //      children somewhere else entirely.
    //
    //      A SOCKET-ATTACHED ENTITY'S SAVED POSITION IS A POSE SAMPLE, not an authored fact:
    //      `AttachmentSystem` overwrites its transform from a bone every frame while the scene runs,
    //      and whatever it held when the file was saved is what is in the file. It is used as-is —
    //      there is no skeleton at partition time and inventing one would be worse — and it is
    //      harmless precisely BECAUSE the relation is containment: the entity is in the hand of a
    //      target that is in the same composite, so it is near the anchor whatever the sample was.
    //
    //   2. COMPOSITES, as the connected components of the containment relations. Both relations, one
    //      union: an entity can have a hierarchy parent AND a socket target (a weapon parented under
    //      a rack while socketed to a hand is exactly that), and both make it part of one whole.
    //
    //   3. THE CELL, from the ANCHOR. The anchor is the first member in FILE ORDER that hangs off
    //      nothing — a composite joined by a socket can have two such, and file order is the same
    //      tie-break the identity stitch uses, so the answer does not depend on iteration order of a
    //      hash map. A composite that is nothing but a cycle (a corrupt file) has no such member and
    //      anchors on its first member instead, which is a definition rather than a crash.
    //
    // @p settings is the world's own block. `CellSize` <= 0 is not sanitised into something plausible:
    //    every composite lands in cell (0,0) and the world is one cell, which is what "no grid" means
    //    and is visibly wrong rather than quietly approximate.
    [[nodiscard]] inline WorldPartitionPlan PlanWorldPartition( std::span<const Assets::EntityData> records,
                                                                const WorldPartitionSerialized&     settings )
    {
        WorldPartitionPlan plan;

        std::unordered_map<Common::UUID, std::size_t> byId;
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            if ( records[record].id.has_value() && !records[record].id->IsNull() )
                byId.emplace( *records[record].id, record );
        }

        // ── 1. World positions ────────────────────────────────────────────────────────────────────
        //
        // Resolved by walking each record's parent chain rather than by a topological sort: a chain is
        // short, a file can name a parent that comes later, and a corrupt file can name a cycle. The
        // depth cap is what makes a cycle terminate; it is generous enough that no authored hierarchy
        // can reach it.
        std::vector<glm::mat4>   world( records.size(), glm::mat4( 1.0f ) );
        std::vector<bool>        resolved( records.size(), false );
        std::vector<std::size_t> chain;
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            if ( resolved[record] )
                continue;

            chain.clear();
            std::size_t walk = record;
            while ( walk != kNoRecord && !resolved[walk] && chain.size() < records.size() + 1 )
            {
                chain.push_back( walk );
                const Assets::EntityData& data = records[walk];
                std::size_t               next = kNoRecord;
                if ( data.parent.has_value() && !data.parent->IsNull() )
                {
                    const auto found = byId.find( *data.parent );
                    if ( found != byId.end() && found->second != walk )
                        next = found->second;
                }
                walk = next;
            }

            // Compose from the far end of the chain back down to `record`. `walk` is either kNoRecord
            // (a root), an already-resolved ancestor, or — on a cycle — a record already in `chain`,
            // whose partial matrix is the identity and which therefore terminates without looping.
            glm::mat4 above = ( walk != kNoRecord && resolved[walk] ) ? world[walk] : glm::mat4( 1.0f );
            for ( std::size_t index = chain.size(); index-- > 0; )
            {
                const std::size_t slot = chain[index];
                above                  = above * Detail::ComposeLocal( records[slot] );
                world[slot]            = above;
                resolved[slot]         = true;
            }
        }

        // ── 2. Containment edges, then composites ─────────────────────────────────────────────────
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            const Assets::EntityData& data = records[record];

            if ( data.PrefabPath.has_value() && !data.Translation.has_value() )
                plan.UnplacedPrefabInstances.push_back( record );

            if ( data.parent.has_value() && !data.parent->IsNull() )
            {
                const auto found = byId.find( *data.parent );
                if ( found == byId.end() )
                    plan.Dangling.push_back( DanglingContainment{ record, Containment::Hierarchy, *data.parent } );
                else if ( found->second != record )
                    plan.Containment.push_back(
                         ContainmentEdge{ found->second, record, Containment::Hierarchy, *data.parent } );
            }

            for ( const EntityReferenceRow& row : kEntityReferences )
            {
                if ( row.Kind != ReferenceKind::Containment )
                    continue;

                const auto payload = data.Components.get( std::string( row.ComponentKey ) );
                if ( !payload.has_value() )
                    continue;
                const auto block = payload.value().to_object();
                if ( !block.has_value() )
                    continue;

                Common::UUID target;
                if ( !Detail::ReadReference( block.value(), row.Field, target ) || target.IsNull() )
                    continue;

                const auto found = byId.find( target );
                if ( found == byId.end() )
                    plan.Dangling.push_back(
                         DanglingContainment{ record, Containment::SocketAttachment, target } );
                else if ( found->second != record )
                    plan.Containment.push_back(
                         ContainmentEdge{ found->second, record, Containment::SocketAttachment, target } );
            }
        }

        // Union-find over the containment edges. A composite is a connected component of them, which
        // is the only reading of "parts of one whole" that survives a weapon socketed to a character
        // that is itself a child of a vehicle.
        std::vector<std::size_t> forest( records.size() );
        for ( std::size_t record = 0; record < records.size(); ++record )
            forest[record] = record;

        // A free function rather than a lambda: a recursive lambda needs a y-combinator or a
        // std::function, and the parameter-less multi-line form is one of the three constructs
        // clang-format 18 and 22 disagree about (see the CI note in MEMORY).
        struct Find
        {
            static std::size_t Of( std::vector<std::size_t>& forest, std::size_t node )
            {
                while ( forest[node] != node )
                {
                    forest[node] = forest[forest[node]];
                    node         = forest[node];
                }
                return node;
            }
        };

        for ( const ContainmentEdge& edge : plan.Containment )
        {
            const std::size_t holder = Find::Of( forest, edge.Holder );
            const std::size_t member = Find::Of( forest, edge.Member );
            if ( holder != member )
                forest[member] = holder;
        }

        // What each member hangs off, for the refusal's sentence. First edge wins; an entity with both
        // a hierarchy parent and a socket target is named by its hierarchy parent, which is the one a
        // reader of the file can see without opening a component block.
        std::vector<std::size_t> holderOf( records.size(), kNoRecord );
        std::vector<Containment> relationOf( records.size(), Containment::Hierarchy );
        for ( const ContainmentEdge& edge : plan.Containment )
        {
            if ( holderOf[edge.Member] == kNoRecord )
            {
                holderOf[edge.Member]   = edge.Holder;
                relationOf[edge.Member] = edge.Relation;
            }
        }

        // ── 3. One cell per composite ─────────────────────────────────────────────────────────────
        std::unordered_map<std::size_t, std::size_t> composite; // root of the set -> index in Composites
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            const std::size_t root  = Find::Of( forest, record );
            auto              found = composite.find( root );
            if ( found == composite.end() )
            {
                found = composite.emplace( root, plan.Composites.size() ).first;
                plan.Composites.push_back( PlannedComposite{} );
            }
            plan.Composites[found->second].Members.push_back( record );
        }

        const float cellSize = settings.CellSize;
        for ( PlannedComposite& group : plan.Composites )
        {
            group.Anchor = group.Members.front();
            for ( const std::size_t member : group.Members )
            {
                if ( holderOf[member] == kNoRecord )
                {
                    group.Anchor = member;
                    break;
                }
            }

            // A NON-POSITIVE CELL SIZE IS ONE CELL, and there is nothing to refuse in a world that has
            // one cell. It is not repaired into something plausible and it is not a second kind of
            // refusal: every composite reports cell (0,0), which is what "this world has no grid"
            // looks like from the outside. Nothing in the editor can produce it — the format's default
            // is 12800 — so the only way to get here is by hand-editing the file.
            if ( cellSize <= 0.0f )
                continue;

            const glm::mat4& anchorWorld = world[group.Anchor];
            group.Cell                   = CellOf( anchorWorld[3].x, anchorWorld[3].z, cellSize );

            // THE REFUSAL. Chebyshev and not Euclidean on purpose: the promise being kept is about
            // CELL BOUNDS, which are axis-aligned, so the distance that decides is the larger of the
            // two axis distances and a diagonal neighbour is no worse than a side one.
            for ( const std::size_t member : group.Members )
            {
                if ( member == group.Anchor )
                    continue;

                const float dx    = world[member][3].x - anchorWorld[3].x;
                const float dz    = world[member][3].z - anchorWorld[3].z;
                const float reach = std::max( std::fabs( dx ), std::fabs( dz ) );
                if ( reach <= cellSize )
                    continue;

                plan.Refusals.push_back( OversizedComposite{ group.Anchor, member, holderOf[member],
                                                             relationOf[member], reach, cellSize } );
            }
        }

        return plan;
    }

    // THE SENTENCE A HUMAN READS. Names both entities by tag and id, the entity the member hangs off,
    // the relation, the two numbers and what to do about it.
    //
    // Tags, because an id is not something anyone recognises in a viewport, and ids as well, because a
    // tag is not unique and is frequently "Entity".
    [[nodiscard]] inline std::string DescribeRefusal( std::span<const Assets::EntityData> records,
                                                      const OversizedComposite&           refusal )
    {
        struct Named
        {
            static std::string Of( std::span<const Assets::EntityData> records, std::size_t record )
            {
                if ( record == kNoRecord || record >= records.size() )
                    return "<no record>";
                const Assets::EntityData& data = records[record];
                std::ostringstream        text;
                text << '\'' << data.Tag.value_or( "Entity" ) << "' (id "
                     << ( data.id.has_value() ? std::to_string( static_cast<std::uint64_t>( *data.id ) )
                                              : std::string( "none" ) )
                     << ')';
                return text.str();
            }
        };

        std::ostringstream text;
        text << "world partition REFUSED: " << Named::Of( records, refusal.Member ) << " is " << refusal.Reach
             << " world units from " << Named::Of( records, refusal.Anchor )
             << ", the anchor of the composite it belongs to, which is more than one cell (" << refusal.CellSize
             << "). It hangs off " << Named::Of( records, refusal.MemberHolder ) << " by "
             << NameOf( refusal.Relation )
             << ", and that is CONTAINMENT: the two are parts of one whole, so the partitioner will not "
                "put them in different cells and no single cell holds them both. Raise the world's cell "
                "size above "
             << refusal.Reach
             << ", move the piece closer, or — if this relation is really 'that thing over there' rather "
                "than 'part of this' — remove it, and the reference becomes an observation that nulls "
                "across the boundary.";
        return text.str();
    }

} // namespace Desert::Core::Rules
