#pragma once

// WHICH CELL EACH THING IN A PARTITIONED WORLD BELONGS TO, AND HOW FAR EACH CELL HAD TO GROW TO HOLD IT.
//
// ── WHAT THIS IS NOT ──────────────────────────────────────────────────────────────────────────────
//
// It is not streaming. There is no residency here, no coarse layer, no root set that moves with the
// camera, and no spatial index: `PROGRAMME.md` §8 names those as three separate absences and this file
// is the first of them only — making the scene FORMAT able to express a partition, and making the
// partitioner assign every whole to exactly one cell and say what that cost. A cell is a set of records
// and a rectangle here, nothing more.
//
// ── THE UNIT IS THE COMPOSITE, NOT THE ENTITY ─────────────────────────────────────────────────────
//
// What a reference across a cell boundary does depends on WHAT KIND OF RELATION it is:
//
//   * CONTAINMENT — a parent and its child, a weapon in a hand, a player inside a vehicle. These are
//     parts of ONE WHOLE, and a whole that is half-loaded is incoherent: nulling the link drops the
//     weapon through the floor. So the composite — not the entity — is what a cell holds, and a
//     composite is never divided. This is also what UE does with a cluster of actors that reference
//     each other: the cluster goes into one cell whole.
//
//   * OBSERVATION — "that building over there". It nulls, and the reader handles null. This is not a
//     new rule: `AttachmentSystem` already takes exactly that path for a target it cannot find, and
//     `SocketAttachmentComponent::Target` is a `UUID` with a `Null()` for the purpose.
//
// ── THE CELL GROWS (owner decision, 2026-09-18, Docs/World/PROGRAMME.md) ──────────────────────────
//
// The owner chose UE's approach to partitioning on 2026-09-18, and with it UE's answer to the one
// question the composite rule leaves open — a composite wider than a cell. The answer is that THE CELL
// GROWS: its bounds become the union of its own grid square and everything assigned to it, so a
// composite of any size is held whole and nothing is refused. (Commit 6a122bae on this branch refused
// such a composite instead and called that an owner decision of 2026-09-23; it was not one, and the
// refusal is gone.)
//
// What UE's answer costs is named rather than hidden: with a grown cell, what is resident at a camera
// position is no longer a function of the grid alone but of the content. So the growth is not left to be
// discovered — it is a NUMBER the plan states for every cell, and for the world as a whole:
//
//   > EVERY CELL CARRIES ITS GRID SQUARE, THE BOUNDS OF WHAT IT HOLDS, AND HOW FAR THE LATTER
//   > OVERHANGS THE FORMER. `WorldPartitionPlan::MaxGrowth` IS THE WORST OF THEM.
//
// It is a number and not a gate. A threshold that refuses or warns would be a policy, and the policy
// belongs with the thing that pays for growth — streaming — which does not exist yet. UE's own answer
// for a very large cluster is to lift it into a coarser level of the grid; that is deliberately not
// here either, because a coarser level is a streaming concept and has nothing to be resident in yet.
//
// ── WHAT A CELL'S BOUNDS ARE MADE OF: POINTS, AND WHERE THEY COME FROM ────────────────────────────
//
// A scene record carries no extent of its own: a mesh's bounds live in the mesh asset, a prefab's are
// not stored anywhere (see UnplacedPrefabInstances), so what this file can see is a POSITION per record.
// There are exactly two exceptions. A LANDSCAPE TILE is a rectangle derived from its root's frame (see
// kLandscapeTileComponent) — the one record whose extent the file states in full. The other is taken
// because it is the case that grows a cell the most:
// an InstancedStaticMesh record carries the WORLD-space matrix of every instance it draws
// (MeshECSSystem.hpp submits the snapshot without the entity's transform), so its contents are that set
// of points, not the entity's own position. A foliage field spanning a kilometre is one record; treating
// it as one point would report a cell of zero growth that loads a kilometre of grass.
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
#include <Engine/World/Landscape/LandscapeLayout.hpp>

