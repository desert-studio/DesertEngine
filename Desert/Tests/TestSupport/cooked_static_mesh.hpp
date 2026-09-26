#pragma once

// A STATIC MESH AS A COOKED PACKAGE HOLDS IT (AF4d): the source asset on disk and, in the DDC under that
// asset's key, the render form StaticMeshAsset draws. An engine suite has no mesh builder -- it lives in the
// editor -- so a fixture that wants StaticMeshAsset to load chosen render data stores both halves itself, which
// is exactly what the cook leaves behind for a game.

#include <Common/Content/DerivedDataCache.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>

namespace Desert::TestSupport
{
    // A one-triangle recovered source, UNIQUE PER CALL. The DDC key is the source's content hash, so two
    // fixtures sharing a source would share one render form, and an entry left by an earlier run would answer
    // for this one. The triangle's width carries a per-process random base plus a counter, so no key repeats.
    inline Assets::MeshSourceAsset UniqueStandInSource( const std::string& name )
    {
        static const uint32_t        base = std::random_device{}() % 1000000u;
        static std::atomic<uint32_t> counter{ 0 };
        const float width = 1.0f + static_cast<float>( base ) + 1000000.0f * static_cast<float>( counter++ );

        Assets::MeshSourceAsset asset;
        asset.Guid              = Common::Content::AssetGuid::Generate();
        asset.Name              = name;
        asset.Import.Provenance = Assets::MeshSourceProvenance::Recovered;
        Geometry::EditMeshSer mesh;
        mesh.Positions             = { 0.0f, 0.0f, 0.0f, width, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
        mesh.Triangles             = { 0, 1, 2 };
        mesh.PolyGroups            = { 0 };
        mesh.MaterialIds           = { 0 };
        asset.Source.Models        = { { mesh } };
        asset.Source.MaterialSlots = { { "Default", {} } };
        return asset;
    }

    // Writes @p file as a mesh source asset whose render form in the DDC is @p renderBytes, and returns the DDC
    // entry's path so the caller can remove it (the sandbox DDC is shared by every run). An empty path means the
    // write failed; the failure is already reported to gtest.
    inline std::filesystem::path WriteCookedStaticMesh( const std::filesystem::path& file,
                                                        const std::string_view       renderBytes )
    {
        const Assets::MeshSourceAsset asset = UniqueStandInSource( file.stem().string() );
        std::error_code               ec;
        std::filesystem::create_directories( file.parent_path(), ec );
        if ( const auto wrote = Assets::WriteMeshSourceAssetFile( file, asset ); !wrote.IsSuccess() )
        {
            ADD_FAILURE() << wrote.GetError();
            return {};
        }
        const uint64_t key = Assets::MeshAssetDerivedDataKey( asset );
        if ( const auto put = Common::DDC::Put( Assets::kMeshDeriver, key, renderBytes ); !put.IsSuccess() )
        {
            ADD_FAILURE() << put.GetError();
            return {};
        }
        return Common::DDC::PathFor( Assets::kMeshDeriver, key );
    }
} // namespace Desert::TestSupport
