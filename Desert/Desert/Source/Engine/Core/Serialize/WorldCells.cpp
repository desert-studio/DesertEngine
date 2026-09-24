#include <Engine/Core/Serialize/WorldCells.hpp>

#include <Engine/Assets/ContainerBytes.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyRules.hpp>

#include <Common/Utilities/Crc32c.hpp>

#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <set>
#include <unordered_map>
#include <utility>

namespace Desert::Core::WorldCells
{
    namespace
    {
        using Magic                        = std::array<unsigned char, 4>;
        constexpr Magic       kIndexMagic  = { 'D', 'W', 'I', 'X' };
        constexpr Magic       kCellMagic   = { 'D', 'W', 'C', 'L' };
        constexpr std::size_t kHeaderSize  = 4 + 4 + 8;
        constexpr std::size_t kTrailerSize = 4;

        std::vector<unsigned char> Wrap( const Magic& magic, const std::string& payload )
        {
            std::vector<unsigned char> out;
            out.reserve( kHeaderSize + payload.size() + kTrailerSize );
            out.insert( out.end(), magic.begin(), magic.end() );
            Assets::WriteU32( out, kContainerVersion );
            Assets::WriteU64( out, static_cast<std::uint64_t>( payload.size() ) );
            out.insert( out.end(), payload.begin(), payload.end() );
            Assets::WriteU32( out, Common::Utils::Crc32c( out.data(), out.size() ) );
            return out;
        }

        std::string MagicText( const unsigned char* at )
        {
            std::string text;
            for ( int i = 0; i < 4; ++i )
                text += ( at[i] >= 0x20 && at[i] < 0x7F ) ? static_cast<char>( at[i] ) : '?';
            return text;
        }

        // The payload, once the envelope is believed: magic, version, CHECKSUM, then the length.
        Common::ResultStr<std::string> Unwrap( std::string_view fileName, const Magic& magic,
                                               std::span<const unsigned char> bytes )
        {
            const std::string name( fileName );
            const std::string expected( magic.begin(), magic.end() );
            if ( bytes.size() < kHeaderSize + kTrailerSize )
                return Common::MakeError<std::string>(
                     "'" + name + "' is " + std::to_string( bytes.size() ) + " bytes, shorter than the " +
                     std::to_string( kHeaderSize + kTrailerSize ) + "-byte envelope of a cooked world file" );
            const unsigned char* at = bytes.data();
            if ( std::memcmp( at, magic.data(), magic.size() ) != 0 )
                return Common::MakeError<std::string>( "'" + name + "' has magic '" + MagicText( at ) +
                                                       "', expected '" + expected + "'" );
            const std::uint32_t version = Assets::ReadU32( at + 4 );
            if ( version != kContainerVersion )
                return Common::MakeError<std::string>(
                     "'" + name + "' is cooked-world container version " + std::to_string( version ) +
                     "; this build reads " + std::to_string( kContainerVersion ) + " only. Re-cook it." );
            // The checksum before any length is believed: a flipped bit that keeps the lengths consistent is
            // what the length check cannot see.
            const std::size_t   covered = bytes.size() - kTrailerSize;
            const std::uint32_t stored  = Assets::ReadU32( at + covered );
            const std::uint32_t actual  = Common::Utils::Crc32c( at, covered );
            if ( stored != actual )
            {
                char text[96];
                std::snprintf( text, sizeof( text ), "checksum %08x does not match its contents' %08x", stored,
                               actual );
                return Common::MakeError<std::string>( "'" + name + "' is damaged: " + text + " (" +
                                                       std::to_string( bytes.size() ) + " bytes)" );
            }
            const std::uint64_t payloadBytes = Assets::ReadU64( at + 8 );
            if ( payloadBytes != covered - kHeaderSize )
                return Common::MakeError<std::string>( "'" + name + "' states a payload of " +
                                                       std::to_string( payloadBytes ) + " bytes and holds " +
                                                       std::to_string( covered - kHeaderSize ) );
            return Common::MakeSuccess(
                 std::string( reinterpret_cast<const char*>( at + kHeaderSize ), covered - kHeaderSize ) );
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
        index.SceneVersion      = scene.SceneVersion.value_or( 0 );
        index.UnitVersion       = scene.UnitVersion.value_or( 0 );
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
            CookedFile file{ name, Wrap( kCellMagic, rfl::json::write( payload ) ) };
            index.Files.push_back(
                 { name, file.Bytes.size(), Common::Utils::Crc32c( file.Bytes.data(), file.Bytes.size() ) } );
            cooked.Files.push_back( std::move( file ) );
        }
        cooked.Files.push_back(
             { std::string( kIndexFileName ), Wrap( kIndexMagic, rfl::json::write( index ) ) } );
        return Common::MakeSuccess( std::move( cooked ) );
    }

    Common::ResultStr<WorldIndex> ReadWorldIndex( std::string_view fileName, std::span<const unsigned char> bytes )
    {
        const std::string name( fileName );
        auto              payload = Unwrap( fileName, kIndexMagic, bytes );
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
        auto              payload = Unwrap( fileName, kCellMagic, bytes );
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

    Common::ResultStr<std::vector<Assets::EntityData>> CookedCellSource::UnitRecords( std::size_t unit )
    {
        using Result = std::vector<Assets::EntityData>;
        if ( unit >= m_Index->Units.size() )
            return Common::MakeError<Result>( "world '" + m_Index->SceneName + "' has no unit " +
                                              std::to_string( unit ) + " (it has " +
                                              std::to_string( m_Index->Units.size() ) + ")" );
        const IndexUnit& wanted = m_Index->Units[unit];
        auto             held   = m_Read.find( wanted.File );
        if ( held == m_Read.end() )
        {
            auto bytes = m_Reader( wanted.File );
            if ( !bytes )
                return Common::MakeError<Result>( bytes.GetError() );
            auto cell = ReadCellFile( *m_Index, wanted.File, bytes.GetValue() );
            if ( !cell )
                return Common::MakeError<Result>( cell.GetError() );
            held = m_Read.emplace( wanted.File, cell.ExtractValue() ).first;
        }

        // The unit's records are a run of the file's, after the units the file lists before it.
        std::size_t offset = 0;
        for ( std::size_t other = 0; other < unit; ++other )
            if ( m_Index->Units[other].File == wanted.File )
                offset += m_Index->Units[other].Ids.size();
        const auto& records = held->second.Records;
        return Common::MakeSuccess(
             Result( records.begin() + static_cast<std::ptrdiff_t>( offset ),
                     records.begin() + static_cast<std::ptrdiff_t>( offset + wanted.Ids.size() ) ) );
    }

    Common::ResultStr<SceneSerialized> AssembleWorld( const WorldIndex& index, const FileReader& reader )
    {
        SceneSerialized scene;
        scene.SceneName      = index.SceneName;
        scene.Settings       = index.Settings;
        scene.SceneVersion   = index.SceneVersion;
        scene.UnitVersion    = index.UnitVersion;
        scene.WorldPartition = index.WorldPartition;
        scene.Entities.reserve( static_cast<std::size_t>( index.Records ) );

        CookedCellSource source( index, reader );
        for ( std::size_t unit = 0; unit < index.Units.size(); ++unit )
        {
            auto records = source.UnitRecords( unit );
            if ( !records )
                return Common::MakeError<SceneSerialized>( records.GetError() );
            for ( auto& record : records.ExtractValue() )
                scene.Entities.push_back( std::move( record ) );
        }
        return Common::MakeSuccess( std::move( scene ) );
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