// The transform composition below, and NOT Engine/ECS/Components.hpp for it. TransformComponent's
// GetTransform() is the three lines this file needs, but that header carries entt, the reflection
// macros and ninety-odd component definitions — including it here would make this file exactly as
// unreachable by a test as SceneSerializer.cpp is, which is the thing the split exists to prevent.
// ComposeLocal below states the same composition; the suite pins it against hand-worked numbers.
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm> // std::max — MSVC does not get it transitively (scripts/CI/StandardIncludes.py)
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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

    // WHAT ONE ENTITY REFERENCE MEANS. The only two answers; which of the two a given reference is, is
    // what the register below records.
    enum class ReferenceKind
    {
        // Parts of one whole. The partitioner keeps them in one cell, whole, and grows the cell.
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
         // A landscape tile names its root, and it is OBSERVATION ON PURPOSE (landscape analysis A1). As
         // containment every tile of a landscape would be one composite, and one cell would grow to hold
         // the whole terrain — the opposite of why a landscape is cut into tiles. The reader handles
         // null the way observation requires: a tile whose root is not loaded has no frame and is placed
         // nowhere (WorldPartitionPlan::UnplacedLandscapeTiles), it does not drag the root in.
         { "LandscapeTile", "Landscape", ReferenceKind::Observation },
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
    // REPORTED, NOT ACTED ON. A dangling parent is already a counted warning at load (`StitchPlan::
    // UnresolvedParents`) and a dangling socket target is already a silent `continue` in
    // `AttachmentSystem` — neither is a cut this partitioner is making. It is reported so that "the
    // composite is smaller than you think" is visible rather than inferred.
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

    // A RECTANGLE ON THE GROUND, in world units (centimetres). X and Z only, for the same reason the
    // grid is two-dimensional: height does not partition.
    struct CellBounds
    {
        float MinX = 0.0f;
        float MinZ = 0.0f;
        float MaxX = 0.0f;
        float MaxZ = 0.0f;
    };

    // The square the grid gives a cell before anything is put in it.
    [[nodiscard]] inline CellBounds GridSquareOf( const CellCoord& cell, float cellSize )
    {
        CellBounds square;
        square.MinX = static_cast<float>( cell.X ) * cellSize;
        square.MinZ = static_cast<float>( cell.Z ) * cellSize;
        square.MaxX = square.MinX + cellSize;
        square.MaxZ = square.MinZ + cellSize;
        return square;
    }

    // ONE CELL THAT HOLDS SOMETHING, AND WHAT HOLDING IT COST.
    //
    // `Bounds` is the cell's grid square GROWN to contain every point of every composite assigned to it
    // (owner decision 2026-09-18: the cell grows). `Content` is those points alone — it can be smaller
    // than the square, and it is what a later coarse layer or a residency test would want to know.
    // `Growth` is how far `Bounds` overhangs the square, the worst of its four sides, in world units: zero
    // means the cell is exactly its square, one cell size means a streamer must treat it as three
    // squares wide along that axis.
    //
    // `Furthest` is the record whose point sets `Growth` — the thing to look at when the number is large.
    // kNoRecord when nothing overhangs.
    struct PlannedCell
    {
        CellCoord Cell;
        // nullopt when nothing in the cell has a known position — only unplaced prefab instances.
        std::optional<CellBounds> Content;
        CellBounds                Bounds;
        float                     Growth   = 0.0f;
        std::size_t               Furthest = kNoRecord;
        std::vector<std::size_t>  Composites; // indices into WorldPartitionPlan::Composites
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
        //
        // AND THEY DO NOT GROW A CELL. Their origin is not a position, it is the absence of one, so it
        // contributes no point to any cell's Content: a cell grown to reach the origin by a record that
        // is not there would state a number that is simply false.
        std::vector<std::size_t> UnplacedPrefabInstances;

        // LANDSCAPE TILES THAT HAVE NO PLACE, for the same reason and treated the same way: a tile is placed
        // by its ROOT's frame (World/Landscape/LandscapeLayout.hpp), and a tile whose root this file does not
        // contain — or whose root cannot be tiled — has no rectangle. Its own entity position is not a
        // stand-in: nothing reads it. Listed, planned at that position, and growing no cell.
        std::vector<std::size_t> UnplacedLandscapeTiles;

        // Every cell that holds at least one composite, ordered by X then Z so the plan does not depend
        // on the iteration order of a hash map. A cell nothing is assigned to is not listed.
        std::vector<PlannedCell> Cells;

        // THE WORLD'S ONE NUMBER FOR "HOW MUCH DID HOLDING WHOLES COST": the largest Growth of any cell,
        // and which cell it is (an index into Cells, kNoRecord for a world with no cells). Zero means
        // every cell is exactly its grid square.
        float       MaxGrowth     = 0.0f;
        std::size_t MaxGrowthCell = kNoRecord;
    };

    // THE ONE RECORD KIND WHOSE CONTENTS ARE NOT AT ITS OWN POSITION, by its names on disk. One row,
    // named, so a suite can pin it: see "WHAT A CELL'S BOUNDS ARE MADE OF" at the top of this file.
    inline constexpr std::string_view kInstancePointsComponent = "InstancedStaticMesh";
    inline constexpr std::string_view kInstancePointsField     = "InstanceTransforms";

    // THE SECOND SUCH KIND: a landscape tile. Its contents are a RECTANGLE computed from its coordinate
    // and its root's frame — not its own position, which nothing reads — so the partitioner asks the root.
    // The root is not a part of the tile (see the register row above): it is read, never joined.
    inline constexpr std::string_view kLandscapeRootComponent = "Landscape";
    inline constexpr std::string_view kLandscapeTileComponent = "LandscapeTile";

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
        // scale — because that is what the loader builds out of these same three fields. The suite cannot
        // include Components.hpp either, so it pins the composition on a rotated, scaled, offset parent
        // against numbers worked out by hand (WorldPartitionCells.ARotatedScaledParent...), which is what
        // goes red if either side changes convention.
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

        // A number inside a component payload. JSON does not keep "0" and "0.0" apart and neither does
        // rfl::Generic: an integral literal reads as an integer and `to_double` refuses it, so both are
        // asked for. Not asking for both would silently drop every instance on an integral coordinate.
        [[nodiscard]] inline bool ReadNumber( const rfl::Generic& value, float& out )
        {
            if ( const auto real = value.to_double(); real.has_value() )
            {
                out = static_cast<float>( real.value() );
                return true;
            }
            if ( const auto whole = value.to_int64(); whole.has_value() )
            {
                out = static_cast<float>( whole.value() );
                return true;
            }
            return false;
        }

        // THE WORLD-SPACE GROUND POSITION OF EVERY INSTANCE AN InstancedStaticMesh RECORD DRAWS.
        //
        // WORLD and not local, and the line that decides it is not the member's comment but the submit in
        // MeshECSSystem.hpp, which hands the snapshot of InstanceTransforms to the renderer without the
        // entity's own transform. Each matrix is written by ComponentRegistry as the 16 floats of a
        // column-major glm::mat4 (a memcpy), so the translation is elements 12, 13 and 14.
        //
        // A matrix that is not 16 numbers is skipped rather than guessed at; the loader would reject the
        // same block, so it cannot be drawn either.
        inline void AppendInstancePoints( const Assets::EntityData& record, std::vector<glm::vec2>& out )
        {
            const auto payload = record.Components.get( std::string( kInstancePointsComponent ) );
            if ( !payload.has_value() )
                return;
            const auto block = payload.value().to_object();
            if ( !block.has_value() )
                return;
            const auto field = block.value().get( std::string( kInstancePointsField ) );
            if ( !field.has_value() )
                return;
            const auto matrices = field.value().to_array();
            if ( !matrices.has_value() )
                return;

            for ( const rfl::Generic& matrix : matrices.value() )
            {
                const auto elements = matrix.to_array();
                if ( !elements.has_value() || elements.value().size() != 16 )
                    continue;
                float x = 0.0f;
                float z = 0.0f;
                if ( ReadNumber( elements.value()[12], x ) && ReadNumber( elements.value()[14], z ) )
                    out.emplace_back( x, z );
            }
        }

        // A whole number inside a component payload; absent leaves @p out alone, as the loader does.
        template <class T>
        inline bool ReadWhole( const rfl::Generic::Object& block, std::string_view field, T& out )
        {
            const auto value = block.get( std::string( field ) );
            if ( !value.has_value() )
                return true;
            const auto whole = value.value().to_int64();
            if ( !whole.has_value() )
                return false;
            out = static_cast<T>( whole.value() );
            return true;
        }

        inline std::optional<rfl::Generic::Object> BlockOf( const Assets::EntityData& record,
                                                            std::string_view          key )
        {
            const auto payload = record.Components.get( std::string( key ) );
            if ( !payload.has_value() )
                return std::nullopt;
            const auto block = payload.value().to_object();
            if ( !block.has_value() )
                return std::nullopt;
            return block.value();
        }

        // THE GROUND RECTANGLE OF A LANDSCAPE TILE RECORD, or nullopt when it has none. The root's fields are
        // read with the loader's rule — an absent field is the component's default, which is LandscapeRoot's
        // default because both are spelled from the same constants — and the root is refused by the same
        // ValidateLandscapeRoot the loader applies, so the partition never places a tile the loader refuses.
        //
        // @p rootWorld is the root record's composed world matrix: its translation is the frame's origin, as
        // Entity::GetWorldTransform is for the loaded root.
        inline std::optional<World::Landscape::LandscapeTileRect>
        LandscapeTileRectOf( const Assets::EntityData& tile, const std::vector<glm::mat4>& world,
                             std::span<const Assets::EntityData>                  records,
                             const std::unordered_map<Common::UUID, std::size_t>& byId )
        {
            const auto tileBlock = BlockOf( tile, kLandscapeTileComponent );
            if ( !tileBlock.has_value() )
                return std::nullopt;

            Common::UUID rootId;
            if ( !ReadReference( *tileBlock, "Landscape", rootId ) || rootId.IsNull() )
                return std::nullopt;
            const auto found = byId.find( rootId );
            if ( found == byId.end() )
                return std::nullopt;
            const auto rootBlock = BlockOf( records[found->second], kLandscapeRootComponent );
            if ( !rootBlock.has_value() )
                return std::nullopt;

            World::Landscape::LandscapeRoot root;
            root.Origin = glm::vec3( world[found->second][3] );
            if ( !ReadWhole( *rootBlock, "QuadsPerTile", root.QuadsPerTile ) )
                return std::nullopt;
            if ( const auto spacing = rootBlock->get( "SpacingCm" ); spacing.has_value() )
                if ( !ReadNumber( spacing.value(), root.SpacingCm ) )
                    return std::nullopt;
            if ( const auto zScale = rootBlock->get( "ZScale" ); zScale.has_value() )
                if ( !ReadNumber( zScale.value(), root.ZScale ) )
                    return std::nullopt;
            if ( !World::Landscape::ValidateLandscapeRoot( root ) )
                return std::nullopt;

            std::int32_t tileX = 0;
            std::int32_t tileZ = 0;
            if ( !ReadWhole( *tileBlock, "TileX", tileX ) || !ReadWhole( *tileBlock, "TileZ", tileZ ) )
                return std::nullopt;
            return World::Landscape::LandscapeTileBounds( root, tileX, tileZ );
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
    // THE FOUR STEPS, and why in this order:
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
    //      target that is in the same composite, so whatever the sample was, the cell holding the
    //      target holds it too — at worst it grows that cell by the length of an arm.
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
    //   4. THE GROWTH, last, because it needs all three: every point of every composite is folded into
    //      the bounds of the cell its anchor chose, and how far that overhangs the grid square is
    //      recorded per cell and as the world's maximum. Nothing is refused.
    //
    // @p settings is the world's own block. `CellSize` <= 0 is not sanitised into something plausible:
    //    every composite lands in cell (0,0) and the world is one cell, which is what "no grid" means
    //    and is visibly wrong rather than quietly approximate. That cell's Bounds are its Content and its
    //    Growth is zero: there is no square for it to outgrow.
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

        // A landscape tile's place is its rectangle, from its root — after step 1, because the root's origin
        // is the root's WORLD position.
        std::vector<std::optional<World::Landscape::LandscapeTileRect>> tileRect( records.size() );
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            if ( !records[record].Components.get( std::string( kLandscapeTileComponent ) ).has_value() )
                continue;
            tileRect[record] = Detail::LandscapeTileRectOf( records[record], world, records, byId );
            if ( !tileRect[record].has_value() )
                plan.UnplacedLandscapeTiles.push_back( record );
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

        // What hangs off something. The anchor is the first member in file order that does not.
        std::vector<bool> hangs( records.size(), false );
        for ( const ContainmentEdge& edge : plan.Containment )
            hangs[edge.Member] = true;

        const float cellSize = settings.CellSize;
        for ( PlannedComposite& group : plan.Composites )
        {
            group.Anchor = group.Members.front();
            for ( const std::size_t member : group.Members )
            {
                if ( !hangs[member] )
                {
                    group.Anchor = member;
                    break;
                }
            }

            // A NON-POSITIVE CELL SIZE IS ONE CELL. It is not repaired into something plausible: every
            // composite reports cell (0,0), which is what "this world has no grid" looks like from the
            // outside. Nothing in the editor can produce it — the format's default is 12800 — so the only
            // way to get here is by hand-editing the file.
            if ( cellSize <= 0.0f )
                continue;

            // A tile anchors on the CENTRE of its rectangle: its corners lie on the grid lines of a
            // landscape cut to the grid, and a corner would put it in whichever cell the floor picks.
            if ( const auto& rect = tileRect[group.Anchor]; rect.has_value() )
            {
                group.Cell =
                     CellOf( 0.5f * ( rect->MinX + rect->MaxX ), 0.5f * ( rect->MinZ + rect->MaxZ ), cellSize );
                continue;
            }
            const glm::mat4& anchorWorld = world[group.Anchor];
            group.Cell                   = CellOf( anchorWorld[3].x, anchorWorld[3].z, cellSize );
        }

        // ── 4. THE CELL GROWS to hold what was assigned to it ─────────────────────────────────────
        //
        // Every point of every composite in a cell is folded into that cell's Content, and Bounds is the
        // grid square grown to contain it. No point is tested against a limit: a composite of any size is
        // held whole, and what holding it cost is the Growth computed here.
        std::vector<bool> unplaced( records.size(), false );
        for ( const std::size_t record : plan.UnplacedPrefabInstances )
            unplaced[record] = true;
        for ( const std::size_t record : plan.UnplacedLandscapeTiles )
            unplaced[record] = true;

        // Ordered by X then Z, so the plan does not depend on a hash map's iteration order.
        std::map<std::pair<std::int32_t, std::int32_t>, std::size_t> cellIndex;
        for ( const PlannedComposite& group : plan.Composites )
            cellIndex.emplace( std::make_pair( group.Cell.X, group.Cell.Z ), kNoRecord );
        for ( auto& [coord, index] : cellIndex )
        {
            index = plan.Cells.size();
            PlannedCell cell;
            cell.Cell.X = coord.first;
            cell.Cell.Z = coord.second;
            // No grid, no square: the one cell is exactly what it holds and has nothing to outgrow.
            if ( cellSize > 0.0f )
                cell.Bounds = GridSquareOf( cell.Cell, cellSize );
            plan.Cells.push_back( cell );
        }

        // How far a point lies outside a square on the worse of the two axes; zero inside. Chebyshev and
        // not Euclidean: the bounds are axis-aligned, so a streamer asking "is the camera inside this
        // cell's bounds" is asking per axis.
        struct Overhang
        {
            static float Of( const CellBounds& square, const glm::vec2& point )
            {
                const float x = std::max( square.MinX - point.x, point.x - square.MaxX );
                const float z = std::max( square.MinZ - point.y, point.y - square.MaxZ );
                return std::max( 0.0f, std::max( x, z ) );
            }
        };

        std::vector<glm::vec2> points;
        for ( std::size_t group = 0; group < plan.Composites.size(); ++group )
        {
            const PlannedComposite& held = plan.Composites[group];
            PlannedCell&            cell = plan.Cells[cellIndex.at( std::make_pair( held.Cell.X, held.Cell.Z ) )];
            cell.Composites.push_back( group );

            for ( const std::size_t member : held.Members )
            {
                if ( unplaced[member] )
                    continue;

                points.clear();
                if ( const auto& rect = tileRect[member]; rect.has_value() )
                {
                    // The four corners and NOT the entity's own position: nothing reads a tile's transform.
                    points.emplace_back( rect->MinX, rect->MinZ );
                    points.emplace_back( rect->MaxX, rect->MinZ );
                    points.emplace_back( rect->MinX, rect->MaxZ );
                    points.emplace_back( rect->MaxX, rect->MaxZ );
                }
                else
                {
                    points.emplace_back( world[member][3].x, world[member][3].z );
                    Detail::AppendInstancePoints( records[member], points );
                }

                for ( const glm::vec2& point : points )
                {
                    if ( !cell.Content.has_value() )
                        cell.Content = CellBounds{ point.x, point.y, point.x, point.y };
                    CellBounds& content = *cell.Content;
                    content.MinX        = std::min( content.MinX, point.x );
                    content.MinZ        = std::min( content.MinZ, point.y );
                    content.MaxX        = std::max( content.MaxX, point.x );
                    content.MaxZ        = std::max( content.MaxZ, point.y );

                    if ( cellSize <= 0.0f )
                        continue;
                    const float overhang = Overhang::Of( GridSquareOf( cell.Cell, cellSize ), point );
                    if ( overhang > cell.Growth )
                    {
                        cell.Growth   = overhang;
                        cell.Furthest = member;
                    }
                }
            }
        }

        for ( std::size_t index = 0; index < plan.Cells.size(); ++index )
        {
            PlannedCell& cell = plan.Cells[index];
            if ( cell.Content.has_value() )
            {
                if ( cellSize > 0.0f )
                {
                    cell.Bounds.MinX = std::min( cell.Bounds.MinX, cell.Content->MinX );
                    cell.Bounds.MinZ = std::min( cell.Bounds.MinZ, cell.Content->MinZ );
                    cell.Bounds.MaxX = std::max( cell.Bounds.MaxX, cell.Content->MaxX );
                    cell.Bounds.MaxZ = std::max( cell.Bounds.MaxZ, cell.Content->MaxZ );
                }
                else
                {
                    cell.Bounds = *cell.Content;
                }
            }

            if ( plan.MaxGrowthCell == kNoRecord || cell.Growth > plan.MaxGrowth )
            {
                plan.MaxGrowth     = cell.Growth;
                plan.MaxGrowthCell = index;
            }
        }

        return plan;
    }

} // namespace Desert::Core::Rules
