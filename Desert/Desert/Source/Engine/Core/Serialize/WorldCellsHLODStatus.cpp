// WP-HLOD-HASH: what one HLOD node is built from and whether its saved HLOD is current - THE one answer
// (SavedHLODStatus) Build HLODs, WorldCook, the map check, Play and the editor read. Split from WorldCells.cpp by
// responsibility: the cells and the cook stay there, the HLOD freshness lives here.
#include <Engine/Core/Serialize/WorldCellsHLODStatus.hpp>

#include <Engine/Core/Serialize/WorldPartitionResidencyRules.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Json/Document.hpp>
#include <Common/Json/Json.hpp>
#include <Common/Json/ReadWrite.hpp>
#include <Common/Utilities/Crc32c.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Desert::Core::WorldCells
{
    using Detail::HLODNodeSpec;
    using Detail::HLODWorldSpec;

    namespace
    {
        namespace CC = Common::Content;

        // The content hash of the file registry key @p key names (HLODContentHasher): an asset envelope's from its
        // table of contents - the tag and hash of every section, read with the header - any other file's from its
        // bytes. A file that does not read (deleted, locked, a path that resolves nowhere) is an error naming the
        // key and the file: the build could not read it either, and no number may stand for "no content".
        Common::ResultStr<std::uint64_t> SavedContentHash( const std::string& key )
        {
            const std::filesystem::path file = Common::AssetHandle::PathForStableKey( key );
            {
                std::ifstream                                       in( file, std::ios::binary );
                std::array<std::byte, CC::ASSET_HEADER_SNIFF_BYTES> leading{};
                if ( in &&
                     in.read( reinterpret_cast<char*>( leading.data() ),
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
                        return Common::MakeSuccess<std::uint64_t>(
                             Common::Utils::Crc32c( toc.data(), toc.size() ) );
                    }
                }
            }
            // Not an envelope, or one whose header does not read: its bytes are its content.
            auto bytes = Common::Utils::FileSystem::ReadByteFileContent( file );
            if ( !bytes )
                return Common::MakeError<std::uint64_t>( std::format( "the HLOD input '{}' ({}) does not read: {}",
                                                                      key, file.generic_string(),
                                                                      bytes.GetError() ) );
            return Common::MakeSuccess<std::uint64_t>(
                 Common::Utils::Crc32c( bytes.GetValue().data(), bytes.GetValue().size() ) );
        }

        // THE CONTENT OF THE ASSET KEYS A NODE'S MEMBERS NAME (UE ComputeHLODHash folds each component's mesh and
        // material CRCs; WP-HLOD-HASH): every key resolved in the registries - "handle:<n>" by handle, a GUID text
        // by GUID, a path by its stable key - and walked down the registry's dependency closure (a mesh's
        // materials, a material's textures), every file once, as "<stable key> <SavedContentHash>". A path no
        // registry knows is a file the build reads directly (a landscape tile's height file): its own bytes are
        // hashed. A GUID or handle no registry knows is stated absent - the build cannot read it either.
        class HLODContentHasher
        {
        public:
            explicit HLODContentHasher( std::span<const Common::Utils::AssetRegistry> registries )
                 : m_Registries( registries )
            {
            }

            [[nodiscard]] Common::ResultStr<std::string> LinesOf( const std::set<std::string>& keys )
            {
                std::set<std::string>      files;
                std::set<std::string>      absent;
                std::vector<std::uint64_t> pending;
                const auto                 reach = [&]( const Common::Utils::AssetRegistryEntry& entry )
                {
                    files.insert( entry.Key );
                    pending.insert( pending.end(), entry.Dependencies.begin(), entry.Dependencies.end() );
                };
                for ( const std::string& key : keys )
                {
                    if ( key.starts_with( kHandlePrefix ) )
                    {
                        std::uint64_t     handle = 0;
                        const std::string digits = key.substr( kHandlePrefix.size() );
                        const auto [end, failure] =
                             std::from_chars( digits.data(), digits.data() + digits.size(), handle );
                        const Common::Utils::AssetRegistryEntry* entry =
                             failure == std::errc() && end == digits.data() + digits.size() ? ByHandle( handle )
                                                                                            : nullptr;
                        if ( entry != nullptr )
                            reach( *entry );
                        else
                            absent.insert( key );
                        continue;
                    }
                    if ( auto guid = CC::AssetGuidFromText( key ) )
                    {
                        const Common::Utils::AssetRegistryEntry* entry = nullptr;
                        for ( const Common::Utils::AssetRegistry& registry : m_Registries )
                            if ( entry == nullptr )
                                entry = registry.FindByGuidReference( guid.GetValue(), {} );
                        if ( entry != nullptr )
                            reach( *entry );
                        else
                            absent.insert( key );
                        continue;
                    }
                    const Common::Utils::AssetRegistryEntry* entry = nullptr;
                    for ( const Common::Utils::AssetRegistry& registry : m_Registries )
                        if ( entry == nullptr )
                            entry = registry.FindByReference( 0, key );
                    if ( entry != nullptr )
                        reach( *entry );
                    else
                        files.insert( Common::AssetHandle::StableKeyForPath( key ) );
                }
                std::set<std::uint64_t> visited;
                while ( !pending.empty() )
                {
                    const std::uint64_t handle = pending.back();
                    pending.pop_back();
                    if ( !visited.insert( handle ).second )
                        continue;
                    if ( const Common::Utils::AssetRegistryEntry* entry = ByHandle( handle ) )
                        reach( *entry );
                    else
                        absent.insert( std::format( "{}{}", kHandlePrefix, handle ) );
                }
                std::string text;
                for ( const std::string& file : files )
                {
                    auto known = m_Hashes.find( file );
                    if ( known == m_Hashes.end() )
                    {
                        auto hash = SavedContentHash( file );
                        if ( !hash )
                            return Common::MakeError<std::string>( hash.GetError() );
                        known = m_Hashes.emplace( file, hash.GetValue() ).first;
                    }
                    text += std::format( "{} {:016x}\n", file, known->second );
                }
                for ( const std::string& key : absent )
                    text += key + " absent\n";
                return Common::MakeSuccess( std::move( text ) );
            }

        private:
            static constexpr std::string_view kHandlePrefix = "handle:";

            [[nodiscard]] const Common::Utils::AssetRegistryEntry* ByHandle( std::uint64_t handle ) const
            {
                for ( const Common::Utils::AssetRegistry& registry : m_Registries )
                    if ( const Common::Utils::AssetRegistryEntry* entry = registry.FindByHandle( handle ) )
                        return entry;
                return nullptr;
            }

            std::span<const Common::Utils::AssetRegistry>  m_Registries;
            std::unordered_map<std::string, std::uint64_t> m_Hashes; // by stable key
        };

        // " <id>: <T> <R> <S>" for @p index and every ancestor up its parent chain (they place it), each component
        // in hex ("{:a}": the exact bits, no rounding), "-" for one the record does not state. Bounded by the
        // file: a parent cycle (which the loader refuses) cannot hang the checksum.
        void AppendPlacement( std::string& text, std::span<const Assets::EntityData> records,
                              const std::unordered_map<Common::UUID, std::size_t>& byId, std::size_t index )
        {
            const auto vec = []( const std::optional<glm::vec3>& v ) -> std::string
            { return v.has_value() ? std::format( "{:a},{:a},{:a}", v->x, v->y, v->z ) : std::string( "-" ); };
            std::optional<std::size_t> at = index;
            for ( std::size_t depth = 0; at.has_value() && depth <= records.size(); ++depth )
            {
                const Assets::EntityData& record = records[*at];
                text += std::format( " {}: {} {} {}",
                                     record.id.has_value() ? static_cast<std::uint64_t>( *record.id ) : 0,
                                     vec( record.Translation ), vec( record.Rotation ), vec( record.Scale ) );
                at.reset();
                if ( record.parent.has_value() )
                    if ( const auto parent = byId.find( *record.parent ); parent != byId.end() )
                        at = parent->second;
            }
            text += '\n';
        }

        // THE CHECKSUM OF ONE WANTED NODE (UE FWorldPartitionHLODUtilities::ComputeHLODHash): per part its layer's
        // name, type, builder and the builder's canonical settings (IHLODBuilder::SettingsKey); per member that
        // brings something (ReadHLODMember) its id, holes, placement chain, batch keys and instances, and a module
        // record's own inputs (HLODBuilderComponent::Inputs) with the placement of every record they name; the
        // distance it is seen from and the names its file and ids go by; the content
        // of every asset key read (HLODContentHasher); kHLODBuilderVersion. Nothing else of the world: a field no
        // builder reads cannot make an HLOD stale.
        Common::ResultStr<std::uint32_t> NodeChecksum( const HLODWorldSpec& spec, const SceneSerialized& scene,
                                                       std::size_t n, const Rules::HLODBuilderRegistry& builders,
                                                       HLODContentHasher& contents )
        {
            using Result                  = std::uint32_t;
            const HLODNodeSpec&   node    = spec.Nodes[n];
            const std::string&    name    = spec.Hierarchy.Nodes[n].Name;
            const auto&           records = scene.Entities;
            std::string           text;
            std::set<std::string> keys;
            for ( const Rules::CellHLODPart& part : node.Parts )
            {
                auto builder = builders.BuilderOf( *part.Layer );
                if ( !builder )
                    return Common::MakeError<Result>(
                         std::format( "'{}', {}: {}", scene.SceneName, name, builder.GetError() ) );
                auto settings = builder.GetValue()->SettingsKey(
                     part.Layer->Settings.has_value() ? &*part.Layer->Settings : nullptr );
                if ( !settings )
                    return Common::MakeError<Result>( std::format( "'{}', {}: HLOD layer '{}': {}",
                                                                   scene.SceneName, name, part.Layer->Name,
                                                                   settings.GetError() ) );
                text += std::format( "part '{}' type {} builder '{}' settings {}\n", part.Layer->Name,
                                     static_cast<int>( part.Layer->Type ), Rules::HLODBuilderNameOf( *part.Layer ),
                                     settings.GetValue() );
                for ( const std::size_t index : part.Members )
                {
                    // SpecHLODs reads every member of a unit whose cell has a layer; a parent's sources are such
                    // units. A member it did not read is a broken invariant, refused by name.
                    if ( !spec.Members.at( index ).has_value() )
                        return Common::MakeError<Result>( std::format(
                             "'{}', {}: record {} (id {}) was never read as an HLOD member", scene.SceneName, name,
                             index, static_cast<std::uint64_t>( *records[index].id ) ) );
                    const Rules::HLODMember& member = *spec.Members.at( index );
                    if ( !member.Relevant && !member.FoliageLeftOut )
                        continue;
                    text += std::format( "member {}{}", static_cast<std::uint64_t>( *records[index].id ),
                                         member.FoliageLeftOut ? " foliage-left-out" : "" );
                    for ( const Rules::HLODExclusion hole : member.Holes )
                        text += std::format( " hole {}", static_cast<int>( hole ) );
                    AppendPlacement( text, records, spec.Planned.ById, index );
                    for ( const Rules::HLODMemberBatch& batch : member.Batches )
                    {
                        text += "batch " + Common::Json::Write( batch.Key );
                        if ( batch.Instances.has_value() )
                            text += std::format( " instances {} {:08x}", batch.Instances->size(),
                                                 Common::Utils::Crc32c( batch.Instances->data(),
                                                                        batch.Instances->size() *
                                                                             sizeof( std::array<float, 16> ) ) );
                        text += '\n';
                    }
                    if ( member.OwnBuilder != nullptr )
                    {
                        auto inputs = member.OwnBuilder->Inputs( records, spec.World, index );
                        if ( !inputs )
                            return Common::MakeError<Result>(
                                 std::format( "'{}', {}: record {} ({}): {}", scene.SceneName, name,
                                              static_cast<std::uint64_t>( *records[index].id ),
                                              member.OwnBuilder->Component, inputs.GetError() ) );
                        text +=
                             std::format( "module {} {}\n", member.OwnBuilder->Component, inputs.GetValue().Text );
                        for ( const std::size_t placed : inputs.GetValue().Placed )
                        {
                            text += "placed";
                            AppendPlacement( text, records, spec.Planned.ById, placed );
                        }
                        keys.insert( inputs.GetValue().Content.begin(), inputs.GetValue().Content.end() );
                    }
                    keys.insert( member.Content.begin(), member.Content.end() );
                }
            }
            text += std::format( "visible from {:a} row {} cooked {} file {}\n", node.MinVisibleDistance, name,
                                 node.CookedName, node.FileName );
            auto content = contents.LinesOf( keys );
            if ( !content )
                return Common::MakeError<Result>(
                     std::format( "'{}', {}: {}", scene.SceneName, name, content.GetError() ) );
            text += content.GetValue();
            text += std::format( "builder {}\n", kHLODBuilderVersion );
            return Common::MakeSuccess( Common::Utils::Crc32c( text.data(), text.size() ) );
        }
    } // namespace

    namespace Detail
    {
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

        Common::ResultStr<HLODWorldSpec> SpecHLODs( const SceneSerialized&                        scene,
                                                    std::span<const Rules::EntityDescriptor>      descriptors,
                                                    std::span<const Common::Utils::AssetRegistry> registries )
        {
            using Result = HLODWorldSpec;
            auto planned = PlanHLODs( scene, descriptors, registries );
            if ( !planned )
                return Common::MakeError<Result>( planned.GetError() );
            HLODWorldSpec spec;
            spec.Planned                              = planned.ExtractValue();
            const WorldPartitionSerialized& partition = *scene.WorldPartition;
            if ( auto layers = Rules::ValidateHLODLayers( partition ); !layers )
                return Common::MakeError<Result>( "'" + scene.SceneName + "': " + layers.GetError() );
            auto hierarchy = Rules::PlanHLODHierarchy( partition, spec.Planned.Plan );
            if ( !hierarchy )
                return Common::MakeError<Result>( "'" + scene.SceneName + "': " + hierarchy.GetError() );
            spec.Hierarchy      = hierarchy.ExtractValue();
            const auto& records = scene.Entities;
            spec.World          = Rules::Detail::ComposeWorld( records, spec.Planned.ById );
            spec.Members.resize( records.size() );
            const Rules::HLODMemberSources sources{ CustomShaderFrom( registries ),
                                                    FoliageInHLODFrom( registries ) };

            const HLODPlan&                               hp    = spec.Planned;
            const std::size_t                             first = hp.Plan.AlwaysLoaded.size();
            std::vector<std::vector<Rules::CellHLODPart>> partsOf( hp.Members.size() );
            std::vector<bool>                             unitWanted( hp.Members.size(), false );
            for ( std::size_t unit = first; unit < hp.Members.size(); ++unit )
            {
                // The layer is the cell's PARTITION's first HLOD setup (WP22c, WP-HASH-SET), and a composite's own
                // where it names one (WP-SETTINGS-UI, Rules::CellHLODParts).
                auto parts = Rules::CellHLODParts( partition, hp.Plan, hp.Plan.Cells.at( unit - first ),
                                                   hp.Members[unit] );
                if ( !parts )
                    return Common::MakeError<Result>( "'" + scene.SceneName + "', " + hp.Names[unit] + ": " +
                                                      parts.GetError() );
                partsOf[unit] = parts.ExtractValue();
                if ( partsOf[unit].empty() )
                    continue; // a grid with no layer: its cells have no HLOD node
                for ( const std::size_t member : hp.Members[unit] )
                {
                    if ( !spec.Members[member].has_value() )
                    {
                        auto read = Rules::ReadHLODMember( records, spec.World, member, sources );
                        if ( !read )
                            return Common::MakeError<Result>( std::format(
                                 "'{}', {}: record {} (id {}): {}", scene.SceneName, hp.Names[unit], member,
                                 static_cast<std::uint64_t>( *records[member].id ), read.GetError() ) );
                        spec.Members[member] = read.ExtractValue();
                    }
                    if ( spec.Members[member]->Relevant )
                        unitWanted[unit] = true;
                }
            }

            spec.Nodes.resize( spec.Hierarchy.Nodes.size() );
            for ( std::size_t n = 0; n < spec.Hierarchy.Nodes.size(); ++n )
            {
                const Rules::HLODNode& node = spec.Hierarchy.Nodes[n];
                HLODNodeSpec&          out  = spec.Nodes[n];
                if ( !node.ForHLODCell )
                {
                    const std::size_t         unit = node.StandsFor;
                    const Rules::PlannedCell& cell = hp.Plan.Cells.at( unit - first );
                    out.Members                    = hp.Members.at( unit );
                    out.Parts                      = partsOf.at( unit );
                    out.MinVisibleDistance         = Rules::HLODMinVisibleDistance( partition, hp.Plan, unit );
                    out.CookedName                 = HLODFileName( CellFileName( partition, cell ) );
                    out.FileName                   = BuiltHLODFileName( partition, cell );
                    out.Wanted                     = unitWanted.at( unit );
                    continue;
                }
                // THE PARENTS (WP-FAR-8, WorldPartitionHLODHierarchy.hpp): per HLOD cell of a spatially loaded
                // layer, the HLOD of its next setup's layer, built from the records of every world cell beneath
                // it and seen from the child layer's LoadingRange on.
                for ( const std::size_t unit : node.SourceUnits )
                {
                    out.Members.insert( out.Members.end(), hp.Members.at( unit ).begin(),
                                        hp.Members.at( unit ).end() );
                    out.Wanted = out.Wanted || unitWanted.at( unit );
                }
                std::sort( out.Members.begin(), out.Members.end() );
                // A parent exists only for a layer of HLODLayers (PlanHLODHierarchy).
                const std::vector<HLODLayerSerialized>& layers = *partition.HLODLayers;
                const Rules::HLODCell&                  child  = spec.Hierarchy.Cells.at( node.StandsFor );
                const std::string stem = ParentHLODFileStem( partition, spec.Hierarchy, node );
                out.Parts              = { Rules::CellHLODPart{ &layers.at( node.Layer ), out.Members } };
                out.MinVisibleDistance = *layers.at( child.Layer ).LoadingRange;
                out.CookedName         = stem + std::string( kCellExtension );
                out.FileName           = stem + std::string( kBuiltHLODExtension );
            }
            return Common::MakeSuccess( std::move( spec ) );
        }

        Common::BoolResultStr StatusOf( HLODWorldStatus& status, const HLODWorldSpec& spec,
                                        const SceneSerialized& scene, const BuiltHLODIndex* index,
                                        std::span<const Common::Utils::AssetRegistry> registries,
                                        const Rules::HLODBuilderRegistry&             builders )
        {
            std::unordered_map<std::string, const BuiltHLODCell*> rows;
            if ( index != nullptr )
                for ( const BuiltHLODCell& row : index->Cells )
                    rows.emplace( row.Unit, &row );
            HLODContentHasher     contents( registries );
            std::set<std::string> wanted;
            status.Nodes.clear();
            status.Orphans.clear();
            for ( std::size_t n = 0; n < spec.Nodes.size(); ++n )
            {
                HLODNodeStatus state;
                state.Name = spec.Hierarchy.Nodes[n].Name;
                state.Node = n;
                if ( spec.Nodes[n].Wanted )
                {
                    auto checksum = NodeChecksum( spec, scene, n, builders, contents );
                    if ( !checksum )
                        return Common::MakeError( checksum.GetError() );
                    state.Checksum = checksum.GetValue();
                    wanted.insert( state.Name );
                    const auto row = rows.find( state.Name );
                    if ( row == rows.end() )
                        state.State = HLODNodeState::Missing;
                    else
                    {
                        state.Row   = row->second;
                        state.State = row->second->BuiltFrom == state.Checksum ? HLODNodeState::Current
                                                                               : HLODNodeState::Stale;
                    }
                }
                status.Nodes.push_back( std::move( state ) );
            }
            if ( index != nullptr )
                for ( const BuiltHLODCell& row : index->Cells )
                    if ( !wanted.contains( row.Unit ) )
                        status.Orphans.push_back( row.Unit );
            return BOOLSUCCESS;
        }
    } // namespace Detail

    std::string_view HLODNodeStateName( const HLODNodeState state )
    {
        // Indexed by the enum, in its order.
        static constexpr std::array<std::string_view, 4> kNames{ "no source", "current", "missing", "stale" };
        return kNames.at( static_cast<std::size_t>( state ) );
    }

    std::size_t HLODWorldStatus::Wanted() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
             Nodes, []( const HLODNodeStatus& node ) { return node.State != HLODNodeState::NoSource; } ) );
    }

    std::size_t HLODWorldStatus::Outdated() const
    {
        return OutdatedNames().size();
    }

    std::vector<std::string> HLODWorldStatus::OutdatedNames() const
    {
        std::vector<std::string> names;
        for ( const HLODNodeStatus& node : Nodes )
            if ( node.State == HLODNodeState::Missing || node.State == HLODNodeState::Stale )
                names.push_back( std::format( "{} {}", node.Name, HLODNodeStateName( node.State ) ) );
        for ( const std::string& orphan : Orphans )
            names.push_back( orphan + " orphan" );
        return names;
    }

    const HLODNodeStatus* HLODWorldStatus::Find( const std::string_view name ) const
    {
        for ( const HLODNodeStatus& node : Nodes )
            if ( node.Name == name )
                return &node;
        return nullptr;
    }

    Common::ResultStr<HLODWorldStatus> SavedHLODStatus( const SceneSerialized&                        scene,
                                                        std::span<const Rules::EntityDescriptor>      descriptors,
                                                        const BuiltHLODIndex*                         index,
                                                        std::span<const Common::Utils::AssetRegistry> registries,
                                                        const Rules::HLODBuilderRegistry&             builders )
    {
        auto specified = Detail::SpecHLODs( scene, descriptors, registries );
        if ( !specified )
            return Common::MakeError<HLODWorldStatus>( specified.GetError() );
        Detail::HLODWorldSpec spec = specified.ExtractValue();
        HLODWorldStatus       status;
        if ( auto judged = Detail::StatusOf( status, spec, scene, index, registries, builders ); !judged )
            return Common::MakeError<HLODWorldStatus>( judged.GetError() );
        status.Plan      = std::move( spec.Planned.Plan );
        status.Hierarchy = std::move( spec.Hierarchy );
        return Common::MakeSuccess( std::move( status ) );
    }
} // namespace Desert::Core::WorldCells
