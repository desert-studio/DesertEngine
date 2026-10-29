#include "VirtualGeometryGate.hpp"

#include "ImportManager.hpp"
#include "MaterialAdoption.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Assets/MaterialFormat.hpp>

#include <algorithm>
#include <format>
#include <vector>

namespace Desert::Editor
{
    namespace
    {
        // Why the material @p slot names cannot be drawn by the cluster raster; nullopt when it can. Only
        // Translucent / Additive refuse; an Opaque or Masked material on any shader, import template or not, stands
        // (a Masked one clips in its `.VirtualGeometry` cells, Mesh/Surface/SurfaceTypes.glslh DESERT_SURFACE_CLIP).
        std::optional<std::string> MaterialRefusal( const Assets::MeshMaterialSlot& slot,
                                                    const std::filesystem::path&    source )
        {
            const std::filesystem::path path = MaterialAdoption::MaterialAssetPath( source, slot.Name );
            const auto                  read = Common::Utils::FileSystem::ReadFileContentIfExists( path );
            if ( !read )
                return std::format( "material '{}' could not be read: {}", path.generic_string(),
                                    read.GetError() );
            if ( !read.GetValue().has_value() )
                return std::format( "material '{}' is not on disk, so its blend mode is unknown",
                                    path.generic_string() );
            const auto material = Assets::ParseMaterialJson( path.generic_string(), *read.GetValue() );
            if ( !material )
                return std::format( "material '{}' does not parse: {}", path.generic_string(),
                                    material.GetError() );
            if ( material.GetValue().Guid() != slot.Material )
                return std::format( "material '{}' states another GUID than the slot '{}' names",
                                    path.generic_string(), slot.Name );
            const Common::Content::AssetGuid shader = material.GetValue().ShaderGuid();
            if ( shader.IsNull() )
                return std::format( "material '{}' states no shader of its own (an instance), so its blend mode "
                                    "is not read here",
                                    path.generic_string() );
            const auto blend = ImportManager::PublishedShaderBlend( shader );
            if ( !blend )
                return std::format( "material '{}' draws with a shader whose BlendMode was not published (not "
                                    "loaded, or not a DSL shader), so its blend mode is unknown",
                                    path.generic_string() );
            if ( ::Desert::Core::Formats::IsTranslucentBlend( *blend ) )
                return std::format( "material '{}' is Translucent/Additive (drawn by the translucency pass, never "
                                    "into depth)",
                                    path.generic_string() );
            return std::nullopt;
        }
    } // namespace

    std::optional<std::string> VirtualGeometryRefusal( const Assets::MeshImportSettings&           settings,
                                                       const Assets::Serialization::MeshAssetData& mesh,
                                                       std::span<const Assets::MeshMaterialSlot>   named,
                                                       const std::filesystem::path&                source )
    {
        if ( !settings.VirtualGeometry )
            return std::nullopt;
        if ( mesh.IsSkinned )
            return std::string( "a skinned mesh is drawn by the binned path" );
        if ( settings.Displacement == Assets::MeshDisplacementMode::Runtime )
            return std::string( "Displacement Runtime dices the surface per frame, so it has no cooked clusters" );
        std::vector<Common::Content::AssetGuid> judged;
        for ( const auto& submesh : mesh.Submeshes )
        {
            // A null GUID is an unassigned slot: it draws the engine's default material, which is Opaque.
            const Common::Content::AssetGuid guid = submesh.MaterialGuid;
            if ( guid.IsNull() || std::ranges::find( judged, guid ) != judged.end() )
                continue;
            judged.push_back( guid );
            const auto slot = std::ranges::find_if( named, [&]( const Assets::MeshMaterialSlot& s )
                                                    { return s.Material == guid; } );
            if ( slot == named.end() )
                return std::string( "a submesh names a material the import did not resolve, so its blend mode is "
                                    "unknown" );
            if ( auto refused = MaterialRefusal( *slot, source ) )
                return refused;
        }
        return std::nullopt;
    }

    std::optional<std::string> VirtualGeometryRefusal( const Assets::MeshImportSettings&         settings,
                                                       std::span<const Assets::MeshMaterialSlot> slots,
                                                       const std::filesystem::path&              source )
    {
        if ( !settings.VirtualGeometry )
            return std::nullopt;
        if ( settings.Displacement == Assets::MeshDisplacementMode::Runtime )
            return std::string( "Displacement Runtime dices the surface per frame, so it has no cooked clusters" );
        for ( const Assets::MeshMaterialSlot& slot : slots )
        {
            // A null GUID is an unassigned slot: it draws the engine's default material, which is Opaque.
            if ( slot.Material.IsNull() )
                continue;
            if ( auto refused = MaterialRefusal( slot, source ) )
                return refused;
        }
        return std::nullopt;
    }

    Assets::MeshImportSettings GateVirtualGeometry( Assets::MeshImportSettings                  settings,
                                                    const Assets::Serialization::MeshAssetData& mesh,
                                                    std::span<const Assets::MeshMaterialSlot>   named,
                                                    const std::filesystem::path&                source,
                                                    std::string_view                            meshName )
    {
        if ( const auto refused = VirtualGeometryRefusal( settings, mesh, named, source ) )
        {
            LOG_WARN( "[VirtGeomImport] '{}': Virtual Geometry refused, the mesh stores it off: {}", meshName,
                      *refused );
            settings.VirtualGeometry = false;
        }
        return settings;
    }
} // namespace Desert::Editor
