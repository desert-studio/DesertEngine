#include <Engine/Assets/AssetEviction.hpp>

#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/CloudTypeAsset.hpp>
#include <Engine/Assets/FoliageTypeAsset.hpp>
#include <Engine/Assets/LandscapeGrassTypeAsset.hpp>
#include <Engine/Assets/LandscapeLayerInfoAsset.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/RetargetAsset.hpp>

#include <Engine/Core/TextTemplate.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Assets/ContentRegistry.hpp>

#include <chrono>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Desert::Assets
{
    namespace
    {
        // The sweep's report: one summary sentence, then a line per refusal, kept texture and built mesh left.
        constexpr std::string_view kEvictionReportText =
             "swept {{ registered }} registered asset(s): {{ roots }} root(s) expanded to {{ reachable }} "
             "reachable; released {{ released }}, already cold {{ alreadyCold }}, refused {{ refused }}, "
             "project-scoped {{ projectScoped }}. Dropped {{ meshesDropped }} built mesh(es) and "
             "{{ materialsDropped }} built material(s) and {{ texturesDropped }} built texture(s). GPU rows "
             "{{ rowsBefore }} -> {{ rowsAfter }}."
             "{% for line in refusals %}\n  refused: {{ line }}{% endfor %}"
             "{% for line in keptTextures %}\n  kept texture: {{ line }}{% endfor %}"
             "{% for line in builtMeshesLeft %}\n  built mesh left: {{ line }}{% endfor %}";

        Common::Json::Value Lines( const std::vector<std::string>& lines )
        {
            return Common::Json::Value( Common::Json::Value::Array( lines.begin(), lines.end() ) );
        }
    } // namespace

    std::string EvictionOutcome::Describe() const
    {
        static const Desert::Core::TextTemplate kReport( "EvictionReport", kEvictionReportText );
        Common::Json::ObjectBuilder             model;
        model.Set( "registered", Registered )
             .Set( "roots", Roots )
             .Set( "reachable", Reachable )
             .Set( "released", Released )
             .Set( "alreadyCold", AlreadyCold )
             .Set( "refused", Refused )
             .Set( "projectScoped", ProjectScoped )
             .Set( "meshesDropped", MeshesDropped )
             .Set( "materialsDropped", MaterialsDropped )
             .Set( "texturesDropped", TexturesDropped )
             .Set( "rowsBefore", LedgerRowsBefore )
             .Set( "rowsAfter", LedgerRowsAfter )
             .Set( "refusals", Lines( Refusals ) )
             .Set( "keptTextures", Lines( KeptTextures ) )
             .Set( "builtMeshesLeft", Lines( BuiltMeshesLeft ) );
        return kReport.RenderOrLog( Common::Json::Value( model.Build() ) );
    }

    void AssetEviction::EdgesOf( AssetManager& manager, const Common::AssetHandle& handle,
                                 const std::function<void( const Common::UUID&, const std::string& )>& visit )
    {
        // THE EDGES OF THE GRAPH, IN ONE PLACE. Every asset class that names another asset is listed
        // here and nowhere else. A dropped edge is invisible in every other way: the asset it should
        // have kept is released, reloads on the next access, and the only symptom is work.
        //
        // THREE OF THESE FIVE HAD NO TEST when that was measured, and the header used to claim all of
        // them did. Two still do not — see the note beside the edge tests in
        // `Desert/Tests/Engine/AssetEviction`. Adding an edge here without adding its test there is
        // adding a line nothing can hold you to.
        //
        // AND THE RISK IS NOT THEORETICAL FOR THIS FUNCTION SPECIFICALLY: it was lifted out of
        // `Expand` in one branch while another added the retarget edge to the body it was leaving. A
        // merge that took the leaving copy would have dropped the edge with nothing red anywhere.
        //
        // AN UNLOADED ASSET CONTRIBUTES NO EDGES, deliberately. Reading its references would mean parsing
        // it — the exact work the lazy registration exists to avoid — and it is harmless: an asset that is
        // not loaded is protecting nothing, and when it does load, `EnsureLoaded` re-resolves and whatever
        // it needs is rebuilt through the same build-on-miss path a first use takes.
        if ( const auto mesh = manager.ProbeByHandle<MeshAsset>( handle ) )
        {
            if ( mesh->IsReadyForUse() )
            {
                for ( const Common::UUID& material : mesh->GetMaterialHandles() )
                    visit( material, "a submesh of a reachable mesh names it" );
            }

            // The skinned mesh's rig. Held as a resolved dependency rather than as a plain field, so it
            // is read through the dependency's handle and not through the signature it was matched by —
            // the signature names a shape, the handle names the file.
            if ( const auto skinned = manager.ProbeByHandle<SkinnedMeshAsset>( handle ) )
            {
                if ( skinned->IsReadyForUse() )
                    visit( skinned->GetSkeletonDependency().Handle, "a reachable skinned mesh is rigged to it" );
            }
        }

        if ( const auto material = manager.ProbeByHandle<SurfaceMaterialAsset>( handle ) )
        {
            if ( material->IsReadyForUse() )
            {
                // EVERY slot of all three lists (textures, cloud types and layouts, the Medium shader), not
                // the three lit slots: most references in this repository's materials are cloud assets, and
                // marking them all is correct precisely because this loop does not need to know what they are.
                material->Data().ForEachSlotHandle(
                     [&]( const std::string& name, const auto slot )
                     {
                         visit( Common::AssetHandle( static_cast<uint64_t>( slot ) ),
                                "a reachable material names it in its '" + name + "' slot" );
                     } );
            }
        }

        // THE RETARGET'S SOURCE RIG, and without this row it is unreachable by construction: a source
        // `.skeleton` is named by no component and by no mesh in the scene — only by the `.retarget`
        // that plays clips from it. The first sweep recorded what the equivalent missing row did to the
        // mesh's rig (410 x "Skeleton dependency invalid" in twelve seconds, and no character drawn);
        // this is the same hole one content kind over.
        //
        // It reached this function through a MERGE, not through the branch that extracted it: the edge
        // was added to the inline body on `dev` while `EdgesOf` was being lifted out of it, so taking
        // either side whole would have dropped it in silence — no compiler and no gate names it.
        if ( const auto retarget = manager.ProbeByHandle<RetargetAsset>( handle ) )
        {
            if ( retarget->IsReadyForUse() )
                visit( retarget->GetSourceSkeletonDependency().Handle,
                       "a reachable retarget plays its clips from it" );
        }

        if ( const auto cloudType = manager.ProbeByHandle<CloudTypeAsset>( handle ) )
        {
            if ( cloudType->IsReadyForUse() )
                visit( cloudType->GetNoiseVolume(), "a reachable cloud type cuts its edge from it" );
        }

        // THE LANDSCAPE'S GRASS AND THE FOLIAGE TYPES (JFA-OUTLINE-MISSING). A layer info is a root (a landscape
        // paints with it) but nothing marked what it grows: the first sweep after the scene's own - the editor's
        // ThumbnailPreview scene initialising on a selection - released the grass types and their blade
        // materials, and every grass instance vanished from the frame and from the shadows with nothing in
        // the log. The references are GUID text in the files, so the handle is HandleForGuid of it, as the
        // assets' own accessors resolve it.
        const auto visitGuid = [&visit]( const std::string& text, const std::string& why )
        {
            if ( text.empty() )
                return;
            const auto guid = Common::Content::AssetGuidFromText( text );
            if ( !guid || guid.GetValue().IsNull() )
                return;
            visit( Common::AssetHandle( static_cast<uint64_t>( Common::Content::HandleForGuid( guid.GetValue() ) ) ),
                   why );
        };
        if ( const auto layer = manager.ProbeByHandle<LandscapeLayerInfoAsset>( handle ) )
        {
            if ( layer->IsReadyForUse() )
                for ( const AssetGuidRef& grass : layer->GetData().GrassTypes )
                    visitGuid( grass.Guid, "a reachable landscape layer grows it" );
        }
        if ( const auto grassType = manager.ProbeByHandle<LandscapeGrassTypeAsset>( handle ) )
        {
            if ( grassType->IsReadyForUse() )
            {
                for ( const auto& variety : grassType->GetData().GrassVarieties )
                {
                    if ( variety.Mesh )
                        visitGuid( variety.Mesh->Guid, "a reachable grass type draws it" );
                    if ( variety.Material )
                        visitGuid( variety.Material->Guid, "a reachable grass type shades its blades with it" );
                }
            }
        }
        if ( const auto foliageType = manager.ProbeByHandle<FoliageTypeAsset>( handle ) )
        {
            if ( foliageType->IsReadyForUse() )
            {
                visitGuid( foliageType->GetData().Mesh.Guid, "a reachable foliage type draws it" );
                visitGuid( foliageType->GetData().Prefab.Guid, "a reachable foliage type spawns it" );
            }
        }
    }

    EvictionOutcome AssetEviction::Run( AssetManager& manager, const AssetRootSet& roots, IEvictionSink& sink )
    {
        // The whole sweep in one call: the slices of AssetEvictionSweep with no deadline. What the suite drives,
        // and the same code the frame loop runs a slice at a time.
        AssetEvictionSweep sweep( manager, sink );
        sweep.AddRoots( roots );
        while ( !sweep.Step( std::chrono::steady_clock::time_point::max() ) )
        {
        }
        return sweep.Outcome();
    }

    AssetEvictionSweep::AssetEvictionSweep( AssetManager& manager, IEvictionSink& sink )
         : m_Manager( manager ), m_Sink( sink )
    {
        m_Outcome.LedgerRowsBefore = Graphic::ResourceLedger::Take().Live;
    }

    void AssetEvictionSweep::MarkOne( const Common::AssetHandle& handle, std::string why )
    {
        const std::size_t before = m_Closure.Size();
        m_Closure.Mark( handle, std::move( why ) );
        if ( m_Closure.Size() != before )
            m_Frontier.push_back( handle );
    }

    void AssetEvictionSweep::AddRoots( const AssetRootSet& roots )
    {
        if ( m_Phase == Phase::Done )
            return;
        for ( const Common::AssetHandle& root : roots.Handles() )
        {
            const std::size_t before = m_Closure.Size();
            MarkOne( root, roots.WhyKept( root ) );
            if ( m_Closure.Size() != before )
                m_Outcome.Roots++;
        }
    }

    void AssetEvictionSweep::ExpandOne( const Common::AssetHandle& handle )
    {
        // A WORKLIST, NOT A FIXPOINT: each reachable asset is expanded exactly once, when it is first marked. The
        // fixpoint it replaces walked the whole closure again per level of depth (entity -> mesh -> material ->
        // texture) and once more to see nothing change.
        AssetEviction::EdgesOf( m_Manager, handle, [this]( const Common::UUID& edge, const std::string& why )
                                { MarkOne( edge, why ); } );
    }

    void AssetEvictionSweep::TakeCandidates()
    {
        // The registry's storage is a vector and `RegisteredAssets()` hands back a reference into it, so
        // the handles are copied out BEFORE anything is released -- and the sweep now spans frames, in which the
        // registry may grow. An asset registered after this point is the next sweep's.
        m_Candidates.reserve( m_Manager.RegisteredAssets().size() );
        for ( const auto& [metadata, asset] : m_Manager.RegisteredAssets() )
        {
            m_Outcome.Registered++;
            if ( !asset || !metadata.IsValid() )
                continue;
            if ( m_Closure.Contains( metadata.Handle ) )
            {
                if ( metadata.AssetType == AssetTypeID::Texture2D )
                    m_Outcome.KeptTextures.push_back( "'" + metadata.Filepath.string() + "' <- " +
                                                      m_Closure.WhyKept( metadata.Handle ) );
                continue;
            }
            // NOT EVERY ASSET BELONGS TO A WORLD. A project-scoped type is named by no component, so the
            // root walk can never reach one and "unreachable" says nothing about whether it is in use --
            // see Assets::IsProjectScopedAsset for the measurement that put this line here.
            if ( IsProjectScopedAsset( metadata.AssetType ) )
            {
                m_Outcome.ProjectScoped++;
                continue;
            }
            m_Candidates.emplace_back( metadata.Handle, asset );
        }
    }

    void AssetEvictionSweep::ReleaseOne( const Common::AssetHandle& handle, const Asset<AssetBase>& asset )
    {
        // Reachable SINCE the candidates were taken: a cell activated between two slices named it (AddRoots).
        if ( m_Closure.Contains( handle ) )
            return;

        // THE BUILT GPU OBJECTS FIRST, THE PAYLOAD SECOND, and the order is not arbitrary: both
        // rebuild paths (`MeshService::Get`, `MaterialService::Get`) read the asset shell, so dropping
        // the payload first would leave a window in which a draw between the two statements rebuilds
        // from an emptied asset. There is no such window this way round.
        //
        // `Invalidate` and not `Release`: Release forgets the shell as well, and the shell is what the
        // rebuild reads. Invalidate parks the runtime materials in the graveyard — destroying them
        // here would invalidate a command buffer that is still recording against their descriptor
        // pools — and `CollectGarbage()` destroys them at a later frame start, once no frame in flight can
        // still reference them (FrameRetireQueue).
        if ( m_Sink.HasBuiltMaterial( handle ) )
        {
            m_Sink.DropBuiltMaterial( handle );
            m_Outcome.MaterialsDropped++;
        }

        if ( m_Sink.DropBuiltMesh( handle ) )
            m_Outcome.MeshesDropped++;

        if ( m_Sink.DropBuiltTexture( handle ) )
            m_Outcome.TexturesDropped++;

        // A READ IS IN FLIGHT FOR THIS HANDLE, OR ITS COMPLETION HAS NOT BEEN PUMPED YET.
        //
        // Releasing here is not merely wasteful, it DESTROYS THE READ: the worker has filled the
        // asset and the completion delegate has not run, so the bytes this sweep is about to free
        // are the only copy, and the delegate finds `IsReadyForUse()` false and refuses to upload.
        // Measured on `Clouds_HeroTrio`: the read finished at 04.671, this sweep ran at 04.761, and
        // the frame drew no clouds at all while the log said "was read but could not be uploaded".
        //
        // It is not reachability that is wrong -- a noise volume genuinely is unreachable from a
        // scene root, it is named by a cloud TYPE -- it is that "unreachable" says nothing about an
        // asset somebody is in the middle of loading. The eager model could not hit this because the
        // read and the upload were one call with no sweep between them.
        if ( AsyncAssetLoader::Get().IsRequested( handle ) )
        {
            m_Outcome.Refused++;
            m_Outcome.Refusals.push_back(
                 "'" + asset->GetMetadata().Filepath.string() +
                 "' has a read in flight (or a completion not yet pumped). Releasing it would throw "
                 "away the bytes a worker has just finished reading, and the consumer that asked for "
                 "them would be told the file 'is not loaded'. The asset stays resident; the next "
                 "sweep after the completion will reconsider it." );
            return;
        }

        if ( !asset->IsReadyForUse() )
        {
            m_Outcome.AlreadyCold++;
            return;
        }

        if ( const auto released = asset->Unload(); !released )
        {
            m_Outcome.Refused++;
            m_Outcome.Refusals.push_back( released.GetError() );
            return;
        }

        m_Outcome.Released++;
    }

    bool AssetEvictionSweep::Step( const std::chrono::steady_clock::time_point deadline )
    {
        // AT LEAST ONE UNIT PER STEP, so a sweep finishes whatever the budget; then units until the deadline.
        bool first = true;
        while ( m_Phase != Phase::Done && ( first || std::chrono::steady_clock::now() < deadline ) )
        {
            first = false;
            // MARK BEFORE ANY RELEASE, in every slice: roots added since the last slice are expanded first, so
            // nothing a new root reaches is released in the meantime.
            if ( !m_Frontier.empty() )
            {
                const Common::AssetHandle handle = m_Frontier.back();
                m_Frontier.pop_back();
                ExpandOne( handle );
                continue;
            }
            if ( m_Phase == Phase::Mark )
            {
                TakeCandidates();
                m_Phase = Phase::Release;
                continue;
            }
            if ( m_NextCandidate < m_Candidates.size() )
            {
                const auto& [handle, asset] = m_Candidates[m_NextCandidate++];
                ReleaseOne( handle, asset );
                continue;
            }
            Finish();
        }
        return m_Phase == Phase::Done;
    }

    void AssetEvictionSweep::Finish()
    {
        m_Outcome.Reachable = static_cast<uint32_t>( m_Closure.Size() );
        m_Candidates.clear();

        // COLLECT WHAT HAS RETIRED, THEN READ THE LEDGER BACK. `Invalidate` does not destroy a runtime
        // material and `EvictBuilt` does not destroy a mesh — both are parked until no frame in flight can
        // still reference them (Assets::FrameRetireQueue, WP13), because a sweep asked for by world streaming
        // runs with the departed cell's frames still on the GPU and a device idle per departed cell would stop
        // the GPU mid-flight. So `LedgerRowsAfter` counts what THIS sweep dropped as still live: the rows go
        // a few frames later, when the frame loop collects (MeshService::RetireEvicted, CollectGarbage).
        // Collecting here still releases what earlier sweeps parked, and is free when nothing is due.
        m_Sink.CollectGarbage();

        // THE OTHER SIDE OF THE LEDGER: what is still built, and why. Asked of the sink, not the registry, so
        // a built mesh whose key the registry walk above never met is named instead of silently resident.
        // One map of the registry, not a registry walk per built mesh.
        std::unordered_map<uint64_t, std::string> paths;
        paths.reserve( m_Manager.RegisteredAssets().size() );
        for ( const auto& [metadata, asset] : m_Manager.RegisteredAssets() )
            paths.emplace( static_cast<uint64_t>( metadata.Handle ), "'" + metadata.Filepath.string() + "'" );
        for ( const Common::AssetHandle& built : m_Sink.BuiltMeshHandles() )
        {
            const auto        found = paths.find( static_cast<uint64_t>( built ) );
            const std::string key   = std::to_string( static_cast<uint64_t>( built ) );
            if ( found == paths.end() )
                m_Outcome.BuiltMeshesLeft.push_back( key + " is a key the asset registry does not hold" );
            else if ( m_Closure.Contains( built ) )
                m_Outcome.BuiltMeshesLeft.push_back( key + " " + found->second + " <- " +
                                                     m_Closure.WhyKept( built ) );
            else
                m_Outcome.BuiltMeshesLeft.push_back( key + " " + found->second +
                                                     " is unreachable and was not dropped" );
        }

        m_Outcome.LedgerRowsAfter = Graphic::ResourceLedger::Take().Live;
        m_Phase                   = Phase::Done;
    }

} // namespace Desert::Assets
