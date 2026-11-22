#pragma once

// WHICH CELL OF WHICH GRID LEVEL EACH THING IN A PARTITIONED WORLD BELONGS TO, AND WHAT IS ALWAYS LOADED.
//
// ── WHAT THIS IS NOT ──────────────────────────────────────────────────────────────────────────────
//
// It is not streaming. There is no residency here, no root set that moves with the camera, and no
// spatial index: this file makes the scene FORMAT able to express a partition and makes the partitioner
// assign every whole to exactly one cell of one level — or to the always-loaded set — and say why. A
// cell is a set of records and a rectangle here, nothing more. Which cells are resident at a camera
// position is WP4's pure query over this plan, and `RuntimePartitionLHGridSerialized::LoadingRange` is its input.
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
// ── A WIDE COMPOSITE GOES UP A LEVEL (owner decision O1, 2026-09-23; placement WP-FAR-6B 9g) ──────
//
// The grid has LEVELS. Level L is the same grid with cells CellSize·2^L wide, aligned on the origin, so
// every level-L edge is also an edge of every level below it. A composite goes, whole, to the LOWEST
// level whose cell is at least as wide as its footprint's larger side, and in that level to the cell its
// footprint's CENTRE falls in. A composite wider than the top level can be goes to the ALWAYS-LOADED set.
// The level is derived from the footprint and is never authored.
//
// This replaces WP1's growing cell, and the reason is what growth cost: a cell grown to reach one wide
// thing dragged EVERY small thing in that cell out to the wide thing's loading distance. A level moves
// only the wide thing — the small things beside it stay in their small cell. It is the pattern of UE's
// FSquare2DGridHelper::GetPartitionedActors (UE:Engine/Source/Runtime/Engine/Private/WorldPartition/
// RuntimeSpatialHash/RuntimeSpatialHashGridHelper.cpp:211-262), not its letter:
//
//   * Placement is UE's: the level by the footprint's SIZE, the cell by its CENTRE (FSquare2DGridHelper::
//     GetCellCoords on the bounds' centre). A footprint therefore may reach past its cell's square by at
//     most half its own extent, so a cell's STREAMING bounds (`PlannedCell::Bounds`) are its square grown by
//     half the widest extent placed on its level: the loading range is measured to what the cell holds,
//     not to its square. Before 9g a composite had to fit one origin-aligned cell whole, so everything
//     straddling X = 0 or Z = 0 was always-loaded — on a world centred on the origin, almost everything.
//   * UE sizes the level count from the world bounds; so does this: the top level is the first whose
//     cell reaches the furthest footprint from the origin (`WorldPartitionPlan::LevelCount`), and never
//     less than the level the widest composite needs.
//
// What WP1 called `MaxGrowth` is now `WorldPartitionPlan::MaxLevel`: the level the worst composite was
// promoted to, and which composite it is.
//
// ── ALWAYS-LOADED: DERIVED FROM COMPONENTS, OVERRIDDEN BY ONE AUTHORED FLAG ───────────────────────
//
// The sun does not belong to cell (0,0). A composite is always-loaded when any member carries a
// component classified GLOBAL in `kComponentLoading` (camera, sun, sky, fog, clouds, a screen-space
// canvas, a non-spatial sound — the register below says why for each), when any member carries the
// author's `AlwaysLoaded` marker, or when no level holds its footprint. Every component key the
// registry serialises has exactly one row there, and Desert/Tests/Engine/WorldPartition derives the
// key list from ComponentRegistry.cpp itself: a component added without a row is red.
//
// ── A COMPOSITE'S FOOTPRINT: WHAT THIS FILE CAN SEE, AND WHERE WP15 PLUGS IN ──────────────────────
//
// A scene record carries no extent of its own: a mesh's bounds live in the mesh asset and a prefab's in
// the prefab's registry row (AL1-8a). So a footprint is the XZ rectangle around a set of
// world-space POINTS, and `Detail::AppendFootprint` is the one place that decides which points a record
// contributes. It knows six things, each stated where it is read, and a seventh is handled beside it
// because it needs another record (a LANDSCAPE TILE: the rectangle its root's frame gives it, see
// kLandscapeTileComponent — the tile's own position is read by nothing and is not a point):
//
//   * the record's own world position — every record;
//   * the eight corners of the box a PRIMITIVE draws, through the world matrix. The box is
//     `Geometry::PrimitiveBounds`, the same statement the factory stamps on the submesh and the
//     ShapeGenerators suite holds to the generated vertices. Terrain and LightCube are never a mesh
//     block's primitive and have no box, so such a record's position is its whole extent;
//   * the eight corners of the box around the vertices of a mesh the StaticMesh block CARRIES (EditMesh,
//     built in the editor and saved in the scene, no asset behind it), through the world matrix — UE's
//     GetStreamingBounds: an actor whose geometry is its own component's is bounded by that geometry;
//   * every instance of an InstancedStaticMesh, whose matrices are WORLD-space (MeshECSSystem.hpp submits
//     the snapshot without the entity's transform) — a kilometre of grass is one record;
//   * the eight corners of a mesh ASSET's stored bounds (StaticMesh or SkinnedMesh naming a file) — the
//     one fact that comes from outside this file. It is asked of an `AssetBoundsSource`, which the loader
//     fills from the cooked asset registry's Bounds column: UE's Actor-Descriptor pattern, an extent read
//     without loading the thing it describes. The function stays pure — the registry is handed in, not
//     reached for — and a suite hands in a table.
//
// A record nobody can state an extent for is its position, and `PointOnlyRecords` counts them.
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
// In: parsed records and the world's block. Out: a plan. No Scene, no renderer, no filesystem, no globals —
// same split as `SceneStitchRules.hpp` and for the same reason: `SceneSerializer.cpp` reaches the
// renderer through `Scene.hpp`, so anything left inside it cannot be reached by any suite in this
// repository. Desert/Tests/Engine/WorldPartition is what reaches this.

#include <Common/Core/Math/AABB.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/UUID.hpp>
#include <Common/Json/Document.hpp>
#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/WorldPartitionFormat.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>
#include <Engine/Scripting/ScriptProperty.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>

// The transform composition below, and NOT Engine/ECS/Components.hpp for it. TransformComponent's
// GetTransform() is the three lines this file needs, but that header carries entt, the reflection
// macros and ninety-odd component definitions — including it here would make this file exactly as
// unreachable by a test as SceneSerializer.cpp is, which is the thing the split exists to prevent.
// ComposeLocal below states the same composition; the suite pins it against hand-worked numbers.
#include <glm/gtx/quaternion.hpp>

