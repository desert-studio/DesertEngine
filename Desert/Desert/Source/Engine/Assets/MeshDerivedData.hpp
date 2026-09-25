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
    // build steps (tangents, transform, LOD simplification) produce different bytes for the same source.
    inline constexpr uint32_t kMeshBuilderVersion = 1;

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
} // namespace Desert::Assets
