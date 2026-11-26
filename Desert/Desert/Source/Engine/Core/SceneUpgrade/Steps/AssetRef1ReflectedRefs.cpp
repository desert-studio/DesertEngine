#include <Engine/Core/SceneUpgrade/Steps/AssetRef1ReflectedRefs.hpp>

#include <Engine/Core/SceneUpgrade/Steps/StepEdits.hpp>
#include <Common/Content/TextAssetHeader.hpp>

#include <array>
#include <map>
#include <utility>

#include <format>
#include <string>

namespace Desert::Core
{
    namespace
    {
        using UpgradeEdits::DropKey;
        using UpgradeEdits::EditBlock;
    } // namespace

    ReflectedAssetRefsReport MigrateReflectedAssetRefsV0ToV1( std::vector<Assets::EntityData>& entities,
                                                              const MaterialGuidLookup&        materialGuid )
    {
        ReflectedAssetRefsReport report;
        // The header GUID of the `.demat` an "assets:" key names, read once per key; nullopt = refused.
        std::map<std::string, std::optional<std::string>> guids;
        const auto guidOf = [&]( const std::string& key, const std::string& who ) -> std::optional<std::string>
        {
            if ( const auto found = guids.find( key ); found != guids.end() )
                return found->second;
            std::optional<std::string> guid = materialGuid ? materialGuid( key ) : std::nullopt;
            if ( guid && !Common::Content::AssetGuidFromText( *guid ) )
                guid.reset();
            if ( !guid )
                report.Refused.push_back( std::format( "{}: material '{}' states no header GUID", who, key ) );
            guids.emplace( key, guid );
            return guid;
        };
        const auto ref = []( const std::string& guid, const std::string& path )
        {
            rfl::Generic::Object one;
            one["Guid"] = rfl::Generic( guid );
            one["Path"] = rfl::Generic( path );
            return rfl::Generic( std::move( one ) );
        };
        const auto texts = [&]( const rfl::Generic::Object& block, const char* key, const std::string& who,
                                std::vector<std::string>& out ) -> bool
        {
            const auto stated = block.get( key );
            if ( !stated.has_value() )
                return true;
            const auto array = stated.value().to_array();
            if ( !array.has_value() )
            {
                report.Refused.push_back( std::format( "{}: StaticMesh.{} is not an array", who, key ) );
                return false;
            }
            for ( const rfl::Generic& item : array.value() )
                out.push_back( item.to_string().value_or( std::string() ) );
            return true;
        };
        const auto raise = [&]( rfl::ExtraFields<rfl::Generic>& components, const std::string& who )
        {
            EditBlock( components, "StaticMesh",
                       [&]( rfl::Generic::Object& block )
                       {
                           const auto text = [&]( const char* key )
                           {
                               const auto v = block.get( key );
                               return v.has_value() ? v.value().to_string().value_or( std::string() )
                                                    : std::string();
                           };
                           const bool statesMesh = block.get( "MeshGuid" ).has_value() ||
                                                   block.get( "MeshPath" ).has_value();
                           std::vector<std::string> slotGuids;
                           std::vector<std::string> slotPaths;
                           if ( !texts( block, "MaterialGuids", who, slotGuids ) ||
                                !texts( block, "MaterialPaths", who, slotPaths ) )
                               return false;
                           const bool statesSlots = block.get( "MaterialGuids" ).has_value() ||
                                                    block.get( "MaterialPaths" ).has_value();
                           if ( !statesMesh && !statesSlots && block.get( "MeshHandle" ).has_value() )
                               return false; // already v1
                           const std::string meshGuid = text( "MeshGuid" );
                           const std::string meshPath = text( "MeshPath" );
                           for ( const char* key : { "MeshGuid", "MeshPath", "MaterialGuids", "MaterialPaths" } )
                               DropKey( block, key );
                           // Both keys always, as the save writes them (reflection states every field): a
                           // primitive's MeshHandle is the empty {"", ""}, so a migrated scene and the same
                           // scene saved again are byte-equal.
                           block["MeshHandle"] = ref( meshGuid, meshPath );
                           rfl::Generic::Array slots;
                           const std::size_t   count = std::max( slotGuids.size(), slotPaths.size() );
                           for ( std::size_t i = 0; i < count; ++i )
                               slots.push_back( ref( i < slotGuids.size() ? slotGuids[i] : std::string(),
                                                     i < slotPaths.size() ? slotPaths[i] : std::string() ) );
                           block["MaterialSlots"] = rfl::Generic( std::move( slots ) );
                           ++report.MeshBlocks;
                           return true;
                       } );
            // The reflected Asset<MaterialAsset> fields, stated as a bare key until v1.
            constexpr std::array<std::pair<const char*, const char*>, 4> kMaterialFields = {
                 { { "LandscapeMaterial", "Material" },
                   { "VolumetricCloud", "Material" },
                   { "Destructible", "InteriorMaterial" },
                   { "UIPanel", "Material" } } };
            for ( const auto& [component, field] : kMaterialFields )
                EditBlock( components, component,
                           [&]( rfl::Generic::Object& block )
                           {
                               const auto stated = block.get( field );
                               if ( !stated.has_value() )
                                   return false;
                               const auto key = stated.value().to_string();
                               if ( !key.has_value() )
                                   return false; // already an object: {Guid, Path}
                               std::string guid;
                               if ( !key.value().empty() )
                               {
                                   const auto found =
                                        guidOf( key.value(), std::format( "{} / {}.{}", who, component, field ) );
                                   if ( !found )
                                       return false;
                                   guid = *found;
                               }
                               block[field] = ref( guid, key.value() );
                               ++report.MaterialFields;
                               return true;
                           } );
        };
        for ( auto& entity : entities )
        {
            const std::string who = entity.id.has_value() ? std::format( "entity {}", entity.id->ToString() )
                                                          : std::string( "<record without id>" );
            raise( entity.Components, who );
            if ( entity.PrefabOverrides )
                for ( auto& override_ : *entity.PrefabOverrides )
                    raise( override_.Components, who + " (prefab override)" );
        }
        return report;
    }

    StepOutcome RunReflectedAssetRefs( UpgradeContext& context )
    {
        ReflectedAssetRefsReport report = MigrateReflectedAssetRefsV0ToV1( context.Entities, context.MaterialGuid );
        StepOutcome              outcome;
        outcome.Changed = report.MeshBlocks + report.MaterialFields;
        outcome.Summary = std::format( "{} Static Mesh block(s) and {} material field(s) restated as {{Guid, Path}}",
                                       report.MeshBlocks, report.MaterialFields );
        outcome.Refused = std::move( report.Refused );
        return outcome;
    }
} // namespace Desert::Core
