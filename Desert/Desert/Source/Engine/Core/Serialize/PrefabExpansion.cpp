#include <Engine/Core/Serialize/PrefabExpansion.hpp>

#include <Engine/Core/Serialize/EntityReferenceRemap.hpp>

#include <Engine/Assets/Prefab/PackedPrefab.hpp>
#include <Engine/Assets/Prefab/PrefabFormat.hpp>
#include <Engine/Assets/Prefab/PrefabOverrides.hpp>
#include <Engine/Core/Serialize/SceneStitchRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <array>
#include <optional>
#include <unordered_map>
#include <vector>

#include <cstdint>
#include <format>
#include <functional>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <Common/Json/Detail/RflBridge.hpp>

namespace Desert::Core::WorldCells
{
    namespace
    {
        constexpr std::size_t kNone = static_cast<std::size_t>( -1 );

        // FNV-1a 64 over the instance id's bytes then the path key's: stable across runs and machines.
        Common::UUID ExpandedId( std::uint64_t seed, std::string_view key )
        {
            std::uint64_t hash = 14695981039346656037ull;
            const auto    mix  = [&hash]( unsigned char byte )
            {
                hash ^= byte;
                hash *= 1099511628211ull;
            };
            for ( int shift = 0; shift < 64; shift += 8 )
                mix( static_cast<unsigned char>( ( seed >> shift ) & 0xffu ) );
            for ( const char c : key )
                mix( static_cast<unsigned char>( c ) );
            return Common::UUID( hash );
        }

        struct BodyRecord
        {
            Assets::EntityData        Data;
            std::vector<Common::UUID> Path;           // the loader's SourcePath: prefix + the record's own id
            std::size_t               Parent = kNone; // index into the body
            std::size_t               Scope  = 0;     // which prefab file instance stated it (ids are per file)
            Common::UUID              Local;          // the id the file states (or the stitch minted)
            // The root of a prefab file's body - the top instance's or a nested one's - and, for such a root, how
            // that instance streams: its file's RuntimeBehavior, or the nesting / instance record's override.
            bool                          InstanceRoot = false;
            Assets::PrefabRuntimeBehavior Behavior     = Assets::PrefabRuntimeBehavior::Partitioned;
        };

        struct Expander
        {
            const PrefabReader&      Reader;
            std::vector<std::string> Stack;
            std::size_t              Scopes = 0;

            static std::string KeyOf( const std::vector<Common::UUID>& prefix,
                                      const std::vector<Common::UUID>& path )
            {
                std::vector<Common::UUID> full = prefix;
                full.insert( full.end(), path.begin(), path.end() );
                return Assets::PrefabPathKey( full );
            }

            // Lays @p overrides (addressed relative to @p prefix) over @p body; a miss is refused by name.
            static Common::ResultStr<bool> Layer( std::vector<BodyRecord>& body, std::size_t from,
                                                  const std::vector<Assets::PrefabOverrideData>& overrides,
                                                  const std::vector<Common::UUID>&               prefix,
                                                  std::string_view                               prefab )
            {
                std::map<std::string, std::size_t> byPath;
                for ( std::size_t record = from; record < body.size(); ++record )
                    byPath.emplace( Assets::PrefabPathKey( body[record].Path ), record );
                for ( const Assets::PrefabOverrideData& over : overrides )
                {
                    const auto hit = byPath.find( KeyOf( prefix, over.Path ) );
                    if ( hit == byPath.end() )
                        return Common::MakeFormattedError<bool>(
                             "an override of prefab '{}' addresses '{}', which the prefab no longer contains; the "
                             "cook refuses to write a world that differs from the editor's",
                             prefab, KeyOf( prefix, over.Path ) );
                    Assets::LayerOverrideOntoRecord( body[hit->second].Data, over );
                }
                return Common::MakeSuccess( true );
            }

