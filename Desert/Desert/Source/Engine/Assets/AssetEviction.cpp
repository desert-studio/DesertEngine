#include <Engine/Assets/AssetEviction.hpp>

#include <Engine/Assets/CloudTypeAsset.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>

#include <Engine/Graphic/ResourceLedger.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Assets
{
    std::string EvictionOutcome::Describe() const
    {
        std::string text;
        text += "swept " + std::to_string( Registered ) + " registered asset(s): " + std::to_string( Roots ) +
                " root(s) expanded to " + std::to_string( Reachable ) + " reachable; released " +
                std::to_string( Released ) + ", already cold " + std::to_string( AlreadyCold ) + ", refused " +
                std::to_string( Refused ) + ", project-scoped " + std::to_string( ProjectScoped ) +
                ". Dropped " + std::to_string( MeshesDropped ) +
                " built mesh(es) and " + std::to_string( MaterialsDropped ) + " built material(s). GPU rows " +
                std::to_string( LedgerRowsBefore ) + " -> " + std::to_string( LedgerRowsAfter ) + ".";

        for ( const std::string& refusal : Refusals )
            text += "\n  refused: " + refusal;

        return text;
    }

    void AssetEviction::Expand( AssetManager& manager, AssetRootSet& closure )
    {
        // THE EDGES OF THE GRAPH, IN ONE PLACE. Every asset class that names another asset is listed here
        // and nowhere else, so `Desert/Tests/Engine/AssetEviction` can hold the list against the classes
        // that actually have such a field. A dropped edge is invisible in every other way: the asset it
        // should have kept is released, reloads on the next access, and the only symptom is work.
        //
        // AN UNLOADED ASSET CONTRIBUTES NO EDGES, deliberately. Reading its references would mean parsing
        // it — the exact work the lazy registration exists to avoid — and it is harmless: an asset that is
        // not loaded is protecting nothing, and when it does load, `EnsureLoaded` re-resolves and whatever
        // it needs is rebuilt through the same build-on-miss path a first use takes.
        //
        // Iterated to a fixpoint because the graph has depth: an entity names a mesh, the mesh names a
        // material, the material names a texture. Three passes is the depth of the deepest chain in this
        // engine today and the loop below stops when nothing new was marked, so a fourth level added
        // tomorrow needs no edit here.
        for ( ;; )
        {
            const std::size_t before = closure.Size();

            for ( const Common::AssetHandle& handle : closure.Handles() )
            {
                if ( const auto mesh = manager.ProbeByHandle<MeshAsset>( handle ) )
                {
                    if ( mesh->IsReadyForUse() )
                    {
                        for ( const Common::UUID& material : mesh->GetMaterialHandles() )
                            closure.Mark( material, "a submesh of a reachable mesh names it" );
                    }

                    // The skinned mesh's rig. Held as a resolved dependency rather than as a plain field,
                    // so it is read through the dependency's handle and not through the signature it was
                    // matched by — the signature names a shape, the handle names the file.
                    if ( const auto skinned = manager.ProbeByHandle<SkinnedMeshAsset>( handle ) )
                    {
                        if ( skinned->IsReadyForUse() )
                            closure.Mark( skinned->GetSkeletonDependency().Handle,
                                          "a reachable skinned mesh is rigged to it" );
                    }
                }

                if ( const auto material = manager.ProbeByHandle<SurfaceMaterialAsset>( handle ) )
                {
                    if ( material->IsReadyForUse() )
                    {
                        // EVERY entry of the map, not the three PBR slots. `MaterialData::Textures` is the
                        // material's generic name -> handle table and the shader schema is what says which
                        // KIND of asset each name stands for: seventeen of the twenty-two distinct
                        // references in this repository's materials are cloud types and layouts reached
                        // through `CloudType1..4` and `CloudLayout`, not textures at all. Marking them all
                        // is correct precisely because this loop does not need to know what they are.
                        for ( const auto& texture : material->Data().Textures )
                            closure.Mark( Common::AssetHandle( texture.TextureHandle ),
                                          "a reachable material names it in its '" + texture.Name + "' slot" );
                    }
                }

                if ( const auto cloudType = manager.ProbeByHandle<CloudTypeAsset>( handle ) )
                {
                    if ( cloudType->IsReadyForUse() )
                        closure.Mark( cloudType->GetNoiseVolume(),
                                      "a reachable cloud type cuts its edge from it" );
                }
            }

            if ( closure.Size() == before )
                break;
        }
    }

    EvictionOutcome AssetEviction::Run( AssetManager& manager, const AssetRootSet& roots, IEvictionSink& sink )
    {
        EvictionOutcome outcome;
        outcome.Roots            = static_cast<uint32_t>( roots.Size() );
        outcome.LedgerRowsBefore = Graphic::ResourceLedger::Take().Live;

        AssetRootSet closure = roots;
        Expand( manager, closure );
        outcome.Reachable = static_cast<uint32_t>( closure.Size() );

        // The registry's storage is a vector and `RegisteredAssets()` hands back a reference into it, so
        // the handles are copied out BEFORE anything is released. Nothing below registers an asset today —
        // but "today" is how a container gets mutated under an iterator six months from now.
        std::vector<std::pair<Common::AssetHandle, Asset<AssetBase>>> candidates;
        candidates.reserve( manager.RegisteredAssets().size() );
        for ( const auto& [metadata, asset] : manager.RegisteredAssets() )
        {
            outcome.Registered++;
            if ( !asset || !metadata.IsValid() )
                continue;
            if ( closure.Contains( metadata.Handle ) )
                continue;
            // NOT EVERY ASSET BELONGS TO A WORLD. A project-scoped type is named by no component, so the
            // root walk can never reach one and "unreachable" says nothing about whether it is in use —
            // see Assets::IsProjectScopedAsset for the measurement that put this line here.
            if ( IsProjectScopedAsset( metadata.Type ) )
            {
                outcome.ProjectScoped++;
                continue;
            }
            candidates.emplace_back( metadata.Handle, asset );
        }

        for ( const auto& [handle, asset] : candidates )
        {
            // THE BUILT GPU OBJECTS FIRST, THE PAYLOAD SECOND, and the order is not arbitrary: both
            // rebuild paths (`MeshService::Get`, `MaterialService::Get`) read the asset shell, so dropping
            // the payload first would leave a window in which a draw between the two statements rebuilds
            // from an emptied asset. There is no such window this way round.
            //
            // `Invalidate` and not `Release`: Release forgets the shell as well, and the shell is what the
            // rebuild reads. Invalidate parks the runtime materials in the graveyard — destroying them
            // here would invalidate a command buffer that is still recording against their descriptor
            // pools — and `CollectGarbage()` destroys them at a safe point after a device idle.
            if ( sink.HasBuiltMaterial( handle ) )
            {
                sink.DropBuiltMaterial( handle );
                outcome.MaterialsDropped++;
            }

            if ( sink.DropBuiltMesh( handle ) )
                outcome.MeshesDropped++;

            if ( !asset->IsReadyForUse() )
            {
                outcome.AlreadyCold++;
                continue;
            }

            if ( const auto released = asset->Unload(); !released )
            {
                outcome.Refused++;
                outcome.Refusals.push_back( released.GetError() );
                continue;
            }

            outcome.Released++;
        }

        // COLLECT THE GRAVEYARD BEFORE READING THE LEDGER BACK, or the two numbers this outcome quotes are
        // measuring different things. `Invalidate` does not destroy a runtime material — it parks it, so
        // that a frame in flight recording against its descriptor pools stays valid — and until the
        // collector runs, every material this sweep "dropped" is still a live row. The first version of
        // this function reported `GPU rows 564 -> 564` after invalidating sixteen materials, which is a
        // true statement about an instant nobody cares about.
        //
        // Calling it here is exactly its documented contract: "at the START of a frame, before any command
        // recording", which is where the sweep runs. It waits for the device to go idle and is free when
        // the graveyard is empty.
        sink.CollectGarbage();

        outcome.LedgerRowsAfter = Graphic::ResourceLedger::Take().Live;
        return outcome;
    }

} // namespace Desert::Assets
