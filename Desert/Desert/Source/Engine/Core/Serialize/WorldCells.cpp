#include <Engine/Core/Serialize/WorldCells.hpp>

#include <Engine/Assets/Serialization/FoliageType.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODHierarchy.hpp>
#include <Engine/Core/Serialize/EntityDescriptorIndex.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include <Engine/Scripting/ScriptSchema.hpp>

#include <Common/Core/AssetHandle.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/ExternalEntitiesFolder.hpp>
#include <Common/Content/ShaderAssetHeader.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Utilities/Crc32c.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/PakFile.hpp>

#include <Common/Json/Document.hpp>
#include <Common/Json/Json.hpp>

#include <algorithm>
#include <format>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Desert::Core::WorldCells
{
    namespace
    {
        namespace CC = Common::Content;

        constexpr std::array<CC::SubsystemVersion, 1> kKnownSubsystems = {
             { { kWorldFormatTag, kWorldFormatVersion } } };

        // A cooked file's identity follows from what it IS -- this world's file of this name -- and from nothing
        // else, so two cooks of one source stay byte-identical (a minted GUID would differ on every cook). UE
        // names a cooked cell package the same way: the world plus the cell.
        CC::AssetGuid GuidOf( std::string_view world, std::string_view fileName )
        {
            const std::string identity = std::string( world ) + '\n' + std::string( fileName );
            const std::string salted   = "DesertWorldCell\n" + identity;
            CC::AssetGuid     guid;
            guid.Hi = Common::Utils::PakContentHash( identity.data(), identity.size() );
            guid.Lo = Common::Utils::PakContentHash( salted.data(), salted.size() );
            if ( guid.IsNull() )
                guid.Lo = 1; // the envelope refuses a null GUID; both halves hashing to zero is the only way there
            return guid;
        }

        // The shared asset envelope (AF1) around the JSON payload, which is unchanged. Dependencies stay empty:
        // a cell's records name assets by registry key, and a registry row carries no GUID to list here.
        Common::ResultStr<std::vector<unsigned char>> Wrap( CC::ContentKind kind, std::string_view world,
                                                            std::string_view fileName, const std::string& payload )
        {
            CC::AssetEnvelope envelope;
            envelope.Asset.Kind       = kind;
            envelope.Asset.Guid       = GuidOf( world, fileName );
            envelope.Asset.Subsystems = { kKnownSubsystems.begin(), kKnownSubsystems.end() };
            CC::EnvelopeSectionData section;
            section.Tag = CC::EnvelopeSection::Payload;
            section.Bytes.resize( payload.size() );
            std::memcpy( section.Bytes.data(), payload.data(), payload.size() );
            envelope.Sections.push_back( std::move( section ) );
            auto written = CC::WriteAssetEnvelope( envelope );
            if ( !written )
                return Common::MakeError<std::vector<unsigned char>>( "'" + std::string( fileName ) +
                                                                      "': " + written.GetError() );
            const std::vector<std::byte>& bytes = written.GetValue();
            std::vector<unsigned char>    out( bytes.size() );
            std::memcpy( out.data(), bytes.data(), bytes.size() );
            return Common::MakeSuccess( std::move( out ) );
        }

        // The payload, once the envelope is believed: the envelope checks magic, header CRC, subsystem versions
        // and every section's hash BEFORE any byte is handed on; this adds the kind and the exact format version.
        Common::ResultStr<std::string> Unwrap( std::string_view fileName, CC::ContentKind kind,
                                               std::span<const unsigned char> bytes )
        {
            const std::string name( fileName );
            auto              envelope = CC::ReadAssetEnvelope( std::as_bytes( bytes ), { kKnownSubsystems } );
            if ( !envelope )
                return Common::MakeError<std::string>( "'" + name + "': " + envelope.GetError() );
            const CC::AssetHeader& asset = envelope.GetValue().Asset;
            if ( asset.Kind != kind )
                return Common::MakeError<std::string>(
                     "'" + name + "' is a " + std::string( CC::KindSpec( asset.Kind ).Name ) +
                     " asset, expected a " + std::string( CC::KindSpec( kind ).Name ) );
            // The envelope accepts an OLDER version; a cooked world is re-derivable, so it is refused instead.
            const auto stamped =
                 std::find_if( asset.Subsystems.begin(), asset.Subsystems.end(),
                               []( const CC::SubsystemVersion& s ) { return s.Tag == kWorldFormatTag; } );
            if ( stamped == asset.Subsystems.end() )
                return Common::MakeError<std::string>( "'" + name + "' carries no world format version ('" +
                                                       CC::FourCCToString( kWorldFormatTag ) + "')" );
            if ( stamped->Version != kWorldFormatVersion )
                return Common::MakeError<std::string>(
                     "'" + name + "' is world format version " + std::to_string( stamped->Version ) +
                     "; this build reads " + std::to_string( kWorldFormatVersion ) + " only. Re-cook it." );
            for ( const CC::EnvelopeSectionData& section : envelope.GetValue().Sections )
                if ( section.Tag == CC::EnvelopeSection::Payload )
                    return Common::MakeSuccess( std::string( reinterpret_cast<const char*>( section.Bytes.data() ),
                                                             section.Bytes.size() ) );
            return Common::MakeError<std::string>( "'" + name + "' has no Payload section" );
        }

        std::string ReasonName( Rules::AlwaysLoadedReason reason )
        {
            switch ( reason )
            {
                case Rules::AlwaysLoadedReason::None:
                    return "None";
                case Rules::AlwaysLoadedReason::Author:
                    return "Author";
                case Rules::AlwaysLoadedReason::Component:
                    return "Component";
                case Rules::AlwaysLoadedReason::NoFit:
                    return "NoFit";
                case Rules::AlwaysLoadedReason::NoGrid:
                    return "NoGrid";
                case Rules::AlwaysLoadedReason::Persistent:
                    return "Persistent";
            }
            return "Unknown";
        }

        // A cell's file is named by its grid, level, coordinate and runtime data layers alone (Rules::CellStem), so
        // it does not move when the world gains or loses another cell, two grids' cells never share a file (WP22)
        // and neither do two sets of runtime layers in one square (WP-DATA-LAYERS-RUNTIME).
        std::string CellFileName( const WorldPartitionSerialized& partition, const Rules::PlannedCell& cell )
        {
            return Rules::CellStem( partition, cell ) + std::string( kCellExtension );
        }

        // A parent HLOD's file stem, by the HLOD cell it stands in for: `HLOD_H<layer grid>_<X>_<Z>`, and for a
        // cell of runtime data layers their suffix (`..._DL<hash>`, Rules::DataLayersCellSuffix), as a cell's
        // file.
        std::string ParentHLODFileStem( const WorldPartitionSerialized& partition,
                                        const Rules::HLODHierarchy& hierarchy, const Rules::HLODNode& node )
        {
            const Rules::HLODCell& child = hierarchy.Cells.at( node.StandsFor );
            return std::format( "HLOD_H{}_{}_{}{}", hierarchy.GridOfLayer.at( child.Layer ), child.Coord.X,
                                child.Coord.Z, Rules::DataLayersCellSuffix( partition, child.DataLayers ) );
        }

        // THE COOKED NAME OF ONE HLOD NODE (WP-FAR-8): the name its records' ids were made from at Build HLODs
        // (HLODRecordId), and the file of a node resident the whole session - `HLOD_<cell file>` for one standing
        // in for a world cell (HLODFileName), `HLOD_H<grid>_<X>_<Z>.dwcell` for a parent.
        std::string HLODNodeFileName( const WorldPartitionSerialized& partition, const Rules::HLODHierarchy& hierarchy,
                                      const Rules::WorldPartitionPlan& plan, const Rules::HLODNode& node )
        {
            if ( node.ForHLODCell )
                return ParentHLODFileStem( partition, hierarchy, node ) + std::string( kCellExtension );
            return HLODFileName(
                 CellFileName( partition, plan.Cells.at( node.StandsFor - plan.AlwaysLoaded.size() ) ) );
        }

        // An HLOD cell's file (WP-FAR-8), named as a world cell's is, by its HLOD grid and coordinate:
        // `H<grid>_<X>_<Z>.dwcell` (every HLOD cell is level 0), and for a cell of runtime data layers their suffix
        // (`..._DL<hash>`; WP-HLOD-DATA-LAYERS: an HLOD cell per runtime data layer set).
        std::string HLODCellFileName( const WorldPartitionSerialized& partition, const Rules::PlannedCell& cell )
        {
            return std::format( "H{}_{}_{}{}{}", cell.Grid, cell.Cell.X, cell.Cell.Z,
                                Rules::DataLayersCellSuffix( partition, cell.DataLayers ), kCellExtension );
        }

        // WHAT A UNIT NEEDS FROM THE CONTENT: every value in its records that the registry recognises as an
        // asset — a number that is a row's handle, a string that is a row's path (or a handle spelled in
        // decimal, the way ids travel in component blocks) — and then every row those rows depend on.
        // The registry decides what is content, so no component has to be listed here: a field that names
        // an asset names a row, and a number that names no row is not an asset.
        class AssetClosure
        {
        public:
            explicit AssetClosure( std::span<const Common::Utils::AssetRegistry> registries )
                 : m_Registries( registries )
            {
            }

            void Record( const Assets::EntityData& record, std::set<std::string>& keys )
            {
                if ( record.PrefabPath.has_value() )
                    Add( KeyOfPath( *record.PrefabPath ), keys );
                for ( const auto& [key, payload] : record.Components )
                    Value( Common::Json::Root( payload ), keys );
                if ( record.PrefabOverrides.has_value() )
                    for ( const auto& over : *record.PrefabOverrides )
                        for ( const auto& [key, payload] : over.Components )
                            Value( Common::Json::Root( payload ), keys );
            }

            // Adds, to @p keys, every row the rows in it depend on, transitively.
            void Expand( std::set<std::string>& keys )
            {
                std::vector<std::string> pending( keys.begin(), keys.end() );
                while ( !pending.empty() )
                {
                    const std::string key = std::move( pending.back() );
                    pending.pop_back();
                    for ( const std::string& dependency : DependenciesOf( key ) )
                        if ( keys.insert( dependency ).second )
                            pending.push_back( dependency );
                }
            }

        private:
            // Remembered per value, as the row's key ("" for none): a world names the same few materials tens
            // of thousands of times, and a path lookup normalises its spelling every time it is asked
            // (measured: 8.4 s of a 9.2 s Release cook of World_Grid8km before these two maps, 0.98 s after).
            const std::string& KeyOfHandle( std::uint64_t handle )
            {
                const auto known = m_ByHandle.find( handle );
                if ( known != m_ByHandle.end() )
                    return known->second;
                std::string key;
                for ( const auto& registry : m_Registries )
                    if ( const auto* row = handle != 0 ? registry.FindByHandle( handle ) : nullptr )
                    {
                        key = row->Key;
                        break;
                    }
                return m_ByHandle.emplace( handle, std::move( key ) ).first->second;
            }

            // Built once on the first GUID asked for: the rows are indexed by handle and key only.
            const std::string& KeyOfGuid( const CC::AssetGuid& guid )
            {
                if ( !m_ByGuidBuilt )
                {
                    m_ByGuidBuilt = true;
                    for ( const auto& registry : m_Registries )
                        for ( const auto& row : registry.Entries() )
                            if ( row.Guid.has_value() && !row.Guid->IsNull() )
                                m_ByGuid.emplace( std::pair{ row.Guid->Hi, row.Guid->Lo }, row.Key );
                }
                static const std::string none;
                const auto               known = m_ByGuid.find( std::pair{ guid.Hi, guid.Lo } );
                return known != m_ByGuid.end() ? known->second : none;
            }

            const std::string& KeyOfPath( const std::string& text )
            {
                const auto known = m_ByPath.find( text );
                if ( known != m_ByPath.end() )
                    return known->second;
                std::string key;
                for ( const auto& registry : m_Registries )
                    if ( const auto* row = registry.FindByReference( 0, text ) )
                    {
                        key = row->Key;
                        break;
                    }
                return m_ByPath.emplace( text, std::move( key ) ).first->second;
            }

            static void Add( const std::string& key, std::set<std::string>& keys )
            {
                if ( !key.empty() )
                    keys.insert( key );
            }

            // Every value of a payload, by its SHAPE: the closure does not know the component's fields, so a value
            // of any kind is a legitimate answer and none is an issue — what is not a reference names nothing.
            // An explicit stack walks the nesting: the payload's depth comes from the file, not from the code.
            void Value( const Common::Json::Node& root, std::set<std::string>& keys )
            {
                std::vector<Common::Json::Node> pending{ root };
                while ( !pending.empty() )
                {
                    const Common::Json::Node value = std::move( pending.back() );
                    pending.pop_back();
                    switch ( value.GetKind() )
                    {
                        case Common::Json::Kind::Integer:
                            if ( const auto whole = value.AsInteger() )
                                Add( KeyOfHandle( static_cast<std::uint64_t>( whole.GetValue() ) ), keys );
                            break;
                        case Common::Json::Kind::String:
                            AddString( value, keys );
                            break;
                        case Common::Json::Kind::Object:
                            value.ForEachMember( [&]( std::string_view, const Common::Json::Node& inner )
                                                 { pending.push_back( inner ); } );
                            break;
                        case Common::Json::Kind::Array:
                            value.ForEachElement( [&]( std::size_t, const Common::Json::Node& inner )
                                                  { pending.push_back( inner ); } );
                            break;
                        case Common::Json::Kind::Null:
                        case Common::Json::Kind::Bool:
                        case Common::Json::Kind::Real:
                            break;
                    }
                }
            }

            void AddString( const Common::Json::Node& value, std::set<std::string>& keys )
            {
                const auto         read = value.AsString();
                const std::string& text = read.GetValue();
                // A material slot names its material by header GUID text (SCNE 27). The row is found by its own
                // Guid: a registry read by LoadFrom has Identity only for assets that were parsed, so the handle
                // the GUID folds to finds nothing for a material the cook never opened.
                if ( const auto guid = CC::AssetGuidFromText( text ); guid && !guid.GetValue().IsNull() )
                {
                    Add( KeyOfGuid( guid.GetValue() ), keys );
                    return;
                }
                if ( const auto asHandle = value.AsUuid() )
                {
                    Add( KeyOfHandle( static_cast<std::uint64_t>( asHandle.GetValue() ) ), keys );
                    return;
                }
                Add( KeyOfPath( text ), keys );
            }

            const std::vector<std::string>& DependenciesOf( const std::string& key )
            {
                const auto known = m_Dependencies.find( key );
                if ( known != m_Dependencies.end() )
                    return known->second;
                std::vector<std::string> found;
                for ( const auto& registry : m_Registries )
                    if ( const auto* row = registry.FindByKey( key ) )
                    {
                        for ( const std::uint64_t handle : row->Dependencies )
                            if ( const std::string& dependency = KeyOfHandle( handle ); !dependency.empty() )
                                found.push_back( dependency );
                        break;
                    }
                return m_Dependencies.emplace( key, std::move( found ) ).first->second;
            }

            std::span<const Common::Utils::AssetRegistry>             m_Registries;
            std::unordered_map<std::string, std::vector<std::string>> m_Dependencies;
            std::unordered_map<std::uint64_t, std::string>            m_ByHandle;
            std::unordered_map<std::string, std::string>              m_ByPath;
            // First registry wins, as in KeyOfHandle and KeyOfPath (emplace keeps the first row seen).
            std::map<std::pair<std::uint64_t, std::uint64_t>, std::string> m_ByGuid;
            bool                                                           m_ByGuidBuilt = false;
        };
    } // namespace

    Rules::AssetBoundsSource BoundsFrom( std::span<const Common::Utils::AssetRegistry> registries )
    {
        if ( registries.empty() )
            return {};
        // First registry that answers wins, as KeyOfGuid does.
        return
             [registries]( const CC::AssetGuid& guid, std::string_view path ) -> std::optional<Common::Math::AABB>
        {
            for ( const auto& registry : registries )
                if ( const auto* row = registry.FindByGuidReference( guid, path ) )
                    return row->Bounds;
            return std::nullopt;
        };
    }

    Rules::CustomShaderSource CustomShaderFrom( std::span<const Common::Utils::AssetRegistry> registries )
    {
        if ( registries.empty() )
            return {};
        return [registries]( const CC::AssetGuid& guid, std::string_view path ) -> bool
        {
            const auto byHandle = [registries]( std::uint64_t handle ) -> const Common::Utils::AssetRegistryEntry*
            {
                for ( const auto& registry : registries )
                    if ( const auto* row = registry.FindByHandle( handle ) )
                        return row;
                return nullptr;
            };
            const Common::Utils::AssetRegistryEntry* material = nullptr;
            for ( const auto& registry : registries )
                if ( ( material = registry.FindByGuidReference( guid, path ) ) != nullptr )
                    break;
            // An instance chain is short; the bound only stops a cycle a damaged registry could state.
            constexpr int kMaxParents = 16;
            for ( int depth = 0; material != nullptr && depth < kMaxParents; ++depth )
            {
                const Common::Utils::AssetRegistryEntry* parent = nullptr;
                for ( const std::uint64_t dependency : material->Dependencies )
                {
                    const auto* row = byHandle( dependency );
                    if ( row == nullptr )
                        continue;
                    // The shader's manifest role, as the registry's Role tag carries it — never its name.
                    if ( row->Kind == "Shader" )
                        return row->Role != CC::kStandardSurfaceRole;
                    if ( row->Kind == "Material" && parent == nullptr )
                        parent = row;
                }
                material = parent;
            }
            return false;
        };
    }

    Rules::ScriptReferenceSource ScriptReferenceKinds()
    {
        struct Read
        {
            std::mutex                                               Lock;
            std::unordered_map<std::string, Scripting::ScriptSchema> Schemas; // by script key
        };
        auto read = std::make_shared<Read>();
        return [read]( std::string_view script, std::string_view property ) -> std::optional<Rules::ReferenceKind>
        {
            const std::lock_guard<std::mutex> hold( read->Lock );
            const std::string                 key( script );
            auto                              found = read->Schemas.find( key );
            if ( found == read->Schemas.end() )
            {
                const std::string path = Common::AssetHandle::PathForStableKey( key ).generic_string();
                found                  = read->Schemas.emplace( key, Scripting::ReadScriptSchema( path ) ).first;
            }
            const Scripting::ScriptSchema& schema = found->second;
            if ( schema.Status != Scripting::ScriptStatus::Ok )
                return std::nullopt;
            const Scripting::ScriptPropertyDecl* decl = Scripting::FindDecl( schema, std::string( property ) );
            if ( decl == nullptr )
                return std::nullopt;
            const bool entity = decl->Default.Type == Scripting::PropertyType::Entity ||
                                ( decl->Default.Type == Scripting::PropertyType::Array &&
                                  decl->ElementType == Scripting::PropertyType::Entity );
            if ( !entity )
                return std::nullopt;
            return decl->HardReference ? Rules::ReferenceKind::Containment : Rules::ReferenceKind::Observation;
        };
    }

    Rules::FoliageHLODSource FoliageInHLODFrom( std::span<const Common::Utils::AssetRegistry> registries )
    {
        if ( registries.empty() )
            return {};
        return [registries]( std::string_view guidText, std::string_view path ) -> Common::ResultStr<bool>
        {
            const auto guid = CC::AssetGuidFromText( guidText );
            if ( !guid )
                return Common::MakeFormattedError<bool>( "foliage type '{}' ({}): {}", path, guidText,
                                                         guid.GetError() );
            const Common::Utils::AssetRegistryEntry* row = nullptr;
            for ( const auto& registry : registries )
                if ( ( row = registry.FindByGuidReference( guid.GetValue(), path ) ) != nullptr )
                    break;
            if ( row == nullptr )
                return Common::MakeFormattedError<bool>( "foliage type '{}' ({}) is in no asset registry", path,
                                                         guidText );
            const std::filesystem::path file = Common::AssetHandle::PathForStableKey( row->Key );
            std::ifstream               in( file, std::ios::binary );
            if ( !in )
                return Common::MakeFormattedError<bool>( "foliage type '{}' could not be opened", file.string() );
            const std::string text( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
            const auto        parsed = Assets::Serialization::ParseFoliageType( text );
            if ( !parsed )
                return Common::MakeFormattedError<bool>( "foliage type '{}': {}", file.string(),
                                                         parsed.GetError() );
            return Common::MakeSuccess( parsed.GetValue().IncludeInHLOD );
        };
    }

    // The cook over an expanded world (no record states a prefab) and the descriptors it is planned with.
    static Common::ResultStr<CookedWorld> CookPlanned( const SceneSerialized&                        scene,
                                                       std::span<const Rules::EntityDescriptor>      descriptors,
                                                       std::span<const Common::Utils::AssetRegistry> registries,
                                                       const StoredHLODs*                            hlods );

    Common::ResultStr<CookedWorld> CookWorld( const SceneSerialized&                        scene,
                                              std::span<const Common::Utils::AssetRegistry> registries,
                                              const PrefabReader& prefabs, const StoredHLODs* hlods )
    {
        auto expanded = ExpandPrefabInstances( scene, prefabs );
        if ( !expanded )
            return Common::MakeError<CookedWorld>( "'" + scene.SceneName + "': " + expanded.GetError() );
        const SceneSerialized&                     world       = expanded.GetValue().Scene;
        const std::vector<Rules::EntityDescriptor> descriptors = Rules::DescribeEntities( world.Entities );
        return CookPlanned( world, descriptors, registries, hlods );
    }

    Common::ResultStr<ExpandedWorld> ExpandWorldForCook( const SceneSerialized&                   scene,
                                                         std::span<const Rules::EntityDescriptor> descriptors,
                                                         const PrefabReader&                      prefabs )
    {
        using Result = ExpandedWorld;
        // The index is the SOURCE world's: it must be its records', one for one, before an instance is expanded.
        const auto& source = scene.Entities;
        if ( descriptors.size() != source.size() )
            return Common::MakeError<Result>(
                 std::format( "'{}' has {} records and {} descriptors; the descriptor index is not this world's",
                              scene.SceneName, source.size(), descriptors.size() ) );
        for ( std::size_t record = 0; record < source.size(); ++record )
            if ( !source[record].id.has_value() ||
                 descriptors[record].Id != static_cast<std::uint64_t>( *source[record].id ) )
                return Common::MakeError<Result>(
                     std::format( "record {} of '{}' has no id or its descriptor is another entity's; the "
                                  "descriptor index is stale",
                                  record, scene.SceneName ) );

        auto expanded = ExpandPrefabInstances( scene, prefabs );
        if ( !expanded )
            return Common::MakeError<Result>( "'" + scene.SceneName + "': " + expanded.GetError() );
        Result out;
        out.World = expanded.ExtractValue();
        // An instance's descriptor described the prefab as one unit; its expanded records describe themselves.
        out.Descriptors.reserve( out.World.Scene.Entities.size() );
        for ( std::size_t record = 0; record < out.World.Slots.size(); ++record )
        {
            const ExpandedSlot& slot = out.World.Slots[record];
            if ( !slot.Instance )
            {
                out.Descriptors.push_back( descriptors[record] );
                continue;
            }
            std::vector<Rules::EntityDescriptor> body =
                 Rules::DescribeEntities( std::span<const Assets::EntityData>( out.World.Scene.Entities )
                                               .subspan( slot.First, slot.Count ) );
            out.Descriptors.insert( out.Descriptors.end(), std::make_move_iterator( body.begin() ),
                                    std::make_move_iterator( body.end() ) );
        }
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::ResultStr<CookedWorld> CookWorld( const SceneSerialized&                        scene,
                                              std::span<const Rules::EntityDescriptor>      descriptors,
                                              std::span<const Common::Utils::AssetRegistry> registries,
                                              const PrefabReader& prefabs, const StoredHLODs* hlods )
    {
        auto expanded = ExpandWorldForCook( scene, descriptors, prefabs );
        if ( !expanded )
            return Common::MakeError<CookedWorld>( expanded.GetError() );
        return CookPlanned( expanded.GetValue().World.Scene, expanded.GetValue().Descriptors, registries, hlods );
    }

    static Common::ResultStr<CookedWorld> CookPlanned( const SceneSerialized&                        scene,
                                                       std::span<const Rules::EntityDescriptor>      descriptors,
                                                       std::span<const Common::Utils::AssetRegistry> registries,
                                                       const StoredHLODs*                            hlods )
    {
        using Result = CookedWorld;
        // A LOCATION VOLUME IS EDITOR-ONLY (WP-LOADER-ADAPTERS; UE never cooks an ALocationVolume): its records
        // leave the world before it is planned, so no cell, no always-loaded file and no game ever holds one. A
        // record parented to a volume would lose its place, so the cook refuses it by name. (A descriptor count
        // that is not the records' is refused below, as it always was.)
        if ( descriptors.size() == scene.Entities.size() &&
             std::any_of( descriptors.begin(), descriptors.end(), []( const Rules::EntityDescriptor& descriptor )
                          { return descriptor.LocationVolume.has_value(); } ) )
        {
            std::unordered_set<std::uint64_t> volumes;
            for ( const Rules::EntityDescriptor& descriptor : descriptors )
                if ( descriptor.LocationVolume.has_value() && descriptor.Id.has_value() )
                    volumes.insert( *descriptor.Id );
            SceneSerialized                      kept = scene;
            std::vector<Rules::EntityDescriptor> keptDescriptors;
            keptDescriptors.reserve( descriptors.size() );
            std::size_t write = 0;
            for ( std::size_t record = 0; record < descriptors.size(); ++record )
            {
                const Rules::EntityDescriptor& descriptor = descriptors[record];
                if ( descriptor.LocationVolume.has_value() )
                    continue;
                if ( descriptor.Parent.has_value() && volumes.contains( *descriptor.Parent ) )
                    return Common::MakeError<Result>( std::format(
                         "record {} ('{}') of '{}' is a child of Location Volume {}, which is editor-only and is "
                         "not cooked; detach it from the volume",
                         record, descriptor.Tag, scene.SceneName, *descriptor.Parent ) );
                if ( write != record ) // a self-move would leave the record unspecified
                    kept.Entities[write] = std::move( kept.Entities[record] );
                ++write;
                keptDescriptors.push_back( descriptor );
            }
            kept.Entities.erase( kept.Entities.begin() + static_cast<std::ptrdiff_t>( write ),
                                 kept.Entities.end() );
            return CookPlanned( kept, keptDescriptors, registries, hlods );
        }
        if ( !scene.WorldPartition.has_value() )
            return Common::MakeError<Result>( "'" + scene.SceneName +
                                              "' states no WorldPartition block, so it has no cells to cook" );
        if ( auto layers = Rules::ValidateHLODLayers( *scene.WorldPartition ); !layers )
            return Common::MakeError<Result>( "'" + scene.SceneName + "': " + layers.GetError() );
        const bool anyHLODLayer = Rules::AnyGridHLODLayer( *scene.WorldPartition );

        const auto&                                   records = scene.Entities;
        std::unordered_map<Common::UUID, std::size_t> byId;
        for ( std::size_t record = 0; record < records.size(); ++record )
        {
            const std::string what = "record " + std::to_string( record ) + " ('" +
                                     records[record].Tag.value_or( "" ) + "') of '" + scene.SceneName + "'";
            if ( !records[record].id.has_value() || records[record].id->IsNull() )
                return Common::MakeError<Result>( what + " has no id; a cell's records are found by id" );
            const auto [where, fresh] = byId.emplace( *records[record].id, record );
            if ( !fresh )
                return Common::MakeError<Result>(
                     what + " has id " + std::to_string( static_cast<std::uint64_t>( *records[record].id ) ) +
                     ", which record " + std::to_string( where->second ) + " already has" );
        }

        // The plan is the descriptors': they must be the records', one for one, or the cells would hold
        // records placed by somebody else's descriptor.
        if ( descriptors.size() != records.size() )
            return Common::MakeError<Result>(
                 std::format( "'{}' has {} records and {} descriptors; the descriptor index is not this world's",
                              scene.SceneName, records.size(), descriptors.size() ) );
        for ( std::size_t record = 0; record < records.size(); ++record )
            if ( descriptors[record].Id != static_cast<std::uint64_t>( *records[record].id ) )
                return Common::MakeError<Result>(
                     std::format( "record {} of '{}' is entity {} but its descriptor is another; the descriptor "
                                  "index is stale",
                                  record, scene.SceneName, static_cast<std::uint64_t>( *records[record].id ) ) );
        const Rules::ScriptReferenceSource scripts = ScriptReferenceKinds();
        const Rules::WorldPartitionPlan    plan =
             Rules::PlanWorldPartition( descriptors, *scene.WorldPartition, BoundsFrom( registries ), scripts );
        // What the planner refused is named by the one plan report Play and editor regions use too. Values of
        // the wrong type the reference walk and the HLOD build below find are the cook's own, named once at the
        // end.
        Rules::ReportPlanIssues( plan, scene.SceneName );
        Common::Json::Issues issues;
        const std::size_t unitCount = Rules::ResidencyUnitCount( plan );

        CookedWorld cooked;
        WorldIndex& index       = cooked.Index;
        index.SceneName         = scene.SceneName;
        index.Settings          = scene.Settings;
        index.Header            = scene.Header;
        index.WorldPartition    = *scene.WorldPartition;
        index.LevelCount        = plan.LevelCount;
        index.Records           = records.size();
        index.AssetClosureKnown = !registries.empty();

        AssetClosure                          closure( registries );
        std::vector<std::size_t>              unitOf( records.size(), Rules::kNoRecord );
        std::vector<std::vector<std::size_t>> members( unitCount );
        for ( std::size_t unit = 0; unit < unitCount; ++unit )
        {
            members[unit] = Rules::ResidencyUnitMembers( plan, unit );
            IndexUnit row;
            row.Name = Rules::DescribeResidencyUnit( plan, unit );
            if ( unit < plan.AlwaysLoaded.size() )
            {
                // UE's always-loaded cell of a data layer set: a composite of runtime layers is read only while its
                // layers want it, so it is not in the file the game makes at its start.
                row.DataLayers = Rules::UnitRuntimeDataLayers( plan, *scene.WorldPartition, unit );
                row.File       = row.DataLayers.empty()
                                      ? std::string( kAlwaysLoadedFileName )
                                      : "AlwaysLoaded" + Rules::DataLayersCellSuffix( *scene.WorldPartition, row.DataLayers ) +
                                             std::string( kCellExtension );
                row.Reason = ReasonName( plan.Composites.at( plan.AlwaysLoaded[unit] ).Reason );
            }
            else
            {
                const Rules::PlannedCell& cell = plan.Cells.at( unit - plan.AlwaysLoaded.size() );
                row.File                       = CellFileName( *scene.WorldPartition, cell );
                row.DataLayers                 = cell.DataLayers;
                row.Grid                       = cell.Grid;
                row.Level                      = cell.Level;
                row.X                          = cell.Cell.X;
                row.Z                          = cell.Cell.Z;
                row.Square                     = cell.Square;
                row.Bounds                     = cell.Bounds;
                row.Height                     = cell.Height;
            }
            std::set<std::string> keys;
            for ( const std::size_t record : members[unit] )
            {
                unitOf.at( record ) = unit;
                row.Ids.push_back( static_cast<std::uint64_t>( *records[record].id ) );
                closure.Record( records[record], keys );
            }
            closure.Expand( keys );
            row.Assets.assign( keys.begin(), keys.end() );
            index.Units.push_back( std::move( row ) );
        }
        for ( std::size_t record = 0; record < records.size(); ++record )
            if ( unitOf[record] == Rules::kNoRecord )
                return Common::MakeError<Result>( "record " + std::to_string( record ) + " ('" +
                                                  records[record].Tag.value_or( "" ) + "') of '" +
                                                  scene.SceneName + "' is in no unit of the plan" );

        // The references that cross a unit, resolved as the planner resolved them (the register, the scripts'
        // Entity properties, names: Rules::ResolveEntityReferences), so this list and the plan's composites agree.
        // What the resolver could not resolve is already in plan.Issues, reported above.
        Common::Json::Issues reportedWithThePlan;
        for ( const Rules::ResolvedReference& reference :
              Rules::ResolveEntityReferences( descriptors, scripts, reportedWithThePlan ) )
        {
            const std::size_t record = reference.From;
            if ( reference.To == Rules::kNoRecord || unitOf[reference.To] == unitOf[record] )
                continue;
            if ( reference.Kind == Rules::ReferenceKind::Containment )
                return Common::MakeError<Result>( std::format(
                     "record {} ('{}') of '{}' is held by {}.{} in another unit; containment never crosses one",
                     record, records[record].Tag.value_or( "" ), scene.SceneName, reference.Component,
                     reference.Field ) );
            index.References.push_back( { static_cast<std::uint64_t>( *records[record].id ),
                                          static_cast<std::uint64_t>( *records[reference.To].id ),
                                          static_cast<std::uint32_t>( unitOf[record] ),
                                          static_cast<std::uint32_t>( unitOf[reference.To] ), reference.Component,
                                          reference.Field } );
        }

        // One file per distinct name, in the order the units first name them: the always-loaded file (if any)
        // holds every always-loaded unit, a cell file holds its one cell.
        std::vector<std::string> fileOrder;
        for ( const IndexUnit& row : index.Units )
            if ( std::find( fileOrder.begin(), fileOrder.end(), row.File ) == fileOrder.end() )
                fileOrder.push_back( row.File );
        for ( const std::string& name : fileOrder )
        {
            CellPayload payload;
            payload.World = scene.SceneName;
            for ( std::size_t unit = 0; unit < unitCount; ++unit )
            {
                if ( index.Units[unit].File != name )
                    continue;
                payload.Units.push_back( index.Units[unit].Name );
                for ( const std::size_t record : members[unit] )
                    payload.Records.push_back( records[record] );
            }
            auto wrapped =
                 Wrap( CC::ContentKind::WorldCell, scene.SceneName, name, Common::Json::Write( payload ) );
            if ( !wrapped )
                return Common::MakeError<CookedWorld>( wrapped.GetError() );
            CookedFile file{ name, wrapped.ExtractValue() };
            index.Files.push_back(
                 { name, file.Bytes.size(), Common::Utils::Crc32c( file.Bytes.data(), file.Bytes.size() ) } );
            cooked.Files.push_back( std::move( file ) );
        }
        // THE HLOD OF EVERY CELL, BY ITS GRID'S LAYER (WP10, WP-FAR-7, WP22c), after the cells so a cell's file
        // is written before the one that stands in for it and the index lists them in that order. No grid states a
        // layer: no cell has an HLOD; a cell of a grid that states none has a row without a file
        // (BuildWorldHLODs). The saved HLODs, checked against this world: a missing or stale cell refuses the cook
        // by name.
        if ( anyHLODLayer && hlods == nullptr )
            return Common::MakeError<Result>(
                 "'" + scene.SceneName +
                 "' has a grid with an HLOD layer and no built HLODs; run Build > Build HLODs" );
        std::unordered_map<std::string, const BuiltHLODCell*> builtRows;
        if ( anyHLODLayer )
        {
            for ( const BuiltHLODCell& row : hlods->Index.Cells )
                builtRows.emplace( row.Unit, &row );
            auto stale = StaleHLODCells( hlods->Index, scene, descriptors, registries );
            if ( !stale )
                return Common::MakeError<Result>( stale.GetError() );
            if ( !stale.GetValue().empty() )
            {
                std::string names;
                for ( const std::string& unit : stale.GetValue() )
                    names += ( names.empty() ? "" : ", " ) + unit;
                return Common::MakeError<Result>( std::format(
                     "'{}': the saved HLOD of {} cell(s) is missing or stale ({}); run Build > Build HLODs",
                     scene.SceneName, stale.GetValue().size(), names ) );
            }
        }
        std::set<std::uint64_t> hlodIds;
        // THE HIERARCHY (WP-FAR-8): when a layer is spatially loaded its HLODs are packed into HLOD cells (below)
        // and no HLOD is resident per world cell.
        Rules::HLODHierarchy hierarchy;
        if ( anyHLODLayer )
        {
            auto planned = Rules::PlanHLODHierarchy( *scene.WorldPartition, plan );
            if ( !planned )
                return Common::MakeError<Result>( "'" + scene.SceneName + "': " + planned.GetError() );
            hierarchy = planned.ExtractValue();
        }
        for ( std::size_t unit = plan.AlwaysLoaded.size();
              anyHLODLayer && !hierarchy.Streams() && unit < unitCount; ++unit )
        {
            const BuiltHLODCell& built = *builtRows.at( Rules::HLODRowName( *scene.WorldPartition, plan, unit ) );
            if ( !built.File.has_value() && built.NotInstanced.empty() )
                continue;
            IndexHLOD row;
            row.Unit      = static_cast<std::uint32_t>( unit );
            row.Instances = built.Instances;
            row.NotInstanced = built.NotInstanced;
            if ( built.File.has_value() )
            {
                const std::string name = HLODFileName( index.Units[unit].File );
                CellPayload       payload;
                payload.World = scene.SceneName;
                payload.Units.push_back( "HLOD " + index.Units[unit].Name );
                auto stored = StoredCellRecords( *hlods, built );
                if ( !stored )
                    return Common::MakeError<Result>( "'" + scene.SceneName + "', " + index.Units[unit].Name +
                                                      ": " + stored.GetError() );
                std::vector<Assets::EntityData> hlodRecords = stored.ExtractValue();
                std::set<std::string>           keys;
                for ( Assets::EntityData& hlodRecord : hlodRecords )
                {
                    const Common::UUID id = *hlodRecord.id; // StoredCellRecords checked it against the row
                    if ( id.IsNull() || byId.contains( id ) ||
                         !hlodIds.insert( static_cast<std::uint64_t>( id ) ).second )
                        return Common::MakeError<Result>(
                             std::format( "the HLOD id {} in '{}' is null or already taken; rename the world",
                                          static_cast<std::uint64_t>( id ), name ) );
                    closure.Record( hlodRecord, keys );
                    row.Ids.push_back( static_cast<std::uint64_t>( id ) );
                    payload.Records.push_back( std::move( hlodRecord ) );
                }
                row.Sources           = built.Sources;
                row.ImpostorCards     = built.ImpostorCards;
                row.ImpostorFallbacks = built.ImpostorFallbacks;
                closure.Expand( keys );
                row.Assets.assign( keys.begin(), keys.end() );
                auto wrapped =
                     Wrap( CC::ContentKind::WorldCell, scene.SceneName, name, Common::Json::Write( payload ) );
                if ( !wrapped )
                    return Common::MakeError<CookedWorld>( wrapped.GetError() );
                CookedFile file{ name, wrapped.ExtractValue() };
                index.Files.push_back(
                     { name, file.Bytes.size(), Common::Utils::Crc32c( file.Bytes.data(), file.Bytes.size() ) } );
                cooked.Files.push_back( std::move( file ) );
                row.File = name;
            }
            index.HLODs.push_back( std::move( row ) );
        }
        // THE HLOD CELLS (WP-FAR-8; UE UHLODLayer::bIsSpatiallyLoaded: a layer's HLOD actors are cooked into the
        // runtime cells of its own grid). Every HLOD residency unit gets a file holding its nodes' saved records
        // in node order and a row as a world cell gets one; every node a row naming its unit.
        if ( hierarchy.Streams() )
        {
            std::vector<const BuiltHLODCell*> nodeRows;
            std::vector<std::size_t>          counts;
            for ( const Rules::HLODNode& node : hierarchy.Nodes )
            {
                const auto row = builtRows.find( node.Name );
                if ( row == builtRows.end() )
                    return Common::MakeError<Result>(
                         std::format( "'{}': the HLOD {} has no saved row; run Build > Build HLODs",
                                      scene.SceneName, node.Name ) );
                nodeRows.push_back( row->second );
                counts.push_back( row->second->File.has_value() ? row->second->Ids.size() : 0 );
            }
            auto residency = Rules::MakeHLODResidency( *scene.WorldPartition, hierarchy, counts );
            if ( !residency )
                return Common::MakeError<Result>( "'" + scene.SceneName + "': " + residency.GetError() );
            const Rules::HLODResidency& hlodResidency = residency.GetValue();
            index.HLODNodes.resize( hierarchy.Nodes.size() );
            const std::size_t hlodUnits = Rules::ResidencyUnitCount( hlodResidency.Plan );
            for ( std::size_t unit = 0; unit < hlodUnits; ++unit )
            {
                IndexUnit                row;
                std::vector<std::size_t> unitNodes;
                row.Name = Rules::DescribeHLODUnit( hierarchy, hlodResidency, unit );
                if ( unit < hlodResidency.Plan.AlwaysLoaded.size() )
                {
                    const std::size_t top = hlodResidency.Plan.AlwaysLoaded[unit];
                    unitNodes.push_back( top );
                    row.File   = HLODNodeFileName( *scene.WorldPartition, hierarchy, plan, hierarchy.Nodes[top] );
                    row.Reason = std::string( kHLODResidentReason );
                }
                else
                {
                    // Composite n of the HLOD residency is node n (MakeHLODResidency).
                    const Rules::PlannedCell& cell =
                         hlodResidency.Plan.Cells.at( unit - hlodResidency.Plan.AlwaysLoaded.size() );
                    unitNodes  = cell.Composites;
                    row.File   = HLODCellFileName( *scene.WorldPartition, cell );
                    row.Grid   = cell.Grid;
                    row.Level  = cell.Level;
                    row.X      = cell.Cell.X;
                    row.Z      = cell.Cell.Z;
                    row.Square = cell.Square;
                    row.Bounds = cell.Bounds;
                    row.Height = cell.Height;
                }
                CellPayload payload;
                payload.World = scene.SceneName;
                payload.Units.push_back( row.Name );
                std::set<std::string> keys;
                for ( const std::size_t n : unitNodes )
                {
                    const BuiltHLODCell& built = *nodeRows.at( n );
                    IndexHLODNode&       node  = index.HLODNodes.at( n );
                    node.Name                  = hierarchy.Nodes[n].Name;
                    node.Unit                  = static_cast<std::uint32_t>( unit );
                    node.Instances             = built.Instances;
                    node.NotInstanced          = built.NotInstanced;
                    if ( !built.File.has_value() )
                        continue;
                    auto stored = StoredCellRecords( *hlods, built );
                    if ( !stored )
                        return Common::MakeError<Result>( "'" + scene.SceneName + "', " + node.Name + ": " +
                                                          stored.GetError() );
                    std::vector<Assets::EntityData> hlodRecords = stored.ExtractValue();
                    for ( Assets::EntityData& hlodRecord : hlodRecords )
                    {
                        const Common::UUID id = *hlodRecord.id; // StoredCellRecords checked it against the row
                        if ( id.IsNull() || byId.contains( id ) ||
                             !hlodIds.insert( static_cast<std::uint64_t>( id ) ).second )
                            return Common::MakeError<Result>(
                                 std::format( "the HLOD id {} in '{}' is null or already taken; rename the world",
                                              static_cast<std::uint64_t>( id ), row.File ) );
                        closure.Record( hlodRecord, keys );
                        row.Ids.push_back( static_cast<std::uint64_t>( id ) );
                        node.Ids.push_back( static_cast<std::uint64_t>( id ) );
                        payload.Records.push_back( std::move( hlodRecord ) );
                    }
                    node.Sources           = built.Sources;
                    node.ImpostorCards     = built.ImpostorCards;
                    node.ImpostorFallbacks = built.ImpostorFallbacks;
                }
                closure.Expand( keys );
                row.Assets.assign( keys.begin(), keys.end() );
                auto wrapped =
                     Wrap( CC::ContentKind::WorldCell, scene.SceneName, row.File, Common::Json::Write( payload ) );
                if ( !wrapped )
                    return Common::MakeError<Result>( wrapped.GetError() );
                CookedFile file{ row.File, wrapped.ExtractValue() };
                index.Files.push_back( { row.File, file.Bytes.size(),
                                         Common::Utils::Crc32c( file.Bytes.data(), file.Bytes.size() ) } );
                cooked.Files.push_back( std::move( file ) );
                index.HLODUnits.push_back( std::move( row ) );
            }
        }
        Common::Json::ReportIssues( issues, scene.SceneName );

        auto wrappedIndex =
             Wrap( CC::ContentKind::WorldIndex, scene.SceneName, kIndexFileName, Common::Json::Write( index ) );
        if ( !wrappedIndex )
            return Common::MakeError<CookedWorld>( wrappedIndex.GetError() );
        cooked.Files.push_back( { std::string( kIndexFileName ), wrappedIndex.ExtractValue() } );
        return Common::MakeSuccess( std::move( cooked ) );
    }

    Common::ResultStr<WorldIndex> ReadWorldIndex( std::string_view fileName, std::span<const unsigned char> bytes )
    {
        const std::string name( fileName );
        auto              payload = Unwrap( fileName, CC::ContentKind::WorldIndex, bytes );
        if ( !payload )
            return Common::MakeError<WorldIndex>( payload.GetError() );
        auto parsed = Common::Json::Read<WorldIndex>( payload.GetValue() );
        if ( !parsed )
            return Common::MakeError<WorldIndex>( "'" + name +
                                                  "' is not a readable world index: " + parsed.GetError() );
        WorldIndex index = parsed.ExtractValue();

        // The index is the one thing a streamer decides with, so it is checked whole before it is used.
        std::uint64_t held = 0;
        for ( const IndexUnit& unit : index.Units )
        {
            held += unit.Ids.size();
            const bool listed = std::any_of( index.Files.begin(), index.Files.end(),
                                             [&]( const IndexFile& file ) { return file.Name == unit.File; } );
            if ( !listed )
                return Common::MakeError<WorldIndex>( "'" + name + "': unit " + unit.Name + " names file '" +
                                                      unit.File + "', which the index does not list" );
        }
        if ( held != index.Records )
            return Common::MakeError<WorldIndex>( "'" + name + "' states " + std::to_string( index.Records ) +
                                                  " record(s) and its units hold " + std::to_string( held ) );
        for ( const IndexReference& reference : index.References )
            if ( reference.FromUnit >= index.Units.size() || reference.ToUnit >= index.Units.size() )
                return Common::MakeError<WorldIndex>(
                     "'" + name + "': a reference names unit " +
                     std::to_string( std::max( reference.FromUnit, reference.ToUnit ) ) + " of " +
                     std::to_string( index.Units.size() ) );
        for ( const IndexHLOD& hlod : index.HLODs )
        {
            if ( hlod.Unit >= index.Units.size() || !index.Units[hlod.Unit].Level.has_value() )
                return Common::MakeError<WorldIndex>( "'" + name + "': an HLOD stands in for unit " +
                                                      std::to_string( hlod.Unit ) + ", which is not a cell of " +
                                                      std::to_string( index.Units.size() ) + " unit(s)" );
            const std::string& unitName = index.Units[hlod.Unit].Name;
            if ( hlod.File.has_value() == hlod.Ids.empty() )
                return Common::MakeError<WorldIndex>( "'" + name + "': the HLOD of " + unitName + " has " +
                                                      std::to_string( hlod.Ids.size() ) + " record(s) and " +
                                                      ( hlod.File ? "a" : "no" ) + " file" );
            const bool listed = !hlod.File.has_value() ||
                                std::any_of( index.Files.begin(), index.Files.end(),
                                             [&]( const IndexFile& file ) { return file.Name == *hlod.File; } );
            if ( !listed )
                return Common::MakeError<WorldIndex>( "'" + name + "': the HLOD of " + unitName + " names file '" +
                                                      *hlod.File + "', which the index does not list" );
        }
        // The HLOD cells (WP-FAR-8): never beside session-resident HLODs; every node in a unit; every unit's file
        // listed and its ids as many as its nodes'.
        if ( !index.HLODUnits.empty() && !index.HLODs.empty() )
            return Common::MakeError<WorldIndex>( "'" + name +
                                                  "' states both session-resident HLODs and HLOD cells" );
        std::vector<std::size_t> nodeRecords( index.HLODUnits.size(), 0 );
        for ( const IndexHLODNode& node : index.HLODNodes )
        {
            if ( node.Unit >= index.HLODUnits.size() )
                return Common::MakeError<WorldIndex>( std::format( "'{}': the HLOD {} is in unit {} of {}", name,
                                                                   node.Name, node.Unit,
                                                                   index.HLODUnits.size() ) );
            nodeRecords[node.Unit] += node.Ids.size();
        }
        for ( std::size_t unit = 0; unit < index.HLODUnits.size(); ++unit )
        {
            const IndexUnit& row    = index.HLODUnits[unit];
            const bool       listed = std::any_of( index.Files.begin(), index.Files.end(),
                                                   [&]( const IndexFile& file ) { return file.Name == row.File; } );
            if ( !listed )
                return Common::MakeError<WorldIndex>(
                     std::format( "'{}': HLOD unit {} names file '{}', which the index does not list", name,
                                  row.Name, row.File ) );
            if ( nodeRecords[unit] != row.Ids.size() )
                return Common::MakeError<WorldIndex>(
                     std::format( "'{}': HLOD unit {} lists {} record(s) and its HLODs hold {}", name, row.Name,
                                  row.Ids.size(), nodeRecords[unit] ) );
        }
        return Common::MakeSuccess( std::move( index ) );
    }

    std::string HLODFileName( std::string_view cellFile )
    {
        return "HLOD_" + std::string( cellFile );
    }

    Common::ResultStr<CellPayload> ReadCellFile( const WorldIndex& index, std::string_view fileName,
                                                 std::span<const unsigned char> bytes )
    {
        const std::string name( fileName );
        auto              payload = Unwrap( fileName, CC::ContentKind::WorldCell, bytes );
        if ( !payload )
            return Common::MakeError<CellPayload>( payload.GetError() );

        // Intact on its own is not enough: it must be the file THIS index was cooked with.
        const auto listed = std::find_if( index.Files.begin(), index.Files.end(),
                                          [&]( const IndexFile& file ) { return file.Name == name; } );
        if ( listed == index.Files.end() )
            return Common::MakeError<CellPayload>( "'" + name + "' is not a file of world '" + index.SceneName +
                                                   "'s index" );
        const std::uint32_t crc = Common::Utils::Crc32c( bytes.data(), bytes.size() );
        if ( listed->Bytes != bytes.size() || listed->Crc != crc )
        {
            char text[160];
            std::snprintf( text, sizeof( text ), "%llu bytes, checksum %08x; the index lists %llu bytes, %08x",
                           static_cast<unsigned long long>( bytes.size() ), crc,
                           static_cast<unsigned long long>( listed->Bytes ), listed->Crc );
            return Common::MakeError<CellPayload>( "'" + name + "' is not the file world '" + index.SceneName +
                                                   "' was cooked with (" + text + "): a stale cook. Re-cook it." );
        }

        auto parsed = Common::Json::Read<CellPayload>( payload.GetValue() );
        if ( !parsed )
            return Common::MakeError<CellPayload>( "'" + name +
                                                   "' is not a readable cell file: " + parsed.GetError() );
        CellPayload cell = parsed.ExtractValue();

        std::vector<std::string>   units;
        std::vector<std::uint64_t> ids;
        for ( const IndexUnit& unit : index.Units )
            if ( unit.File == name )
            {
                units.push_back( unit.Name );
                ids.insert( ids.end(), unit.Ids.begin(), unit.Ids.end() );
            }
        for ( const IndexHLOD& hlod : index.HLODs )
            if ( hlod.File == name )
            {
                units.push_back( "HLOD " + index.Units.at( hlod.Unit ).Name );
                ids.insert( ids.end(), hlod.Ids.begin(), hlod.Ids.end() );
            }
        for ( const IndexUnit& unit : index.HLODUnits )
            if ( unit.File == name )
            {
                units.push_back( unit.Name );
                ids.insert( ids.end(), unit.Ids.begin(), unit.Ids.end() );
            }
        if ( cell.Units != units )
            return Common::MakeError<CellPayload>( "'" + name + "' holds " + std::to_string( cell.Units.size() ) +
                                                   " unit(s) and the index gives it " +
                                                   std::to_string( units.size() ) + " others" );
        if ( cell.Records.size() != ids.size() )
            return Common::MakeError<CellPayload>(
                 "'" + name + "' holds " + std::to_string( cell.Records.size() ) +
                 " record(s) and the index lists " + std::to_string( ids.size() ) );
        for ( std::size_t record = 0; record < ids.size(); ++record )
        {
            const auto& id = cell.Records[record].id;
            if ( !id.has_value() || static_cast<std::uint64_t>( *id ) != ids[record] )
                return Common::MakeError<CellPayload>( "'" + name + "': record " + std::to_string( record ) +
                                                       " is not the id " + std::to_string( ids[record] ) +
                                                       " the index lists there" );
        }
        return Common::MakeSuccess( std::move( cell ) );
    }

    CookedCellSource::CookedCellSource( const WorldIndex& index, FileReader reader, const UnitSet set )
         : m_Index( &index ), m_Reader( std::move( reader ) ), m_Set( set )
    {
    }

    const std::vector<IndexUnit>& UnitsOf( const WorldIndex& index, const UnitSet set )
    {
        return set == UnitSet::HLOD ? index.HLODUnits : index.Units;
    }

    namespace
    {
        // Unit @p unit of @p units' records out of its file's payload: a run of them, after the units the file
        // lists before it (a file holds units of one set only).
        std::vector<Assets::EntityData> UnitSlice( const std::vector<IndexUnit>& units, std::size_t unit,
                                                   const CellPayload& cell )
        {
            const IndexUnit& wanted = units[unit];
            std::size_t      offset = 0;
            for ( std::size_t other = 0; other < unit; ++other )
                if ( units[other].File == wanted.File )
                    offset += units[other].Ids.size();
            return { cell.Records.begin() + static_cast<std::ptrdiff_t>( offset ),
                     cell.Records.begin() + static_cast<std::ptrdiff_t>( offset + wanted.Ids.size() ) };
        }

        Common::ResultStr<CellPayload> ReadUnitFile( const WorldIndex& index, const FileReader& reader,
                                                     const std::string& file )
        {
            auto bytes = reader( file );
            if ( !bytes )
                return Common::MakeError<CellPayload>( bytes.GetError() );
            return ReadCellFile( index, file, bytes.GetValue() );
        }

        // The scene-wide part of the index and the records of every unit @p takes, each file read once.
        Common::ResultStr<SceneSerialized> Assemble( const WorldIndex& index, const FileReader& reader,
                                                     const std::function<bool( std::size_t )>& takes )
        {
            SceneSerialized scene;
            scene.SceneName      = index.SceneName;
            scene.Settings       = index.Settings;
            scene.Header         = index.Header;
            scene.WorldPartition = index.WorldPartition;

            std::map<std::string, CellPayload> read;
            for ( std::size_t unit = 0; unit < index.Units.size(); ++unit )
            {
                if ( !takes( unit ) )
                    continue;
                const std::string& file = index.Units[unit].File;
                auto               held = read.find( file );
                if ( held == read.end() )
                {
                    auto cell = ReadUnitFile( index, reader, file );
                    if ( !cell )
                        return Common::MakeError<SceneSerialized>( cell.GetError() );
                    held = read.emplace( file, cell.ExtractValue() ).first;
                }
                for ( auto& record : UnitSlice( index.Units, unit, held->second ) )
                    scene.Entities.push_back( std::move( record ) );
            }
            return Common::MakeSuccess( std::move( scene ) );
        }

        std::optional<Rules::AlwaysLoadedReason> ReasonNamed( std::string_view name )
        {
            for ( const auto reason : { Rules::AlwaysLoadedReason::Author, Rules::AlwaysLoadedReason::Component,
                                        Rules::AlwaysLoadedReason::NoFit, Rules::AlwaysLoadedReason::NoGrid,
                                        Rules::AlwaysLoadedReason::Persistent } )
                if ( ReasonName( reason ) == name )
                    return reason;
            return std::nullopt;
        }
    } // namespace

    Common::ResultStr<std::vector<Assets::EntityData>> CookedCellSource::UnitRecords( std::size_t unit ) const
    {
        using Result                        = std::vector<Assets::EntityData>;
        const std::vector<IndexUnit>& units = UnitsOf( *m_Index, m_Set );
        if ( unit >= units.size() )
            return Common::MakeError<Result>(
                 std::format( "world '{}' has no {}unit {} (it has {})", m_Index->SceneName,
                              m_Set == UnitSet::HLOD ? "HLOD " : "", unit, units.size() ) );
        auto cell = ReadUnitFile( *m_Index, m_Reader, units[unit].File );
        if ( !cell )
            return Common::MakeError<Result>( cell.GetError() );
        return Common::MakeSuccess( UnitSlice( units, unit, cell.GetValue() ) );
    }

    Common::ResultStr<SceneSerialized> AssembleWorld( const WorldIndex& index, const FileReader& reader )
    {
        return Assemble( index, reader, []( std::size_t ) { return true; } );
    }

    Common::ResultStr<std::vector<Assets::EntityData>> HLODRecords( const WorldIndex& index,
                                                                    const FileReader& reader, std::size_t hlod )
    {
        using Result = std::vector<Assets::EntityData>;
        if ( hlod >= index.HLODs.size() )
            return Common::MakeError<Result>( "world '" + index.SceneName + "' has no HLOD " +
                                              std::to_string( hlod ) + " (it has " +
                                              std::to_string( index.HLODs.size() ) + ")" );
        if ( !index.HLODs[hlod].File.has_value() )
            return Common::MakeSuccess( Result{} );
        auto cell = ReadUnitFile( index, reader, *index.HLODs[hlod].File );
        if ( !cell )
            return Common::MakeError<Result>( cell.GetError() );
        return Common::MakeSuccess( cell.ExtractValue().Records );
    }

    Common::ResultStr<SceneSerialized> AssembleAlwaysLoaded( const WorldIndex& index, const FileReader& reader )
    {
        std::size_t alwaysLoaded = 0;
        while ( alwaysLoaded < index.Units.size() && index.Units[alwaysLoaded].Reason.has_value() )
            ++alwaysLoaded;
        // Not an always-loaded composite of runtime data layers: it streams by its layers' state (its own file).
        return Assemble( index, reader, [&index, alwaysLoaded]( std::size_t unit )
                         { return unit < alwaysLoaded && index.Units[unit].DataLayers.empty(); } );
    }

    Common::ResultStr<IndexedWorld> PlanFromIndex( const WorldIndex& index )
    {
        using Result = IndexedWorld;
        IndexedWorld world;
        world.Plan.LevelCount = index.LevelCount;
        std::unordered_map<std::uint64_t, std::pair<std::size_t, std::size_t>> recordOf; // id -> (record, unit)
        for ( std::size_t unit = 0; unit < index.Units.size(); ++unit )
        {
            const IndexUnit&  from = index.Units[unit];
            const std::string name =
                 "world '" + index.SceneName + "' unit " + std::to_string( unit ) + " ('" + from.Name + "')";
            Rules::PlannedComposite composite;
            for ( const std::uint64_t id : from.Ids )
            {
                const std::size_t record = world.RecordIds.size();
                if ( !recordOf.emplace( id, std::make_pair( record, unit ) ).second )
                    return Common::MakeError<Result>( name + " holds record id " + std::to_string( id ) +
                                                      ", which an earlier unit holds too" );
                world.RecordIds.push_back( id );
                composite.Members.push_back( record );
            }
            composite.Anchor          = composite.Members.empty() ? Rules::kNoRecord : composite.Members.front();
            const std::size_t placeAt = world.Plan.Composites.size();
            if ( from.Reason.has_value() )
            {
                if ( !world.Plan.Cells.empty() )
                    return Common::MakeError<Result>( name + " is always-loaded and comes after a cell; the "
                                                             "index lists every always-loaded unit first" );
                const auto reason = ReasonNamed( *from.Reason );
                if ( !reason.has_value() )
                    return Common::MakeError<Result>( name + " states the always-loaded reason '" + *from.Reason +
                                                      "', which is none this build knows" );
                composite.Reason     = *reason;
                composite.DataLayers = from.DataLayers; // its runtime layers: all the cooked plan needs of them
                world.Plan.AlwaysLoaded.push_back( placeAt );
                if ( from.DataLayers.empty() )
                    world.AlwaysLoadedRecords += from.Ids.size();
            }
            else
            {
                if ( !from.Grid.has_value() || !from.Level.has_value() || !from.X.has_value() ||
                     !from.Z.has_value() || !from.Square.has_value() || !from.Bounds.has_value() ||
                     !from.Height.has_value() )
                    return Common::MakeError<Result>(
                         name + " is a cell without its grid, level, coordinate, square, bounds or height span" );
                if ( Rules::CellGridOf( index.WorldPartition, *from.Grid ) == nullptr )
                    return Common::MakeError<Result>( std::format(
                         "{} is a cell of runtime partition {}, but the world states {} partition(s) and only an "
                         "LHGrid one makes cells",
                         name, *from.Grid, index.WorldPartition.RuntimePartitions.size() ) );
                Rules::PlannedCell cell;
                cell.Grid   = *from.Grid;
                cell.Level  = *from.Level;
                cell.Cell   = Rules::CellCoord{ *from.X, *from.Z };
                cell.Square = *from.Square;
                cell.Bounds = *from.Bounds;
                cell.Height     = *from.Height;
                cell.DataLayers = from.DataLayers;
                for ( const std::size_t layer : cell.DataLayers )
                    if ( !index.WorldPartition.DataLayers.has_value() || layer >= index.WorldPartition.DataLayers->size() ||
                         ( *index.WorldPartition.DataLayers )[layer].Type != DataLayerType::Runtime )
                        return Common::MakeError<Result>( std::format( "{} names data layer {}, which is no runtime "
                                                                       "layer of the world",
                                                                       name, layer ) );
                cell.Composites.push_back( placeAt );
                if ( !world.Plan.Cells.empty() )
                {
                    const Rules::PlannedCell& last = world.Plan.Cells.back();
                    if ( std::tie( last.Grid, last.Level, last.Cell.X, last.Cell.Z, last.DataLayers ) >=
                         std::tie( cell.Grid, cell.Level, cell.Cell.X, cell.Cell.Z, cell.DataLayers ) )
                        return Common::MakeError<Result>( name + " is out of (grid, level, X, Z, data layers) order after '" +
                                                          index.Units[unit - 1].Name + "'" );
                }
                composite.Grid      = cell.Grid;
                composite.Level     = cell.Level;
                composite.Cell      = cell.Cell;
                composite.Footprint = cell.Bounds;
                composite.Height    = cell.Height;
                composite.DataLayers = cell.DataLayers;
                world.Plan.MaxLevel = std::max( world.Plan.MaxLevel, cell.Level );
                world.Plan.Cells.push_back( std::move( cell ) );
            }
            world.Plan.Composites.push_back( std::move( composite ) );
        }
        for ( const IndexReference& reference : index.References )
        {
            const auto from = recordOf.find( reference.From );
            const auto to   = recordOf.find( reference.To );
            if ( from == recordOf.end() || to == recordOf.end() )
                return Common::MakeError<Result>( "world '" + index.SceneName + "': the reference " +
                                                  reference.Component + "." + reference.Field + " from " +
                                                  std::to_string( reference.From ) + " to " +
                                                  std::to_string( reference.To ) + " names an id no unit holds" );
            // Every listed reference is an observation: only observation can cross a unit (the cook refuses a
            // crossing containment, whatever its origin - register, script or name).
            world.Observations.emplace_back( from->second.first, to->second.first );
        }
        world.Plan.Observations = world.Observations;
        return Common::MakeSuccess( std::move( world ) );
    }

    Common::ResultStr<std::optional<IndexedHLODs>> PlanHLODsFromIndex( const WorldIndex&                index,
                                                                       const Rules::WorldPartitionPlan& plan )
    {
        using Result       = std::optional<IndexedHLODs>;
        const auto refused = [&index]( const std::string& why )
        { return Common::MakeError<Result>( "world '" + index.SceneName + "': " + why + "; re-cook it" ); };
        const bool anySpatialLayer =
             index.WorldPartition.HLODLayers.has_value() &&
             std::any_of( index.WorldPartition.HLODLayers->begin(), index.WorldPartition.HLODLayers->end(),
                          []( const HLODLayerSerialized& layer ) { return layer.CellSize.has_value(); } );
        if ( index.HLODUnits.empty() && index.HLODNodes.empty() && !anySpatialLayer )
            return Common::MakeSuccess( Result{} );
        auto hierarchy = Rules::PlanHLODHierarchy( index.WorldPartition, plan );
        if ( !hierarchy )
            return refused( hierarchy.GetError() );
        IndexedHLODs out;
        out.Hierarchy                             = hierarchy.ExtractValue();
        const std::vector<Rules::HLODNode>& nodes = out.Hierarchy.Nodes;
        // A world whose hierarchy streams and whose index states no HLOD cell is a cook that dropped them: its
        // game would draw no HLOD at all, so it is refused, never played without them.
        if ( index.HLODUnits.empty() && index.HLODNodes.empty() )
        {
            if ( !out.Hierarchy.Streams() )
                return Common::MakeSuccess( Result{} );
            return refused( std::format( "an HLOD layer of the world is spatially loaded ({} HLOD cell(s)) and "
                                         "the index states no HLOD unit",
                                         out.Hierarchy.Cells.size() ) );
        }
        if ( !out.Hierarchy.Streams() )
            return refused( std::format( "the index states {} HLOD unit(s) and no HLOD layer of the world is "
                                         "spatially loaded",
                                         index.HLODUnits.size() ) );
        if ( nodes.size() != index.HLODNodes.size() )
            return refused( std::format( "the index states {} HLOD(s) and the world's hierarchy has {}",
                                         index.HLODNodes.size(), nodes.size() ) );
        std::vector<std::size_t> counts;
        for ( std::size_t n = 0; n < nodes.size(); ++n )
        {
            if ( nodes[n].Name != index.HLODNodes[n].Name )
                return refused( std::format( "HLOD {} of the index is {} and the world's hierarchy has {} there",
                                             n, index.HLODNodes[n].Name, nodes[n].Name ) );
            counts.push_back( index.HLODNodes[n].Ids.size() );
        }
        auto residency = Rules::MakeHLODResidency( index.WorldPartition, out.Hierarchy, counts );
        if ( !residency )
            return refused( residency.GetError() );
        out.Residency                             = residency.ExtractValue();
        const Rules::WorldPartitionPlan& hlodPlan = out.Residency.Plan;
        const std::size_t                units    = Rules::ResidencyUnitCount( hlodPlan );
        if ( units != index.HLODUnits.size() )
            return refused( std::format( "the index states {} HLOD unit(s) and the hierarchy streams {}",
                                         index.HLODUnits.size(), units ) );
        for ( std::size_t unit = 0; unit < units; ++unit )
        {
            const IndexUnit&  row  = index.HLODUnits[unit];
            const std::string name = Rules::DescribeHLODUnit( out.Hierarchy, out.Residency, unit );
            if ( row.Name != name )
                return refused( std::format( "HLOD unit {} of the index is {} and the hierarchy has {} there",
                                             unit, row.Name, name ) );
            // A session-resident top and an HLOD cell can share a name (the parent standing in for HLOD cell
            // (X,Z) and that cell), so the kind and the cell are checked too: they place the file (ChunkLayoutOf).
            const bool resident = unit < hlodPlan.AlwaysLoaded.size();
            if ( row.Reason.has_value() != resident )
                return refused( std::format( "HLOD unit {} ({}) is {} in the index and {} in the hierarchy", unit,
                                             name, row.Reason.has_value() ? "session-resident" : "a cell",
                                             resident ? "session-resident" : "a cell" ) );
            if ( !resident )
            {
                const Rules::PlannedCell& cell = hlodPlan.Cells.at( unit - hlodPlan.AlwaysLoaded.size() );
                if ( row.Grid != cell.Grid || row.X != cell.Cell.X || row.Z != cell.Cell.Z ||
                     !row.Square.has_value() )
                    return refused( std::format(
                         "HLOD unit {} ({}) does not state the hierarchy's cell and square", unit, name ) );
            }
            const std::vector<std::size_t> unitNodes =
                 unit < hlodPlan.AlwaysLoaded.size()
                      ? std::vector<std::size_t>{ hlodPlan.AlwaysLoaded[unit] }
                      : hlodPlan.Cells.at( unit - hlodPlan.AlwaysLoaded.size() ).Composites;
            std::vector<std::uint64_t> ids;
            for ( const std::size_t n : unitNodes )
            {
                if ( index.HLODNodes[n].Unit != unit )
                    return refused(
                         std::format( "the HLOD {} is in unit {} of the index and in {} of the hierarchy",
                                      nodes[n].Name, index.HLODNodes[n].Unit, name ) );
                ids.insert( ids.end(), index.HLODNodes[n].Ids.begin(), index.HLODNodes[n].Ids.end() );
            }
            if ( ids != row.Ids )
                return refused(
                     std::format( "HLOD unit {} does not list its HLODs' record ids in their order", name ) );
        }
        return Common::MakeSuccess( Result( std::move( out ) ) );
    }

    std::string CookedWorldDirectory( std::string_view scenePath )
    {
        const std::size_t slash = scenePath.find_last_of( "/\\" );
        const std::size_t dot   = scenePath.find_last_of( '.' );
        const std::size_t stem =
             ( dot != std::string_view::npos && ( slash == std::string_view::npos || dot > slash ) )
                  ? dot
                  : scenePath.size();
        return std::string( scenePath.substr( 0, stem ) ) + ".dwworld/";
    }

    Common::ResultStr<std::optional<WorldChunkLayout>> ChunkLayoutOf( const WorldIndex& index,
                                                                       std::string_view  worldDirectory )
    {
        using Result = std::optional<WorldChunkLayout>;
        const auto& size = index.WorldPartition.ChunkRegionSize;
        if ( !size.has_value() )
            return Common::MakeSuccess( Result{} );
        if ( !std::isfinite( *size ) || *size <= 0.0f )
            return Common::MakeError<Result>( std::format(
                 "world '{}': ChunkRegionSize {} is not a positive finite length (cm)", index.SceneName, *size ) );

        // A chunk name is alphanumerics, '_' and '-' (ContentChunks IsNameableChunk): the directory's other
        // characters become '_', and the trailing separator of `X.dwworld/` is dropped.
        std::string stem;
        for ( const char c : worldDirectory )
            stem.push_back( ( std::isalnum( static_cast<unsigned char>( c ) ) || c == '_' || c == '-' ) ? c : '_' );
        while ( !stem.empty() && stem.back() == '_' )
            stem.pop_back();
        const std::string prefix = "World_" + stem;

        WorldChunkLayout layout;
        layout.Shared = prefix + "_Shared";
        std::set<std::string> regions;
        auto place = [&]( const std::string& file, const std::string& chunk ) -> std::optional<std::string> {
            const auto [it, inserted] = layout.FileChunk.emplace( file, chunk );
            if ( !inserted && it->second != chunk )
                return std::format( "world '{}': file '{}' is placed in chunk '{}' and in '{}'", index.SceneName,
                                    file, it->second, chunk );
            return std::nullopt;
        };

        layout.FileChunk.emplace( std::string( kIndexFileName ), layout.Shared );
        // A unit resident from the start ships Shared; a cell - a world cell or an HLOD cell (WP-FAR-8) - in the
        // region holding the centre of its square.
        for ( const std::vector<IndexUnit>* units : { &index.Units, &index.HLODUnits } )
            for ( const IndexUnit& unit : *units )
            {
                std::string chunk = layout.Shared;
                if ( !unit.Reason.has_value() )
                {
                    if ( !unit.Square.has_value() )
                        return Common::MakeError<Result>( std::format(
                             "world '{}': cell unit '{}' states no square", index.SceneName, unit.Name ) );
                    const double cx = 0.5 * ( double( unit.Square->MinX ) + double( unit.Square->MaxX ) );
                    const double cz = 0.5 * ( double( unit.Square->MinZ ) + double( unit.Square->MaxZ ) );
                    const auto   rx = static_cast<std::int64_t>( std::floor( cx / double( *size ) ) );
                    const auto   rz = static_cast<std::int64_t>( std::floor( cz / double( *size ) ) );
                    chunk           = std::format( "{}_R{}_{}", prefix, rx, rz );
                    regions.insert( chunk );
                }
                if ( auto clash = place( unit.File, chunk ) )
                    return Common::MakeError<Result>( std::move( *clash ) );
            }
        for ( const IndexHLOD& hlod : index.HLODs )
            if ( hlod.File.has_value() )
                if ( auto clash = place( *hlod.File, layout.Shared ) )
                    return Common::MakeError<Result>( std::move( *clash ) );
        for ( const IndexFile& file : index.Files )
            if ( !layout.FileChunk.contains( file.Name ) )
                return Common::MakeError<Result>( std::format(
                     "world '{}': the index lists file '{}', which no unit or HLOD names", index.SceneName, file.Name ) );

        layout.Regions.assign( regions.begin(), regions.end() );
        return Common::MakeSuccess( Result{ std::move( layout ) } );
    }

    std::vector<std::string> CanonicalRecords( const SceneSerialized& scene )
    {
        std::vector<std::pair<std::uint64_t, std::string>> keyed;
        keyed.reserve( scene.Entities.size() );
        for ( const auto& record : scene.Entities )
            keyed.emplace_back( record.id.has_value() ? static_cast<std::uint64_t>( *record.id ) : 0,
                                Common::Json::Write( record ) );
        std::sort( keyed.begin(), keyed.end() );
        std::vector<std::string> out;
        out.reserve( keyed.size() );
        for ( auto& [id, text] : keyed )
            out.push_back( std::move( text ) );
        return out;
    }
    // ── Built HLODs ──────────────────────────────────────────────────────────────────────────────

    namespace
    {
        // What a build and a staleness check plan alike: the plan from the descriptors, the members and name of
        // every unit, the records by id. Refusals are CookWorld's, worded the same.
        struct HLODPlan
        {
            Rules::WorldPartitionPlan                     Plan;
            std::unordered_map<Common::UUID, std::size_t> ById;
            std::vector<std::vector<std::size_t>>         Members;
            std::vector<std::string>                      Names;
        };

        Common::ResultStr<HLODPlan> PlanHLODs( const SceneSerialized&                        scene,
                                               std::span<const Rules::EntityDescriptor>      descriptors,
                                               std::span<const Common::Utils::AssetRegistry> registries )
        {
            if ( !scene.WorldPartition.has_value() )
                return Common::MakeError<HLODPlan>( "'" + scene.SceneName +
                                                    "' states no WorldPartition block, so it has no cells" );
            const auto& records = scene.Entities;
            HLODPlan    out;
            for ( std::size_t record = 0; record < records.size(); ++record )
            {
                if ( !records[record].id.has_value() || records[record].id->IsNull() )
                    return Common::MakeError<HLODPlan>( std::format( "record {} ('{}') of '{}' has no id", record,
                                                                     records[record].Tag.value_or( "" ),
                                                                     scene.SceneName ) );
                if ( !out.ById.emplace( *records[record].id, record ).second )
                    return Common::MakeError<HLODPlan>(
                         std::format( "record {} of '{}' has id {}, which another record already has", record,
                                      scene.SceneName, static_cast<std::uint64_t>( *records[record].id ) ) );
            }
            if ( descriptors.size() != records.size() )
                return Common::MakeError<HLODPlan>( std::format(
                     "'{}' has {} records and {} descriptors; the descriptor index is not this world's",
                     scene.SceneName, records.size(), descriptors.size() ) );
            for ( std::size_t record = 0; record < records.size(); ++record )
                if ( descriptors[record].Id != static_cast<std::uint64_t>( *records[record].id ) )
                    return Common::MakeError<HLODPlan>( std::format(
                         "record {} of '{}' is entity {} but its descriptor is another; the descriptor index is "
                         "stale",
                         record, scene.SceneName, static_cast<std::uint64_t>( *records[record].id ) ) );
            out.Plan = Rules::PlanWorldPartition( descriptors, *scene.WorldPartition, BoundsFrom( registries ),
                                                  ScriptReferenceKinds() );
            const std::size_t unitCount = Rules::ResidencyUnitCount( out.Plan );
            for ( std::size_t unit = 0; unit < unitCount; ++unit )
            {
                out.Members.push_back( Rules::ResidencyUnitMembers( out.Plan, unit ) );
                out.Names.push_back( Rules::HLODRowName( *scene.WorldPartition, out.Plan, unit ) );
            }
            return Common::MakeSuccess( std::move( out ) );
        }

        // "HLOD_<cell file stem>.dhlod".
        std::string BuiltHLODFileName( const WorldPartitionSerialized& partition, const Rules::PlannedCell& cell )
        {
            const std::string cellFile = CellFileName( partition, cell );
            return "HLOD_" + cellFile.substr( 0, cellFile.size() - kCellExtension.size() ) +
                   std::string( kBuiltHLODExtension );
        }
    } // namespace

    namespace
    {
        // A JSON value with the keys of every object in it sorted: the content without its write order.
        Common::Json::Value SortedKeys( const Common::Json::Value& value )
        {
            if ( const auto* object = std::get_if<Common::Json::Object>( &value.variant() ) )
            {
                std::vector<std::pair<std::string, Common::Json::Value>> members( object->begin(), object->end() );
                std::ranges::stable_sort( members, {}, &std::pair<std::string, Common::Json::Value>::first );
                Common::Json::Object sorted;
                for ( const auto& [key, member] : members )
                    sorted.insert( key, SortedKeys( member ) );
                return Common::Json::Value( sorted );
            }
            if ( const auto* array = std::get_if<Common::Json::Value::Array>( &value.variant() ) )
            {
                Common::Json::Value::Array sorted;
                sorted.reserve( array->size() );
                for ( const Common::Json::Value& element : *array )
                    sorted.push_back( SortedKeys( element ) );
                return Common::Json::Value( sorted );
            }
            return value;
        }

        Common::Json::KeyedValues SortedKeys( const Common::Json::KeyedValues& blocks )
        {
            std::vector<std::pair<std::string, Common::Json::Value>> members( blocks.begin(), blocks.end() );
            std::ranges::stable_sort( members, {}, &std::pair<std::string, Common::Json::Value>::first );
            Common::Json::KeyedValues sorted;
            for ( const auto& [key, member] : members )
                sorted.insert( key, SortedKeys( member ) );
            return sorted;
        }
    } // namespace

    namespace
    {
        // The content hash of the file registry key @p key names (HLODAssetHasher): an asset envelope's from its
        // table of contents - the tag and hash of every section, read with the header - any other file's from its
        // bytes, 0 for a file that does not read.
        std::uint64_t SavedContentHash( const std::string& key )
        {
            const std::filesystem::path file = Common::AssetHandle::PathForStableKey( key );
            {
                std::ifstream                                       in( file, std::ios::binary );
                std::array<std::byte, CC::ASSET_HEADER_SNIFF_BYTES> leading{};
                if ( in && in.read( reinterpret_cast<char*>( leading.data() ),
                                    static_cast<std::streamsize>( leading.size() ) ) &&
                     CC::BinaryEnvelopeHeaderFormat().Recognises( leading ) )
                {
                    in.clear();
                    in.seekg( 0 );
                    CC::AssetHeaderReadContext context;
                    context.RecordOnly = true;
                    if ( auto header = CC::ReadEnvelopeHeader( in, context ) )
                    {
                        std::string toc;
                        for ( const CC::EnvelopeTocEntry& entry : header.GetValue().Toc )
                            toc += std::format( "{:08x}:{:016x};", static_cast<std::uint32_t>( entry.Tag ),
                                                entry.Hash );
                        return Common::Utils::Crc32c( toc.data(), toc.size() );
                    }
                }
            }
            // Not an envelope, or one whose header does not read: its bytes are its content.
            auto bytes = Common::Utils::FileSystem::ReadByteFileContent( file );
            if ( !bytes )
                return 0;
            return Common::Utils::Crc32c( bytes.GetValue().data(), bytes.GetValue().size() );
        }
    } // namespace

    struct HLODAssetHasher::State
    {
        explicit State( std::span<const Common::Utils::AssetRegistry> registries ) : Closure( registries ) {}

        AssetClosure                                   Closure;
        std::unordered_map<std::string, std::uint64_t> Hashes; // by registry key
    };

    HLODAssetHasher::HLODAssetHasher( std::span<const Common::Utils::AssetRegistry> registries )
         : m_State( std::make_unique<State>( registries ) )
    {
    }

    HLODAssetHasher::~HLODAssetHasher() = default;

    std::string HLODAssetHasher::ContentOf( std::span<const Assets::EntityData> records,
                                            std::span<const std::size_t>        members )
    {
        std::set<std::string> keys;
        for ( const std::size_t member : members )
            m_State->Closure.Record( records[member], keys );
        m_State->Closure.Expand( keys );
        std::string text;
        for ( const std::string& key : keys )
        {
            auto known = m_State->Hashes.find( key );
            if ( known == m_State->Hashes.end() )
                known = m_State->Hashes.emplace( key, SavedContentHash( key ) ).first;
            text += std::format( "{} {:016x}\n", key, known->second );
        }
        return text;
    }

    std::uint32_t HLODBuildChecksum( std::span<const Assets::EntityData> records,
                                     std::span<const std::size_t>        members,
                                     const WorldPartitionSerialized& partition, HLODAssetHasher& assets )
    {
        // THE ONE FORM A MEMBER IS HASHED IN (Build HLODs, CookWorld and Play all come here). The record minus
        // `siblingIndex`: that is the outliner's sort key, not content - the scene saver writes its own sparse
        // keys while Play's snapshot (SerializeToJson) numbers the same order densely, so hashing it made every
        // saved HLOD "stale" in Play. Nothing an HLOD is built from reads it.
        std::string text;
        for ( const std::size_t member : members )
        {
            Assets::EntityData canonical = records[member];
            canonical.siblingIndex.reset();
            // ...and with every object's keys in one order: the saved file keeps the order it was written in
            // (StaticMesh before RuntimeGrid, MaterialSlots before MeshHandle) while Play's snapshot writes the
            // component registry's order, so the same content hashed differently and every Main/Foliage cell
            // with two components read as stale in Play (WP22-REST: 194 errors right after Build HLODs).
            canonical.Components = SortedKeys( canonical.Components );
            text += Common::Json::Write( canonical );
            text += '\n';
        }
        // ...and the partition minus what no HLOD is built from (WP-HLOD-DATA-LAYERS): a data layer's
        // InitiallyLoadedInEditor (what the editor opens loaded), its InitialRuntimeState and the world's
        // DataLayersLogicOperator (which state Play shows it in) decide when an HLOD is drawn, never what it
        // holds; hashing them made every saved HLOD of the world stale the moment one was toggled. A layer's Name
        // and Type stay: the Type splits the cells (and so the members), the Name keys the rows and files.
        WorldPartitionSerialized built = partition;
        if ( built.DataLayers.has_value() )
            for ( DataLayerSerialized& layer : *built.DataLayers )
            {
                layer.InitiallyLoadedInEditor = true;
                layer.InitialRuntimeState.reset();
            }
        built.DataLayersLogicOperator = WorldPartitionDataLayersLogicOperator::Or;
        // The minimap (WP v11) is a picture OF the world, never a source of its HLODs: hashing it made every HLOD
        // stale the moment Build Minimap ran. A world without one writes no key, so its checksums are unchanged.
        built.MiniMap.reset();
        // ...and a runtime partition's DebugColor (WP v10): the colour the gameplay debugger paints its cells in,
        // never what they hold - recolouring a partition must not make its HLODs stale.
        for ( RuntimePartitionSerialized& runtime : built.RuntimePartitions )
            runtime.DebugColor = glm::vec3( 0.5f );
        text += Common::Json::Write( built );
        text += '\n';
        text += assets.ContentOf( records, members );
        text += std::format( "builder {}\n", kHLODBuilderVersion );
        return Common::Utils::Crc32c( text.data(), text.size() );
    }

    // The build over an expanded world, as CookPlanned is the cook over one.
    static Common::ResultStr<HLODBuild>
    BuildPlannedHLODs( const SceneSerialized& scene, std::span<const Rules::EntityDescriptor> descriptors,
                       std::span<const Common::Utils::AssetRegistry> registries,
                       const Rules::HLODBuilderRegistry& builders, const StoredHLODs* previous,
                       const BuiltHLODSink& sink, HLODFileOutput output );

    Common::ResultStr<HLODBuild>
    BuildWorldHLODs( const SceneSerialized& scene, std::span<const Rules::EntityDescriptor> descriptors,
                     std::span<const Common::Utils::AssetRegistry> registries, const PrefabReader& prefabs,
                     const Rules::HLODBuilderRegistry& builders, const StoredHLODs* previous,
                     const BuiltHLODSink& sink, const HLODFileOutput output )
    {
        // The HLODs are built from the world the cook cuts: every prefab instance expanded the same way, by the
        // same function, so a cook finds them fresh (StaleHLODCells over the expanded records).
        auto expanded = ExpandWorldForCook( scene, descriptors, prefabs );
        if ( !expanded )
            return Common::MakeError<HLODBuild>( expanded.GetError() );
        return BuildPlannedHLODs( expanded.GetValue().World.Scene, expanded.GetValue().Descriptors, registries,
                                  builders, previous, sink, output );
    }

    static Common::ResultStr<HLODBuild>
    BuildPlannedHLODs( const SceneSerialized& scene, std::span<const Rules::EntityDescriptor> descriptors,
                       std::span<const Common::Utils::AssetRegistry> registries,
                       const Rules::HLODBuilderRegistry& builders, const StoredHLODs* previous,
                       const BuiltHLODSink& sink, const HLODFileOutput output )
    {
        using Result = HLODBuild;
        auto planned = PlanHLODs( scene, descriptors, registries );
        if ( !planned )
            return Common::MakeError<Result>( planned.GetError() );
        const HLODPlan& hp = planned.GetValue();
        if ( auto layers = Rules::ValidateHLODLayers( *scene.WorldPartition ); !layers )
            return Common::MakeError<Result>( "'" + scene.SceneName + "': " + layers.GetError() );
        if ( !Rules::AnyGridHLODLayer( *scene.WorldPartition ) )
            return Common::MakeError<Result>(
                 "'" + scene.SceneName + "' has no grid with an HLOD layer, so its cells have no HLODs to build" );
        // The builders' say (UE: a layer whose builder class is not loaded, or refuses its settings, fails the
        // build).
        if ( const std::vector<std::string> refused =
                  Rules::HLODLayerBuilderIssues( *scene.WorldPartition, builders );
             !refused.empty() )
            return Common::MakeError<Result>( std::format( "'{}': {}", scene.SceneName, refused.front() ) );

        const auto&                     records       = scene.Entities;
        Common::Json::Issues            issues        = hp.Plan.Issues;
        const std::vector<glm::mat4>    world         = Rules::Detail::ComposeWorld( records, hp.ById );
        const Rules::CustomShaderSource customShader  = CustomShaderFrom( registries );
        const Rules::FoliageHLODSource  foliageInHLOD = FoliageInHLODFrom( registries );
        std::set<std::uint64_t>         hlodIds;
        HLODAssetHasher                 assets( registries );
        HLODBuild                       out;
        out.Index.SceneName = scene.SceneName;
        // The rows of the previous build, by unit: one whose checksum is the one now is kept (UE's incremental
        // Build HLODs), unless its file no longer reads back as that row's - then it is built again.
        std::unordered_map<std::string, const BuiltHLODCell*> previousRows;
        if ( previous != nullptr )
            for ( const BuiltHLODCell& row : previous->Index.Cells )
                previousRows.emplace( row.Unit, &row );
        const auto keep = [&]( const BuiltHLODCell& row ) -> Common::ResultStr<bool>
        {
            const auto found = previousRows.find( row.Unit );
            if ( found == previousRows.end() || found->second->BuiltFrom != row.BuiltFrom ||
                 !StoredCellRecords( *previous, *found->second ) )
                return Common::MakeSuccess( false );
            for ( const std::uint64_t id : found->second->Ids )
                if ( !hlodIds.insert( id ).second )
                    return Common::MakeError<bool>(
                         std::format( "the kept HLOD of {} in '{}' has id {}, which another HLOD already has",
                                      row.Unit, scene.SceneName, id ) );
            out.Index.Cells.push_back( *found->second );
            return Common::MakeSuccess( true );
        };
        // One HLOD built and handed to the sink: @p members are the records it stands in for, @p parts the same
        // records grouped by the layer that builds them (Rules::CellHLODParts; one part for a parent), @p cookedName
        // names its ids (HLODRecordId), @p name is its saved file.
        const auto build = [&]( BuiltHLODCell& row, std::span<const Rules::CellHLODPart> parts,
                                std::span<const std::size_t> members, float minVisibleDistance,
                                const std::string& cookedName, const std::string& name ) -> Common::BoolResultStr
        {
            // Every part by its own layer and that layer's builder with its own Settings (UE
            // FWorldPartitionHLODUtilities::CreateHLODActors builds each layer's group with its builder), then one
            // HLOD of their batches: the cell is one residency unit.
            Rules::InstancingHLOD built;
            for ( const Rules::CellHLODPart& part : parts )
            {
                Rules::HLODBuildContext context;
                context.Records            = records;
                context.World              = world;
                context.Members            = part.Members;
                context.MinVisibleDistance = minVisibleDistance;
                context.Settings           = part.Layer->Settings.has_value() ? &*part.Layer->Settings : nullptr;
                context.Registries         = registries;
                context.Geometry           = builders.MeshGeometry();
                auto builtPart =
                     Rules::BuildCellHLOD( *part.Layer, builders, context, customShader, issues, foliageInHLOD );
                if ( !builtPart )
                    return Common::MakeError( "'" + scene.SceneName + "', " + row.Unit + ": " + builtPart.GetError() );
                Rules::InstancingHLOD piece = builtPart.ExtractValue();
                built.Instances += piece.Instances;
                built.FoliageLeftOut += piece.FoliageLeftOut;
                built.NotInstanced.insert( built.NotInstanced.end(), piece.NotInstanced.begin(),
                                           piece.NotInstanced.end() );
                for ( Rules::HLODBatch& batch : piece.Batches )
                    built.Batches.push_back( std::move( batch ) );
            }
            row.Instances                     = built.Instances;
            for ( const Rules::HLODNotInstanced& missing : built.NotInstanced )
                row.NotInstanced.push_back(
                     { static_cast<std::uint64_t>( *records[missing.Record].id ), missing.Reason } );
            if ( built.Batches.empty() )
                return BOOLSUCCESS;
            // The records keep the ids a cook gives them (named by the cooked HLOD file), so a stored HLOD
            // packed by the cook is the same world as one built by it.
            CellPayload payload;
            payload.World = scene.SceneName;
            payload.Units.push_back( "HLOD " + row.Unit );
            std::set<std::uint64_t> sources;
            for ( std::size_t batch = 0; batch < built.Batches.size(); ++batch )
            {
                const Common::UUID id = Rules::HLODRecordId( scene.SceneName, cookedName, batch );
                if ( id.IsNull() || hp.ById.contains( id ) ||
                     !hlodIds.insert( static_cast<std::uint64_t>( id ) ).second )
                    return Common::MakeError( std::format(
                         "the HLOD id {} of batch {} in '{}' is null or already taken; rename the world",
                         static_cast<std::uint64_t>( id ), batch, name ) );
                row.Ids.push_back( static_cast<std::uint64_t>( id ) );
                if ( built.Batches[batch].Variant == Rules::HLODBatchVariant::ImpostorCard )
                    row.ImpostorCards.push_back( static_cast<std::uint64_t>( id ) );
                else if ( built.Batches[batch].Variant == Rules::HLODBatchVariant::ImpostorFallback )
                    row.ImpostorFallbacks.push_back( static_cast<std::uint64_t>( id ) );
                payload.Records.push_back( Rules::HLODRecord( built.Batches[batch], id, batch ) );
                for ( const std::size_t source : built.Batches[batch].Sources )
                    sources.insert( static_cast<std::uint64_t>( *records[source].id ) );
            }
            for ( const std::size_t member : members )
                if ( sources.contains( static_cast<std::uint64_t>( *records[member].id ) ) )
                    row.Sources.push_back( static_cast<std::uint64_t>( *records[member].id ) );
            // A probe's sink gets the name alone: the payload is not encoded for a file nobody keeps
            // (HLODFileOutput).
            CookedFile file{ name, {} };
            if ( output == HLODFileOutput::Encoded )
            {
                auto wrapped =
                     Wrap( CC::ContentKind::WorldCell, scene.SceneName, name, Common::Json::Write( payload ) );
                if ( !wrapped )
                    return Common::MakeError( wrapped.GetError() );
                file = CookedFile{ name, wrapped.ExtractValue() };
            }
            if ( auto sunk = sink( std::move( file ) ); !sunk )
                return Common::MakeError( "'" + scene.SceneName + "', " + row.Unit + ": " + sunk.GetError() );
            row.File = name;
            return BOOLSUCCESS;
        };
        for ( std::size_t unit = hp.Plan.AlwaysLoaded.size(); unit < hp.Members.size(); ++unit )
        {
            const Rules::PlannedCell& cell = hp.Plan.Cells.at( unit - hp.Plan.AlwaysLoaded.size() );
            BuiltHLODCell             row;
            row.Unit      = hp.Names[unit];
            row.BuiltFrom = HLODBuildChecksum( records, hp.Members[unit], *scene.WorldPartition, assets );
            auto kept     = keep( row );
            if ( !kept )
                return Common::MakeError<Result>( kept.GetError() );
            if ( kept.GetValue() )
                continue;
            // The layer is the cell's PARTITION's first HLOD setup (WP22c, WP-HASH-SET), and a composite's own where it
            // names one (WP-SETTINGS-UI, Rules::CellHLODParts): a partition that states none gives its cells a row with no file.
            auto parts = Rules::CellHLODParts( *scene.WorldPartition, hp.Plan, cell, hp.Members[unit] );
            if ( !parts )
                return Common::MakeError<Result>( "'" + scene.SceneName + "', " + row.Unit + ": " + parts.GetError() );
            if ( !parts.GetValue().empty() )
                if ( auto built = build( row, parts.GetValue(), hp.Members[unit],
                                         Rules::HLODMinVisibleDistance( *scene.WorldPartition, hp.Plan, unit ),
                                         HLODFileName( CellFileName( *scene.WorldPartition, cell ) ),
                                         BuiltHLODFileName( *scene.WorldPartition, cell ) );
                     !built )
                    return Common::MakeError<Result>( built.GetError() );
            out.Built.push_back( row.Unit );
            out.Index.Cells.push_back( std::move( row ) );
        }
        // THE PARENTS (WP-FAR-8, WorldPartitionHLODHierarchy.hpp): per HLOD cell of a spatially loaded layer, the
        // HLOD of its next setup's layer, built from the records of every world cell beneath it and seen from the
        // child layer's LoadingRange on. Their rows follow the cells', keyed by the node's name.
        auto hierarchy = Rules::PlanHLODHierarchy( *scene.WorldPartition, hp.Plan );
        if ( !hierarchy )
            return Common::MakeError<Result>( "'" + scene.SceneName + "': " + hierarchy.GetError() );
        const std::vector<HLODLayerSerialized>& layers = *scene.WorldPartition->HLODLayers;
        for ( const Rules::HLODNode& node : hierarchy.GetValue().Nodes )
        {
            if ( !node.ForHLODCell )
                continue;
            std::vector<std::size_t> members;
            for ( const std::size_t unit : node.SourceUnits )
                members.insert( members.end(), hp.Members.at( unit ).begin(), hp.Members.at( unit ).end() );
            std::sort( members.begin(), members.end() );
            const Rules::HLODCell& child = hierarchy.GetValue().Cells.at( node.StandsFor );
            const std::string      stem  = ParentHLODFileStem( *scene.WorldPartition, hierarchy.GetValue(), node );
            BuiltHLODCell          row;
            row.Unit      = node.Name;
            row.BuiltFrom = HLODBuildChecksum( records, members, *scene.WorldPartition, assets );
            auto kept     = keep( row );
            if ( !kept )
                return Common::MakeError<Result>( kept.GetError() );
            if ( kept.GetValue() )
                continue;
            const Rules::CellHLODPart whole{ &layers.at( node.Layer ), members };
            if ( auto built =
                      build( row, std::span<const Rules::CellHLODPart>( &whole, 1 ), members,
                             *layers.at( child.Layer ).LoadingRange,
                             stem + std::string( kCellExtension ), stem + std::string( kBuiltHLODExtension ) );
                 !built )
                return Common::MakeError<Result>( built.GetError() );
            out.Built.push_back( row.Unit );
            out.Index.Cells.push_back( std::move( row ) );
        }
        Common::Json::ReportIssues( issues, scene.SceneName );
        return Common::MakeSuccess( std::move( out ) );
    }

    std::filesystem::path BuiltHLODDirectory( const std::filesystem::path& scenePath )
    {
        // The rule lives in Common: the content browser's Duplicate / Delete carry the folder with the scene.
        return Common::Content::BuiltHLODDirectoryOf( scenePath );
    }

    BuiltHLODSink BuiltHLODWriter( const std::filesystem::path& scenePath )
    {
        return [dir = BuiltHLODDirectory( scenePath )]( CookedFile file ) -> Common::BoolResultStr
        {
            std::error_code ec;
            std::filesystem::create_directories( dir, ec );
            if ( ec )
                return Common::MakeError( "Build HLODs: cannot create '" + dir.string() + "': " + ec.message() );
            if ( auto written = Common::Utils::FileSystem::WriteBytesToFileAtomic(
                      dir / file.Name, std::as_bytes( std::span( file.Bytes.data(), file.Bytes.size() ) ) );
                 !written )
                return Common::MakeError( "Build HLODs: '" + ( dir / file.Name ).string() +
                                          "': " + written.GetError() );
            return BOOLSUCCESS;
        };
    }

    Common::BoolResultStr WriteBuiltHLODIndex( const std::filesystem::path& scenePath,
                                               const BuiltHLODIndex&        index )
    {
        namespace fs        = std::filesystem;
        const fs::path  dir = BuiltHLODDirectory( scenePath );
        std::error_code ec;
        fs::create_directories( dir, ec );
        if ( ec )
            return Common::MakeError( "Build HLODs: cannot create '" + dir.string() + "': " + ec.message() );
        auto wrappedIndex = Wrap( CC::ContentKind::WorldIndex, index.SceneName, kBuiltHLODIndexName,
                                  Common::Json::Write( index ) );
        if ( !wrappedIndex )
            return Common::MakeError( wrappedIndex.GetError() );
        // The index last: a build cut short leaves the previous index, whose checksums name what it built.
        const std::vector<unsigned char>& bytes = wrappedIndex.GetValue();
        if ( auto written = Common::Utils::FileSystem::WriteBytesToFileAtomic(
                  dir / kBuiltHLODIndexName, std::as_bytes( std::span( bytes.data(), bytes.size() ) ) );
             !written )
            return Common::MakeError( "Build HLODs: '" + ( dir / kBuiltHLODIndexName ).string() +
                                      "': " + written.GetError() );
        std::set<std::string> kept{ std::string( kBuiltHLODIndexName ) };
        for ( const BuiltHLODCell& cell : index.Cells )
            if ( cell.File )
                kept.insert( *cell.File );
        for ( const fs::directory_entry& entry : fs::directory_iterator( dir, ec ) )
            if ( entry.is_regular_file() && entry.path().extension() == kBuiltHLODExtension &&
                 !kept.contains( entry.path().filename().string() ) )
                fs::remove( entry.path(), ec );
        return BOOLSUCCESS;
    }

    Common::ResultStr<StoredHLODs> ReadBuiltHLODs( const std::filesystem::path& scenePath )
    {
        const std::filesystem::path dir = BuiltHLODDirectory( scenePath );
        auto bytes = Common::Utils::FileSystem::ReadByteFileContent( dir / kBuiltHLODIndexName );
        if ( !bytes )
            return Common::MakeError<StoredHLODs>( "'" + scenePath.stem().string() + "' has no built HLODs (" +
                                                   dir.string() + "); run Build > Build HLODs" );
        auto payload = Unwrap( kBuiltHLODIndexName, CC::ContentKind::WorldIndex, bytes.GetValue() );
        if ( !payload )
            return Common::MakeError<StoredHLODs>( payload.GetError() + "; run Build > Build HLODs again" );
        auto parsed = Common::Json::Read<BuiltHLODIndex>( payload.GetValue() );
        if ( !parsed )
            return Common::MakeError<StoredHLODs>( "'" + ( dir / kBuiltHLODIndexName ).string() +
                                                   "' is not a readable HLOD index: " + parsed.GetError() );
        StoredHLODs stored;
        stored.Index  = parsed.ExtractValue();
        stored.Reader = [dir]( std::string_view name ) -> Common::ResultStr<std::vector<unsigned char>>
        {
            auto read = Common::Utils::FileSystem::ReadByteFileContent( dir / std::string( name ) );
            if ( !read )
                return Common::MakeError<std::vector<unsigned char>>( read.GetError() );
            return Common::MakeSuccess( read.ExtractValue() );
        };
        return Common::MakeSuccess( std::move( stored ) );
    }

    Common::ResultStr<std::vector<Assets::EntityData>> StoredCellRecords( const StoredHLODs&   stored,
                                                                          const BuiltHLODCell& cell )
    {
        using Result = std::vector<Assets::EntityData>;
        if ( !cell.File.has_value() )
            return Common::MakeSuccess( Result{} );
        const std::string& name  = *cell.File;
        auto               bytes = stored.Reader( name );
        if ( !bytes )
            return Common::MakeError<Result>( bytes.GetError() );
        auto payload = Unwrap( name, CC::ContentKind::WorldCell, bytes.GetValue() );
        if ( !payload )
            return Common::MakeError<Result>( payload.GetError() );
        auto parsed = Common::Json::Read<CellPayload>( payload.GetValue() );
        if ( !parsed )
            return Common::MakeError<Result>( "'" + name + "' is not a readable HLOD file: " + parsed.GetError() );
        CellPayload file = parsed.ExtractValue();
        if ( file.Units != std::vector<std::string>{ "HLOD " + cell.Unit } )
            return Common::MakeError<Result>( "'" + name + "' is not the HLOD of " + cell.Unit );
        if ( file.Records.size() != cell.Ids.size() )
            return Common::MakeError<Result>( std::format( "'{}' holds {} record(s) and the index lists {}", name,
                                                           file.Records.size(), cell.Ids.size() ) );
        for ( std::size_t record = 0; record < cell.Ids.size(); ++record )
            if ( !file.Records[record].id.has_value() ||
                 static_cast<std::uint64_t>( *file.Records[record].id ) != cell.Ids[record] )
                return Common::MakeError<Result>(
                     std::format( "record {} of '{}' is not the one the index lists", record, name ) );
        return Common::MakeSuccess( std::move( file.Records ) );
    }

    Common::ResultStr<std::vector<std::string>>
    StaleHLODCells( const BuiltHLODIndex& index, const SceneSerialized& scene,
                    std::span<const Rules::EntityDescriptor>      descriptors,
                    std::span<const Common::Utils::AssetRegistry> registries )
    {
        auto planned = PlanHLODs( scene, descriptors, registries );
        if ( !planned )
            return Common::MakeError<std::vector<std::string>>( planned.GetError() );
        const HLODPlan&                                       hp = planned.GetValue();
        std::unordered_map<std::string, const BuiltHLODCell*> rows;
        for ( const BuiltHLODCell& row : index.Cells )
            rows.emplace( row.Unit, &row );
        HLODAssetHasher          assets( registries );
        std::vector<std::string> stale;
        for ( std::size_t unit = hp.Plan.AlwaysLoaded.size(); unit < hp.Members.size(); ++unit )
        {
            const auto row = rows.find( hp.Names[unit] );
            if ( row == rows.end() ||
                 row->second->BuiltFrom !=
                      HLODBuildChecksum( scene.Entities, hp.Members[unit], *scene.WorldPartition, assets ) )
                stale.push_back( hp.Names[unit] );
        }
        // The parents (WP-FAR-8): each over the records of every cell beneath it.
        auto hierarchy = Rules::PlanHLODHierarchy( *scene.WorldPartition, hp.Plan );
        if ( !hierarchy )
            return Common::MakeError<std::vector<std::string>>( hierarchy.GetError() );
        for ( const Rules::HLODNode& node : hierarchy.GetValue().Nodes )
        {
            if ( !node.ForHLODCell )
                continue;
            std::vector<std::size_t> members;
            for ( const std::size_t unit : node.SourceUnits )
                members.insert( members.end(), hp.Members.at( unit ).begin(), hp.Members.at( unit ).end() );
            std::sort( members.begin(), members.end() );
            const auto row = rows.find( node.Name );
            if ( row == rows.end() ||
                 row->second->BuiltFrom !=
                      HLODBuildChecksum( scene.Entities, members, *scene.WorldPartition, assets ) )
                stale.push_back( node.Name );
        }
        return Common::MakeSuccess( std::move( stale ) );
    }

    std::vector<Rules::GenerationIssue> StaleHLODIssues( std::span<const std::string> units )
    {
        std::vector<Rules::GenerationIssue> issues;
        for ( const std::string& unit : units )
            issues.push_back( Rules::GenerationIssue{
                 Rules::GenerationIssueKind::StaleHLOD, Rules::kNoRecord, Rules::kNoRecord,
                 std::format( "the saved HLOD of {} is missing or older than its cell; run Build > Build HLODs", unit ) } );
        return issues;
    }

    Rules::GenerationIssue MissingHLODsIssue( std::string_view why )
    {
        return Rules::GenerationIssue{ Rules::GenerationIssueKind::StaleHLOD, Rules::kNoRecord, Rules::kNoRecord,
                                       std::format( "no built HLODs: {}; run Build > Build HLODs", why ) };
    }

    Common::ResultStr<WorldPartitionCheck>
    CheckWorldPartition( const SceneSerialized& scene, std::vector<Rules::EntityDescriptor> descriptors,
                         std::span<const Common::Utils::AssetRegistry> registries,
                         const std::filesystem::path&                  scenePath )
    {
        using Result = WorldPartitionCheck;
        if ( !scene.WorldPartition.has_value() )
            return Common::MakeError<Result>( "'" + scene.SceneName +
                                              "' states no WorldPartition block: it is not a partitioned world" );
        WorldPartitionCheck check;
        check.World       = scene.SceneName;
        check.Partition   = *scene.WorldPartition;
        check.Descriptors = std::move( descriptors );
        check.Plan        = Rules::PlanWorldPartition( std::span<const Rules::EntityDescriptor>( check.Descriptors ),
                                                       check.Partition, BoundsFrom( registries ) );
        check.Issues      = Rules::PlanIssues( check.Plan, check.Descriptors );
        if ( Rules::AnyGridHLODLayer( check.Partition ) )
        {
            auto stored = scenePath.empty() ? Common::MakeError<StoredHLODs>(
                                                   "'" + scene.SceneName + "' is not saved, so it has no built HLODs" )
                                            : ReadBuiltHLODs( scenePath );
            if ( !stored )
                check.Issues.push_back( MissingHLODsIssue( stored.GetError() ) );
            else
            {
                auto stale = StaleHLODCells( stored.GetValue().Index, scene, check.Descriptors, registries );
                if ( !stale )
                    return Common::MakeError<Result>( stale.GetError() );
                const std::vector<Rules::GenerationIssue> named = StaleHLODIssues( stale.GetValue() );
                check.Issues.insert( check.Issues.end(), named.begin(), named.end() );
            }
        }
        return Common::MakeSuccess( std::move( check ) );
    }

    Common::ResultStr<std::optional<WorldPartitionCheck>>
    CheckSavedWorldPartition( const std::filesystem::path&                  scenePath,
                              std::span<const Common::Utils::AssetRegistry> registries )
    {
        using Result = std::optional<WorldPartitionCheck>;
        auto text    = ExternalEntities::ReadSceneFileText( scenePath );
        if ( !text )
            return Common::MakeError<Result>( text.GetError() );
        auto parsed = ParseLoadableScene( scenePath.string(), text.GetValue() );
        if ( !parsed )
            return Common::MakeError<Result>( parsed.GetError() );
        const SceneSerialized& scene = parsed.GetValue().Scene;
        if ( !scene.WorldPartition.has_value() )
            return Common::MakeSuccess( Result() );
        auto index = DescriptorIndex::Refresh( scenePath );
        if ( !index )
            return Common::MakeError<Result>( index.GetError() );
        auto check = CheckWorldPartition( scene, DescriptorIndex::Descriptors( index.GetValue().Index ), registries,
                                          scenePath );
        if ( !check )
            return Common::MakeError<Result>( check.GetError() );
        return Common::MakeSuccess( Result( check.ExtractValue() ) );
    }

    std::string ReportWorldPartitionCheck( const WorldPartitionCheck& check )
    {
        std::string                         text = Rules::ReportPlanIssues( check.Plan, check.World );
        std::vector<Rules::GenerationIssue> hlods;
        for ( const Rules::GenerationIssue& issue : check.Issues )
            if ( issue.Kind == Rules::GenerationIssueKind::StaleHLOD )
                hlods.push_back( issue );
        const std::string stale = Rules::ReportGenerationIssues( hlods, check.World );
        if ( !text.empty() && !stale.empty() )
            text += "\n";
        return text + stale;
    }

    std::filesystem::path StreamingGenerationLogPath( const std::filesystem::path& projectDir, std::string_view world )
    {
        return projectDir / "Saved" / "Logs" / "WorldPartition" / std::format( "{}-StreamingGeneration.log", world );
    }

    std::string StreamingGenerationLogText( std::string_view world, const WorldPartitionSerialized& partition,
                                            const Rules::WorldPartitionPlan&         plan,
                                            std::span<const Rules::EntityDescriptor> descriptors,
                                            std::span<const Rules::GenerationIssue>  issues )
    {
        std::string out = std::format( "World Partition streaming generation of '{}'\n{}\n", world,
                                       Rules::SummarisePartition( plan, partition ) );
        const auto  name   = [&]( std::size_t record ) { return Rules::Detail::RecordName( descriptors, record ); };
        const auto  layers = [&]( std::span<const std::size_t> held )
        { return Rules::Detail::LayerNames( partition, held ); };
        std::unordered_map<std::uint64_t, std::size_t> byId;
        for ( std::size_t record = 0; record < descriptors.size(); ++record )
            if ( descriptors[record].Id.has_value() )
                byId.emplace( *descriptors[record].Id, record );
        std::vector<std::size_t> compositeOf( descriptors.size(), Rules::kNoRecord );
        for ( std::size_t group = 0; group < plan.Composites.size(); ++group )
            for ( const std::size_t member : plan.Composites[group].Members )
                if ( member < compositeOf.size() )
                    compositeOf[member] = group;
        std::vector<std::string> unitOf( plan.Composites.size(), std::string( "no place" ) );
        for ( const std::size_t group : plan.AlwaysLoaded )
            unitOf[group] = "the always-loaded set";
        for ( const Rules::PlannedCell& cell : plan.Cells )
            for ( const std::size_t group : cell.Composites )
                unitOf[group] = "cell " + Rules::CellStem( partition, cell );

        const auto composite = [&]( std::size_t group )
        {
            const Rules::PlannedComposite& held = plan.Composites[group];
            std::string                    text = std::format( "    composite of {}", name( held.Anchor ) );
            if ( held.Footprint.has_value() )
                std::format_to( std::back_inserter( text ), ", footprint X {:.0f}..{:.0f} Z {:.0f}..{:.0f}",
                                held.Footprint->MinX, held.Footprint->MaxX, held.Footprint->MinZ, held.Footprint->MaxZ );
            if ( held.Height.has_value() )
                std::format_to( std::back_inserter( text ), ", height Y {:.0f}..{:.0f}", held.Height->MinY,
                                held.Height->MaxY );
            std::format_to( std::back_inserter( text ), ", data layers {}\n", layers( held.DataLayers ) );
            for ( const std::size_t member : held.Members )
            {
                std::format_to( std::back_inserter( text ), "      {}\n", name( member ) );
                for ( const Rules::DescriptorReference& reference : descriptors[member].References )
                {
                    const auto target = byId.find( reference.Target );
                    if ( target == byId.end() )
                        std::format_to( std::back_inserter( text ), "        -> {} through {}.{}: no such record\n",
                                        reference.Target, reference.Component, reference.Field );
                    else if ( compositeOf[target->second] != group )
                        std::format_to( std::back_inserter( text ), "        -> {} in {} through {}.{}\n",
                                        name( target->second ), unitOf[compositeOf[target->second]],
                                        reference.Component, reference.Field );
                }
            }
            return text;
        };

        std::format_to( std::back_inserter( out ), "\nAlways-loaded: {} composite(s)\n", plan.AlwaysLoaded.size() );
        for ( const std::size_t group : plan.AlwaysLoaded )
        {
            const Rules::PlannedComposite& held = plan.Composites[group];
            std::format_to( std::back_inserter( out ), "  reason {}{}\n", ReasonName( held.Reason ),
                            held.Because == Rules::kNoRecord ? std::string() : " by " + name( held.Because ) );
            out += composite( group );
        }
        std::format_to( std::back_inserter( out ), "\nCells: {}\n", plan.Cells.size() );
        for ( const Rules::PlannedCell& cell : plan.Cells )
        {
            std::format_to( std::back_inserter( out ),
                            "  cell {}: grid '{}', level {}, square X {:.0f}..{:.0f} Z {:.0f}..{:.0f}, bounds X "
                            "{:.0f}..{:.0f} Z {:.0f}..{:.0f}, height Y {:.0f}..{:.0f}, data layers {}, {} composite(s)\n",
                            Rules::CellStem( partition, cell ), partition.RuntimePartitions.at( cell.Grid ).Name, cell.Level,
                            cell.Square.MinX, cell.Square.MaxX, cell.Square.MinZ, cell.Square.MaxZ, cell.Bounds.MinX,
                            cell.Bounds.MaxX, cell.Bounds.MinZ, cell.Bounds.MaxZ, cell.Height.MinY, cell.Height.MaxY,
                            layers( cell.DataLayers ), cell.Composites.size() );
            for ( const std::size_t group : cell.Composites )
                out += composite( group );
        }
        std::size_t unplaced = 0;
        for ( std::size_t group = 0; group < plan.Composites.size(); ++group )
            if ( unitOf[group] == "no place" )
            {
                if ( unplaced++ == 0 )
                    out += "\nWithout a place (UnplacedPrefabInstances, UnplacedLandscapeTiles):\n";
                out += composite( group );
            }
        std::format_to( std::back_inserter( out ), "\nIssues: {}\n", issues.size() );
        for ( const Rules::GenerationIssue& issue : issues )
            std::format_to( std::back_inserter( out ), "  {}: {}\n", Rules::GenerationIssueKindName( issue.Kind ),
                            issue.Text );
        return out;
    }

    Common::ResultStr<std::filesystem::path>
    WriteStreamingGenerationLog( const std::filesystem::path& projectDir, std::string_view world,
                                 const WorldPartitionSerialized& partition, const Rules::WorldPartitionPlan& plan,
                                 std::span<const Rules::EntityDescriptor> descriptors,
                                 std::span<const Rules::GenerationIssue>  issues )
    {
        using Result                     = std::filesystem::path;
        const std::filesystem::path file = StreamingGenerationLogPath( projectDir, world );
        std::error_code             made;
        std::filesystem::create_directories( file.parent_path(), made );
        if ( made )
            return Common::MakeError<Result>(
                 std::format( "cannot create '{}': {}", file.parent_path().string(), made.message() ) );
        const std::string text    = StreamingGenerationLogText( world, partition, plan, descriptors, issues );
        const auto        written = Common::Utils::FileSystem::WriteBytesToFileAtomic( file, std::as_bytes( std::span( text ) ) );
        if ( !written )
            return Common::MakeError<Result>( written.GetError() );
        return Common::MakeSuccess( file );
    }
} // namespace Desert::Core::WorldCells