            Common::ResultStr<std::vector<BodyRecord>> Body( std::string_view                 stated,
                                                             const std::vector<Common::UUID>& prefix )
            {
                using Result = std::vector<BodyRecord>;
                for ( const std::string& open : Stack )
                    if ( open == stated )
                        return Common::MakeFormattedError<Result>( "prefab '{}' nests itself", stated );
                auto read = Reader( stated );
                if ( !read )
                    return Common::MakeFormattedError<Result>( "prefab '{}' does not read: {}", stated,
                                                               read.GetError() );
                const Assets::PrefabData& prefab  = read.GetValue();
                const auto&               records = prefab.Entities;
                const std::size_t         scope   = Scopes++;
                std::uint64_t             minted  = 0;
                const Rules::StitchPlan   plan    = Rules::PlanSceneStitch(
                     records, [&]() { return ExpandedId( ++minted, std::string( stated ) + "#minted" ); },
                     Rules::PrefabRecordPolicy::InstantiatedLater );
                if ( plan.Created.empty() )
                    return Common::MakeFormattedError<Result>( "prefab '{}' has no entity", stated );
                if ( plan.Shadowed > 0 )
                    return Common::MakeFormattedError<Result>( "prefab '{}': {} record(s) repeat an id", stated,
                                                               plan.Shadowed );
                for ( const Assets::EntityData& record : records )
                    if ( record.RuntimeBehavior.has_value() && !record.PrefabPath.has_value() )
                        return Common::MakeFormattedError<Result>(
                             "prefab '{}': record '{}' states a RuntimeBehavior and instances no prefab; only an "
                             "instance streams by one",
                             stated, record.Tag.value_or( "" ) );

                Result body( plan.Created.size() );
                for ( std::size_t slot = 0; slot < plan.Created.size(); ++slot )
                {
                    BodyRecord& out = body[slot];
                    out.Data        = records[plan.Created[slot].Record];
                    out.Local       = plan.Created[slot].Id;
                    out.Scope       = scope;
                    out.Path        = prefix;
                    out.Path.push_back( out.Local );
                }
                for ( const Rules::PlannedLoad& load : plan.Loads )
                    body[load.Target].Parent = load.Parent == Rules::kNoSlot ? kNone : load.Parent;
                body.front().InstanceRoot = true;
                body.front().Behavior     = prefab.RuntimeBehavior;

                Stack.emplace_back( stated );
                for ( const Rules::PlannedPrefab& nesting : plan.PrefabRecords )
                {
                    const Assets::EntityData& data = records[nesting.Record];
                    if ( !data.id.has_value() )
                        return Common::MakeFormattedError<Result>(
                             "prefab '{}' nests '{}' from a record with no id; nothing in it is addressable",
                             stated, *data.PrefabPath );
                    std::vector<Common::UUID> nestedPrefix = prefix;
                    nestedPrefix.push_back( *data.id );
                    auto nested = Body( *data.PrefabPath, nestedPrefix );
                    if ( !nested )
                        return Common::MakeFormattedError<Result>( "prefab '{}': {}", stated, nested.GetError() );
                    const std::size_t from = body.size();
                    for ( BodyRecord record :
                          nested.GetValue() ) // GetValue is const-only: copy, then move into the body
                    {
                        if ( record.Parent != kNone )
                            record.Parent += from;
                        body.push_back( std::move( record ) );
                    }
                    body[from].Parent = nesting.Parent == Rules::kNoSlot ? kNone : nesting.Parent;
                    // The nesting record's override of how the nested instance streams (absent = its file's).
                    body[from].Behavior =
                         Assets::ResolvePrefabRuntimeBehavior( data.RuntimeBehavior, body[from].Behavior );
                    if ( data.PrefabOverrides.has_value() )
                        if ( auto layered =
                                  Layer( body, from, *data.PrefabOverrides, nestedPrefix, *data.PrefabPath );
                             !layered )
                            return Common::MakeError<Result>( layered.GetError() );
                }
                Stack.pop_back();
                return Common::MakeSuccess( std::move( body ) );
            }
        };

