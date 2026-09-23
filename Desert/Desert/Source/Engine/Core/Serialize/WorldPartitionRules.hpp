#pragma once

// WHICH CELL OF WHICH GRID LEVEL EACH THING IN A PARTITIONED WORLD BELONGS TO, AND WHAT IS ALWAYS LOADED.
//
// ── WHAT THIS IS NOT ──────────────────────────────────────────────────────────────────────────────
//
// It is not streaming. There is no residency here, no root set that moves with the camera, and no
// spatial index: this file makes the scene FORMAT able to express a partition and makes the partitioner
// assign every whole to exactly one cell of one level — or to the always-loaded set — and say why. A
// cell is a set of records and a rectangle here, nothing more. Which cells are resident at a camera
// position is WP4's pure query over this plan, and `GridSerialized::LoadingRange` is its input.
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
// ── A WIDE COMPOSITE GOES UP A LEVEL; THE CELL DOES NOT GROW (owner decision O1, 2026-09-23) ──────
//
// The grid has LEVELS. Level L is the same grid with cells CellSize·2^L wide, aligned on the origin, so
// every level-L edge is also an edge of every level below it. A composite is put, whole, on the LOWEST
// level at which its footprint lies inside exactly ONE cell; a composite that no level can hold that way
// goes to the ALWAYS-LOADED set. The level is derived from the footprint and is never authored.
//
// This replaces WP1's growing cell, and the reason is what growth cost: a cell grown to reach one wide
// thing dragged EVERY small thing in that cell out to the wide thing's loading distance. A level moves
// only the wide thing — the small things beside it stay in their small cell. It is the pattern of UE's
// FSquare2DGridHelper::GetPartitionedActors (UE:Engine/Source/Runtime/Engine/Private/WorldPartition/
// RuntimeSpatialHash/RuntimeSpatialHashGridHelper.cpp:211-262), not its letter:
//
//   * UE's top level is ONE cell centred on the origin and as wide as the world, so nothing spatial ever
//     misses it. Ours stays aligned on the origin to the top, so the top level has up to four cells and a
//     composite straddling X = 0 or Z = 0 fits no level. That composite is ALWAYS-LOADED, which is what
//     UE's world-sized top cell amounts to anyway — a cell containing every camera position there is.
//   * UE sizes the level count from the world bounds; so does this: the top level is the first whose
//     cell reaches the furthest footprint from the origin (`WorldPartitionPlan::LevelCount`).
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
// A scene record carries no extent of its own: a mesh's bounds live in the mesh asset and a prefab's are
// not stored anywhere (see UnplacedPrefabInstances). So a footprint is the XZ rectangle around a set of
// world-space POINTS, and `Detail::AppendFootprint` is the one place that decides which points a record
// contributes. Today it knows four things, each stated where it is read:
//
//   * the record's own world position — every record;
//   * the eight corners of the primitive CUBE, when the record draws one: that cube is 100 cm a side and
//     centred (Engine/Geometry/PrimitiveMeshFactory.cpp, CreateCube), so its corners through the world
//     matrix are exact, and 2351 of the corpus' 2432 primitive draws are cubes, as is every WorldGen record;
//   * the four corners of a Terrain's square, `Size` wide and centred (TerrainMeshFactory.hpp);
//   * every instance of an InstancedStaticMesh, whose matrices are WORLD-space (MeshECSSystem.hpp submits
//     the snapshot without the entity's transform) — a kilometre of grass is one record.
//
// A mesh ASSET's bounds are not here: they need the asset, and this function is pure over one file.
// WP15 stores them in the registry; that is the day `AppendFootprint` gains a fifth source, and the only
// function that changes. Until then such a record is its position, and `PointOnlyRecords` counts them.
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