#include <algorithm> // std::max — MSVC does not get it transitively (scripts/CI/StandardIncludes.py)
#include <format>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
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
    //
    // `double` for the size because a level's cell is CellSize·2^L, and the division happens in double so
    // a coordinate on a high level's edge is not rounded into the neighbouring cell.
    [[nodiscard]] CellCoord CellOf( float worldX, float worldZ, double cellSize );

    // WHAT ONE ENTITY REFERENCE MEANS. The only two answers; which of the two a given reference is, is
    // what the register below records.
    enum class ReferenceKind
    {
        // Parts of one whole. The partitioner keeps them in one cell, whole.
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
         // containment every tile of a landscape would be one composite, and the whole terrain would be
         // placed as one footprint — the opposite of why a landscape is cut into tiles. The reader handles
         // null the way observation requires: a tile whose root is not loaded has no frame and is placed
         // nowhere (WorldPartitionPlan::UnplacedLandscapeTiles); it does not drag the root in.
         { "LandscapeTile", "Landscape", ReferenceKind::Observation },
         // A landscape spline names its root the same way and for the same reason (UE: ALandscapeSplineActor
         // is its own spatially loaded actor in a partitioned world). Its points are world cm, so it is placed by
         // its band (EntityDescriptor::LandscapeSpline) whether the root is loaded or not, and never joins it.
         { "LandscapeSpline", "Landscape", ReferenceKind::Observation },
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
    // (UI/UIOverlay.cpp:418) — no overlay carrying the name is an ERROR RETURNED to the caller,
    // which opens nothing. The reader already handles absence, which is what observation means, and it
    // is the same shape `AttachmentSystem` uses for a socket target it cannot find. Neither is a part
    // of the other: a button is not made incoherent by the panel it opens being elsewhere.
    //
    // AND THE STANDING RISK, because it is not zero: the overlay lookup searches the WHOLE scene
    // (Components.hpp:1992, "names are looked up across the scene"), so a name link is not confined to
    // one canvas the way the hierarchy would confine it. If a name-addressed reference is ever
    // CONTAINMENT, this partitioner cannot enforce it without an index from name to record — and that
    // index is the work that row would cost. That index exists now (WP-ENTITY-REFS): ResolveEntityReferences
    // matches every name to the records carrying it at planning time, so a row of kind Containment clusters its
    // holder with every one of them.
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

    // How a member of a composite hangs off the rest of it. The census found two; scripts and names add theirs
    // (WP-ENTITY-REFS, UE: a Blueprint's hard actor references join the actor's cluster, FActorClusterContext).
    enum class Containment
    {
        Hierarchy,        // the entity record's `parent` meta field (RelationshipComponent)
        SocketAttachment, // SocketAttachmentComponent::Target - the register's one Containment row
        ScriptReference,  // a script's hard Entity property: Property.Entity( { hard = true } )
        NamedReference,   // a kEntityReferencesByName row of kind Containment, its name matched to a record
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

    // ── WHAT STREAMING GENERATION FINDS WRONG WITH A WORLD (WP-MAP-CHECK) ──────────────────────────────────
    //
    // UE 5.5 IStreamingGenerationErrorHandler (WorldPartitionStreamingGeneration.h): the streaming generation calls
    // one handler per finding - OnInvalidReference, OnInvalidReferenceGridPlacement, OnInvalidReferenceDataLayers,
    // OnInvalidReferenceRuntimeGrid - and the Map Check window (FStreamingGenerationMapCheckErrorHandler), the cook
    // log (FStreamingGenerationLogErrorHandler) and the check commandlet are three handlers of ONE list. Here the
    // planner is the generation: it states every finding in WorldPartitionPlan::Generation as it plans, PlanIssues
    // is the one list of everything wrong with a plan, and the three consumers (Check World Partition in the
    // editor, the warning after a save, `WorldCook --check`) and the StreamingGeneration log read that list - none
    // of them checks anything of its own. A finding never changes the plan: it names what the plan already does.
    enum class GenerationIssueKind
    {
        // A value the planner could not read or a name it could not resolve (WorldPartitionPlan::Issues).
        Planner,
        // UE OnInvalidReference: a reference naming an id no record of the world is.
        InvalidReference,
        // UE OnInvalidReferenceGridPlacement: an always-loaded record references one that is spatially loaded. By
        // containment the target is pulled into the always-loaded set whole; by observation it is null wherever
        // its cell is not loaded.
        AlwaysLoadedReferencesSpatial,
        // UE OnInvalidReferenceDataLayers: an observation between two different sets of runtime data layers.
        ReferenceAcrossDataLayers,
        // UE OnInvalidReferenceDataLayers: from a record in runtime data layers to one in none.
        ReferenceIntoNoDataLayer,
        // UE OnInvalidReferenceRuntimeGrid: a member of a composite states a runtime grid other than the one its
        // composite streams on (its anchor's), so its own is not used.
        ReferenceAcrossRuntimeGrids,
        // The saved HLOD of a cell is missing or was built from other records (WorldCells::StaleHLODCells).
        StaleHLOD,
    };

    [[nodiscard]] std::string_view GenerationIssueKindName( GenerationIssueKind kind );

    // ONE FINDING. `Record` is what an editor selects (the referer; kNoRecord: the world as a whole), `Target` the
    // record it references when there is one - both indices into the records the plan was made from.
    struct GenerationIssue
    {
        GenerationIssueKind Kind   = GenerationIssueKind::Planner;
        std::size_t         Record = kNoRecord;
        std::size_t         Target = kNoRecord;
        std::string         Text;
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

    // WHAT A COMPOSITE OR A CELL HOLDS, VERTICALLY (WP-SRC-ALTITUDE): the lowest and the highest world Y, cm, of
    // every point its footprint was built from - UE's cell content bounds in Z (UWorldPartitionRuntimeCell::
    // GetContentBounds, which the spatial hash's Z culling reads). Height still does not PARTITION - a cell is a
    // square on the ground - but it is MEASURED: the loading range reaches a cell in 3D, so a source 2 km up does
    // not want the 25 m cells of the ground below it (WorldPartitionStreamingRules.hpp, THE RULE).
    struct HeightSpan
    {
        float MinY = 0.0f;
        float MaxY = 0.0f;
    };

    [[nodiscard]] HeightSpan UnionHeight( const HeightSpan& a, const HeightSpan& b );

    // Edge of one cell of grid level @p level: CellSize·2^level. ldexp and not a loop of doublings, so a
    // level is exactly a power of two times the authored number with no accumulated rounding.
    [[nodiscard]] double LevelCellSize( float cellSize, int level );

    // The square a cell of a level covers.
    [[nodiscard]] CellBounds GridSquareOf( const CellCoord& cell, double levelCellSize );

    // THE ONE CELL OF A LEVEL THAT HOLDS @p footprint WHOLE, if there is one.
    //
    // A cell is HALF-OPEN, [k·s, (k+1)·s), so a footprint whose maximum lies exactly on an edge does not
    // spill into the next cell. That is not a nicety: WorldGen's ground tile is exactly one cell wide and
    // sits exactly on the edges, and a closed interval would push every tile of a generated world up a
    // level. A zero-width footprint (one point) is in the cell of its minimum.
    //
    // AN EDGE WITHIN FLOAT ROUNDING OF A CELL BOUNDARY IS ON THAT BOUNDARY. Footprints are float and are
    // built by arithmetic: a landscape root at -120 m with 255 quads of 23.529411 cm (a float) puts its
    // third tile's edge at -0.0002 cm, not 0. Read literally that is a sliver in the cell before, and 0 is
    // an edge at EVERY level, so the tile fitted no cell and fell to always-loaded (FIELD_Barn: 7 of 16
    // tiles, a cross along x = 0 and z = 0). The tolerance is a few float epsilons of the edge's own
    // magnitude, measured in cells — the error a float coordinate of that size carries, nothing coarser:
    // a real overhang of a millimetre on a 128 m cell is still an overhang.
    [[nodiscard]] std::optional<CellCoord> SingleCellHolding( const CellBounds& footprint,
                                                                     double            levelCellSize );

    // WHY A COMPOSITE IS IN THE ALWAYS-LOADED SET. Most specific first: the author said so, a component
    // says so, the grid could not hold it, or there is no grid.
    enum class AlwaysLoadedReason
    {
        None,      // it is in a cell
        Author,    // a member carries the `AlwaysLoaded` marker
        Component, // a member carries a component `kComponentLoading` classifies Global
        NoFit,     // its footprint is wider than the top level a grid can have, or not a finite rectangle
        NoGrid,    // the world states no usable partition: no `RuntimePartitions` entry, a bad one, or a name it
                   // lacks
        // Its anchor streams on a Persistent partition (UE URuntimePartitionPersistent): the WORLD's choice that
        // it never leaves. Last, so AlwaysLoadedByReason's size follows it.
        Persistent,
    };

    // ONE COMPOSITE AND WHERE IT WENT. Members are record indices in file order and always include the
    // anchor. `Level`/`Cell` are meaningful only when `Reason` is None.
    struct PlannedComposite
    {
        std::size_t              Anchor = kNoRecord; // first member in file order that hangs off nothing
        std::vector<std::size_t> Members;
        // nullopt when no member has a known position — only refused prefab instances
        // (UnplacedPrefabInstances). Such a composite is in NO cell and is not always loaded: it has no place.
        std::optional<CellBounds> Footprint;
        // The Y span of the same points; set exactly when Footprint is (WP-SRC-ALTITUDE).
        std::optional<HeightSpan> Height;

        AlwaysLoadedReason Reason  = AlwaysLoadedReason::None;
        std::size_t        Because = kNoRecord; // the member that made it Author/Component
        int                Level   = 0;
        CellCoord          Cell;
        std::size_t        Grid = 0; // index into WorldPartitionSerialized::RuntimePartitions (UE: RuntimeGrid)
        // The data layers the composite belongs to - its ANCHOR's (UE AActor::DataLayerAssets), as indices into
        // WorldPartitionSerialized::DataLayers, ascending and unique. Empty = no layer. Whatever the Reason: an
        // always-loaded composite belongs to its layers too, and the editor holds it by them (EditorRegions).
        std::vector<std::size_t> DataLayers;
        // The HLOD layer that builds this composite's part of its cell's HLOD - its ANCHOR's HLODLayer block (UE
        // AActor::HLODLayer), as an index into WorldPartitionSerialized::HLODLayers. nullopt = its partition's first
        // HLOD setup's layer (Rules::GridHLODLayer), and also a name the world has no layer of (an Issue, never
        // matched to some other layer by a guess).
        std::optional<std::size_t> HLODLayer;
    };

    // ONE CELL OF ONE LEVEL THAT HOLDS SOMETHING. `Square` is the cell's own square at its level and holds
    // the CENTRE of everything the cell holds. `Bounds` is the square grown on every side by half the widest
    // footprint placed on that level of that grid (UE's loose cell bounds), so every footprint the cell
    // holds lies inside it: the loading range is measured to `Bounds`.
    struct PlannedCell
    {
        std::size_t              Grid  = 0; // index into RuntimePartitions; always an LHGrid partition's
        int                      Level = 0;
        CellCoord                Cell;
        CellBounds               Square;
        CellBounds               Bounds;
        std::vector<std::size_t> Composites; // indices into WorldPartitionPlan::Composites
        // The union of its composites' Height (UE's content bounds in Z): what a source's altitude is measured to.
        HeightSpan Height;
        // THE THIRD COORDINATE (WP-DATA-LAYERS-RUNTIME; UE FWorldPartitionRuntimeCellData::DataLayers): the RUNTIME
        // data layers every composite of the cell belongs to, as indices into WorldPartitionSerialized::DataLayers,
        // ascending. Empty = a cell of no runtime layer. Composites of one square in different sets of runtime
        // layers are different cells (UE generates its cells per data layer set), so a layer that is Unloaded
        // reads not a byte of its cells and one that is Loaded holds them without making them entities.
        std::vector<std::size_t> DataLayers;
    };

    struct WorldPartitionPlan
    {
        std::vector<PlannedComposite>    Composites;
        std::vector<ContainmentEdge>     Containment;
        std::vector<DanglingContainment> Dangling;

        // RECORDS THIS FILE CANNOT PLACE, AND THEY ARE LISTED RATHER THAN SILENTLY PUT AT THE ORIGIN.
        //
        // Since scene v37 a `.desce` record naming a prefab file states its root's translation, rotation
        // and scale itself (SceneSerializer, PFX1), so an instance is placed like any record: by its world
        // matrix and the box the prefab's registry row states (AL1-8a). One that does not state them was
        // not written by this engine; it is listed here AND named in Issues with what it lacks, contributes
        // no point to any footprint, and the loader refuses the same record.
        std::vector<std::size_t> UnplacedPrefabInstances;

        // Records whose footprint is their position alone — a light, a script, a mesh whose bounds the
        // source could not state. A number, so the size of that gap is visible.
        std::size_t PointOnlyRecords = 0;

        // LANDSCAPE TILES THAT HAVE NO PLACE, for the same reason and treated the same way as unplaced prefab
        // instances: a tile is placed by its ROOT's frame (World/Landscape/LandscapeLayout.hpp), and a tile
        // whose root this file does not contain — or whose root cannot be tiled — has no rectangle. Its own
        // entity position is not a stand-in: nothing reads it. Listed, and contributing no point.
        std::vector<std::size_t> UnplacedLandscapeTiles;

        // EVERY COMPONENT VALUE THE PLANNER READ AND FOUND OF THE WRONG TYPE, each with its full path
        // (`Entities[id=7].LandscapeTile.TileX`). The planner reads such a value as the loader does — the
        // component's default is kept — and says so here instead of in silence; every place that plans a world
        // to act on it (cook, Play, editor regions) names them through ReportPlanIssues.
        Common::Json::Issues Issues;

        // WHAT THE STREAMING GENERATION FOUND WRONG WITH THE REFERENCES (WP-MAP-CHECK, UE IStreamingGenerationErrorHandler):
        // invalid references, always-loaded records referencing spatially loaded ones, references across runtime data
        // layers or grids. Named, never acted on: the plan above already is what it is. PlanIssues lists these with
        // Issues; ReportPlanIssues logs both.
        std::vector<GenerationIssue> Generation;

        // How many levels the grid has for THIS world: level LevelCount-1 is the first whose cell reaches
        // the footprint furthest from the origin. At least 1 when there is a grid; 0 when there is none.
        int LevelCount = 0;

        // Every cell that holds at least one composite, ordered by grid, level, then X, then Z, so the plan
        // does not depend on a hash map's iteration order. A cell nothing is assigned to is not listed.
        std::vector<PlannedCell> Cells;

        // The always-loaded composites, as indices into Composites, in file order of their anchors.
        std::vector<std::size_t> AlwaysLoaded;

        // THE WORLD'S ONE NUMBER FOR "HOW MUCH DID HOLDING WHOLES COST" (WP1's MaxGrowth): the highest
        // level any composite was promoted to, and which composite (kNoRecord when no composite is in a
        // cell). Zero means every placed composite fits a cell of the authored size.
        int         MaxLevel          = 0;
        std::size_t MaxLevelComposite = kNoRecord;

        // How many levels each grid has for THIS world, index = grid: every grid has its own ladder (UE: one
        // FSpatialHashStreamingGrid per runtime grid). LevelCount above is the highest of them.
        std::vector<int> GridLevelCount;

        // (holder, target) record pairs of every Observation reference ResolveEntityReferences found, self and
        // unresolved ones left out (WP-ENTITY-REFS) - resolved ONCE, by the plan, so residency
        // (ResidencyExecutor:: Begin) observes exactly what the plan clustered by, without describing the records
        // again. A plan read from a cooked index states the crossing ones (WorldCells::PlanFromIndex), the only
        // ones a cook keeps.
        std::vector<std::pair<std::size_t, std::size_t>> Observations;
    };

    // ── WHICH COMPONENTS MAKE THEIR ENTITY ALWAYS-LOADED ──────────────────────────────────────────
    //
    // ONE ROW PER COMPONENT KEY THE REGISTRY SERIALISES, and the census in Desert/Tests/Engine/
    // WorldPartition reads the key list out of ComponentRegistry.cpp and requires the two sets to be
    // equal — so a new component cannot reach a scene file without somebody deciding here whether a
    // world keeps it loaded everywhere.
    //
    // SPATIAL does not mean "has a position of its own"; it means "does not by itself make the entity
    // global": the entity's composite is placed by its footprint. A UI element is Spatial because it is a
    // child of its canvas, and the canvas decides for the whole tree.
    enum class ComponentLoading
    {
        Spatial,
        Global,
        // Decided by one scalar field of the block: Spatial when it equals `SpatialWhen`, Global otherwise.
        ByField,
    };

    struct ComponentLoadingRow
    {
        std::string_view ComponentKey;
        ComponentLoading Loading;
        // ByField only: which field, which value means Spatial, and what an ABSENT field means. The last is
        // the struct default restated, because this file cannot include Components.hpp (see the note on
        // ComposeLocal below); every reflected block is written whole, so only a hand-edited file omits it.
        std::string_view Field          = {};
        double           SpatialWhen    = 0.0;
        bool             AbsentIsGlobal = false;
    };

    inline constexpr ComponentLoadingRow kComponentLoading[] = {
         // ── Global: seen from everywhere, or owns the frame rather than a place in it ──
         { "Camera", ComponentLoading::Global },         // the view itself; brief O1: sun/sky/camera
         { "DirectionLight", ComponentLoading::Global }, // the sun lights every cell at once
         { "AnchorField",
           ComponentLoading::Spatial }, // DST-04: its box anchors the leaves inside it, where it stands
         { "KillField", ComponentLoading::Spatial },           // DST-04: a field acts where it stands
         { "RadialImpulseField", ComponentLoading::Spatial },  // DST-04: a field acts where it stands
         { "Skybox", ComponentLoading::Global },               // at infinity
         { "SkyAtmosphere", ComponentLoading::Global },        // at infinity
         { "ExponentialHeightFog", ComponentLoading::Global }, // a world-wide medium, not a volume
         { "StrainField", ComponentLoading::Spatial },         // DST-04: a field acts where it stands
         { "VolumetricCloud", ComponentLoading::Global },      // the cloud layer covers the planet
         // A hero cloud stands kilometres up and is seen from tens of kilometres away; no ground loading
         // range is that wide, so it would vanish while in plain view.
         { "HeroCloud", ComponentLoading::Global },
         // A landscape root owns the FRAME every tile is placed by, not a place of its own; UE loads
         // ALandscape the same way (not spatially loaded) and streams the proxies.
         { "Landscape", ComponentLoading::Global },
         // The root's look travels with the root: every tile of it is drawn with it.
         { "LandscapeMaterial", ComponentLoading::Global },
         // WATER-W1's body is UE's AWaterBodyOcean, the one body kind it has: the sea the whole world floats
         // on and swims in, answered by buoyancy wherever a hull is. UE keeps the ocean out of the spatial
         // hash for that reason (lakes and rivers are spatially loaded by their spline bounds); a footprint
         // read from the entity's point would unload the sea under a boat standing far from its origin. A
         // bounded body kind gets its own row (or ByField on the kind) when it exists, with its extent.
         { "WaterBody", ComponentLoading::Global },
         // WATER-W6's zone is the quadtree the surface of every body inside it is drawn from (UE AWaterZone,
         // bIsSpatiallyLoaded = false, WaterZoneActor.cpp:133): its own point says nothing about where its water
         // is, and unloading it with one cell would take the sea's surface away everywhere.
         { "WaterZone", ComponentLoading::Global },
         // The author's override, and the only authored input to partitioning besides the grid.
         { "AlwaysLoaded", ComponentLoading::Global },
         // The grid a composite streams on (WP22): it chooses the cell size, never whether the entity is placed.
         { "RuntimeGrid", ComponentLoading::Spatial },
         // The data layers an entity belongs to (WP-DATA-LAYERS): they decide what the editor holds, never whether
         // the entity is placed.
         { "DataLayers", ComponentLoading::Spatial },
         // The HLOD layer that builds the entity's part of its cell's HLOD (WP-SETTINGS-UI, UE AActor::HLODLayer):
         // it decides how the far cell stands in for it, never whether the entity is placed.
         { "HLODLayer", ComponentLoading::Spatial },
         // Play chooses among ALL starts (a tag must name exactly one) before the streamer begins, so a
         // start in an unloaded cell would silently change which one wins, or make a tag unresolvable.
         { "PlayerStart", ComponentLoading::Global },
         // A source decides what streams; one inside a cell would unload the cell and with it itself, and the
         // world around it would never come back.
         { "StreamingSource", ComponentLoading::Global },
         // A Level Sequence actor directs entities anywhere in the world and cuts the camera; in a cell it
         // would stop mid-cinematic when the player walks away (UE: ALevelSequenceActor is not spatially
         // loaded).
         { "LevelSequence", ComponentLoading::Global },
         // The level's clock of day drives the one sun and sky (UE: the Sun Position actor); it has no place.
         { "TimeOfDay", ComponentLoading::Global },
         // An editor loader of the world (WP-LOADER-ADAPTERS): the World Partition window lists every volume, loaded
         // or not, and loads by its box - UE ALocationVolume is not spatially loaded (bIsSpatiallyLoaded = false).
         // Editor-only: the cook leaves it out (WorldCells), so it never reaches the always-loaded part of a game.
         { "LocationVolume", ComponentLoading::Global },
         // ── By field ──
         // RenderMode 1 = WorldSpace (a nameplate, a floating panel): it lives at its entity. 0 =
         // ScreenSpace (HUD, menus), the default: it has no place in the world at all.
         { "UICanvas", ComponentLoading::ByField, "RenderMode", 1.0, true },
         // `Spatial` true (the default) attenuates from the entity; false is music/ambience — no place.
         { "AudioSource", ComponentLoading::ByField, "Spatial", 1.0, false },
         // `Unbound` true (the default) is the level's base grade, applied wherever the camera is; false is a
         // box around the entity, which grades only the cells it covers (UE's bUnbound on APostProcessVolume).
         { "PostProcessVolume", ComponentLoading::ByField, "Unbound", 0.0, true },
         // `PointWind` false (the default) blows everywhere along Direction (UE's directional wind source); true
         // fades to nothing at Radius around the entity, so it lives in its cell.
         { "WindSource", ComponentLoading::ByField, "PointWind", 1.0, true },
         // ── Spatial ──
         { "Animation", ComponentLoading::Spatial },
         { "CharacterController", ComponentLoading::Spatial },
         // The pawn's input, its camera boom and its ragdoll travel with the pawn they sit on.
         { "EnhancedInputPlayer", ComponentLoading::Spatial },
         { "Ragdoll", ComponentLoading::Spatial },
         { "SpringArm", ComponentLoading::Spatial },
         { "Collider", ComponentLoading::Spatial },
         { "ControlRig", ComponentLoading::Spatial },
         { "CubeGridBlockout", ComponentLoading::Spatial }, // the voxels its own mesh was baked from
         // A fractured mesh is a placed body like StaticMesh + RigidBody; its pieces fall within its cell.
         { "Destructible", ComponentLoading::Spatial },
         { "Folder", ComponentLoading::Spatial }, // an outliner grouping: its children decide
         { "Foliage", ComponentLoading::Spatial },
         // UE AProceduralFoliageVolume is an actor placed in a cell like any other: its box (translation +-
         // Extent) is its footprint, and what it grows lands in its own foliage fields, placed by theirs.
         { "ProceduralFoliage", ComponentLoading::Spatial },
         // The marker on a field a volume grew (UE: instances carrying the volume's ProceduralGuid): it sits on
         // that field's entity and travels with its Foliage, which is Spatial.
         { "ProceduralFoliageField", ComponentLoading::Spatial },
         { "InstancedStaticMesh", ComponentLoading::Spatial }, // its instances are its footprint
         { "Lock", ComponentLoading::Spatial },
         { "Material", ComponentLoading::Spatial },
         { "Morph", ComponentLoading::Spatial },
         { "PointLight", ComponentLoading::Spatial }, // has a radius; the sun is the global light
         { "Projectile", ComponentLoading::Spatial },
         // Its influence radius is where it lights; outside it no surface reads the capture (UE: reflection
         // captures are spatially loaded actors of their cell, like the local lights).
         { "ReflectionCapture", ComponentLoading::Spatial },
         { "Retarget", ComponentLoading::Spatial },
         { "RigidBody", ComponentLoading::Spatial },
         { "VFX", ComponentLoading::Spatial },
         // A script's reach is whatever it does, which no file states. Spatial, and a game-manager script
         // is exactly what the author's AlwaysLoaded marker is for.
         { "Script", ComponentLoading::Spatial },
         { "SkinnedMesh", ComponentLoading::Spatial },
         { "SocketAttachment", ComponentLoading::Spatial },
         { "SpotLight", ComponentLoading::Spatial },
         { "StaticMesh", ComponentLoading::Spatial },
         { "LandscapeTile", ComponentLoading::Spatial },   // its rectangle is its footprint
         { "LandscapeSpline", ComponentLoading::Spatial }, // the box around its band is its footprint
         { "Text", ComponentLoading::Spatial },
         { "TwoBoneIK", ComponentLoading::Spatial },
         { "Visibility", ComponentLoading::Spatial },
         // UI elements: children of a canvas, which decides for the tree (a canvas-less element draws
         // nothing).
         { "UIAnim", ComponentLoading::Spatial },
         { "UIBinding", ComponentLoading::Spatial },
         { "UIButton", ComponentLoading::Spatial },
         { "UIDraggable", ComponentLoading::Spatial },
         { "UIDropTarget", ComponentLoading::Spatial },
         { "UIDropdown", ComponentLoading::Spatial },
         { "UIIcon", ComponentLoading::Spatial },
         { "UIImage", ComponentLoading::Spatial },
         { "UIInputField", ComponentLoading::Spatial },
         { "UILayout", ComponentLoading::Spatial },
         { "UILayoutGroup", ComponentLoading::Spatial },
         { "UIListView", ComponentLoading::Spatial },
         { "UINavigation", ComponentLoading::Spatial }, // UI-FOCUS: per-element navigation rules
         { "UIOverlay", ComponentLoading::Spatial },    // lives on a canvas entity
         { "UIOverlayTrigger", ComponentLoading::Spatial },
         { "UIPanel", ComponentLoading::Spatial },
         { "UIPath", ComponentLoading::Spatial },
         { "UIPointerEvents", ComponentLoading::Spatial },
         { "UIProgressBar", ComponentLoading::Spatial },
         { "UIRenderTexture", ComponentLoading::Spatial },
         { "UIRetainer", ComponentLoading::Spatial },
         { "UIScreen", ComponentLoading::Spatial },
         { "UIScreenStack", ComponentLoading::Spatial }, // on a canvas, or on a nested switcher element
         { "UIScrollView", ComponentLoading::Spatial },
         { "UISlider", ComponentLoading::Spatial },
         { "UIStyle", ComponentLoading::Spatial },
         { "UITabs", ComponentLoading::Spatial }, // UI-SWITCHER: a tab bar container
         { "UIText", ComponentLoading::Spatial },
         { "UITileView", ComponentLoading::Spatial },
         { "UIToggle", ComponentLoading::Spatial },
         { "UITreeView", ComponentLoading::Spatial },
         { "UITween", ComponentLoading::Spatial },
    };

    // The author's marker, by its key on disk. Named once: the register row above and the reason
    // Author are both about it.
    inline constexpr std::string_view kAlwaysLoadedComponent = "AlwaysLoaded";
    inline constexpr std::string_view kRuntimeGridComponent  = "RuntimeGrid";
    inline constexpr std::string_view kDataLayersComponent   = "DataLayers";
    inline constexpr std::string_view kHLODLayerComponent    = "HLODLayer";
    inline constexpr std::string_view kStreamingSourceComponent = "StreamingSource";
    // UE ALocationVolume (WP-LOADER-ADAPTERS): an editor loader's box; the one editor-only record kind.
    inline constexpr std::string_view kLocationVolumeComponent = "LocationVolume";

    // THE RECORD KINDS WHOSE CONTENTS ARE NOT A POINT AT THEIR OWN POSITION, by their names on disk. One
    // constant per name, so a suite can pin them: see "A COMPOSITE'S FOOTPRINT" at the top of this file.
    inline constexpr std::string_view kInstancePointsComponent = "InstancedStaticMesh";
    inline constexpr std::string_view kInstancePointsField     = "InstanceTransforms";
    inline constexpr std::string_view kPrimitiveComponent      = "StaticMesh";
    inline constexpr std::string_view kPrimitiveField          = "Primitive";
    // A mesh built in the editor lives IN the StaticMesh block (StaticMeshComponentSer::EditMesh, the
    // saved form of SavedMeshForm.hpp): its vertex positions, xyz per vertex in the entity's own frame.
    inline constexpr std::string_view kEditMeshField          = "EditMesh";
    inline constexpr std::string_view kEditMeshPositionsField = "Positions";
    // A landscape tile's footprint is a RECTANGLE computed from its coordinate and its root's frame. The
    // root is not a part of the tile (see the register row): it is read, never joined.
    inline constexpr std::string_view kLandscapeRootComponent = "Landscape";
    inline constexpr std::string_view kLandscapeTileComponent = "LandscapeTile";
    // ECS::ScriptComponent's block (ComponentRegistry MakeScript): Scripts[] of { ScriptKey, Props[] of { Name, Type,
    // Ref, ElementType, Items[] } } - Type is Scripting::PropertyType's stored number (WP-ENTITY-REFS).
    inline constexpr std::string_view kScriptComponent = "Script";
    // A landscape spline's footprint is the box around its band (World::Landscape::LandscapeSplinesBounds), read
    // from its own block: its points are world cm and nothing reads the entity's transform.
    inline constexpr std::string_view kLandscapeSplineComponent = "LandscapeSpline";

    // THE MESH BLOCKS THAT NAME A MESH ASSET, and where each states its reference (PrefabData.hpp): the Static
    // Mesh in its reflected `MeshHandle` object {Guid, Path} (asset references v1), the Skinned Mesh in the
    // block's own MeshGuid / MeshPath (SCNE 28). The GUID is the header GUID as text, the path beside it.
    inline constexpr std::array<std::string_view, 2> kMeshAssetComponents = { "StaticMesh", "SkinnedMesh" };

    // The node holding @p component's mesh reference, and its GUID / path field names; nullopt = none stated.
    struct MeshReferenceNode
    {
        Common::Json::Node Node;
        std::string_view   GuidField;
        std::string_view   PathField;
    };
    [[nodiscard]] std::optional<MeshReferenceNode> MeshReferenceOf( const Common::Json::Node& block,
                                                                           std::string_view          component );

    // WHAT THE PARTITIONER MAY KNOW FROM OUTSIDE THE FILE: a mesh asset's box around its own origin, asked
    // by the GUID its block names (null when it names none) and the path beside it.
    //
    // AN EMPTY SOURCE IS A STATED CONDITION, not a fallback: every mesh-asset record is then its position
    // and is counted in `PointOnlyRecords`, which is what a caller with no registry — a suite over one
    // file, a tool run outside a project — is actually able to say.
    using AssetBoundsSource = std::function<std::optional<Common::Math::AABB>(
         const Common::Content::AssetGuid& guid, std::string_view path )>;

    namespace Detail
    {
        // WHERE A RECORD'S COMPONENT PAYLOAD LIVES, as the loader names it: `Entities[id=7].<Component>`. A
        // record without an id (the planner still places it) is named by the literal "none".
        [[nodiscard]] Common::Json::Path ComponentPath( const Assets::EntityData& record,
                                                               std::string_view          key );

        // A component payload of the record, of any kind, read in place (no copy of the block).
        [[nodiscard]] std::optional<Common::Json::Node> PayloadOf( const Assets::EntityData& record,
                                                                          std::string_view          key );

        // A component block of the record, if it has one. A payload that is not an object is an Issue: the
        // loader could not read it either.
        [[nodiscard]] std::optional<Common::Json::Node>
        BlockOf( const Assets::EntityData& record, std::string_view key, Common::Json::Issues& issues );

        // The id a named field of a component block holds (a DECIMAL STRING, because a 64-bit id does not
        // survive JSON's double). False when the field is absent; a value that is not an id is an Issue.
        [[nodiscard]] bool ReadReference( const Common::Json::Node& block, std::string_view field,
                                                 Common::UUID& out, Common::Json::Issues& issues );

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
        [[nodiscard]] glm::mat4 ComposeLocal( const Assets::EntityData& record );

        // THE WORLD-SPACE POSITION OF EVERY INSTANCE AN InstancedStaticMesh RECORD DRAWS.
        //
        // WORLD and not local, and the line that decides it is not the member's comment but the submit in
        // MeshECSSystem.hpp, which hands the snapshot of InstanceTransforms to the renderer without the
        // entity's own transform. Each matrix is written by ComponentRegistry as the 16 floats of a
        // column-major glm::mat4 (a memcpy), so the translation is elements 12, 13 and 14.
        //
        // A matrix that is not 16 numbers is an Issue and is skipped rather than guessed at; the loader
        // would reject the same block, so it cannot be drawn either.
        void AppendInstancePoints( const Assets::EntityData& record, std::vector<glm::vec3>& out,
                                          Common::Json::Issues& issues );

        // A scalar field of a component block as a number: a bool reads as 1/0, an enum is written as its
        // integer. False when the field is absent; a value that is neither is an Issue.
        [[nodiscard]] bool ReadScalar( const Common::Json::Node& block, std::string_view field, double& out,
                                              Common::Json::Issues& issues );

        // Whether this record's own components make it always-loaded, and why. Author before Component,
        // because the author's marker is the more specific statement.
        [[nodiscard]] AlwaysLoadedReason GlobalReasonOf( const Assets::EntityData& record,
                                                                Common::Json::Issues&     issues );

        // Appends the world position of a local-space box's eight corners, through @p world.
        void AppendBoxCorners( const glm::mat4& world, const glm::vec3& half, std::vector<glm::vec3>& out );

        // Appends the world position of a local-space AABB's eight corners, through @p world.
        void AppendBoundsCorners( const glm::mat4& world, const Common::Math::AABB& box,
                                         std::vector<glm::vec3>& out );

        // The GUID a mesh block names, written as GUID text (SCNE 28). Absent or empty text is the null GUID:
        // the block names no asset the bounds source could answer for. A value that is not GUID text is an
        // Issue, and is the null GUID too.
        [[nodiscard]] Common::Content::AssetGuid
        ReadGuid( const Common::Json::Node& block, std::string_view field, Common::Json::Issues& issues );

        // THE BOX AROUND A SCENE-HELD MESH (the StaticMesh block's EditMesh), in the record's own frame, or
        // nullopt when the block holds none. UE's GetStreamingBounds pattern: an actor whose geometry is its
        // own component's, not an asset's, is bounded by that geometry - so the extent is read from the
        // vertices the block carries, needing no registry. A Positions array that is not whole xyz triples,
        // or holds no vertex, is an Issue on its path (Entities[id=..].StaticMesh.EditMesh.Positions) and
        // gives no box: the loader's FromSerialized refuses the same block, so the mesh is never drawn.
        [[nodiscard]] std::optional<Common::Math::AABB> EditMeshBox( const Common::Json::Node& block,
                                                                            Common::Json::Issues&     issues );


        // THE BOX AROUND A LANDSCAPE SPLINE RECORD'S BAND (world cm), or nullopt when the record has no
        // LandscapeSpline block, its splines do not read (an Issue), or they have no control point.
        std::optional<Common::Math::AABB> LandscapeSplineBandOf( const Assets::EntityData& record,
                                                                        Common::Json::Issues&     issues );

        // The two corners of a spline's band box: its whole footprint, and NOT the entity's position.
        void AppendSplineBand( const Common::Math::AABB& band, std::vector<glm::vec3>& out );

        // THE POINTS ONE RECORD CONTRIBUTES TO ITS COMPOSITE'S FOOTPRINT — the extension point named at
        // the top of this file. Returns false when the record contributed its position and nothing more.
        // A landscape spline contributes its band's box alone (UE ALandscapeSplineActor's streaming bounds).
        bool AppendFootprint( const Assets::EntityData& record, const glm::mat4& world,
                                     const AssetBoundsSource& bounds, std::vector<glm::vec3>& out,
                                     Common::Json::Issues& issues );

        // THE GROUND RECTANGLE OF A LANDSCAPE TILE RECORD, or nullopt when it has none. The root's fields are
        // read with the loader's rule — an absent field is the component's default, which is LandscapeRoot's
        // default because both are spelled from the same constants, and a field of the wrong type is an Issue
        // that keeps that default — and the root is refused by the same ValidateLandscapeRoot the loader
        // applies, so the partition never places a tile the loader refuses.
        //
        // @p world holds the composed world matrices: the root's translation is the frame's origin, as
        // Entity::GetWorldTransform is for the loaded root.
        std::optional<World::Landscape::LandscapeTileRect>
        LandscapeTileRectOf( const Assets::EntityData& tile, const std::vector<glm::mat4>& world,
                             std::span<const Assets::EntityData>                  records,
                             const std::unordered_map<Common::UUID, std::size_t>& byId,
                             Common::Json::Issues&                                issues );
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

    [[nodiscard]] std::vector<UnregisteredReference>
    FindUnregisteredEntityReferences( std::span<const Assets::EntityData> records );

    // ── THE ENTITY DESCRIPTOR: WHAT THE PLANNER READS OF A RECORD (WP18) ─────────────────────────────
    //
    // UE's FWorldPartitionActorDesc: everything streaming generation decides with, read once out of the
    // actor's package and kept in a cache beside the world, so generation never loads an actor
    // (WorldPartitionActorDesc.h; the container refreshes a desc when its package changes, ActorDescContainer.h).
    // The pattern and not the letter: a record states its transform RELATIVE TO ITS PARENT, so a descriptor
    // keeps the local frame and the parent, and the world frame is composed at planning time, as for records.
    // Mesh assets are kept as references, not as boxes: the box is the registry's answer at planning time, so a
    // descriptor depends on its entity's file alone and goes stale only when that file changes.
    //
    // PlanWorldPartition plans from descriptors only; its record overload describes and plans, so a scene in
    // memory and a world on disk (EntityDescriptorIndex.hpp) take the same planner.

    // A mesh asset a record names; the AssetBoundsSource answers its box at planning time.
    struct DescriptorMesh
    {
        std::string Guid; // AssetGuid text; empty when the record names the asset by path alone
        std::string Path;
    };

    // One kEntityReferences reference the record states, its target read and not null.
    struct DescriptorReference
    {
        std::string   Component;
        std::string   Field;
        std::uint64_t Target = 0;
        ReferenceKind Kind   = ReferenceKind::Observation;
    };

    // AN ENTITY PROPERTY A SCRIPT SLOT OF THE RECORD OVERRIDES (Property.Entity, or an item of a Property.Array of
    // them), its target read and not null (WP-ENTITY-REFS). What it IS - hard or soft - is the script's declaration,
    // answered at planning time by a ScriptReferenceSource, so a descriptor depends on its entity's file alone, as a
    // DescriptorMesh does (UE: a Blueprint actor's references in FWorldPartitionActorDesc::GetReferences).
    struct DescriptorScriptReference
    {
        std::string   Script;   // the slot's ScriptKey (ECS::ScriptSlot)
        std::string   Property; // the property's name
        std::uint64_t Target = 0;
    };

    // A kEntityReferencesByName reference the record holds: the NAME, matched to records at planning time.
    struct DescriptorNamedReference
    {
        std::string Component; // the row's ComponentKey
        std::string Field;     // the row's Field
        std::string Name;
    };

    // A name the record carries as the TARGET of a kEntityReferencesByName row: its TargetComponentKey's `Name`.
    struct DescriptorName
    {
        std::string Component; // the row's TargetComponentKey
        std::string Name;
    };

    // A landscape tile: its rectangle is its root's, so it keeps the root and its own tile coordinate.
    struct DescriptorLandscapeTile
    {
        std::uint64_t Root = 0; // 0: the reference is absent, null or unreadable - the tile has no place
        std::int32_t  X    = 0;
        std::int32_t  Z    = 0;
    };

    // A landscape root's tiling, as LandscapeTileRectOf reads it (the origin is the root's world position).
    struct DescriptorLandscapeRoot
    {
        std::uint32_t QuadsPerTile = World::Landscape::kLandscapeDefaultTileQuads;
        float         SpacingCm    = World::Landscape::kLandscapeDefaultSpacingCm;
        float         ZScale       = World::Landscape::kLandscapeDefaultZScale;
    };

    // A Location Volume's box (UE ALocationVolume): half size around the entity's world position, cm, and whether
    // the editor loads it when the world opens. Read here so the editor resolves the volume before any record is.
    struct DescriptorLocationVolume
    {
        glm::vec3 Extent{ 0.0f };
        bool      AutoLoad = false;
    };

    struct EntityDescriptor
    {
        std::optional<std::uint64_t> Id;     // absent: the record states none (or a null one)
        std::optional<std::uint64_t> Parent; // absent: none, or a null one
        std::string                  Tag;
        std::vector<std::string>     Components; // the record's component keys, in file order: its type

        std::optional<glm::vec3> Translation;
        std::optional<glm::vec3> Rotation;
        std::optional<glm::vec3> Scale;

        // A prefab instance: its path (its box is the registry's), and what it lacks of the root transform
        // scene v37 requires (MissingInstanceTransform; empty when nothing) - such an instance has no place.
        std::optional<std::string> PrefabPath;
        std::string                MissingTransform;

        std::vector<Common::Math::AABB> Boxes;  // local boxes the record states itself (primitive, edit mesh)
        std::vector<DescriptorMesh>     Meshes; // mesh assets, answered by the bounds source
        std::vector<glm::vec3>          InstancePoints;

        AlwaysLoadedReason                     Reason = AlwaysLoadedReason::None; // None / Author / Component
        std::vector<DescriptorReference>       References;
        std::vector<DescriptorScriptReference> ScriptReferences; // WP-ENTITY-REFS
        std::vector<DescriptorNamedReference>  NamedReferences;  // WP-ENTITY-REFS
        std::vector<DescriptorName>            Names;            // WP-ENTITY-REFS
        std::optional<DescriptorLandscapeTile> LandscapeTile;
        std::optional<DescriptorLandscapeRoot> LandscapeRoot;
        // A landscape spline: the world box around its band (LandscapeSplineBandOf); absent for any other record,
        // and for a spline whose block does not read or has no control point (then its position, as any record).
        std::optional<Common::Math::AABB> LandscapeSpline;
        // The RuntimeGrid block's grid name (WP22); absent = the world's first grid.
        std::optional<std::string> RuntimeGrid;
        // The StreamingSource block's Target Grids (UE FWorldPartitionStreamingSource::TargetGrids); empty = every
        // grid. Read here so the plan names a grid the world does not have
        // (Entities[i].StreamingSource.TargetGrids).
        std::vector<std::string> TargetGrids;
        // The StreamingSource block's Target HLOD Layers (UE FWorldPartitionStreamingSource::TargetHLODLayers),
        // read so the plan names a layer the world does not have (Entities[i].StreamingSource.TargetHLODLayers).
        std::vector<std::string> TargetHLODLayers;
        // The DataLayers block's layer names (WP-DATA-LAYERS, UE AActor::DataLayerAssets); empty = no layer.
        std::vector<std::string> DataLayers;
        // The LocationVolume block (WP-LOADER-ADAPTERS); absent = not a Location Volume.
        std::optional<DescriptorLocationVolume> LocationVolume;
        // The HLODLayer block's layer name (WP-SETTINGS-UI, UE AActor::HLODLayer); absent = its grid's layer.
        std::optional<std::string> HLODLayer;

        // Every value read here and found of the wrong type, with its path: the planner reports them as its own.
        Common::Json::Issues Issues;
    };

    namespace Detail
    {
        [[nodiscard]] std::optional<std::uint64_t> IdBits( const std::optional<Common::UUID>& id );
    } // namespace Detail

    // THE ONE READER OF A RECORD FOR PLANNING. Everything the planner used to read off the record is read
    // here, once, with the same helpers.
    [[nodiscard]] EntityDescriptor DescribeEntity( const Assets::EntityData& record );

    [[nodiscard]] std::vector<EntityDescriptor>
    DescribeEntities( std::span<const Assets::EntityData> records );

    // ── EVERY ENTITY→ENTITY REFERENCE, RESOLVED TO RECORDS (WP-ENTITY-REFS) ──────────────────────────────
    //
    // UE 5.5: an actor's hard references - its Blueprint's included (FWorldPartitionActorDesc::GetReferences) - join
    // it and what it names into one cluster (FActorClusterContext), and a soft reference (TSoftObjectPtr) to an actor
    // of another cell resolves while that actor is loaded. Here the three ways one entity names another - the
    // register's component fields, a script's Entity properties, a name - are resolved by ONE function, read by the
    // planner (Containment clusters), the cook (a crossing reference is listed, WorldCells::IndexReference) and
    // residency (an Observation is rebound when its target arrives and unbound when it leaves).

    // Where a reference comes from.
    enum class ReferenceOrigin
    {
        Register, // a kEntityReferences row: a component field holding an id
        Script,   // a script's Entity property (DescriptorScriptReference)
        Name,     // a kEntityReferencesByName row, its name matched to the records carrying it
    };

    // WHAT A SCRIPT'S ENTITY PROPERTY IS: the script's own declaration - Property.Entity( { hard = true } ) is UE's
    // hard actor reference (Containment: both entities in one cell, kept whole), Property.Entity() a soft one
    // (Observation: bound while the target is resident, nil while it is not). nullopt: the script does not declare
    // @p property as an Entity (or does not load) - ScriptEngine::ApplyProperties does not apply such a value either,
    // so it is reported and observes. The engine's source reads the script files (WorldCells::ScriptReferenceKinds),
    // as an AssetBoundsSource reads mesh boxes. EMPTY: the caller has no script declarations (a tool, a suite); every
    // script reference observes and the plan says so in one Issue.
    using ScriptReferenceSource =
         std::function<std::optional<ReferenceKind>( std::string_view script, std::string_view property )>;

    // One reference of a world, its holder and target as records.
    struct ResolvedReference
    {
        std::size_t                 From   = kNoRecord;
        std::size_t                 To     = kNoRecord; // kNoRecord: no record claims the target
        ReferenceKind               Kind   = ReferenceKind::Observation;
        ReferenceOrigin             Origin = ReferenceOrigin::Register;
        std::string                 Component; // the holder's block key
        std::string                 Field;     // its field; a script's: "<script key>:<property>"
        std::optional<Common::UUID> TargetId;  // the id it names; absent for a name
        Common::UUID                TargetRecordId = Common::UUID::Null(); // the target record's id, when it has one
    };

    // How a Containment reference of this origin hangs its holder off its target.
    [[nodiscard]] Containment ContainmentOf( ReferenceOrigin origin );

    // Every reference of @p records, in record order (a record's register references, then its script ones, then
    // its named ones). A name carried by several records names each of them (the readers search the whole scene and
    // may find any). What cannot be resolved as declared goes to @p issues, naming the entity.
    [[nodiscard]] std::vector<ResolvedReference>
    ResolveEntityReferences( std::span<const EntityDescriptor> records, const ScriptReferenceSource& scripts,
                             Common::Json::Issues& issues );

    namespace Detail
    {
        [[nodiscard]] std::optional<Common::UUID> ParentOf( const Assets::EntityData& record );

        [[nodiscard]] std::optional<Common::UUID> ParentOf( const EntityDescriptor& descriptor );

        [[nodiscard]] glm::mat4 ComposeLocal( const EntityDescriptor& descriptor );

        // EVERY RECORD'S WORLD MATRIX, composed from its parent chain (ComposeLocal per link). `Records` is a
        // sequence of Assets::EntityData or of EntityDescriptor: one walk for both.
        template <class Records>
        [[nodiscard]] inline std::vector<glm::mat4>
        ComposeWorld( const Records& records, const std::unordered_map<Common::UUID, std::size_t>& byId )
        {
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
                    std::size_t next = kNoRecord;
                    if ( const auto parent = ParentOf( records[walk] ); parent.has_value() )
                    {
                        const auto found = byId.find( *parent );
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
            return world;
        }
    } // namespace Detail

    // THE PARTITION, as a pure function of the parsed records and the world's own block.
    //
    // THE STEPS, and why in this order:
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
    //      target in the same composite, so at worst it widens that composite by the length of an arm.
    //
    //   2. COMPOSITES, as the connected components of the containment relations. Both relations, one
    //      union: an entity can have a hierarchy parent AND a socket target (a weapon parented under
    //      a rack while socketed to a hand is exactly that), and both make it part of one whole. The
    //      anchor is the first member in FILE ORDER that hangs off nothing — the same tie-break the
    //      identity stitch uses; a composite that is nothing but a cycle (a corrupt file) anchors on its
    //      first member, which is a definition rather than a crash.
    //
    //   3. FOOTPRINT AND ALWAYS-LOADED, per composite: the rectangle around every point its members
    //      contribute (Detail::AppendFootprint), and whether any member makes the whole global. One
    //      global member makes the whole composite global — the composite is never divided, and a
    //      camera parented to a character keeps the character with it.
    //
    //   4. THE LEVEL COUNT, from the furthest footprint of a composite that still has to be placed, and
    //      the widest one.
    //
    //   5. PLACEMENT: the lowest level whose cell is as wide as the footprint, the cell of its centre;
    //      wider than any level can be: always-loaded (NoFit).
    //
    // `bounds` answers for the one extent that lives in other files, a mesh asset's (AssetBoundsSource);
    // left empty, every mesh-asset record is its position and is counted as such.
    namespace Detail
    {
        // A descriptor's footprint points: its world position, then the corners of every box it states or
        // the bounds source answers for its meshes, then its instance points. True when it had more than
        // the position (AppendFootprint over the record, the same points).
        bool AppendFootprint( const EntityDescriptor& descriptor, const glm::mat4& world,
                                     const AssetBoundsSource& bounds, std::vector<glm::vec3>& out );

        // A landscape tile's rectangle from its root's descriptor and world position (LandscapeTileRectOf).
        std::optional<World::Landscape::LandscapeTileRect>
        LandscapeTileRectOf( const EntityDescriptor& tile, const std::vector<glm::mat4>& world,
                             std::span<const EntityDescriptor>                    descriptors,
                             const std::unordered_map<Common::UUID, std::size_t>& byId );
    } // namespace Detail

    // THE STREAMING GENERATION FINDINGS OF @p issues, logged one warning line each (UE FStreamingGenerationLogErrorHandler)
    // after the plan's Issues line; Planner findings are not logged here - Common::Json::ReportIssues logs them as
    // the one error line of a read. Returns the logged text ("" when there is none).
    std::string ReportGenerationIssues( std::span<const GenerationIssue> issues, std::string_view world );

    // THE ONE REPORT OF A PLAN'S ISSUES. Every caller that plans a world to act on it - the cook (WorldCells),
    // Play (BeginWorldStreaming) and editor regions (EditorRegions) - names what the planner refused and what the
    // streaming generation found (WorldPartitionPlan::Generation) through this, so an Issue (an unknown
    // StreamingSource.TargetGrids name, a RuntimeGrid.Grid the world lacks, ...) is never seen by one of them and
    // dropped by another. Returns the logged text ("" when there is none).
    std::string ReportPlanIssues( const WorldPartitionPlan& plan, std::string_view world );

    // THE ONE LIST OF EVERYTHING WRONG WITH A PLAN (WP-MAP-CHECK): every value the planner could not read or name it
    // could not resolve (WorldPartitionPlan::Issues, as Planner findings) and every streaming generation finding
    // (WorldPartitionPlan::Generation), in that order. @p records are the ones the plan was made from: a Planner
    // finding's path (`Entities[7]...` or `Entities[id=9000001]...`) names the record it selects.
    [[nodiscard]] std::vector<GenerationIssue> PlanIssues( const WorldPartitionPlan&         plan,
                                                                  std::span<const EntityDescriptor> records );

    // THE GRID A RuntimeGrid NAME STREAMS ON (UE AActor::RuntimeGrid), the one rule the planner and every editor
    // tool that files content by grid (the foliage brush, the procedural volume) read: absent = the world's
    // first grid, UE's main grid; a name the world does not state = nullopt - never read as the first grid by a
    // guess (the planner reports it as a MissingReference Issue with the record's path).
    [[nodiscard]] std::optional<std::size_t> GridIndexOf( const WorldPartitionSerialized&   settings,
                                                                 const std::optional<std::string>& wanted );

    // WHAT CheckRuntimeGrids FOUND. `CellsUsable` is false when the CELLS cannot be made - no partition, a name
    // missing or stated twice (which partition a record's name reaches would be a guess), settings its class
    // cannot place content with (an LHGrid's cell size that is not positive); a bad loading range is an Issue that
    // leaves the cells as they are, because the partition does not read the range
    // (RuntimePartitionLHGridSerialized::LoadingRange).
    struct RuntimeGridsCheck
    {
        Common::Json::Issues Issues;
        bool                 CellsUsable = true;
    };

    // THE DATA LAYERS A WORLD MAY STATE (WP-DATA-LAYERS, UE UDataLayerInstance) - the one rule the planner (step
    // 4d) and every editor edit of the block (EditWorldPartition: the panel's Data Layers section, `desertctl set
    // world WorldPartition.DataLayers...`) read: every layer named, once - a name is the only way an entity reaches
    // a layer, so two layers of one name would make membership a guess. No layers at all is a world without them.
    [[nodiscard]] Common::Json::Issues CheckDataLayers( const WorldPartitionSerialized& settings );

    // THE RUNTIME LAYERS OF A SET OF LAYERS (WP-DATA-LAYERS-RUNTIME): those of @p layers (indices into
    // settings.DataLayers) whose Type is Runtime, in the same order. An editor layer is nothing to the cook or to
    // Play, so it neither splits a cell nor gates one.
    [[nodiscard]] std::vector<std::size_t> RuntimeDataLayersOf( const WorldPartitionSerialized& settings,
                                                                       std::span<const std::size_t>    layers );

    // THE NAME A SET OF RUNTIME LAYERS GIVES ITS CELLS (UE FDataLayersID: a hash of the layers' names in the runtime
    // cell's name). FNV-1a over the NAMES, sorted and each closed by a 0 byte - not over the indices, so a cell's
    // file does not move when the world gains, loses or reorders another layer. Empty set = "" (a cell of no runtime
    // layer keeps the name it always had); otherwise "_DL" and eight hex digits.
    [[nodiscard]] std::string DataLayersCellSuffix( const WorldPartitionSerialized& settings,
                                                           std::span<const std::size_t>    runtimeLayers );

    // A CELL'S NAME ON DISK, without the extension: grid, level, coordinate and its runtime layers
    // (`G0_L1_3_-2`, `G0_L0_1_0_DL1A2B3C4D`). The one statement of it: the cook names the cell's file
    // (WorldCells::CellFileName) and Play names the cell in its log by it.
    [[nodiscard]] std::string CellStem( const WorldPartitionSerialized& settings, const PlannedCell& cell );

    // The index of the HLOD layer @p name names in @p settings (WorldPartitionSerialized::HLODLayers), or nullopt
    // when the world states none of that name.
    [[nodiscard]] std::optional<std::size_t> HLODLayerIndexOf( const WorldPartitionSerialized& settings,
                                                                      std::string_view                name );

    // The index of the layer @p name names in @p settings, or nullopt when the world states none of that name.
    [[nodiscard]] std::optional<std::size_t> DataLayerIndexOf( const WorldPartitionSerialized& settings,
                                                                      std::string_view                name );

    // The index of @p layer's parent (DataLayerSerialized::Parent, WP v4), or nullopt for a top-level layer or a
    // parent the world does not state (CheckDataLayers names that one).
    [[nodiscard]] std::optional<std::size_t> DataLayerParentIndex( const WorldPartitionSerialized& settings,
                                                                          const std::size_t               layer );

    // Whether @p layer is @p ancestor or nests under it at any depth: a member of the one is a member of the other
    // (UE resolves an actor's layers with their parents). Bounded by the layer count, so an unchecked cycle ends.
    [[nodiscard]] bool DataLayerIsWithin( const WorldPartitionSerialized& settings, const std::size_t layer,
                                                 const std::size_t ancestor );

    // UE UDataLayerInstance::IsEffectiveVisible / IsEffectiveLoadedInEditor: each layer's own flag (index = the
    // world's layer) AND every ancestor's - a child is loaded or visible only while all its parents are.
    [[nodiscard]] std::vector<bool> EffectiveDataLayerFlags( const WorldPartitionSerialized& settings,
                                                                    const std::vector<bool>&        own );

    // The DebugColor a layer made in the editor starts with (UE FColor::MakeRandomSeededColor over the asset's
    // name): a hue from the name's FNV-1a hash at one saturation and value, so two layers of a world differ and
    // the same name is the same colour on every machine. sRGB 0..1.
    [[nodiscard]] std::array<float, 3> SeededDataLayerColor( std::string_view name );

    // A footprint's larger side: what its level is chosen by. Not finite = no level can hold it.
    [[nodiscard]] double FootprintExtent( const CellBounds& box );

    // ── THE RUNTIME PARTITIONS (WP-HASH-SET, UE UWorldPartitionRuntimeHashSet) ───────────────────────────
    //
    // ONE CLASS PER RuntimePartitionType, stateless (UE's class default object of a URuntimePartition subclass):
    // the planner asks it to check a partition's settings and to place what streams on it; the HLOD rules ask it
    // whether anything of it is ever away; the streaming query and the cook ask it for the cells' grid. Nothing
    // that plans, cooks or streams a world switches on the type - a new kind of partition is a new class here.
    class IRuntimePartition
    {
    public:
        IRuntimePartition()                                      = default;
        IRuntimePartition( const IRuntimePartition& )            = delete;
        IRuntimePartition& operator=( const IRuntimePartition& ) = delete;
        IRuntimePartition( IRuntimePartition&& )                 = delete;
        IRuntimePartition& operator=( IRuntimePartition&& )      = delete;
        virtual ~IRuntimePartition()                             = default;

        // The issues of @p desc's own settings, each with its path under WorldPartition.RuntimePartitions[@p
        // index]; clears @p check.CellsUsable when what streams on it cannot be placed (UE
        // URuntimePartition::IsValidGrid).
        virtual void CheckSettings( const RuntimePartitionSerialized& desc, std::size_t index,
                                    RuntimeGridsCheck& check ) const = 0;

        // UE URuntimePartition::GenerateStreaming: places @p composites - indices into @p plan.Composites, every
        // one on partition @p index, placed by a footprint, Reason None, in ascending order. Cells it makes are
        // appended to @p plan.Cells ordered by level, X, Z, then the set of runtime data layers (@p world's
        // DataLayers: the cell's third coordinate, WP-DATA-LAYERS-RUNTIME); a composite it cannot or will not put
        // in a cell gets its always-loaded Reason (the planner lists AlwaysLoaded afterwards, in file order).
        // Called once per partition, in partition order, after CheckSettings passed for every partition.
        virtual void GenerateStreaming( const WorldPartitionSerialized& world, const RuntimePartitionSerialized& desc,
                                        std::size_t index, std::span<const std::size_t> composites,
                                        WorldPartitionPlan& plan ) const = 0;

        // Whether what streams on it can be away, so that HLOD setups stand in for it (UE: a partition's HLOD
        // setups are read for its streamed cells only).
        [[nodiscard]] virtual bool CanBeAway() const = 0;

        // The grid its cells stream on (UE URuntimePartitionLHGrid), or null when it makes no cells.
        [[nodiscard]] virtual const RuntimePartitionLHGridSerialized*
        CellGrid( const RuntimePartitionSerialized& desc ) const = 0;
    };

    // URuntimePartitionLHGrid: the square hierarchical grid (UE FSquare2DGridHelper).
    class RuntimePartitionLHGrid final : public IRuntimePartition
    {
    public:
        void CheckSettings( const RuntimePartitionSerialized& desc, std::size_t index,
                            RuntimeGridsCheck& check ) const override
        {
            if ( !desc.LHGrid.has_value() )
            {
                check.Issues.push_back( { std::format( "WorldPartition.RuntimePartitions[{}].LHGrid", index ),
                                          "the LHGrid block of an LHGrid partition", "none" } );
                check.CellsUsable = false;
                return;
            }
            const RuntimePartitionLHGridSerialized& grid = *desc.LHGrid;
            if ( !( grid.CellSize > 0.0f ) || !std::isfinite( grid.CellSize ) )
            {
                check.Issues.push_back(
                     { std::format( "WorldPartition.RuntimePartitions[{}].LHGrid.CellSize", index ),
                       "a positive cell size", std::format( "{}", grid.CellSize ) } );
                check.CellsUsable = false;
            }
            // A range of 0 is a grid that never streams anything in; the cells do not read the range, so they
            // are still made.
            if ( !( grid.LoadingRange > 0.0f ) || !std::isfinite( grid.LoadingRange ) )
                check.Issues.push_back(
                     { std::format( "WorldPartition.RuntimePartitions[{}].LHGrid.LoadingRange", index ),
                       "a positive loading range", std::format( "{}", grid.LoadingRange ) } );
        }

        void GenerateStreaming( const WorldPartitionSerialized& world, const RuntimePartitionSerialized& desc,
                                std::size_t index, std::span<const std::size_t> composites,
                                WorldPartitionPlan& plan ) const override
        {
            const float cellSize = desc.LHGrid->CellSize;

            // HOW MANY LEVELS THIS GRID HAS FOR THIS WORLD. Its top level is the first whose cell reaches the
            // furthest footprint edge from the origin among the composites ON THIS PARTITION, so at the top every
            // footprint that does not straddle an axis is inside one of the four cells around the origin. Only
            // composites still to be placed count: the sun at 10 km does not widen any grid. The cap keeps a
            // corrupt coordinate (1e30) from asking for a level past what an int32 cell index can express at
            // level 0; a footprint beyond it is NoFit.
            constexpr int kMaxLevels = 31;
            double        furthest   = 0.0;
            double        widest     = 0.0;
            for ( const std::size_t group : composites )
            {
                const CellBounds& box = *plan.Composites[group].Footprint;
                furthest              = std::max( { furthest, std::fabs( static_cast<double>( box.MinX ) ),
                                                    std::fabs( static_cast<double>( box.MaxX ) ),
                                                    std::fabs( static_cast<double>( box.MinZ ) ),
                                                    std::fabs( static_cast<double>( box.MaxZ ) ) } );
                const double extent   = FootprintExtent( box );
                if ( std::isfinite( extent ) )
                    widest = std::max( widest, extent );
            }
            int levels = 1;
            while ( levels < kMaxLevels && ( LevelCellSize( cellSize, levels - 1 ) < furthest ||
                                             LevelCellSize( cellSize, levels - 1 ) < widest ) )
                ++levels;
            plan.GridLevelCount[index] = levels;
            plan.LevelCount            = std::max( plan.LevelCount, levels );

            // PLACEMENT (UE FSquare2DGridHelper): the lowest level whose cell is at least as wide as the
            // footprint's larger side; the cell of the footprint's centre in it. A centre whose cell index an
            // int32 cannot hold (a corrupt 1e30, a NaN) is not converted - that conversion is undefined
            // behaviour - and the composite is NoFit.
            // A cell is keyed by level, square and the composite's RUNTIME data layers (UE generates its runtime
            // cells per data layer set): composites of one square in different sets are different cells.
            std::map<std::tuple<int, std::int32_t, std::int32_t, std::vector<std::size_t>>, std::vector<std::size_t>>
                 cells;
            for ( const std::size_t group : composites )
            {
                PlannedComposite& held      = plan.Composites[group];
                const CellBounds  footprint = *held.Footprint;
                const double      extent    = FootprintExtent( footprint );
                const double      centreX =
                     ( static_cast<double>( footprint.MinX ) + static_cast<double>( footprint.MaxX ) ) * 0.5;
                const double centreZ =
                     ( static_cast<double>( footprint.MinZ ) + static_cast<double>( footprint.MaxZ ) ) * 0.5;
                bool placed = false;
                for ( int level = 0; std::isfinite( extent ) && level < levels; ++level )
                {
                    const double size = LevelCellSize( cellSize, level );
                    if ( size < extent )
                        continue;
                    if ( !( std::fabs( centreX / size ) < 2147483647.0 ) ||
                         !( std::fabs( centreZ / size ) < 2147483647.0 ) )
                        break;
                    held.Level = level;
                    held.Cell  = CellCoord{ static_cast<std::int32_t>( std::floor( centreX / size ) ),
                                           static_cast<std::int32_t>( std::floor( centreZ / size ) ) };
                    placed     = true;
                    break;
                }
                if ( !placed )
                {
                    held.Reason = AlwaysLoadedReason::NoFit;
                    continue;
                }
                cells[std::make_tuple( held.Level, held.Cell.X, held.Cell.Z,
                                       RuntimeDataLayersOf( world, held.DataLayers ) )]
                     .push_back( group );
            }

            // THE CELLS, ordered by level, X, Z, runtime data layers. Half the widest footprint each level holds is how far past its
            // square a cell of it may reach (UE's loose cell bounds).
            std::map<int, double> reach;
            for ( const auto& [key, held] : cells )
            {
                double& margin = reach[std::get<0>( key )];
                for ( const std::size_t group : held )
                    margin = std::max( margin, FootprintExtent( *plan.Composites[group].Footprint ) * 0.5 );
            }
            for ( auto& [key, held] : cells )
            {
                PlannedCell cell;
                cell.Grid           = index;
                cell.Level          = std::get<0>( key );
                cell.Cell.X         = std::get<1>( key );
                cell.Cell.Z         = std::get<2>( key );
                cell.DataLayers     = std::get<3>( key );
                cell.Square         = GridSquareOf( cell.Cell, LevelCellSize( cellSize, cell.Level ) );
                const double margin = reach.at( cell.Level );
                cell.Bounds = CellBounds{ static_cast<float>( static_cast<double>( cell.Square.MinX ) - margin ),
                                          static_cast<float>( static_cast<double>( cell.Square.MinZ ) - margin ),
                                          static_cast<float>( static_cast<double>( cell.Square.MaxX ) + margin ),
                                          static_cast<float>( static_cast<double>( cell.Square.MaxZ ) + margin ) };
                // What the cell holds, vertically: the union of its composites' spans (each has one: it has a
                // footprint).
                cell.Height = *plan.Composites[held.front()].Height;
                for ( const std::size_t group : held )
                    cell.Height = UnionHeight( cell.Height, *plan.Composites[group].Height );
                cell.Composites = std::move( held );
                plan.Cells.push_back( std::move( cell ) );
            }
        }

        [[nodiscard]] bool CanBeAway() const override
        {
            return true;
        }

        [[nodiscard]] const RuntimePartitionLHGridSerialized*
        CellGrid( const RuntimePartitionSerialized& desc ) const override
        {
            return desc.LHGrid.has_value() ? &*desc.LHGrid : nullptr;
        }
    };

    // URuntimePartitionPersistent: everything on it is always loaded, by the world's choice.
    class RuntimePartitionPersistent final : public IRuntimePartition
    {
    public:
        void CheckSettings( const RuntimePartitionSerialized& desc, std::size_t index,
                            RuntimeGridsCheck& check ) const override
        {
            if ( desc.LHGrid.has_value() )
                check.Issues.push_back( { std::format( "WorldPartition.RuntimePartitions[{}].LHGrid", index ),
                                          "no LHGrid block on a Persistent partition (nothing reads it)",
                                          "an LHGrid block" } );
        }

        void GenerateStreaming( const WorldPartitionSerialized& /*world*/, const RuntimePartitionSerialized& /*desc*/,
                                std::size_t /*index*/, std::span<const std::size_t> composites,
                                WorldPartitionPlan& plan ) const override
        {
            for ( const std::size_t group : composites )
                plan.Composites[group].Reason = AlwaysLoadedReason::Persistent;
        }

        [[nodiscard]] bool CanBeAway() const override
        {
            return false;
        }

        [[nodiscard]] const RuntimePartitionLHGridSerialized*
        CellGrid( const RuntimePartitionSerialized& /*desc*/ ) const override
        {
            return nullptr;
        }
    };

    // The class of @p type (UE FRuntimePartitionDesc::Class). Every RuntimePartitionType has one; an enumerator
    // with no case fails the build (-Wswitch), never a silent default.
    [[nodiscard]] const IRuntimePartition& RuntimePartitionClass( RuntimePartitionType type );

    // The name a RuntimePartitionType is stated by in the file (reflect-cpp writes the enumerator's name) - the
    // one spelling the panel, `desertctl get/set world` and the loader's plan line show. An enumerator with no
    // case fails the build (-Wswitch), never a silent default.
    [[nodiscard]] std::string_view RuntimePartitionTypeName( RuntimePartitionType type );

    // The grid cells of partition @p partition stream on, or null when @p settings states no such partition or
    // it makes no cells - the streaming query's, the residency's and the cook's one way to a cell's numbers.
    [[nodiscard]] const RuntimePartitionLHGridSerialized*
    CellGridOf( const WorldPartitionSerialized& settings, std::size_t partition );

    // THE PARTITIONS A WORLD MAY STATE (WP22, WP-HASH-SET) - the one rule the planner (step 4a) and every editor
    // edit of the block (EditWorldPartition: the panel's Runtime partitions section, `desertctl set world
    // WorldPartition...`) read, so the editor refuses exactly what the planner would. At least one partition;
    // every partition named, once; then each partition's own settings, by its class.
    [[nodiscard]] RuntimeGridsCheck CheckRuntimeGrids( const WorldPartitionSerialized& settings );

    namespace Detail
    {
        // The highest level count a grid may have: a cell index of a coordinate past it does not fit an int32.
        inline constexpr int kMaxGridLevels = 31;

        // THE PLACEMENT RULE (UE FSquare2DGridHelper), its one statement: the lowest of @p levelCount levels whose
        // cell is at least as wide as the footprint's larger side, and the cell of the footprint's centre in it.
        // nullopt: no level holds it, or its centre's cell index does not fit an int32 (a corrupt 1e30, a NaN - that
        // conversion is undefined behaviour) - the planner's NoFit.
        [[nodiscard]] std::optional<std::pair<int, CellCoord>>
        PlaceFootprint( const CellBounds& footprint, float cellSize, int levelCount );

        // A record as a finding names it: its tag and id (`House (9200001)`), or `Entities[<id>]` without a tag.
        [[nodiscard]] std::string RecordName( std::span<const EntityDescriptor> records, std::size_t record );

        // The names of @p layers (indices into settings.DataLayers), or "none".
        [[nodiscard]] std::string LayerNames( const WorldPartitionSerialized& settings,
                                                     std::span<const std::size_t>    layers );

        // STEP 7 OF THE PLANNER: THE REFERENCES, VALIDATED (UE FWorldPartitionStreamingGenerator's validation of the
        // actor desc views, each finding to IStreamingGenerationErrorHandler). Into plan.Generation, never into the
        // plan itself:
        //   * a reference naming an id no record is (OnInvalidReference) - a dangling parent or socket target, an
        //     observation of nothing;
        //   * an always-loaded record referencing one that would stream on its own (OnInvalidReferenceGridPlacement,
        //     UE's rule: invalid when the referer is not spatially loaded and the reference is). By CONTAINMENT the
        //     target is pulled into the always-loaded set whole - named with the cell it would have had; by
        //     OBSERVATION it reads null wherever its cell is not loaded;
        //   * an observation between different sets of runtime data layers (OnInvalidReferenceDataLayers), split as
        //     the owner's rule list splits it: into no layer, or across two sets. (Containment across layers is the
        //     planner's own Issue in step 4d: a composite has its anchor's layers.)
        //   * a composite member stating a runtime grid other than the composite's (OnInvalidReferenceRuntimeGrid).
        void ValidateReferences( WorldPartitionPlan& plan, std::span<const EntityDescriptor> records,
                                        const WorldPartitionSerialized& settings, bool gridsUsable,
                                        const std::unordered_map<Common::UUID, std::size_t>& byId,
                                        const std::vector<std::optional<CellBounds>>&        ownFootprint );
    } // namespace Detail

    // THE PLANNER, over descriptors (an index on disk, EntityDescriptorIndex.hpp, or DescribeEntities of a
    // scene in memory). Record `r` of the plan is descriptor `r`.
    [[nodiscard]] WorldPartitionPlan PlanWorldPartition( std::span<const EntityDescriptor> records,
                                                                const WorldPartitionSerialized&   settings,
                                                                const AssetBoundsSource&          bounds  = {},
                                                                const ScriptReferenceSource&      scripts = {} );

    // A scene in memory: described, then planned by the one planner.
    [[nodiscard]] WorldPartitionPlan PlanWorldPartition( std::span<const Assets::EntityData> records,
                                                                const WorldPartitionSerialized&     settings,
                                                                const AssetBoundsSource&            bounds  = {},
                                                                const ScriptReferenceSource&        scripts = {} );

    // How many cells each level holds, index = level. Sized LevelCount.
    [[nodiscard]] std::vector<std::size_t> CellsPerLevel( const WorldPartitionPlan& plan );

    // How many always-loaded composites each reason accounts for, indexed by AlwaysLoadedReason.
    [[nodiscard]] std::vector<std::size_t> AlwaysLoadedByReason( const WorldPartitionPlan& plan );

    // THE PLAN IN ONE LINE — one wording, used by the loader's log and by `WorldGen --partition`, so the
    // two never describe the same world differently. Lengths are printed as whole world units.
    [[nodiscard]] std::string SummarisePartition( const WorldPartitionPlan&       plan,
                                                         const WorldPartitionSerialized& settings );

    // THE BOX A PREFAB STATES (AL1-8a): what its registry row carries in the `bounds` column, so a world
    // cell holding an instance learns its extent without loading the file (UE's Actor Descriptor).
    //
    // IN THE ROOT'S OWN FRAME, WITHOUT THE ROOT'S TRANSFORM: an instance record states its own
    // Translation/Rotation/Scale and those are what the instantiated root carries, so the partitioner
    // applies the instance's matrix to this box exactly as it applies a mesh record's matrix to the mesh's.
    // Every record's boxes are ForEachLocalBox's - the partitioner's rule - composed to the root through
    // ComposeWorld. A root that names no record of @p records leaves the frame at the file's origin.
    //
    // nullopt when nothing in the body has an extent (a UI prefab, an empty group): the column's "absent".
    // A component block that does not read is added to @p issues and contributes no box, as in the plan.
    [[nodiscard]] std::optional<Common::Math::AABB>
    PrefabBounds( std::span<const Assets::EntityData> records, const Common::UUID& root,
                  const AssetBoundsSource& bounds, Common::Json::Issues& issues );
} // namespace Desert::Core::Rules