        // Rewrites every kEntityReferences field of @p record that names a body record of its own scope - through
        // Rules::RemapEntityReferences, the one remap the factory and the capture use as well.
        void RemapReferences( Assets::EntityData& record, std::size_t scope,
                              const std::map<std::pair<std::size_t, std::uint64_t>, Common::UUID>& remap )
        {
            Rules::RemapEntityReferences( record,
                                          [&remap, scope]( const Common::UUID& stated ) -> std::optional<Common::UUID>
                                          {
                                              const auto hit =
                                                   remap.find( { scope, static_cast<std::uint64_t>( stated ) } );
                                              if ( hit == remap.end() )
                                                  return std::nullopt;
                                              return hit->second;
                                          } );
        }
    } // namespace

    Common::ResultStr<ExpandedScene> ExpandPrefabInstances( const SceneSerialized& scene,
                                                            const PrefabReader&    reader )
    {
        using Result = ExpandedScene;
        ExpandedScene expanded;
        expanded.Scene = scene;
        expanded.Scene.Entities.clear();

        std::unordered_set<std::uint64_t> taken;
        for ( const Assets::EntityData& record : scene.Entities )
            if ( record.id.has_value() )
                taken.insert( static_cast<std::uint64_t>( *record.id ) );
        // Every source record's world matrix, composed once when the first packed instance needs its root's.
        std::optional<std::vector<glm::mat4>> sourceWorld;

        for ( std::size_t source = 0; source < scene.Entities.size(); ++source )
        {
            const Assets::EntityData& instance = scene.Entities[source];
            auto&                     out      = expanded.Scene.Entities;
            if ( !instance.PrefabPath.has_value() )
            {
                if ( instance.RuntimeBehavior.has_value() )
                    return Common::MakeFormattedError<Result>(
                         "record {} ('{}') of '{}' states a RuntimeBehavior and instances no prefab; only an "
                         "instance streams by one",
                         source, instance.Tag.value_or( "" ), scene.SceneName );
                expanded.Slots.push_back( ExpandedSlot{ out.size(), 1, false } );
                out.push_back( instance );
                continue;
            }
            const std::string what =
                 std::format( "record {} ('{}') of '{}'", source, instance.Tag.value_or( "" ), scene.SceneName );
            if ( !reader )
                return Common::MakeFormattedError<Result>(
                     "{} instances prefab '{}' and the cook was given no prefab reader", what,
                     *instance.PrefabPath );
            if ( !instance.id.has_value() || instance.id->IsNull() )
                return Common::MakeFormattedError<Result>( "{} instances prefab '{}' with no id", what,
                                                           *instance.PrefabPath );
            const auto stated = Assets::StatedInstanceTransform( instance );
            if ( !stated )
                return Common::MakeFormattedError<Result>( "{}: {}", what, stated.GetError() );

            // A PACKED INSTANCE (EntityData::PrefabPacked) streams as what the editor draws: its root and one
            // InstancedStaticMesh record per group of the prefab's packed form (UE: a Packed Level Actor in a
            // World Partition cell is one actor of ISM components, not the level it was packed from). The form is
            // the asset's own rule - the file's stated form while its checksum is the body's, otherwise the body
            // packed (PrefabAsset::AdoptOrRepack) - and a record the loader would place UNPACKED (overrides, a
            // prefab that no longer packs) is expanded unpacked below, so the cooked world is the edited one.
            if ( instance.PrefabPacked.value_or( false ) &&
                 !( instance.PrefabOverrides.has_value() && !instance.PrefabOverrides->empty() ) )
            {
                auto file = reader( *instance.PrefabPath );
                if ( !file )
                    return Common::MakeFormattedError<Result>( "{}: {}", what, file.GetError() );
                const Assets::PrefabData&     prefab = file.GetValue();
                const Assets::PackedPrefabSer form =
                     prefab.Packed.has_value() &&
                               prefab.Packed->SourceHash == Assets::PrefabSourceHash( prefab.Entities )
                          ? *prefab.Packed
                          : Assets::PackPrefab( prefab.Entities );
                if ( !form.Refused.has_value() && !prefab.Entities.empty() )
                {
                    if ( !sourceWorld.has_value() )
                    {
                        std::unordered_map<Common::UUID, std::size_t> byId;
                        for ( std::size_t record = 0; record < scene.Entities.size(); ++record )
                            if ( scene.Entities[record].id.has_value() && !scene.Entities[record].id->IsNull() )
                                byId.emplace( *scene.Entities[record].id, record );
                        sourceWorld = Rules::Detail::ComposeWorld( scene.Entities, byId );
                    }

                    // The root: the instance's own record (id, parent, sibling place, the transform it states),
                    // named as the prefab's root and carrying nothing else - what InstantiatePacked builds.
                    Assets::EntityData root = instance;
                    root.PrefabPath.reset();
                    root.PrefabOverrides.reset();
                    root.PrefabPacked.reset();
                    root.Components.clear();
                    root.Tag = prefab.Entities.front().Tag;

                    // THE SPLIT, as below: each group hangs off the instance's own parent. Its instances are WORLD
                    // matrices (InstancedStaticMeshComponent), the form's root-frame ones composed with the
                    // root's.
                    const glm::mat4     rootWorld = ( *sourceWorld )[source];
                    const std::uint64_t seed      = static_cast<std::uint64_t>( *instance.id );
                    expanded.Slots.push_back( ExpandedSlot{ out.size(), 1 + form.Groups.size(), true } );
                    ++expanded.Instances;
                    out.push_back( std::move( root ) );
                    for ( std::size_t group = 0; group < form.Groups.size(); ++group )
                    {
                        Assets::InstancedStaticMeshComponentSer block = form.Groups[group].Block;
                        if ( block.InstanceTransforms.has_value() )
                            for ( std::array<float, 16>& flat : *block.InstanceTransforms )
                                flat = Rules::Detail::Flatten( rootWorld * glm::make_mat4( flat.data() ) );

                        Assets::EntityData record;
                        record.id = ExpandedId( seed, std::format( "packed/{}", group ) );
                        if ( record.id->IsNull() ||
                             !taken.insert( static_cast<std::uint64_t>( *record.id ) ).second )
                            return Common::MakeFormattedError<Result>( "{}: the expanded id of packed group {} of "
                                                                       "prefab '{}' is null or another record's",
                                                                       what, group, *instance.PrefabPath );
                        record.parent = instance.parent;
                        record.Tag    = form.Groups[group].Name;
                        record.Components[std::string( Rules::kInstancePointsComponent )] =
                             Common::Json::FromStruct( block );
                        out.push_back( std::move( record ) );
                    }
                    continue;
                }
            }

            Expander expander{ reader, {}, 0 };
            auto     read = expander.Body( *instance.PrefabPath, {} );
            if ( !read )
                return Common::MakeFormattedError<Result>( "{}: {}", what, read.GetError() );
            std::vector<BodyRecord> body = read.GetValue(); // a copy: GetValue is const-only
            if ( instance.PrefabOverrides.has_value() )
                if ( auto layered =
                          Expander::Layer( body, 0, *instance.PrefabOverrides, {}, *instance.PrefabPath );
                     !layered )
                    return Common::MakeFormattedError<Result>( "{}: {}", what, layered.GetError() );
            Assets::PlaceInstanceRoot( body.front().Data, stated.GetValue() );
            body.front().Behavior =
                 Assets::ResolvePrefabRuntimeBehavior( instance.RuntimeBehavior, body.front().Behavior );

            // Ids: the root is the instance (as the loader does), the rest stable hashes of their path.
            const std::uint64_t       seed = static_cast<std::uint64_t>( *instance.id );
            std::vector<Common::UUID> ids( body.size() );
            std::map<std::pair<std::size_t, std::uint64_t>, Common::UUID> remap;
            for ( std::size_t record = 0; record < body.size(); ++record )
            {
                ids[record] =
                     record == 0 ? *instance.id : ExpandedId( seed, Assets::PrefabPathKey( body[record].Path ) );
                if ( record != 0 && ( ids[record].IsNull() ||
                                      !taken.insert( static_cast<std::uint64_t>( ids[record] ) ).second ) )
                    return Common::MakeFormattedError<Result>(
                         "{}: the expanded id of '{}' in prefab '{}' is null or another record's", what,
                         Assets::PrefabPathKey( body[record].Path ), *instance.PrefabPath );
                remap.emplace( std::pair{ body[record].Scope, static_cast<std::uint64_t>( body[record].Local ) },
                               ids[record] );
            }

            // THE SPLIT, by each instance's runtime behaviour (WP21-WHOLE; UE
            // ALevelInstance::DesiredRuntimeBehavior). A record is LIFTED out of the instance - it hangs off the
            // instance's own parent, its transform composed up to it - when its parent is an OPENED instance root.
            // The top root is always lifted (it takes the instance's place) and opens when the instance streams
            // Partitioned; a nested instance's root opens when it was lifted itself and streams Partitioned (UE: a
            // Partitioned level instance's actors, a nested Partitioned one's included, are the outer world's).
            // Every other link is kept, and is containment: a Standalone instance stays one composite under its
            // root - one cell, one HLOD source, carried by the root.
            std::vector<char>      lifted( body.size(), 0 ), opened( body.size(), 0 ), resolved( body.size(), 0 );
            std::vector<glm::mat4> liftedLocal( body.size(), glm::mat4( 1.0f ) );
            const std::function<void( std::size_t )> resolve = [&]( std::size_t record )
            {
                if ( resolved[record] != 0 )
                    return;
                resolved[record]       = 1; // before the parent: a corrupt cycle ends here, unopened
                const bool partitioned = body[record].InstanceRoot &&
                                         body[record].Behavior == Assets::PrefabRuntimeBehavior::Partitioned;
                if ( record == 0 )
                {
                    lifted[0]      = 1;
                    liftedLocal[0] = Rules::Detail::ComposeLocal( body.front().Data );
                    opened[0]      = partitioned ? 1 : 0;
                    return;
                }
                const std::size_t parent = body[record].Parent;
                if ( parent == kNone )
                    return;
                resolve( parent );
                if ( opened[parent] == 0 )
                    return;
                lifted[record]      = 1;
                liftedLocal[record] = liftedLocal[parent] * Rules::Detail::ComposeLocal( body[record].Data );
                opened[record]      = partitioned ? 1 : 0;
            };
            for ( std::size_t record = 0; record < body.size(); ++record )
                resolve( record );

            const Assets::PrefabRuntimeBehavior behavior = body.front().Behavior;
            expanded.Slots.push_back( ExpandedSlot{ out.size(), body.size(), true, behavior } );
            ++expanded.Instances;
            if ( behavior == Assets::PrefabRuntimeBehavior::Standalone )
                ++expanded.Standalone;
            for ( std::size_t record = 0; record < body.size(); ++record )
            {
                Assets::EntityData data = std::move( body[record].Data );
                data.id                 = ids[record];
                data.PrefabPath.reset();
                data.PrefabOverrides.reset();
                data.RuntimeBehavior.reset();
                if ( lifted[record] != 0 )
                {
                    data.parent = instance.parent;
                    if ( record != 0 )
                    {
                        glm::vec3 scale, translation, skew;
                        glm::vec4 perspective;
                        glm::quat rotation;
                        glm::decompose( liftedLocal[record], scale, rotation, translation, skew, perspective );
                        data.Translation = translation;
                        data.Rotation    = glm::eulerAngles( rotation );
                        data.Scale       = scale;
                    }
                }
                else if ( body[record].Parent != kNone )
                    data.parent = ids[body[record].Parent];
                else
                    data.parent.reset();
                RemapReferences( data, body[record].Scope, remap );
                out.push_back( std::move( data ) );
            }
        }
        return Common::MakeSuccess( std::move( expanded ) );
    }

    PrefabReader PrefabFileReader()
    {
        return []( std::string_view stated ) -> Common::ResultStr<Assets::PrefabData>
        {
            const std::filesystem::path file = Assets::ResolvePrefabPath( stated );
            std::ifstream               in( file, std::ios::binary );
            if ( !in )
                return Common::MakeError<Assets::PrefabData>(
                     std::format( "prefab '{}' ({}) does not open", stated, file.string() ) );
            const std::string text{ std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() };
            return Assets::ParseLoadablePrefab( stated, text );
        };
    }
} // namespace Desert::Core::WorldCells
