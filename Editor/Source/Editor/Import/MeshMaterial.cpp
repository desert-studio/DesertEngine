#include "MeshMaterial.hpp"

#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/UUID.hpp>

#include <filesystem>

namespace Desert::Editor::MeshMaterial
{
    Assets::AssetHandle ResolveSidecar( Assets::AssetManager& mgr, const std::string& meshSourcePath )
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path  mesh( meshSourcePath );
        const fs::path  dir = mesh.parent_path();

        auto firstDematIn = [&]( const fs::path& d ) -> fs::path
        {
            if ( d.empty() || !fs::exists( d, ec ) )
                return {};
            for ( const auto& f : fs::directory_iterator( d, ec ) )
                if ( f.path().extension() == ".demat" )
                    return f.path();
            return {};
        };

        // 1) <stem>.demat next to the mesh  2) any .demat in the mesh folder  3) any .demat in the parent.
        fs::path matPath = dir / ( mesh.stem().string() + ".demat" );
        if ( !fs::exists( matPath, ec ) )
            matPath = firstDematIn( dir );
        if ( matPath.empty() )
            matPath = firstDematIn( dir.parent_path() );
        if ( matPath.empty() )
            return Common::UUID::Null();

        const std::string matStr = matPath.generic_string();
        auto              asset  = mgr.FindByPath<Assets::SurfaceMaterialAsset>( matStr );
        if ( !asset )
            asset = mgr.CreateAsset<Assets::SurfaceMaterialAsset>( matStr, false );
        if ( !asset )
            return Common::UUID::Null();

        const auto h = asset->GetMetadata().Handle;
        // A MAP WRITE, NOT A READ (AL1-5c). `Get` built the material on a miss and `Register` parsed it —
        // both inside the frame of a mesh drop or a mesh thumbnail (`base_basic_shaded/model.demat` on
        // Starter). The service reads a registered shell through AsyncAssetLoader when it is first drawn.
        Runtime::EnsureMaterialRegistered( asset );
        return h;
    }
} // namespace Desert::Editor::MeshMaterial
