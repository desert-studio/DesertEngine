#include <Engine/Assets/ContentRegistry.hpp>

#include <Engine/Assets/AssetEviction.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Geometry/MeshBounds.hpp>

#include <Common/Content/ContentScan.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <map>
#include <mutex>

// THE COOK — the one function of the content registry that still walks a directory, and the only walk
// left in this engine. It is a translation unit of its own because it reads dependency edges through
// `AssetEviction`, which reaches the renderer; see ContentRegistryInternals.hpp for what that costs
// everything else if the two share a file.
namespace Desert::Assets::ContentRegistry
{
    std::string RefreshOutcome::Describe() const
    {
        return std::to_string( Rows ) + " row(s) in the content registry: " + std::to_string( Added ) +
               " added, " + std::to_string( Removed ) + " removed, " + std::to_string( Edges ) +
               " dependency edge(s) recorded, " + std::to_string( Bounded ) + " row(s) with bounds; " +
               ( Written ? "written" : "unchanged, not written" );
    }

    Common::ResultStr<RefreshOutcome> Refresh( AssetManager& manager )
    {
        RefreshOutcome outcome;

        // THE COOK'S WALK, AND THE ONLY CONTENT SCAN LEFT IN THIS ENGINE. It is here rather than in
        // the boot on purpose: `[ContentScan] boot finished` is the number this tier is judged by, and
        // a walk before that line would have made the registry a place the cost moved to rather than a
        // place it stopped being paid.
        //
        // Through `Common::Content::ScanContentRoots`, which is the SAME walk the tool, the CI gate
        // and the packager use. It had three copies for about an hour of this task's life, which is
        // one hour more than the shape deserves.
        const std::map<std::string, Common::Content::ContentFile> onDisk = Common::Content::ScanContentRoots();

        {
            Detail::State&                    state = Detail::Get_();
            const std::lock_guard<std::mutex> lock( state.Mutex );

            for ( const auto& [key, file] : onDisk )
            {
                if ( state.Registry.FindByKey( key ) != nullptr )
                    continue;

                Common::Utils::AssetRegistryEntry entry;
                entry.Key  = key;
                entry.Kind = std::string( Common::Content::KindName( file.Kind ) );
                entry.Size = file.Size;
                if ( const auto inserted = state.Registry.Insert( std::move( entry ) ); !inserted )
                {
                    LOG_ERROR( "[ContentRegistry] '{}' was found on disk and could not be entered: {}", key,
                               inserted.GetError() );
                    continue;
                }
                ++outcome.Added;
                state.Dirty = true;
            }

            // ROWS WHOSE FILE IS GONE LEAVE. Collected first and erased after, because Remove mutates
            // the container the loop above would otherwise be walking.
            std::vector<std::string> vanished;
            for ( const Common::Utils::AssetRegistryEntry& row : state.Registry.Entries() )
            {
                if ( onDisk.find( row.Key ) == onDisk.end() )
                    vanished.push_back( row.Key );
            }
            for ( const std::string& key : vanished )
            {
                state.Registry.Remove( key );
                ++outcome.Removed;
                state.Dirty = true;
            }
        }

        // THE EDGES, READ FROM THE ONE TABLE THAT ALREADY OWNS THEM. `AssetEviction::EdgesOf` is the
        // list of "which asset classes name another asset", and `Desert/Tests/Engine/AssetEviction`
        // holds it against the classes that actually have such a field. Reading the edges here through
        // a second list would be the two-lists-that-must-agree shape this whole task is about.
        //
        // IT WALKS THE MANAGER'S OWN RECORDS AND NOT THE REGISTRY'S ROWS, and that is what keeps the
        // file from churning. An UNLOADED asset contributes no edges — reading its references would
        // mean parsing it, which is the work the demand-driven model exists to avoid — so a pass over
        // the rows would have written `no edges` for every asset this session happened not to touch,
        // and the next session would have written them back. A file that differs after every run is a
        // file nobody can diff and a gate nobody can trust. Iterating the records lets the rule be the
        // exact one: an asset that is loaded tells the truth about its edges, and an asset that is not
        // loaded has taught us nothing, so its row is left alone.
        {
            Detail::State&                    state = Detail::Get_();
            const std::lock_guard<std::mutex> lock( state.Mutex );

            for ( const auto& [metadata, asset] : manager.RegisteredAssets() )
            {
                if ( !asset || !asset->IsReadyForUse() )
                    continue;

                const Common::Utils::AssetRegistryEntry* row =
                     state.Registry.FindByHandle( static_cast<uint64_t>( metadata.Handle ) );
                if ( row == nullptr )
                    continue; // not scanned content (a procedural clip, a `.dgraph` document)

                std::vector<uint64_t> edges;
                AssetEviction::EdgesOf( manager, metadata.Handle,
                                        [&edges]( const Common::UUID& edge, const std::string& )
                                        {
                                            // Null edges are dropped HERE and not in EdgesOf: the sweep
                                            // wants them marked (a null in a slot is a slot that names
                                            // nothing, and marking it costs nothing), the registry must
                                            // not store a dependency on the null handle.
                                            if ( static_cast<uint64_t>( edge ) != 0 )
                                                edges.push_back( static_cast<uint64_t>( edge ) );
                                        } );

                std::sort( edges.begin(), edges.end() );
                edges.erase( std::unique( edges.begin(), edges.end() ), edges.end() );

                outcome.Edges += edges.size();
                if ( edges != row->Dependencies )
                {
                    const std::string key = row->Key; // SetDependencies invalidates `row`
                    state.Registry.SetDependencies( key, std::move( edges ) );
                    state.Dirty = true;
                }
            }

            // ── BOUNDS: every loaded mesh's box ─────────────────────────────────────────────────────
            //
            // The box the draw side uses (Geometry::LocalBounds over the submeshes), so the partitioner and
            // the renderer cannot disagree about how big a mesh is. The mesh cook states the same box when
            // it writes the file (ContentRegistry::NoteBounds); this is the safety net for a mesh that came
            // from a `git pull` rather than this machine's import. A mesh not loaded this session keeps
            // whatever its row says — the convergence rule the edges follow: learned once, carried after.
            for ( const auto& [metadata, asset] : manager.RegisteredAssets() )
            {
                if ( !asset || !asset->IsReadyForUse() || metadata.AssetType != AssetTypeID::Mesh )
                    continue;
                const auto*                              mesh = dynamic_cast<const MeshAsset*>( asset.get() );
                const Common::Utils::AssetRegistryEntry* row =
                     state.Registry.FindByHandle( static_cast<uint64_t>( metadata.Handle ) );
                if ( mesh == nullptr || row == nullptr )
                    continue;

                std::optional<Common::Math::AABB> box = Geometry::LocalBounds( mesh->GetSubmeshes() );
                if ( Geometry::IsEmpty( *box ) )
                    box.reset();
                if ( !Common::Utils::SameBounds( row->Bounds, box ) )
                {
                    const std::string key = row->Key;
                    state.Registry.SetBounds( key, box );
                    state.Dirty = true;
                }
            }

            for ( const Common::Utils::AssetRegistryEntry& row : state.Registry.Entries() )
                outcome.Bounded += row.Bounds.has_value() ? 1 : 0;

            outcome.Rows = state.Registry.Count();
        }

        if ( Dirty() )
        {
            if ( const auto written = Save(); !written )
                return Common::MakeError<RefreshOutcome>( written.GetError() );
            outcome.Written = true;
        }

        return Common::MakeSuccess( RefreshOutcome( outcome ) );
    }
} // namespace Desert::Assets::ContentRegistry