#include <Common/Core/UUID.hpp>
#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

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
#include <initializer_list>
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
    [[nodiscard]] inline CellCoord CellOf( float worldX, float worldZ, double cellSize )
    {
        CellCoord cell;
        cell.X = static_cast<std::int32_t>( std::floor( static_cast<double>( worldX ) / cellSize ) );
        cell.Z = static_cast<std::int32_t>( std::floor( static_cast<double>( worldZ ) / cellSize ) );
        return cell;
    }

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

    // A RECTANGLE ON THE GROUND, in world units (centimetres). X and Z only, for the same reason the
    // grid is two-dimensional: height does not partition.
    struct CellBounds
    {
        float MinX = 0.0f;
        float MinZ = 0.0f;
        float MaxX = 0.0f;
        float MaxZ = 0.0f;
    };

    // Edge of one cell of grid level @p level: CellSize·2^level. ldexp and not a loop of doublings, so a
    // level is exactly a power of two times the authored number with no accumulated rounding.
    [[nodiscard]] inline double LevelCellSize( float cellSize, int level )
    {
        return std::ldexp( static_cast<double>( cellSize ), level );
    }

    // The square a cell of a level covers.
    [[nodiscard]] inline CellBounds GridSquareOf( const CellCoord& cell, double levelCellSize )
    {
        CellBounds square;
        square.MinX = static_cast<float>( static_cast<double>( cell.X ) * levelCellSize );
        square.MinZ = static_cast<float>( static_cast<double>( cell.Z ) * levelCellSize );
        square.MaxX = static_cast<float>( static_cast<double>( cell.X + 1 ) * levelCellSize );
        square.MaxZ = static_cast<float>( static_cast<double>( cell.Z + 1 ) * levelCellSize );
        return square;
    }

    // THE ONE CELL OF A LEVEL THAT HOLDS @p footprint WHOLE, if there is one.
    //
    // A cell is HALF-OPEN, [k·s, (k+1)·s), so a footprint whose maximum lies exactly on an edge does not
    // spill into the next cell. That is not a nicety: WorldGen's ground tile is exactly one cell wide and
    // sits exactly on the edges, and a closed interval would push every tile of a generated world up a
    // level. A zero-width footprint (one point) is in the cell of its minimum.
    [[nodiscard]] inline std::optional<CellCoord> SingleCellHolding( const CellBounds& footprint,
                                                                     double            levelCellSize )
    {
        // The last cell the footprint touches: the one before its maximum's cell when the maximum lies
        // exactly on an edge, never before its first cell.
        const auto last = []( float min, float max, double size )
        {
            const double low = std::floor( static_cast<double>( min ) / size );
            return std::max( low, std::ceil( static_cast<double>( max ) / size ) - 1.0 );
        };

        // A coordinate whose cell index an int32 cannot hold (a corrupt 1e30, a NaN) fits no cell of this
        // level: it is not converted, because that conversion is undefined behaviour.
        for ( const float edge : { footprint.MinX, footprint.MinZ, footprint.MaxX, footprint.MaxZ } )
        {
            if ( !( std::fabs( static_cast<double>( edge ) / levelCellSize ) < 2147483647.0 ) )
                return std::nullopt;
        }

        const CellCoord cell = CellOf( footprint.MinX, footprint.MinZ, levelCellSize );
        if ( static_cast<double>( cell.X ) != last( footprint.MinX, footprint.MaxX, levelCellSize ) ||
             static_cast<double>( cell.Z ) != last( footprint.MinZ, footprint.MaxZ, levelCellSize ) )
            return std::nullopt;
        return cell;
    }

    // WHY A COMPOSITE IS IN THE ALWAYS-LOADED SET. Most specific first: the author said so, a component
    // says so, the grid could not hold it, or there is no grid.
    enum class AlwaysLoadedReason
    {
        None,      // it is in a cell
        Author,    // a member carries the `AlwaysLoaded` marker
        Component, // a member carries a component `kComponentLoading` classifies Global
        NoFit,     // no level has one cell holding its footprint (it straddles X = 0 or Z = 0)
        NoGrid,    // the world states no usable grid: no `Grids` entry, or a CellSize <= 0
    };

    // ONE COMPOSITE AND WHERE IT WENT. Members are record indices in file order and always include the
    // anchor. `Level`/`Cell` are meaningful only when `Reason` is None.
    struct PlannedComposite
    {
        std::size_t              Anchor = kNoRecord; // first member in file order that hangs off nothing
        std::vector<std::size_t> Members;
        // nullopt when no member has a known position — only unplaced prefab instances. Such a composite
        // is put in the level-0 cell of the origin, which is wrong and is why they are listed.
        std::optional<CellBounds> Footprint;

        AlwaysLoadedReason Reason  = AlwaysLoadedReason::None;
        std::size_t        Because = kNoRecord; // the member that made it Author/Component
        int                Level   = 0;
        CellCoord          Cell;
    };

    // ONE CELL OF ONE LEVEL THAT HOLDS SOMETHING. `Square` is the cell's own square at its level: a cell
    // never grows, so everything it holds lies inside it — the suite asserts exactly that over the corpus.
    struct PlannedCell
    {
        int                      Level = 0;
        CellCoord                Cell;
        CellBounds               Square;
        std::vector<std::size_t> Composites; // indices into WorldPartitionPlan::Composites
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
        // and a pure function of THIS file cannot have it. They contribute no point to any footprint
        // and are listed here, which is what makes it a stated limitation instead of a wrong answer.
        std::vector<std::size_t> UnplacedPrefabInstances;

        // Records whose footprint is their position alone — a mesh asset, a light, a script: what WP15's
        // stored bounds will turn into rectangles. A number, so the size of that gap is visible.
        std::size_t PointOnlyRecords = 0;

        // How many levels the grid has for THIS world: level LevelCount-1 is the first whose cell reaches
        // the footprint furthest from the origin. At least 1 when there is a grid; 0 when there is none.
        int LevelCount = 0;

        // Every cell that holds at least one composite, ordered by level, then X, then Z, so the plan
        // does not depend on a hash map's iteration order. A cell nothing is assigned to is not listed.
        std::vector<PlannedCell> Cells;

        // The always-loaded composites, as indices into Composites, in file order of their anchors.
        std::vector<std::size_t> AlwaysLoaded;

        // THE WORLD'S ONE NUMBER FOR "HOW MUCH DID HOLDING WHOLES COST" (WP1's MaxGrowth): the highest
        // level any composite was promoted to, and which composite (kNoRecord when no composite is in a
        // cell). Zero means every placed composite fits a cell of the authored size.
        int         MaxLevel          = 0;
        std::size_t MaxLevelComposite = kNoRecord;

        // A world may state more grids than the planner uses. Every composite goes to `Grids[0]` until
        // WP22 lets a record choose its grid; how many were stated and not used is said, not dropped.
        std::size_t UnusedGrids = 0;
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
         { "Camera", ComponentLoading::Global },               // the view itself; brief O1: sun/sky/camera
         { "DirectionLight", ComponentLoading::Global },       // the sun lights every cell at once
         { "Skybox", ComponentLoading::Global },               // at infinity
         { "SkyAtmosphere", ComponentLoading::Global },        // at infinity
         { "ExponentialHeightFog", ComponentLoading::Global }, // a world-wide medium, not a volume
         { "VolumetricCloud", ComponentLoading::Global },      // the cloud layer covers the planet
         // A hero cloud stands kilometres up and is seen from tens of kilometres away; no ground loading
         // range is that wide, so it would vanish while in plain view.
         { "HeroCloud", ComponentLoading::Global },
         // The author's override, and the only authored input to partitioning besides the grid.
         { "AlwaysLoaded", ComponentLoading::Global },
         // ── By field ──
         // RenderMode 1 = WorldSpace (a nameplate, a floating panel): it lives at its entity. 0 =
         // ScreenSpace (HUD, menus), the default: it has no place in the world at all.
         { "UICanvas", ComponentLoading::ByField, "RenderMode", 1.0, true },
         // `Spatial` true (the default) attenuates from the entity; false is music/ambience — no place.
         { "AudioSource", ComponentLoading::ByField, "Spatial", 1.0, false },
         // ── Spatial ──
         { "Animation", ComponentLoading::Spatial },
         { "CharacterController", ComponentLoading::Spatial },
         { "Collider", ComponentLoading::Spatial },
         { "ControlRig", ComponentLoading::Spatial },
         { "Folder", ComponentLoading::Spatial }, // an outliner grouping: its children decide
         { "Foliage", ComponentLoading::Spatial },
         { "InstancedStaticMesh", ComponentLoading::Spatial }, // its instances are its footprint
         { "Lock", ComponentLoading::Spatial },
         { "Locomotion", ComponentLoading::Spatial },
         { "Material", ComponentLoading::Spatial },
         { "Morph", ComponentLoading::Spatial },
         { "ParticleEmitter", ComponentLoading::Spatial },
         { "PointLight", ComponentLoading::Spatial }, // has a radius; the sun is the global light
         { "Projectile", ComponentLoading::Spatial },
         { "Retarget", ComponentLoading::Spatial },
         { "RigidBody", ComponentLoading::Spatial },
         // A script's reach is whatever it does, which no file states. Spatial, and a game-manager script
         // is exactly what the author's AlwaysLoaded marker is for.
         { "Script", ComponentLoading::Spatial },
         { "SkinnedMesh", ComponentLoading::Spatial },
         { "SocketAttachment", ComponentLoading::Spatial },
         { "SpotLight", ComponentLoading::Spatial },
         { "StaticMesh", ComponentLoading::Spatial },
         { "Terrain", ComponentLoading::Spatial }, // its square is its footprint
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
         { "UIOverlay", ComponentLoading::Spatial }, // lives on a canvas entity
         { "UIOverlayTrigger", ComponentLoading::Spatial },
         { "UIPanel", ComponentLoading::Spatial },
         { "UIPointerEvents", ComponentLoading::Spatial },
         { "UIProgressBar", ComponentLoading::Spatial },
         { "UIRenderTexture", ComponentLoading::Spatial },
         { "UIScreen", ComponentLoading::Spatial },
         { "UIScreenStack", ComponentLoading::Spatial }, // lives on a canvas entity
         { "UIScrollView", ComponentLoading::Spatial },
         { "UISlider", ComponentLoading::Spatial },
         { "UIStyle", ComponentLoading::Spatial },
         { "UIText", ComponentLoading::Spatial },
         { "UIToggle", ComponentLoading::Spatial },
         { "UITween", ComponentLoading::Spatial },
    };

    // The author's marker, by its key on disk. Named once: the register row above and the reason
    // Author are both about it.
    inline constexpr std::string_view kAlwaysLoadedComponent = "AlwaysLoaded";

    // THE RECORD KINDS WHOSE CONTENTS ARE NOT A POINT AT THEIR OWN POSITION, by their names on disk. One
    // constant per name, so a suite can pin them: see "A COMPOSITE'S FOOTPRINT" at the top of this file.
    inline constexpr std::string_view kInstancePointsComponent = "InstancedStaticMesh";
    inline constexpr std::string_view kInstancePointsField     = "InstanceTransforms";
    inline constexpr std::string_view kPrimitiveComponent      = "StaticMesh";
    inline constexpr std::string_view kPrimitiveField          = "Primitive";
    inline constexpr std::string_view kCubePrimitive           = "Cube";
    // The primitive cube's half edge in world units: a unit cube scaled to one metre (PrimitiveMeshFactory).
    inline constexpr float            kCubeHalfEdge     = 50.0f;
    inline constexpr std::string_view kTerrainComponent = "Terrain";
    inline constexpr std::string_view kTerrainSizeField = "Size";
    // TerrainData::Size's default, for a block that omits it (same restatement as AbsentIsGlobal above).
    inline constexpr float kTerrainDefaultSize = 5000.0f;

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

        // A scalar field of a component block as a number: a bool reads as 1/0, an enum is written as its
        // integer. False when the field is absent or is not a scalar.
        [[nodiscard]] inline bool ReadScalar( const rfl::Generic::Object& block, std::string_view field,
                                              double& out )
        {
            const auto value = block.get( std::string( field ) );
            if ( !value.has_value() )
                return false;
            if ( const auto flag = value.value().to_bool(); flag.has_value() )
            {
                out = flag.value() ? 1.0 : 0.0;
                return true;
            }
            float number = 0.0f;
            if ( !ReadNumber( value.value(), number ) )
                return false;
            out = static_cast<double>( number );
            return true;
        }

        // Whether this record's own components make it always-loaded, and why. Author before Component,
        // because the author's marker is the more specific statement.
        [[nodiscard]] inline AlwaysLoadedReason GlobalReasonOf( const Assets::EntityData& record )
        {
            if ( record.Components.get( std::string( kAlwaysLoadedComponent ) ).has_value() )
                return AlwaysLoadedReason::Author;

            for ( const ComponentLoadingRow& row : kComponentLoading )
            {
                if ( row.Loading == ComponentLoading::Spatial )
                    continue;
                const auto payload = record.Components.get( std::string( row.ComponentKey ) );
                if ( !payload.has_value() )
                    continue;
                if ( row.Loading == ComponentLoading::Global )
                    return AlwaysLoadedReason::Component;

                bool global = row.AbsentIsGlobal;
                if ( const auto block = payload.value().to_object(); block.has_value() )
                {
                    double value = 0.0;
                    if ( ReadScalar( block.value(), row.Field, value ) )
                        global = value != row.SpatialWhen;
                }
                if ( global )
                    return AlwaysLoadedReason::Component;
            }
            return AlwaysLoadedReason::None;
        }

        // A component block of the record, if it has one and it is an object.
        [[nodiscard]] inline std::optional<rfl::Generic::Object> BlockOf( const Assets::EntityData& record,
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

        // Appends the XZ of a local-space box's eight corners, through @p world.
        inline void AppendBoxCorners( const glm::mat4& world, const glm::vec3& half, std::vector<glm::vec2>& out )
        {
            for ( int corner = 0; corner < 8; ++corner )
            {
                const glm::vec4 local( ( corner & 1 ) ? half.x : -half.x, ( corner & 2 ) ? half.y : -half.y,
                                       ( corner & 4 ) ? half.z : -half.z, 1.0f );
                const glm::vec4 placed = world * local;
                out.emplace_back( placed.x, placed.z );
            }
        }

        // THE POINTS ONE RECORD CONTRIBUTES TO ITS COMPOSITE'S FOOTPRINT — the extension point named at
        // the top of this file. Returns false when the record contributed its position and nothing more.
        //
        // WP15 adds a mesh asset's stored bounds here, and nowhere else.
        inline bool AppendFootprint( const Assets::EntityData& record, const glm::mat4& world,
                                     std::vector<glm::vec2>& out )
        {
            out.emplace_back( world[3].x, world[3].z );
            const std::size_t before = out.size();

            if ( const auto mesh = BlockOf( record, kPrimitiveComponent ); mesh.has_value() )
            {
                const auto primitive = mesh.value().get( std::string( kPrimitiveField ) );
                if ( primitive.has_value() && primitive.value().to_string().value_or( "" ) == kCubePrimitive )
                    AppendBoxCorners( world, glm::vec3( kCubeHalfEdge ), out );
            }

            if ( const auto terrain = BlockOf( record, kTerrainComponent ); terrain.has_value() )
            {
                // An absent Size is the struct default; an unreadable one is taken as the same.
                double size = 0.0;
                if ( !ReadScalar( terrain.value(), kTerrainSizeField, size ) )
                    size = kTerrainDefaultSize;
                const float half = static_cast<float>( size ) * 0.5f;
                AppendBoxCorners( world, glm::vec3( half, 0.0f, half ), out );
            }

            AppendInstancePoints( record, out );
            return out.size() > before;
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
    //   4. THE LEVEL COUNT, from the furthest footprint of a composite that still has to be placed.
    //
    //   5. PLACEMENT: the lowest level with one cell holding the footprint, else always-loaded (NoFit).
    [[nodiscard]] inline WorldPartitionPlan PlanWorldPartition( std::span<const Assets::EntityData> records,
                                                                const WorldPartitionSerialized&     settings )
    {
        WorldPartitionPlan                            plan;
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

        // ── 2b. Composites, and each one's anchor ──────────────────────────────────────────────────
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
        }

        // ── 3. Footprint and always-loaded, per composite ─────────────────────────────────────────
        std::vector<bool> unplaced( records.size(), false );
        for ( const std::size_t record : plan.UnplacedPrefabInstances )
            unplaced[record] = true;

        std::vector<glm::vec2> points;
        for ( PlannedComposite& group : plan.Composites )
        {
            for ( const std::size_t member : group.Members )
            {
                const AlwaysLoadedReason reason = Detail::GlobalReasonOf( records[member] );
                // Author outranks Component, and the first member in file order names it.
                if ( reason != AlwaysLoadedReason::None &&
                     ( group.Reason == AlwaysLoadedReason::None ||
                       ( reason == AlwaysLoadedReason::Author && group.Reason != AlwaysLoadedReason::Author ) ) )
                {
                    group.Reason  = reason;
                    group.Because = member;
                }

                if ( unplaced[member] )
                    continue;

                points.clear();
                if ( !Detail::AppendFootprint( records[member], world[member], points ) )
                    ++plan.PointOnlyRecords;
                for ( const glm::vec2& point : points )
                {
                    if ( !group.Footprint.has_value() )
                        group.Footprint = CellBounds{ point.x, point.y, point.x, point.y };
                    CellBounds& box = *group.Footprint;
                    box.MinX        = std::min( box.MinX, point.x );
                    box.MinZ        = std::min( box.MinZ, point.y );
                    box.MaxX        = std::max( box.MaxX, point.x );
                    box.MaxZ        = std::max( box.MaxZ, point.y );
                }
            }
        }

        // ── 4. The grid, and how many levels it has for this world ────────────────────────────────
        //
        // NO USABLE GRID IS NOT REPAIRED INTO A PLAUSIBLE ONE. A partitioned world with no `Grids` entry,
        // or with a cell size that is not positive, has nothing to stream by: every composite is
        // always-loaded and says NoGrid, which is what "this world has no grid" looks like from outside.
        plan.UnusedGrids     = settings.Grids.size() > 1 ? settings.Grids.size() - 1 : 0;
        const float cellSize = settings.Grids.empty() ? 0.0f : settings.Grids.front().CellSize;
        if ( !( cellSize > 0.0f ) )
        {
            for ( std::size_t group = 0; group < plan.Composites.size(); ++group )
            {
                if ( plan.Composites[group].Reason == AlwaysLoadedReason::None )
                    plan.Composites[group].Reason = AlwaysLoadedReason::NoGrid;
                plan.AlwaysLoaded.push_back( group );
            }
            return plan;
        }

        // The top level is the first whose cell reaches the furthest footprint edge from the origin, so
        // at the top every footprint that does not straddle an axis is inside one of the four cells
        // around the origin. Only composites still to be placed count: the sun at 10 km does not widen
        // the grid. The cap keeps a corrupt coordinate (1e30) from asking for a level past what an
        // int32 cell index can express at level 0; a footprint beyond it is simply NoFit.
        constexpr int kMaxLevels = 31;
        double        furthest   = 0.0;
        for ( const PlannedComposite& group : plan.Composites )
        {
            if ( group.Reason != AlwaysLoadedReason::None || !group.Footprint.has_value() )
                continue;
            const CellBounds& box = *group.Footprint;
            furthest              = std::max( { furthest, std::fabs( static_cast<double>( box.MinX ) ),
                                                std::fabs( static_cast<double>( box.MaxX ) ),
                                                std::fabs( static_cast<double>( box.MinZ ) ),
                                                std::fabs( static_cast<double>( box.MaxZ ) ) } );
        }
        plan.LevelCount = 1;
        while ( plan.LevelCount < kMaxLevels && LevelCellSize( cellSize, plan.LevelCount - 1 ) < furthest )
            ++plan.LevelCount;

        // ── 5. Placement: the lowest level with ONE cell holding the whole footprint ──────────────
        for ( std::size_t group = 0; group < plan.Composites.size(); ++group )
        {
            PlannedComposite& held = plan.Composites[group];
            if ( held.Reason != AlwaysLoadedReason::None )
            {
                plan.AlwaysLoaded.push_back( group );
                continue;
            }

            // Unplaced prefab instances alone: the origin's level-0 cell, and they are listed.
            const CellBounds footprint = held.Footprint.value_or( CellBounds{} );

            bool placed = false;
            for ( int level = 0; level < plan.LevelCount; ++level )
            {
                const auto cell = SingleCellHolding( footprint, LevelCellSize( cellSize, level ) );
                if ( cell.has_value() )
                {
                    held.Level = level;
                    held.Cell  = *cell;
                    placed     = true;
                    break;
                }
            }
            if ( !placed )
            {
                held.Reason = AlwaysLoadedReason::NoFit;
                plan.AlwaysLoaded.push_back( group );
                continue;
            }

            if ( plan.MaxLevelComposite == kNoRecord || held.Level > plan.MaxLevel )
            {
                plan.MaxLevel          = held.Level;
                plan.MaxLevelComposite = group;
            }
        }

        // ── 6. The cells, ordered by level, X, Z ──────────────────────────────────────────────────
        std::map<std::tuple<int, std::int32_t, std::int32_t>, std::vector<std::size_t>> cells;
        for ( std::size_t group = 0; group < plan.Composites.size(); ++group )
        {
            const PlannedComposite& held = plan.Composites[group];
            if ( held.Reason == AlwaysLoadedReason::None )
                cells[std::make_tuple( held.Level, held.Cell.X, held.Cell.Z )].push_back( group );
        }
        for ( auto& [key, composites] : cells )
        {
            PlannedCell cell;
            cell.Level      = std::get<0>( key );
            cell.Cell.X     = std::get<1>( key );
            cell.Cell.Z     = std::get<2>( key );
            cell.Square     = GridSquareOf( cell.Cell, LevelCellSize( cellSize, cell.Level ) );
            cell.Composites = std::move( composites );
            plan.Cells.push_back( std::move( cell ) );
        }

        return plan;
    }

    // How many cells each level holds, index = level. Sized LevelCount.
    [[nodiscard]] inline std::vector<std::size_t> CellsPerLevel( const WorldPartitionPlan& plan )
    {
        std::vector<std::size_t> perLevel( static_cast<std::size_t>( plan.LevelCount ), 0 );
        for ( const PlannedCell& cell : plan.Cells )
            ++perLevel[static_cast<std::size_t>( cell.Level )];
        return perLevel;
    }

    // How many always-loaded composites each reason accounts for, indexed by AlwaysLoadedReason.
    [[nodiscard]] inline std::vector<std::size_t> AlwaysLoadedByReason( const WorldPartitionPlan& plan )
    {
        std::vector<std::size_t> byReason( static_cast<std::size_t>( AlwaysLoadedReason::NoGrid ) + 1, 0 );
        for ( const std::size_t group : plan.AlwaysLoaded )
            ++byReason[static_cast<std::size_t>( plan.Composites[group].Reason )];
        return byReason;
    }

    // THE PLAN IN ONE LINE — one wording, used by the loader's log and by `WorldGen --partition`, so the
    // two never describe the same world differently. Lengths are printed as whole world units.
    [[nodiscard]] inline std::string SummarisePartition( const WorldPartitionPlan&       plan,
                                                         const WorldPartitionSerialized& settings )
    {
        const auto units = []( float value ) { return std::to_string( std::llround( value ) ); };

        std::string line = std::to_string( plan.Composites.size() ) +
                           " composite(s): " + std::to_string( plan.Cells.size() ) + " cell(s) over " +
                           std::to_string( plan.LevelCount ) + " level(s) [";
        const auto perLevel = CellsPerLevel( plan );
        for ( std::size_t level = 0; level < perLevel.size(); ++level )
            line +=
                 ( level == 0 ? "L" : ", L" ) + std::to_string( level ) + ": " + std::to_string( perLevel[level] );

        const auto byReason = AlwaysLoadedByReason( plan );
        line += "], " + std::to_string( plan.AlwaysLoaded.size() ) + " always-loaded (author " +
                std::to_string( byReason[static_cast<std::size_t>( AlwaysLoadedReason::Author )] ) +
                ", component " +
                std::to_string( byReason[static_cast<std::size_t>( AlwaysLoadedReason::Component )] ) +
                ", no fit " + std::to_string( byReason[static_cast<std::size_t>( AlwaysLoadedReason::NoFit )] ) +
                ", no grid " + std::to_string( byReason[static_cast<std::size_t>( AlwaysLoadedReason::NoGrid )] ) +
                "); highest promotion: level " + std::to_string( plan.MaxLevel ) + "; " +
                std::to_string( plan.PointOnlyRecords ) + " record(s) placed by position alone";
        if ( !settings.Grids.empty() )
            line += "; grid 0: cell " + units( settings.Grids.front().CellSize ) + ", loading range " +
                    units( settings.Grids.front().LoadingRange ) + " world units";
        return line;
    }

} // namespace Desert::Core::Rules
