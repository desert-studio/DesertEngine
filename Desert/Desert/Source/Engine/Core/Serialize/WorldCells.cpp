#include <Engine/Core/Serialize/WorldCells.hpp>

#include <Engine/Core/Serialize/WorldPartitionResidencyRules.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Utilities/Crc32c.hpp>
#include <Common/Utilities/PakFile.hpp>

#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <tuple>
#include <set>
#include <unordered_map>
#include <utility>

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
            }
            return "Unknown";
        }

        // A cell's file is named by its level and coordinate alone, so it does not move when the world gains
        // or loses another cell.
        std::string CellFileName( const Rules::PlannedCell& cell )
        {
            return "L" + std::to_string( cell.Level ) + "_" + std::to_string( cell.Cell.X ) + "_" +
                   std::to_string( cell.Cell.Z ) + std::string( kCellExtension );
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
                    Value( payload, keys );
                if ( record.PrefabOverrides.has_value() )
                    for ( const auto& over : *record.PrefabOverrides )
                        for ( const auto& [key, payload] : over.Components )
                            Value( payload, keys );
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

            void Value( const rfl::Generic& value, std::set<std::string>& keys )
            {
                if ( const auto whole = value.to_int64(); whole )
                {
                    Add( KeyOfHandle( static_cast<std::uint64_t>( whole.value() ) ), keys );
                    return;
                }
                if ( const auto text = value.to_string(); text )
                {
                    // A material slot names its material by header GUID text (SCNE 27); the registry row is
                    // keyed by the handle that GUID folds to, the same fold the loader resolves it through.
                    if ( const auto guid = CC::AssetGuidFromText( text.value() );
                         guid && !guid.GetValue().IsNull() )
                    {
                        Add( KeyOfHandle( static_cast<std::uint64_t>( CC::HandleForGuid( guid.GetValue() ) ) ),
                             keys );
                        return;
                    }
                    Common::UUID asHandle;
                    if ( Rules::Detail::ParseIdString( text.value(), asHandle ) )
                    {
                        Add( KeyOfHandle( static_cast<std::uint64_t>( asHandle ) ), keys );
                        return;
                    }
                    Add( KeyOfPath( text.value() ), keys );
                    return;
                }
                if ( const auto object = value.to_object(); object )
                {
                    for ( const auto& [field, inner] : object.value() )
                        Value( inner, keys );
                    return;
                }
                if ( const auto array = value.to_array(); array )
                    for ( const auto& inner : array.value() )
                        Value( inner, keys );
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
        };
    } // namespace

    Rules::AssetBoundsSource BoundsFrom( std::span<const Common::Utils::AssetRegistry> registries )
    {
        if ( registries.empty() )
            return {};
        return [registries]( std::uint64_t handle, std::string_view path ) -> std::optional<Common::Math::AABB>
        {
            for ( const auto& registry : registries )
                if ( const auto* row = registry.FindByReference( handle, path ) )
                    return row->Bounds;
            return std::nullopt;
        };
    }

    Common::ResultStr<CookedWorld> CookWorld( const SceneSerialized&                        scene,
                                              std::span<const Common::Utils::AssetRegistry> registries )
    {
        using Result = CookedWorld;
        if ( !scene.WorldPartition.has_value() )
            return Common::MakeError<Result>( "'" + scene.SceneName +
                                              "' states no WorldPartition block, so it has no cells to cook" );

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

        const Rules::WorldPartitionPlan plan =
             Rules::PlanWorldPartition( records, *scene.WorldPartition, BoundsFrom( registries ) );
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
                row.File   = std::string( kAlwaysLoadedFileName );
                row.Reason = ReasonName( plan.Composites.at( plan.AlwaysLoaded[unit] ).Reason );
            }
            else
            {
                const Rules::PlannedCell& cell = plan.Cells.at( unit - plan.AlwaysLoaded.size() );
                row.File                       = CellFileName( cell );
                row.Level                      = cell.Level;
                row.X                          = cell.Cell.X;
                row.Z                          = cell.Cell.Z;
                row.Square                     = cell.Square;
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

        // The references that cross a unit, from the one register every reader of them uses.
        for ( std::size_t record = 0; record < records.size(); ++record )
            for ( const Rules::EntityReferenceRow& reference : Rules::kEntityReferences )
            {
                const auto   block = Rules::Detail::BlockOf( records[record], reference.ComponentKey );
                Common::UUID target;
                if ( !block.has_value() || !Rules::Detail::ReadReference( *block, reference.Field, target ) ||
                     target.IsNull() )
                    continue;
                const auto found = byId.find( target );
                if ( found == byId.end() || unitOf[found->second] == unitOf[record] )
                    continue;
                if ( reference.Kind == Rules::ReferenceKind::Containment )
                    return Common::MakeError<Result>(
                         "record " + std::to_string( record ) + " ('" + records[record].Tag.value_or( "" ) +
                         "') of '" + scene.SceneName + "' is held by a " + std::string( reference.ComponentKey ) +
                         "." + std::string( reference.Field ) +
                         " in another unit; containment never crosses one" );
                index.References.push_back(
                     { static_cast<std::uint64_t>( *records[record].id ), static_cast<std::uint64_t>( target ),
                       static_cast<std::uint32_t>( unitOf[record] ),
                       static_cast<std::uint32_t>( unitOf[found->second] ), std::string( reference.ComponentKey ),
                       std::string( reference.Field ) } );
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
            auto wrapped = Wrap( CC::ContentKind::WorldCell, scene.SceneName, name, rfl::json::write( payload ) );
            if ( !wrapped )
                return Common::MakeError<CookedWorld>( wrapped.GetError() );
            CookedFile file{ name, wrapped.ExtractValue() };
            index.Files.push_back(
                 { name, file.Bytes.size(), Common::Utils::Crc32c( file.Bytes.data(), file.Bytes.size() ) } );
            cooked.Files.push_back( std::move( file ) );
        }
        auto wrappedIndex =
             Wrap( CC::ContentKind::WorldIndex, scene.SceneName, kIndexFileName, rfl::json::write( index ) );
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
        auto parsed = rfl::json::read<WorldIndex>( payload.GetValue() );
        if ( !parsed )
            return Common::MakeError<WorldIndex>( "'" + name +
                                                  "' is not a readable world index: " + parsed.error().what() );
        WorldIndex index = std::move( parsed.value() );

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
        return Common::MakeSuccess( std::move( index ) );
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

        auto parsed = rfl::json::read<CellPayload>( payload.GetValue() );
        if ( !parsed )
            return Common::MakeError<CellPayload>( "'" + name +
                                                   "' is not a readable cell file: " + parsed.error().what() );
        CellPayload cell = std::move( parsed.value() );

        std::vector<std::string>   units;
        std::vector<std::uint64_t> ids;
        for ( const IndexUnit& unit : index.Units )
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

    CookedCellSource::CookedCellSource( const WorldIndex& index, FileReader reader )
         : m_Index( &index ), m_Reader( std::move( reader ) )
    {
    }

    namespace
    {
        // Unit @p unit's records out of its file's payload: a run of them, after the units the file lists
        // before it.
        std::vector<Assets::EntityData> UnitSlice( const WorldIndex& index, std::size_t unit,
                                                   const CellPayload& cell )
        {
            const IndexUnit& wanted = index.Units[unit];
            std::size_t      offset = 0;
            for ( std::size_t other = 0; other < unit; ++other )
                if ( index.Units[other].File == wanted.File )
                    offset += index.Units[other].Ids.size();
            return std::vector<Assets::EntityData>(
                 cell.Records.begin() + static_cast<std::ptrdiff_t>( offset ),
                 cell.Records.begin() + static_cast<std::ptrdiff_t>( offset + wanted.Ids.size() ) );
        }

        Common::ResultStr<CellPayload> ReadUnitFile( const WorldIndex& index, const FileReader& reader,
                                                     const std::string& file )
        {
            auto bytes = reader( file );
            if ( !bytes )
                return Common::MakeError<CellPayload>( bytes.GetError() );
            return ReadCellFile( index, file, bytes.GetValue() );
        }

        // The scene-wide part of the index and the records of units [0, @p units), each file read once.
        Common::ResultStr<SceneSerialized> Assemble( const WorldIndex& index, const FileReader& reader,
                                                     std::size_t units )
        {
            SceneSerialized scene;
            scene.SceneName      = index.SceneName;
            scene.Settings       = index.Settings;
            scene.Header         = index.Header;
            scene.WorldPartition = index.WorldPartition;

            std::map<std::string, CellPayload> read;
            for ( std::size_t unit = 0; unit < units; ++unit )
            {
                const std::string& file = index.Units[unit].File;
                auto               held = read.find( file );
                if ( held == read.end() )
                {
                    auto cell = ReadUnitFile( index, reader, file );
                    if ( !cell )
                        return Common::MakeError<SceneSerialized>( cell.GetError() );
                    held = read.emplace( file, cell.ExtractValue() ).first;
                }
                for ( auto& record : UnitSlice( index, unit, held->second ) )
                    scene.Entities.push_back( std::move( record ) );
            }
            return Common::MakeSuccess( std::move( scene ) );
        }

        std::optional<Rules::AlwaysLoadedReason> ReasonNamed( std::string_view name )
        {
            for ( const auto reason : { Rules::AlwaysLoadedReason::Author, Rules::AlwaysLoadedReason::Component,
                                        Rules::AlwaysLoadedReason::NoFit, Rules::AlwaysLoadedReason::NoGrid } )
                if ( ReasonName( reason ) == name )
                    return reason;
            return std::nullopt;
        }
    } // namespace

    Common::ResultStr<std::vector<Assets::EntityData>> CookedCellSource::UnitRecords( std::size_t unit ) const
    {
        using Result = std::vector<Assets::EntityData>;
        if ( unit >= m_Index->Units.size() )
            return Common::MakeError<Result>( "world '" + m_Index->SceneName + "' has no unit " +
                                              std::to_string( unit ) + " (it has " +
                                              std::to_string( m_Index->Units.size() ) + ")" );
        auto cell = ReadUnitFile( *m_Index, m_Reader, m_Index->Units[unit].File );
        if ( !cell )
            return Common::MakeError<Result>( cell.GetError() );
        return Common::MakeSuccess( UnitSlice( *m_Index, unit, cell.GetValue() ) );
    }

    Common::ResultStr<SceneSerialized> AssembleWorld( const WorldIndex& index, const FileReader& reader )
    {
        return Assemble( index, reader, index.Units.size() );
    }

    Common::ResultStr<SceneSerialized> AssembleAlwaysLoaded( const WorldIndex& index, const FileReader& reader )
    {
        std::size_t alwaysLoaded = 0;
        while ( alwaysLoaded < index.Units.size() && index.Units[alwaysLoaded].Reason.has_value() )
            ++alwaysLoaded;
        return Assemble( index, reader, alwaysLoaded );
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
                composite.Reason = *reason;
                world.Plan.AlwaysLoaded.push_back( placeAt );
                world.AlwaysLoadedRecords += from.Ids.size();
            }
            else
            {
                if ( !from.Level.has_value() || !from.X.has_value() || !from.Z.has_value() ||
                     !from.Square.has_value() )
                    return Common::MakeError<Result>( name +
                                                      " is a cell without its level, coordinate or square" );
                Rules::PlannedCell cell;
                cell.Level  = *from.Level;
                cell.Cell   = Rules::CellCoord{ *from.X, *from.Z };
                cell.Square = *from.Square;
                cell.Composites.push_back( placeAt );
                if ( !world.Plan.Cells.empty() )
                {
                    const Rules::PlannedCell& last = world.Plan.Cells.back();
                    if ( std::make_tuple( last.Level, last.Cell.X, last.Cell.Z ) >=
                         std::make_tuple( cell.Level, cell.Cell.X, cell.Cell.Z ) )
                        return Common::MakeError<Result>( name + " is out of (level, X, Z) order after '" +
                                                          index.Units[unit - 1].Name + "'" );
                }
                composite.Level     = cell.Level;
                composite.Cell      = cell.Cell;
                composite.Footprint = cell.Square;
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
            // Only observation can cross a unit and dangle (the cook refuses a crossing containment).
            const bool observation =
                 std::any_of( std::begin( Rules::kEntityReferences ), std::end( Rules::kEntityReferences ),
                              [&reference]( const Rules::EntityReferenceRow& row )
                              {
                                  return row.Kind == Rules::ReferenceKind::Observation &&
                                         row.ComponentKey == reference.Component && row.Field == reference.Field;
                              } );
            if ( observation )
                world.Observations.emplace_back( from->second.first, to->second.first );
        }
        return Common::MakeSuccess( std::move( world ) );
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

    std::vector<std::string> CanonicalRecords( const SceneSerialized& scene )
    {
        std::vector<std::pair<std::uint64_t, std::string>> keyed;
        keyed.reserve( scene.Entities.size() );
        for ( const auto& record : scene.Entities )
            keyed.emplace_back( record.id.has_value() ? static_cast<std::uint64_t>( *record.id ) : 0,
                                rfl::json::write( record ) );
        std::sort( keyed.begin(), keyed.end() );
        std::vector<std::string> out;
        out.reserve( keyed.size() );
        for ( auto& [id, text] : keyed )
            out.push_back( std::move( text ) );
        return out;
    }
} // namespace Desert::Core::WorldCells
