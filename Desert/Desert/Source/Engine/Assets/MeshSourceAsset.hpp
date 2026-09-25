#pragma once
// THE MESH ASSET (AF4b) — a `.stmesh` / `.skmesh` that is an AF1 envelope CARRYING its editable source.
//
// UE keeps a static mesh's editable source inside the .uasset: `SourceModels` (FStaticMeshSourceModel, one per
// LOD, each holding an FMeshDescription — Engine/Classes/Engine/StaticMesh.h:657, GetMeshDescription :1627),
// the material slots (`StaticMaterials`, :1096) and `AssetImportData` (:1439): the file it came from, that
// file's hash and the import options. Render data (LODs, tangents, vertex buffers) is DERIVED from that source
// and cached in the DDC (FStaticMeshRenderData::Cache). The same split here, on the envelope:
//
//   header   Kind = StaticMesh or SkinnedMesh; Guid = the mesh's identity, minted once at import and only
//            copied; Dependencies = the material slots' GUIDs (derived from SRCE, never stated separately)
//   Meta     display name + bounds of the source positions (cm) — what the registry and a thumbnail read
//            without the body; it replaces the Reserved/flag bounds of the DESTMESH render header
//   IMPT     MeshImportInfo: provenance (stable key and content hash of the imported file) + import settings
//   SRCE     MeshSourceData: the source models, one EditMeshSer (our FMeshDescription) per authored LOD, the
//            material slots they share, and for a skinned mesh its skin (bone names + per-vertex influences)
//
// Sections are written and read in exactly that order (META, IMPT, SRCE), nothing else: the order is part
// of the format so a read-then-write reproduces the file byte for byte, and a file assembled any other way
// is refused by name rather than half-understood.
//
// What is NOT here: LOD triangle sets, tangents, render vertices. They are the platform data of AF4c, built
// from SRCE + settings under a DDC key, exactly as a texture's mips are built from its SRCE.
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Geometry/SavedMeshForm.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Assets
{
    // The envelope's subsystem stamp for the IMPT and SRCE layouts below (UE custom version).
    inline constexpr uint32_t kMeshAssetSubsystemTag     = Common::Content::FourCC( "MSAS" );
    inline constexpr uint32_t kMeshAssetSubsystemVersion = 1;

    // The header read context a mesh asset is judged with (its one known subsystem).
    Common::Content::AssetHeaderReadContext MeshAssetHeaderReadContext();

    // Which way is up in the imported file. `FromFile` = the file's own node hierarchy decides (the exporter's
    // axis conversion, today's importer behaviour); the others force a conversion to our Y-up.
    enum class MeshSourceUpAxis : uint8_t
    {
        FromFile,
        Y,
        Z,
    };
    // How render LODs are made from the source (UE FStaticMeshSourceModel::ReductionSettings, reduced to the
    // two behaviours the importer has): `Generate` = authored LODs when the file has them, simplified ones
    // otherwise; `None` = LOD0 only.
    enum class MeshLodPolicy : uint8_t
    {
        Generate,
        None,
    };
    // Where SRCE came from. `Imported` names a file (key + hash); `Recovered` is a source rebuilt from an older
    // render-form asset whose original file is not known — an explicit state, never an empty string.
    enum class MeshSourceProvenance : uint8_t
    {
        Imported,
        Recovered,
    };

    struct MeshImportSettings
    {
        float            UniformScale = 1.0f; // applied on import, source units -> cm; finite and > 0
        MeshSourceUpAxis UpAxis       = MeshSourceUpAxis::FromFile;
        MeshLodPolicy    LodPolicy    = MeshLodPolicy::Generate;
        bool             operator==( const MeshImportSettings& ) const = default;
    };

    struct MeshImportInfo
    {
        MeshSourceProvenance Provenance = MeshSourceProvenance::Imported;
        // `AssetHandle::StableKeyForPath` of the imported file (`assets:Meshes/Crate.fbx`) and its
        // Utils::PakContentHash at import. Imported: both set. Recovered: both empty/zero.
        std::string        SourceFile;
        uint64_t           SourceHash = 0;
        MeshImportSettings Settings;
        bool               operator==( const MeshImportInfo& ) const = default;
    };

    // One entry of the slot table EditMeshSer::MaterialIds index into (UE FStaticMaterial). A null GUID is an
    // unassigned slot.
    struct MeshMaterialSlot
    {
        std::string                Name;
        Common::Content::AssetGuid Material;
        bool                       operator==( const MeshMaterialSlot& ) const = default;
    };

    struct MeshSkinInfluence
    {
        uint32_t Vertex                                       = 0; // EditMeshSer vertex index
        uint32_t Bone                                         = 0; // index into MeshSkin::BoneNames
        float    Weight                                       = 0.0f;
        bool     operator==( const MeshSkinInfluence& ) const = default;
    };

    // The skin of a skinned source (UE FSkeletalMeshImportData: RefBonesBinary names + Influences). Bones are
    // named, not numbered against a skeleton asset: binding to a `.skeleton` is the deriver's job (AF4f).
    struct MeshSkin
    {
        uint64_t                       SkeletonSignature = 0; // the rig the file was skinned to; 0 = none claimed
        std::vector<std::string>       BoneNames;
        std::vector<MeshSkinInfluence> Influences;
        bool                           operator==( const MeshSkin& ) const = default;
    };

    // One LOD's editable source (UE FStaticMeshSourceModel, StaticMesh.h:657 - one per LOD, each owning its own
    // FMeshDescription). Model 0 is LOD0; a model k >= 1 is an AUTHORED LOD (the file's "<name>_LOD<k>"
    // sibling), which the builder folds into the render LOD chain instead of simplifying LOD0.
    struct MeshSourceModel
    {
        Geometry::EditMeshSer Mesh;
        bool                  operator==( const MeshSourceModel& ) const = default;
    };

    struct MeshSourceData
    {
        // LOD0 first, never empty. Every model's MaterialIds index the ONE slot table below (UE StaticMaterials
        // are shared by all LODs).
        std::vector<MeshSourceModel>  Models;
        std::vector<MeshMaterialSlot> MaterialSlots;
        // Present exactly when the asset is a SkinnedMesh; its influences name LOD0's vertices, and a skinned
        // source has exactly one model (per-LOD skins arrive with the skinned builder, AF4f).
        std::optional<MeshSkin>       Skin;
        bool                          operator==( const MeshSourceData& ) const = default;
    };

    struct MeshSourceAsset
    {
        Common::Content::ContentKind Kind = Common::Content::ContentKind::StaticMesh; // StaticMesh or SkinnedMesh
        Common::Content::AssetGuid   Guid;
        std::string                  Name;
        MeshImportInfo               Import;
        MeshSourceData               Source;
        bool                         operator==( const MeshSourceAsset& ) const = default;
    };

    // The header's dependency list: every non-null slot material, first occurrence order, no repeats.
    std::vector<Common::Content::AssetGuid> MeshSourceDependencies( const MeshSourceData& source );
    // Bounds of the source positions, cm; nullopt for a mesh with no vertices. The asset's Meta bounds are LOD0's.
    std::optional<Common::Content::EnvelopeBounds> MeshSourceBounds( const Geometry::EditMeshSer& mesh );

    // Refuse anything that is not a well-formed asset: a kind other than the two mesh kinds, a skin that
    // disagrees with the kind, out-of-range indices, a torn overlay, a bad setting, inconsistent provenance.
    // The identity of the editable source (UE: the MeshDescription's bulk-data id that
    // BuildStaticMeshDerivedDataKey hashes): PakContentHash of the SRCE section image, so it changes exactly
    // when the bytes the file stores for the source change - never with the name, the path or the GUID.
    uint64_t MeshSourceHash( const MeshSourceData& source );

    Common::ResultStr<std::vector<std::byte>> EncodeMeshSourceAsset( const MeshSourceAsset& asset );
    Common::ResultStr<MeshSourceAsset>        DecodeMeshSourceAsset( std::span<const std::byte> file );
    Common::ResultStr<MeshSourceAsset>        ReadMeshSourceAssetFile( const std::filesystem::path& file );
    Common::BoolResultStr                     WriteMeshSourceAssetFile( const std::filesystem::path& file,
                                                                        const MeshSourceAsset&       asset );

    std::string_view                    MeshSourceUpAxisName( MeshSourceUpAxis axis );
    std::optional<MeshSourceUpAxis>     MeshSourceUpAxisFromName( std::string_view name );
    std::string_view                    MeshLodPolicyName( MeshLodPolicy policy );
    std::optional<MeshLodPolicy>        MeshLodPolicyFromName( std::string_view name );
    std::string_view                    MeshSourceProvenanceName( MeshSourceProvenance provenance );
    std::optional<MeshSourceProvenance> MeshSourceProvenanceFromName( std::string_view name );
} // namespace Desert::Assets
