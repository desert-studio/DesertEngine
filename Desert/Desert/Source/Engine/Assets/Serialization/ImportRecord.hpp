#pragma once
// The body of an import record, `<name>.<ext>.deimport` (FIX8; UE: a .uasset's persistent GUID and its
// UAssetImportData). Path rules and the reason the record exists: Common/Content/ImportRecord.hpp.
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>
#include <Engine/Assets/ThumbnailInfo.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/ImportRecord.hpp>
#include <Common/Core/Math/AABB.hpp>
#include <Common/Core/ResultStr.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Assets::Serialization
{
    /**
     * @brief The FILE layout's generation, stated under `DIMP`.
     *
     *   1 - the text asset header (Kind "StaticMesh" - or, since THM1l, what the source imports as: see
     *       IsImportRecordKind - the GUID that IS the imported mesh's identity and, through
     *       HandleForGuid, its handle) and `Source`, the file name of the source the record belongs to (FIX8).
     *       No import settings yet: the importer has none that a user sets per source.
     *   2 - `Bounds`, the imported mesh's box in centimetres (Min/Max), written by every import and re-import
     *       (DIMP2). The registry reads it without loading anything - UE's asset registry serves a package's
     *       bounds tag the same way - so a cold DDC still knows the box. Required of a record that imports a mesh
     *       (StaticMesh, SkinnedMesh); a skeleton or clips-only record has no mesh and states none. A version-1
     *       record is refused by its path and re-imported.
     *
     * An unknown value is refused in both directions.
     */
    inline constexpr int32_t kImportRecordVersion = static_cast<int32_t>( Assets::kImportRecordSchemaVersion );

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> ImportRecordTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kImportRecordSchemaTag,
                                                static_cast<uint32_t>( kImportRecordVersion ) } };
        return versions;
    }

    /// Assets::SourceImportSettings as text states it: the enums by NAME (MeshSourceUpAxisName /
    /// MeshLodPolicyName, the names IMPT uses), so reordering an enum never changes what a record says. The
    /// record's `Settings` and the editor's remembered Import Options are this shape.
    struct SourceImportSettingsText
    {
        bool        CombineMeshes = false;
        float       UniformScale  = 1.0f;
        std::string UpAxis;
        std::string LodPolicy;
        /// SourceImportSettings::Skeleton as its GUID's text; absent = none chosen.
        std::optional<std::string> Skeleton;
        /// SourceImportSettings::SpecularMap by FbxSpecularMapName; absent = `FromImage` (the map's texels
        /// decide). A record written before FromImage existed stated nothing for the old default `Specular`, under
        /// which no surface template read the map at all; it now reads as FromImage, so its next import brings the
        /// map in. A record that chose `Specular` explicitly states it.
        std::optional<std::string> SpecularMap;
        /// SourceImportSettings::FileUnit by MeshFileUnitName; absent = `FromFile` (the file or its format
        /// states the unit), so a record that never chose one states nothing new.
        std::optional<std::string> FileUnit;
        /// MeshImportSettings::Displacement by MeshDisplacementModeName; absent = `None`.
        std::optional<std::string> Displacement;
        /// MeshImportSettings::DisplacementBake.EdgeLengthCm; absent = the default (1 cm).
        std::optional<float> DisplacementEdgeLengthCm;
        /// MeshImportSettings::VirtualGeometry; absent = off.
        std::optional<bool> VirtualGeometry;
        /// MeshImportSettings::Canopy.LeafSlots, the material slot names whose sections are the crown's leaf
        /// cards; absent = no canopy.
        std::optional<std::vector<std::string>> CanopyLeafSlots;
        /// MeshImportSettings::LodChain.TriangleRatios, each generated level's share of LOD 0's triangles (LOD 1
        /// first); absent = the default chain's (Geometry::LODChainSettings{}).
        std::optional<std::vector<float>> LodTriangleRatios;
        /// MeshImportSettings::LodChain.MinTriangles; absent = the default chain's floor.
        std::optional<uint32_t> LodMinTriangles;
        /// MeshImportSettings::Canopy.ShellCm / ClearTexels; absent = the defaults (40 cm, 2 texels). Written only
        /// with leaf slots and only when they differ from the default.
        std::optional<float> CanopyShellCm;
        std::optional<float> CanopyClearTexels;
    };
    [[nodiscard]] SourceImportSettingsText ImportSettingsToText( const Assets::SourceImportSettings& settings );
    /// Refused, by name, for an unknown up axis or LOD policy or a scale that is not finite and > 0.
    [[nodiscard]] Common::ResultStr<Assets::SourceImportSettings>
    ImportSettingsFromText( const SourceImportSettingsText& text );

    /// One node a split import wrote (ImportRecordData::Nodes).
    struct ImportRecordNode
    {
        std::string          Name;
        std::array<float, 3> Placement{};

        friend bool operator==( const ImportRecordNode&, const ImportRecordNode& ) = default;
    };

    /// One light source the file states (ImportRecordData::Lights; UE FbxSceneImport / Interchange FbxLight: a
    /// point, spot or directional light becomes a light actor at its node). Everything is in the ENGINE's space
    /// (the import options applied), as a node's Placement is: where the placed light stands under the placed
    /// source's root and where it shines. What each member becomes on the placed light: NodeActors.hpp
    /// PlaceSourceLights.
    struct ImportRecordLight
    {
        std::string Name; // the light's node
        std::string Type; // "Point", "Spot" or "Directional" (ImportRecordLightTypes)
        /// The light's origin, centimetres from the source's origin. A directional light states one too (its
        /// node's), which placing does not use: such a light has a direction and no position.
        std::array<float, 3> Position{};
        /// The unit direction the light TRAVELS (a spot's axis out of its apex, a directional light's sun ->
        /// scene); a point light states its node's, which placing does not use.
        std::array<float, 3> Direction{};
        /// Colour and Intensity are the file's colour times its intensity over 100 (FBX's and assimp's "100 =
        /// full"), split so the colour's brightest channel is 1 and Intensity is the engine's linear multiplier
        /// (the Intensity of the light components); their product is the file's, unchanged.
        std::array<float, 3> Color{};
        float                Intensity = 1.0f;
        /// Where a point or spot light's reach ends, in centimetres (UE AttenuationRadius from FBX DecayStart).
        /// Absent = the file states no decay (FBX DecayType None), which UE imports as an unbounded radius.
        std::optional<float> Radius;
        /// A spot light's cone half-angles in degrees (UE InnerConeAngle / OuterConeAngle, copied from FBX
        /// InnerAngle / OuterAngle as UE copies them); absent on every other type.
        std::optional<float> InnerConeAngle;
        std::optional<float> OuterConeAngle;

        friend bool operator==( const ImportRecordLight&, const ImportRecordLight& ) = default;
    };
    /// The names ImportRecordLight::Type takes.
    inline constexpr std::array<std::string_view, 3> ImportRecordLightTypes = { "Point", "Spot", "Directional" };

    struct ImportRecordData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        /// The source's file name ("base.fbx"): a record copied beside another source is refused by name.
        std::string Source;
        /// The imported mesh's box (version 2): the same `Bounds` member a prefab states.
        struct Box
        {
            std::array<float, 3> Min{};
            std::array<float, 3> Max{};
        };
        std::optional<Box> Bounds;

        // THE SOURCE'S IMPORT OPTIONS (THM1l; UE UFbxImportUI): Combine Meshes, Uniform Scale, Up Axis, LOD
        // policy - the one home of what the Import Options window and the Details' Import Settings edit. Absent
        // means UE's defaults (SourceImportSettings{}), and the defaults are written as no key (one spelling).
        // Written by every import with the options it ran with.
        std::optional<SourceImportSettingsText> Settings;

        // THE NODE MESHES THE LAST SPLIT IMPORT WROTE (THM1j), by node name: <stem>_<node>.stmesh beside the
        // source (NodeMeshSplit). Present only when the source was split; then there is NO combined mesh, and
        // the import's freshness is these files' (each states the source's hash), not a combined envelope's.
        // Each entry also states where its node stands (IMP-NODES; UE: FbxSceneImport places one
        // StaticMeshActor per node at the node's transform): `Placement`, in centimetres in the ENGINE's space
        // (the options applied), is the offset of the node mesh's pivot from the source's origin - the
        // translation a placed node takes under the placed source's root, so the nodes stand as the file
        // arranged them. The node's rotation and scale are in its mesh (the importer bakes the node's world
        // transform into the vertices; NodeMeshSplit rebases them onto the pivot).
        std::optional<std::vector<ImportRecordNode>> Nodes;

        // THE LIGHT SOURCES THE FILE STATES (BISTRO-LIGHTS; UE FbxSceneImport and Interchange's FbxLight turn each
        // FBX point, spot and directional light into a light actor at its node's transform), in the ENGINE's space
        // like `Nodes`. Placing the source places one light entity per entry under its root (NodeActors.hpp
        // PlaceSourceLights), combined or split. Written by every mesh import; a file with no light writes no key
        // (one spelling), so an empty list is refused.
        std::optional<std::vector<ImportRecordLight>> Lights;

        // THE SOURCE'S BYTES THE LAST SKINNED, SKELETON OR CLIP IMPORT READ (UE UAssetImportData's source file
        // hash, HashMeshSourceFile): such an import is current when this states the source's current hash. It
        // lives in the record, not in a written asset, because the rig the import names may be an existing
        // skeleton another source wrote (SkeletonReference.hpp). Absent = no complete import yet. A static
        // import's hash is in each mesh it writes (MeshImportInfo::SourceHash).
        std::optional<uint64_t> SourceHash;

        // HOW EACH MESH THIS IMPORT WRITES IS PHOTOGRAPHED (UE: UStaticMesh::ThumbnailInfo, a USceneThumbnailInfo
        // saved in the mesh's package). The record IS the imported mesh's package: the combined mesh lives in the
        // DDC and every node `.stmesh` is rewritten by each re-import, so an orbit stored in either would be lost;
        // the record is kept by every re-import (EnsureImportRecord rewrites the parsed record). Keyed by the
        // mesh asset's file name beside the source: the source's own name ("base.fbx") for the combined mesh,
        // `<stem>_<node>.stmesh` for a node mesh (NodeMeshAssetPath). A mesh without an entry has the default
        // orbit (ThumbnailInfo.hpp); a stated default is refused, so one picture has one spelling. An entry is a
        // ThumbnailOrbitRecord: a member it leaves out is that member's default (Resolve).
        std::optional<std::map<std::string, ThumbnailOrbitRecord>> Thumbnail;

        // THE SCREEN SIZE AT WHICH EACH LEVEL OF DETAIL BEGINS, per mesh this import writes (UE:
        // FStaticMeshSourceModel::ScreenSize, one per LOD, saved in the mesh's package - here the record, for the
        // reason Thumbnail gives). Keyed like Thumbnail; the list is LOD 0 first, each entry the bounding sphere's
        // diameter as a fraction of the screen (Geometry::LODScreenSizesDefect states what a list must be). A
        // mesh without an entry has its sizes computed (UE bAutoComputeLODScreenSize, the default every import
        // starts from: Geometry::ComputeLODScreenSize); levels past the end of a list are computed the same way.
        // Not a build input: the render data is resolved against it after the cache, as UE's ResolveSectionInfo
        // does, so an edit re-derives nothing.
        std::optional<std::map<std::string, std::vector<float>>> LodScreenSize;
    };

    // THE RECORD STATES WHAT THE SOURCE IMPORTS AS (UE: an imported asset's class - a UStaticMesh, a
    // USkeletalMesh, a USkeleton or a UAnimSequence - is part of its identity): the header's Kind is StaticMesh
    // for a static file, SkinnedMesh for a skinned one, Skeleton for a skeleton with clips and no mesh, Animation
    // for clips only. Only a StaticMesh record stands for a mesh asset with no file of its own (ContentScan).
    [[nodiscard]] bool IsImportRecordKind( Common::Content::ContentKind kind );

    Common::ResultStr<ImportRecordData> ParseImportRecord( const std::string& text );
    /// Refused for a @p kind that is not IsImportRecordKind.
    Common::ResultStr<std::string> WriteImportRecord( const ImportRecordData&      data,
                                                      Common::Content::ContentKind kind );

    /// How many times this session parsed a record file. Every reader below asks the session's held parse of the
    /// record (UE: the asset registry serves an asset's import data from memory): a record is parsed once and
    /// again only when its file changes (write time or size) or is rewritten by a setter here.
    [[nodiscard]] uint64_t ImportRecordParseCount();

    /// The GUID @p source's record states. An error naming the record's path when it is missing, unreadable,
    /// of another generation or written for another source - never a GUID made up from the path.
    Common::ResultStr<Common::Content::AssetGuid> ReadImportRecordGuid( const std::filesystem::path& source );

    /// The import's side: the record's GUID, the record written first (with a new GUID) when @p source has
    /// none. An existing record keeps its GUID, so a re-import keeps the identity; its `Bounds` are rewritten
    /// when the import's box differs from the one it states.
    /// @p source's import options: the record's, UE's defaults when the record states none or when the source has
    /// no record yet (its first import). An error naming the record when it is unreadable.
    Common::ResultStr<Assets::SourceImportSettings>
    ReadImportRecordSettings( const std::filesystem::path& source );

    /// What @p source's record says it imports as (the header's Kind, one IsImportRecordKind names) - the fields
    /// its Details' Import Settings show. An error naming the record when it is missing, unreadable or states a
    /// kind no import writes.
    Common::ResultStr<Common::Content::ContentKind> ReadImportRecordKind( const std::filesystem::path& source );

    /// @p source's whole record; nullopt when the source has no record yet. An error naming the record when it is
    /// unreadable.
    Common::ResultStr<std::optional<ImportRecordData>> ReadImportRecord( const std::filesystem::path& source );

    /// Rewrites the record's `Nodes` (THM1j): the nodes a split import wrote (name and placement), nullopt for a
    /// combined import. The record must exist (EnsureImportRecord runs first); written only when the list changes.
    Common::BoolResultStr SetImportRecordNodes( const std::filesystem::path&                        source,
                                                const std::optional<std::vector<ImportRecordNode>>& nodes );

    /// Rewrites the record's `Lights`: the light sources the last import of @p source read, in the engine's space;
    /// an empty list removes the key. The record must exist (EnsureImportRecord runs first); written only when the
    /// list changes. An error for a light ParseImportRecord would refuse.
    Common::BoolResultStr SetImportRecordLights( const std::filesystem::path&          source,
                                                 const std::vector<ImportRecordLight>& lights );

    /// Rewrites the record's `SourceHash`: the import of @p source that read bytes of @p hash completed. The
    /// record must exist (EnsureImportRecord runs first); written only when the hash changes.
    Common::BoolResultStr SetImportRecordSourceHash( const std::filesystem::path& source, uint64_t hash );

    /// Rewrites the skeleton the record's `Settings` choose (SourceImportSettings::Skeleton; UE: the Skeleton of
    /// the source's AssetImportData, which Reimport repeats): the artist's assignment of another skeleton to the
    /// imported mesh, so a re-import keeps it instead of reverting to the rig the file matched. The record must
    /// exist; its other settings are kept (UE's defaults when it states none); written only when the choice
    /// changes. An error for a null @p skeleton.
    Common::BoolResultStr SetImportRecordSkeleton( const std::filesystem::path& source,
                                                   Common::Content::AssetGuid   skeleton );

    /// The orbit @p source's record states for the mesh asset named @p meshFile (ImportRecordData::Thumbnail);
    /// the default orbit when it states none for it. An error naming the record when it is missing or unreadable.
    Common::ResultStr<ThumbnailOrbit> ReadImportRecordThumbnail( const std::filesystem::path& source,
                                                                 const std::string&           meshFile );

    /// Rewrites the orbit @p source's record states for the mesh asset named @p meshFile (UE: Edit Thumbnail
    /// writes the asset's ThumbnailInfo). The default orbit removes the entry (the default is written as no key),
    /// the last removal the whole `Thumbnail` key. The record must exist; written only when the orbit changes. An
    /// error for an orbit IsValidThumbnailOrbit refuses.
    Common::BoolResultStr SetImportRecordThumbnail( const std::filesystem::path& source,
                                                    const std::string& meshFile, const ThumbnailOrbit& orbit );

    /// The per-LOD screen sizes @p source's record states for the mesh asset named @p meshFile
    /// (ImportRecordData::LodScreenSize); nullopt when it states none (computed). An error naming the record when
    /// it is missing or unreadable.
    Common::ResultStr<std::optional<std::vector<float>>>
    ReadImportRecordLodScreenSizes( const std::filesystem::path& source, const std::string& meshFile );

    /// Rewrites them (UE: editing a source model's ScreenSize in the Static Mesh editor). nullopt removes the
    /// entry (back to computed), the last removal the whole key. The record must exist; written only on a change.
    /// An error for a list Geometry::LODScreenSizesDefect refuses.
    Common::BoolResultStr SetImportRecordLodScreenSizes( const std::filesystem::path&             source,
                                                         const std::string&                       meshFile,
                                                         const std::optional<std::vector<float>>& sizes );

    /// THE AUTHORED SCREEN SIZES OF THE MESH ASSET AT @p meshAsset, read where they live: the record of the source
    /// the asset was imported from (the combined mesh of `<stem>.<raw ext>` with a record beside it, keyed by
    /// the source's file name). nullopt for a mesh no import record speaks for (hand-authored, a node mesh,
    /// a primitive) and for one whose record states none - both computed. Read at every mesh build (MeshService).
    struct MeshLodScreenSizesHome
    {
        std::filesystem::path Source;
        std::string           MeshFile;
    };
    [[nodiscard]] std::optional<MeshLodScreenSizesHome>
    LodScreenSizesHomeOf( const std::filesystem::path& meshAsset );
    Common::ResultStr<std::optional<std::vector<float>>>
    ReadMeshLodScreenSizes( const std::filesystem::path& meshAsset );

    /// ... and its `Settings` rewritten to @p settings, the options this import ran with, and its header's Kind to
    /// @p kind, what the source imports as (IsImportRecordKind). Written by EVERY import, static or skinned (UE:
    /// AssetImportData on every imported asset): a source with a record is not new. @p bounds is the imported
    /// mesh's box in the ENGINE's space, the options applied (SourceToEngineBounds): the box the placed mesh has.
    /// A file with no mesh (skeleton and clips only) passes no @p bounds, and the record keeps the one it states.
    Common::ResultStr<Common::Content::AssetGuid>
    EnsureImportRecord( const std::filesystem::path& source, Common::Content::ContentKind kind,
                        const std::optional<Common::Math::AABB>& bounds,
                        const Assets::SourceImportSettings&      settings );

    /// The GUID of @p source's record when it already states this import - @p kind, @p bounds (as
    /// EnsureImportRecord takes them) and @p settings - else an error naming the record and what it states
    /// otherwise (or that it is missing). Reads only: the cook, which writes nothing into Content, asks this where
    /// an import calls EnsureImportRecord.
    Common::ResultStr<Common::Content::AssetGuid>
    ReadCurrentImportRecord( const std::filesystem::path& source, Common::Content::ContentKind kind,
                             const std::optional<Common::Math::AABB>& bounds,
                             const Assets::SourceImportSettings&      settings );
} // namespace Desert::Assets::Serialization
