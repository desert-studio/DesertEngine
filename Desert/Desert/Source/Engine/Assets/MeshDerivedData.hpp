#pragma once

// THE MESH'S RENDER DATA IS DERIVED (UE FStaticMeshRenderData::Cache, StaticMesh.cpp:4172). A mesh asset
// (MeshSourceAsset.hpp) stores the editable source and the import settings; the render form the GPU draws -
// LOD chain, tangent frames, the MeshBinary container - is built from them and kept in the DDC under a key of
// (the source's hash, the build settings, the builder's version). The asset's name, path and GUID are NOT
// inputs: renaming or moving a mesh hits the same entry, and two assets with the same source share it.
//
// WHERE THE BUILDER LIVES. Only the editor builds (Editor/Import/MeshDeriver.hpp, registered with
// SetMeshPlatformDataBuilder); a packaged game registers none, so a miss there is an error naming the asset
// and the key, because the package was supposed to carry the entry - the texture rule
// (SetTexturePlatformDataBuilder), for the same reason.

#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Assets
{
    // UE: STATICMESH_DERIVEDDATA_VER (StaticMesh.cpp:3678). The bucket's own version: change it when the DDC
    // entry's container changes shape for the same inputs.
    inline constexpr Common::DDC::Deriver kMeshDeriver{
         "StaticMesh", ".stmesh", { 0x3C6B1E0F2A5D4C71ULL, 0x8E94D2B7F06A1C35ULL } };

    // The builder's own version - an input of every mesh key, so it lives beside the deriver and not in the
    // editor that runs the builder: the runtime computes the same key to find the entry. Bump it whenever the
    // build steps (tangents, transform, LOD simplification, section names) produce different bytes for the same
    // source.
    inline constexpr uint32_t kMeshBuilderVersion =
         14; // 14: canopy leaf cards (MeshImportSettings::Canopy: crown normal + depth in COLOR_0.a);
             // 13: MeshBinary v7 container (cluster DAG sections; a VirtualGeometry mesh's LODs cut from its DAG);
             // 12: MeshBinary v6 container (displacement streams); 11: M23 LOD reduction and MikkTSpace
             // tangent frames together; 10: UV weighs 0.5 in the reduction
             // (chart borders priced, not crossed for free); 7-9 were probes; 6: LODs reduced by meshoptimizer 1.3
             // Permissive + Prune (seams crossed natively, small parts pruned); 5: LODs reduced over
             // position-welded topology; 4: LODs reduced to the ratio with normal/UV attributes, no error cap /
             // MikkTSpace tangent frames (import and DynamicMesh derive); 3: vertex colour and UV1 streams; 2:
             // sections named by slot

    // The settings image the key hashes (UE SerializeForKey): fixed order, little-endian, one u32 per field.
    struct MeshBuildSettings
    {
        MeshImportSettings Import;
        uint32_t           BuilderVersion = kMeshBuilderVersion;
    };
    std::vector<std::byte> SerializeMeshSettingsForKey( const MeshBuildSettings& settings );

    // Source hash (MeshSourceHash) + settings + deriver -> DDC key.
    uint64_t MeshDerivedDataKey( uint64_t sourceHash, const MeshBuildSettings& settings,
                                 const Common::DDC::Deriver& deriver = kMeshDeriver );

    // The key of an asset as it stands: its SRCE hash and its IMPT settings with the current builder version.
    uint64_t MeshAssetDerivedDataKey( const MeshSourceAsset& asset );

    // THE SKINNED MESH'S RENDER-DATA KEY (DISPLACE-5): a .skmesh is its own render data (no source envelope, no
    // deriver), so its key is the hash of its MeshBinary bytes with the header's displacement-mode byte zeroed - the
    // mode does not change the geometry that the displacement topology (keyed from this) is built from.
    [[nodiscard]] uint64_t SkinnedMeshRenderDataKey( std::string_view meshBinaryBytes );

    // The MeshBinary container bytes of the asset's render data (EncodeMeshBinary's output).
    using MeshPlatformDataBuilder = std::function<Common::ResultStr<std::string>( const MeshSourceAsset& asset )>;
    void SetMeshPlatformDataBuilder( MeshPlatformDataBuilder builder );

    // DDC hit, or - where a builder is registered - build and Put. A failed Put is an error carrying the file
    // system's reason: a cache that silently stays cold rebuilds on every load and nobody learns why.
    Common::ResultStr<std::string> LoadMeshPlatformData( const std::filesystem::path& asset );

    // The same, with the key the bytes were found under - what the mesh's distance field (MeshDistanceField.hpp)
    // derives its own key from, so the field follows exactly the render data it was built from.
    struct MeshPlatformDataBytes
    {
        std::string Bytes;
        uint64_t    Key = 0;
        // The asset's import settings (IMPT): what the loader reads beside the render data - the displacement
        // mode is not in the render-data key, so it comes from the asset itself.
        MeshImportSettings Settings;
    };
    Common::ResultStr<MeshPlatformDataBytes> LoadMeshPlatformDataKeyed( const std::filesystem::path& asset );

    // AF4h: THE IMPORTED SOURCE ENVELOPE IS ALSO DERIVED. Everything an import writes into a static mesh's
    // MeshSourceAsset - the source's mesh models, its material slot table - is a pure function of the
    // raw file's own bytes (AF4g already made unit/axis resolution pure too, reading them out of the file
    // instead of asking the user). It belongs in the DDC beside the render-data bucket above, keyed by the
    // SOURCE FILE'S bytes rather than by the (not-yet-built) envelope's - not as a multi-megabyte file left
    // beside `base.fbx`: untracked, not gitignored, and on disk indistinguishable from a genuinely
    // hand-authored `.stmesh` source such as StaticProbe.stmesh.
    inline constexpr Common::DDC::Deriver kMeshSourceDeriver{
         "ImportedMeshSource", ".stmesh", { 0x51F3A9C0D4E6B812ULL, 0x2A7C15E8934FD061ULL } };

    // The importer's own determinism version (AssimpImporter -> MeshSourceFromImport): bump it whenever that
    // conversion produces different bytes for the same source file. The envelope layout (MSAS) is keyed beside
    // it by MeshSourceDerivedDataKey, so an MSAS bump needs no bump here.
    // 2 (FIX8): the envelope's GUID is the source's import record's, no longer minted per import.
    // 3 (SKEL-eng3): MeshSourceAsset SRCE version 3 (the skin names its skeleton by GUID).
    inline constexpr uint32_t kMeshSourceBuilderVersion = 4; // 4: MikkTSpace tangents (Engine/Geometry/MikkTangents.hpp), vertices split on frame seams

    // Reads and hashes @p file whole (Utils::PakContentHash over its bytes) - the one place both the
    // importer (keying its Put) and the loader (keying its Get) compute this, so they can never drift into
    // hashing two different things for "the same" file. A .gltf also hashes every external buffer its
    // "buffers" name (the .bin files hold the geometry; a buffer edited alone is a new source, as UE's DDC
    // keys cover every file the source reads); a buffer that cannot be read is an error.
    Common::ResultStr<uint64_t> HashMeshSourceFile( const std::filesystem::path& file );

    // Source file bytes -> DDC key. No settings image beyond the builder version: unlike the render bucket,
    // there is no independent build setting here - AF4g already made unit/axis/LOD-split resolution a pure
    // function of the file's own bytes, so the file's hash is the whole input.
    uint64_t MeshSourceDerivedDataKey( uint64_t sourceFileHash );

    // P9b: AN IMPORTED MESH EDITED IN THE EDITOR (UE: a modeling tool commits into the UStaticMesh itself, and the
    // .fbx stays as it was until a re-import overwrites the edit). The edited source is written at the asset
    // path, `<stem>.stmesh` beside the raw source, and states the SAME GUID as the source's import record - so
    // every scene and foliage type that names the mesh by GUID keeps naming it. True exactly when @p assetPath
    // is such a file: a raw source beside it, its import record, and the file's header GUID equal to the
    // record's. A leftover from a build before AF4h carries the GUID that import minted, never the record's
    // (FIX8 minted the records fresh), so it is still never read in the DDC envelope's place.
    [[nodiscard]] bool IsEditedImportedMesh( const std::filesystem::path& assetPath );

    // THE BUILD A MISS OF kMeshSourceDeriver RUNS (UE: the DDC miss of a static mesh rebuilds from its package).
    // Given the raw source beside the asset, it Puts the envelope of the source's current bytes under
    // MeshSourceDerivedDataKey, or returns why it could not. Registered by the editor, which owns the importers;
    // a packaged game registers none. An empty builder unregisters.
    using MeshSourceBuilder = std::function<Common::BoolResultStr( const std::filesystem::path& source )>;
    void SetMeshSourceBuilder( MeshSourceBuilder builder );

    // Resolves the MeshSourceAsset an asset PATH names (e.g. `Meshes/Props/base.stmesh`).
    //
    // A companion raw source beside it (today: `base.fbx`, same stem) means this .stmesh is DERIVED: its
    // bytes are read from the DDC, keyed by the source file's OWN content, and @p assetPath itself is never
    // opened - a stale beside-source file left over from a build that still wrote one (AF4h retires that
    // write) is therefore never read in its place, whether or not it happens to exist. A DDC miss is a
    // miss is rebuilt from the source by the registered MeshSourceBuilder (the editor's; UE: a DDC miss rebuilds
    // the derived data from the package) and read again; where none is registered (a packaged game) or the
    // rebuild fails, it is a refusal naming the source and the key. Nothing here silently falls back to reading
    // the stale file.
    //
    // An edited import (IsEditedImportedMesh) is the exception: that file IS the asset, read from disk.
    //
    // No companion source (e.g. `StaticProbe.stmesh`, MeshSourceProvenance::Recovered) means @p assetPath IS
    // the source, exactly as before: read directly from disk.
    Common::ResultStr<MeshSourceAsset> LoadMeshSourceAsset( const std::filesystem::path& assetPath );
} // namespace Desert::Assets
