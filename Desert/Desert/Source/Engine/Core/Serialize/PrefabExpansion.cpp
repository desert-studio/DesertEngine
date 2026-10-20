#include <Engine/Core/Serialize/PrefabExpansion.hpp>

#include <Engine/Assets/Prefab/PrefabFormat.hpp>
#include <Engine/Assets/Prefab/PrefabOverrides.hpp>
#include <Engine/Core/Serialize/SceneStitchRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <glm/gtx/matrix_decompose.hpp>

#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>

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
                    for ( BodyRecord& record : nested.GetValue() )
                    {
                        if ( record.Parent != kNone )
                            record.Parent += from;
                        body.push_back( std::move( record ) );
                    }
                    body[from].Parent = nesting.Parent == Rules::kNoSlot ? kNone : nesting.Parent;
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

        // Rewrites every kEntityReferences field of @p record that names a body record of its own scope.
        void RemapReferences( Assets::EntityData& record, std::size_t scope,
                              const std::map<std::pair<std::size_t, std::uint64_t>, Common::UUID>& remap )
        {
            for ( const Rules::EntityReferenceRow& row : Rules::kEntityReferences )
            {
                const std::string key( row.ComponentKey );
                const auto        payload = record.Components.get( key );
                if ( !payload )
                    continue;
                auto object = payload.value().to_object();
                if ( !object )
                    continue;
                Common::Json::Object block = object.value();
                const auto           field = block.get( std::string( row.Field ) );
                if ( !field )
                    continue;
                const auto text = field.value().to_string();
                if ( !text )
                    continue;
                std::uint64_t id = 0;
                try
                {
                    id = std::stoull( text.value() );
                }
                catch ( ... )
                {
                    continue; // not an id; the planner reports it as an Issue
                }
                const auto hit = remap.find( { scope, id } );
                if ( hit == remap.end() )
                    continue;
                block[std::string( row.Field )] =
                     Common::Json::Value( std::to_string( static_cast<std::uint64_t>( hit->second ) ) );
                record.Components[key] = Common::Json::Value( block );
            }
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

        for ( std::size_t source = 0; source < scene.Entities.size(); ++source )
        {
            const Assets::EntityData& instance = scene.Entities[source];
            auto&                     out      = expanded.Scene.Entities;
            if ( !instance.PrefabPath.has_value() )
            {
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

            Expander expander{ reader, {}, 0 };
            auto     read = expander.Body( *instance.PrefabPath, {} );
            if ( !read )
                return Common::MakeFormattedError<Result>( "{}: {}", what, read.GetError() );
            std::vector<BodyRecord>& body = read.GetValue();
            if ( instance.PrefabOverrides.has_value() )
                if ( auto layered =
                          Expander::Layer( body, 0, *instance.PrefabOverrides, {}, *instance.PrefabPath );
                     !layered )
                    return Common::MakeFormattedError<Result>( "{}: {}", what, layered.GetError() );
            Assets::PlaceInstanceRoot( body.front().Data, stated.GetValue() );

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

            const glm::mat4 rootLocal = Rules::Detail::ComposeLocal( body.front().Data );
            expanded.Slots.push_back( ExpandedSlot{ out.size(), body.size(), true } );
            ++expanded.Instances;
            for ( std::size_t record = 0; record < body.size(); ++record )
            {
                Assets::EntityData data = std::move( body[record].Data );
                data.id                 = ids[record];
                data.PrefabPath.reset();
                data.PrefabOverrides.reset();
                if ( record == 0 || body[record].Parent == 0 )
                {
                    // The root, and THE SPLIT: a direct child leaves the root and hangs off the instance's own
                    // parent, its transform composed with the root's.
                    data.parent = instance.parent;
                    if ( record != 0 )
                    {
                        const glm::mat4 local = rootLocal * Rules::Detail::ComposeLocal( data );
                        glm::vec3       scale, translation, skew;
                        glm::vec4       perspective;
                        glm::quat       rotation;
                        glm::decompose( local, scale, rotation, translation, skew, perspective );
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
