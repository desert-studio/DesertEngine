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
    inline constexpr uint32_t kMeshBuilderVersion = 2; // 2: sections are named by their material slot

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

    // The MeshBinary container bytes of the asset's render data (EncodeMeshBinary's output).
    using MeshPlatformDataBuilder = std::function<Common::ResultStr<std::string>( const MeshSourceAsset& asset )>;
    void SetMeshPlatformDataBuilder( MeshPlatformDataBuilder builder );

    // DDC hit, or - where a builder is registered - build and Put. A failed Put is an error carrying the file
    // system's reason: a cache that silently stays cold rebuilds on every load and nobody learns why.
    Common::ResultStr<std::string> LoadMeshPlatformData( const std::filesystem::path& asset );

    // AF4h: THE IMPORTED SOURCE ENVELOPE IS ALSO DERIVED. Everything an import writes into a static mesh's
    // MeshSourceAsset - the source's EditMesh models, its material slot table - is a pure function of the
    // raw file's own bytes (AF4g already made unit/axis resolution pure too, reading them out of the file
    // instead of asking the user). It belongs in the DDC beside the render-data bucket above, keyed by the
    // SOURCE FILE'S bytes rather than by the (not-yet-built) envelope's - not as a multi-megabyte file left
    // beside `base.fbx`: untracked, not gitignored, and on disk indistinguishable from a genuinely
    // hand-authored `.stmesh` source such as StaticProbe.stmesh.
    inline constexpr Common::DDC::Deriver kMeshSourceDeriver{
         "ImportedMeshSource", ".stmesh", { 0x51F3A9C0D4E6B812ULL, 0x2A7C15E8934FD061ULL } };

    // The importer's own determinism version (AssimpImporter -> MeshSourceFromImport): bump it whenever that
    // conversion produces different bytes for the same source file.
    inline constexpr uint32_t kMeshSourceBuilderVersion = 1;

    // Reads and hashes @p file whole (Utils::PakContentHash over its bytes) - the one place both the
    // importer (keying its Put) and the loader (keying its Get) compute this, so they can never drift into
    // hashing two different things for "the same" file.
    Common::ResultStr<uint64_t> HashMeshSourceFile( const std::filesystem::path& file );

    // Source file bytes -> DDC key. No settings image beyond the builder version: unlike the render bucket,
    // there is no independent build setting here - AF4g already made unit/axis/LOD-split resolution a pure
    // function of the file's own bytes, so the file's hash is the whole input.
    uint64_t MeshSourceDerivedDataKey( uint64_t sourceFileHash );

    // Resolves the MeshSourceAsset an asset PATH names (e.g. `Meshes/Props/base.stmesh`).
    //
    // A companion raw source beside it (today: `base.fbx`, same stem) means this .stmesh is DERIVED: its
    // bytes are read from the DDC, keyed by the source file's OWN content, and @p assetPath itself is never
    // opened - a stale beside-source file left over from a build that still wrote one (AF4h retires that
    // write) is therefore never read in its place, whether or not it happens to exist. A DDC miss is a
    // refusal naming the source and the key: nothing here silently falls back to reading the stale file, and
    // nothing here re-imports (that is the editor's job, on request).
    //
    // No companion source (e.g. `StaticProbe.stmesh`, MeshSourceProvenance::Recovered) means @p assetPath IS
    // the source, exactly as before: read directly from disk.
    Common::ResultStr<MeshSourceAsset> LoadMeshSourceAsset( const std::filesystem::path& assetPath );
} // namespace Desert::Assets
