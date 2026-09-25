#include "MaterialService.hpp"

#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Graphic/Materials/MaterialFactory.hpp>
#include <Engine/Graphic/Materials/Mesh/PBR/MaterialPBR.hpp>
#include <Engine/Graphic/Renderer.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Runtime
{
    Common::BoolResultStr MaterialService::Register( const std::shared_ptr<Assets::MaterialAsset>& materialAsset )
    {
        if ( !materialAsset->GetMetadata().IsValid() )
        {
            return Common::MakeError( "Material asset is invalid" );
        }

        auto handle = materialAsset->GetMetadata().Handle;
        if ( const auto refusal = RefuseOnCollision( handle, materialAsset ); !refusal )
            return refusal;

        // Eager build of the (Static x Forward) cell only. The other cells are built on the first draw
        // that asks for them: a scene with no skinned geometry must not pay for a skinned descriptor set
        // per material, a forward scene must not pay for a G-buffer one, and a cell built eagerly for an
        // asset nobody draws that way is a resource with no reader.
        auto material = Graphic::MaterialFactory::CreateMaterial(
             materialAsset.get(), Graphic::MeshVertexPath::Static, Graphic::MeshPass::Forward );

        // The same file re-registering (RefuseOnCollision lets that through deliberately) replaces the
        // cell, so the material this overwrites stops existing. Its address would otherwise stay in the
        // reverse index and be handed to a render pass as a live sibling.
        auto& cell =
             m_Materials[handle][VariantSlot( Graphic::MeshVertexPath::Static, Graphic::MeshPass::Forward )];
        if ( cell )
            m_BuiltToAsset.erase( cell.get() );
        if ( material )
        {
            m_BuiltToAsset[material.get()] = handle;
            // The ledger row this material opened in its constructor now knows whose it is — see
            // Engine/Graphic/ResourceLedger.hpp. A render system's own materials stay attributed to the
            // renderer; these are the ones a `.demat` can rebuild, i.e. the ones eviction may consider.
            material->ClaimOwnership( Graphic::ResourceOwner::AssetService, handle );
        }
        cell                     = material;
        m_MaterialAssets[handle] = materialAsset; // keep the shell too

        m_ExternalToInternal[materialAsset->GetMaterialUUID()] = handle;

        return BOOLSUCCESS;
    }

    Common::BoolResultStr
    MaterialService::RegisterAsset( const std::shared_ptr<Assets::MaterialAsset>& materialAsset )
    {
        if ( !materialAsset->GetMetadata().IsValid() )
        {
            return Common::MakeError( "Material asset is invalid" );
        }
        const auto handle = materialAsset->GetMetadata().Handle;
        if ( const auto refusal = RefuseOnCollision( handle, materialAsset ); !refusal )
            return refusal;

        m_MaterialAssets[handle] = materialAsset; // runtime Material built lazily on first Get
        // The mesh->material link resolves by EXTERNAL id, so the map must exist before any build.
        m_ExternalToInternal[materialAsset->GetMaterialUUID()] = handle;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr
    MaterialService::EnsureLoaded( const std::shared_ptr<Assets::MaterialAsset>& asset ) const
    {
        if ( !asset )
            return Common::MakeError<bool>( "MaterialService: a null material shell cannot be loaded" );
        if ( asset->IsReadyForUse() )
            return BOOLSUCCESS;

        if ( const auto loaded = asset->Load(); !loaded )
        {
            // Loudly, and then the caller decides. A material that cannot be re-read is a surface that
            // will draw with the shader's own defaults, and the ONLY place that knows which file it was is
            // here — see the header for the round trip that found this.
            LOG_ERROR( "[MaterialService] '{}' was released and could not be read back: {}. Anything drawn "
                       "with it falls back to the shader's default parameters.",
                       asset->GetMetadata().Filepath.string(), loaded.GetError() );
            return loaded;
        }

        return BOOLSUCCESS;
    }

    Common::BoolResultStr
    MaterialService::RefuseOnCollision( const Assets::AssetHandle&                    handle,
                                        const std::shared_ptr<Assets::MaterialAsset>& incoming ) const
    {
        const auto it = m_MaterialAssets.find( handle );
        if ( it == m_MaterialAssets.end() || !it->second )
            return BOOLSUCCESS;

        const auto& held = it->second->GetMetadata().Filepath;
        const auto& want = incoming->GetMetadata().Filepath;
        if ( !IsMaterialIdentityCollision( held, want ) )
            return BOOLSUCCESS; // the same file re-registering: rebuild, as before

        // BOTH files, and the id, because the message has to be actionable without a debugger: the fix is
        // to give one of the two `.demat` files a new header GUID and re-point the scenes that name it.
        // The FIRST registration keeps the identity — refusing is what makes the outcome deterministic
        // rather than a property of the order the asset scan happened to run in.
        LOG_ERROR( "[MaterialService] Two materials claim handle {}: '{}' already holds it, so '{}' was "
                   "REFUSED and will not resolve. A `.demat`'s handle is its header GUID folded "
                   "(HandleForGuid), so a shared one makes a mesh slot, an Edit button and a double-click "
                   "open whichever of the two registered first. Give one of them a new header GUID and "
                   "re-point every scene that names it.",
                   static_cast<uint64_t>( handle ), held.generic_string(), want.generic_string() );

        return Common::MakeFormattedError<bool>( "material handle {} is already held by '{}'; '{}' was refused",
                                                 static_cast<uint64_t>( handle ), held.generic_string(),
                                                 want.generic_string() );
    }

    Graphic::Material* MaterialService::Get( const Assets::AssetHandle& handle, Graphic::MeshVertexPath path,
                                             Graphic::MeshPass pass ) const
    {
        // Material-instance assets resolve through the parent chain to the BASE material: an
        // instance never builds a runtime Material of its own (its overrides live on the
        // per-entity MaterialInstance, see CreateRuntimeInstance). Depth-capped cycle guard.
        Assets::AssetHandle current = handle;
        for ( int depth = 0; depth < 8; ++depth )
        {
            if ( auto ait = m_MaterialAssets.find( current ); ait != m_MaterialAssets.end() )
            {
                // The chain is read out of the asset's DATA, so the asset has to have some. A released
                // shell answers IsInstance() with false and the walk stops at the instance instead of
                // resolving to its parent -- the surface then draws with the instance's own (empty)
                // material rather than the base it overrides.
                (void)EnsureLoaded( ait->second );
                auto*      surf = dynamic_cast<Assets::SurfaceMaterialAsset*>( ait->second.get() );
                const auto parentId =
                     ( surf != nullptr ) ? surf->Data().InstanceParentId() : std::optional<Common::UUID>{};
                if ( parentId.has_value() )
                {
                    const auto parent = GetAssetHandleByExternal( *parentId );
                    if ( parent.IsNull() || parent == current )
                        return nullptr;
                    current = parent;
                    continue;
                }
            }
            break;
        }

        const size_t slot = VariantSlot( path, pass );
        if ( auto it = m_Materials.find( current ); it != m_Materials.end() && it->second[slot] )
            return it->second[slot].get();

        // Lazy build: a shell is registered but this CELL's runtime material (with its bound textures)
        // isn't built yet. Every cell builds from the same asset, so the surface is the same by
        // construction rather than by anyone remembering to copy it across.
        if ( auto ait = m_MaterialAssets.find( current ); ait != m_MaterialAssets.end() )
        {
            // THE LINE THE ROUND TRIP WAS MISSING. Building from a released shell produces a material with
            // the shader's default parameters and no textures -- a white surface where a green one was,
            // with nothing in the log. See MaterialService::EnsureLoaded.
            (void)EnsureLoaded( ait->second );

            auto material = Graphic::MaterialFactory::CreateMaterial( ait->second.get(), path, pass );
            if ( !material )
                return nullptr; // MaterialFactory named the material and the cell it refused
            auto* raw           = material.get();
            m_BuiltToAsset[raw] = current;
            raw->ClaimOwnership( Graphic::ResourceOwner::AssetService, current );
            m_Materials[current][slot] = std::move( material );
            return raw;
        }
        return nullptr;
    }

    Graphic::MaterialPBR* MaterialService::GetVariant( const Graphic::MaterialPBR* built,
                                                       Graphic::MeshVertexPath path, Graphic::MeshPass pass ) const
    {
        if ( !built )
            return nullptr;

        const auto it = m_BuiltToAsset.find( built );
        if ( it == m_BuiltToAsset.end() )
            return nullptr; // not service-owned: a renderer's own dedicated material has no `.demat`

        // Both axes come from the caller, because both are the caller's: a render pass owns the pass, and
        // the renderer owns whether this draw is instanced. What is NOT the caller's is the asset, and
        // that is the one thing this function supplies — the sibling is the same `.demat`, so it carries
        // the same parameters and the same textures by construction.
        return dynamic_cast<Graphic::MaterialPBR*>( Get( it->second, path, pass ) );
    }

    bool MaterialService::Owns( const Graphic::Material* material ) const
    {
        return material && m_BuiltToAsset.count( material ) != 0;
    }

    std::vector<Graphic::Material*> MaterialService::GetBuiltVariants( const Assets::AssetHandle& handle ) const
    {
        std::vector<Graphic::Material*> out;
        const auto                      it = m_Materials.find( handle );
        if ( it == m_Materials.end() )
            return out;
        for ( const auto& variant : it->second )
            if ( variant )
                out.push_back( variant.get() );
        return out;
    }

    Graphic::MaterialInstancePtr MaterialService::CreateRuntimeInstance( const Assets::AssetHandle& handle,
                                                                         Graphic::MeshVertexPath    path ) const
    {
        // Collect the instance chain child -> base (depth-capped cycle guard), then create one
        // runtime instance of the base material and apply overrides base-first so the NEAREST
        // (childmost) override wins.
        std::vector<const Assets::SurfaceMaterialAsset*> chain;
        Assets::AssetHandle                              current = handle;
        for ( int depth = 0; depth < 8; ++depth )
        {
            auto ait = m_MaterialAssets.find( current );
            if ( ait == m_MaterialAssets.end() )
                break;
            // A released shell has no Data to walk: see MaterialService::EnsureLoaded.
            (void)EnsureLoaded( ait->second );
            auto* surf = dynamic_cast<Assets::SurfaceMaterialAsset*>( ait->second.get() );
            if ( surf == nullptr )
            {
                break;
            }
            const auto parentId = surf->Data().InstanceParentId();
            if ( !parentId.has_value() )
            {
                break;
            }
            chain.push_back( surf );
            const auto parent = GetAssetHandleByExternal( *parentId );
            if ( parent.IsNull() || parent == current )
                break;
            current = parent;
        }

        auto* base = Get( current, path );
        if ( !base )
            return nullptr;

        auto instance = base->CreateInstance();
        for ( auto it = chain.rbegin(); it != chain.rend(); ++it )
            for ( const auto& p : ( *it )->Data().Params )
                instance->SetParamFromVec4( p.Name, p.Value );
        return instance;
    }

    bool MaterialService::ResolveOverrides( const Assets::AssetHandle&  handle,
                                            Graphic::MaterialOverrides& out ) const
    {
        // Same walk as CreateRuntimeInstance: collect the instance chain child -> base (depth-capped
        // cycle guard), then append base-first so the childmost override lands last and wins.
        std::vector<const Assets::SurfaceMaterialAsset*> chain;
        Assets::AssetHandle                              current = handle;
        for ( int depth = 0; depth < 8; ++depth )
        {
            auto ait = m_MaterialAssets.find( current );
            if ( ait == m_MaterialAssets.end() )
                break;
            // A released shell has no Data to walk: see MaterialService::EnsureLoaded.
            (void)EnsureLoaded( ait->second );
            auto* surf = dynamic_cast<Assets::SurfaceMaterialAsset*>( ait->second.get() );
            if ( surf == nullptr )
            {
                break;
            }
            const auto parentId = surf->Data().InstanceParentId();
            if ( !parentId.has_value() )
            {
                break;
            }
            chain.push_back( surf );
            const auto parent = GetAssetHandleByExternal( *parentId );
            if ( parent.IsNull() || parent == current )
                break;
            current = parent;
        }

        auto baseIt = m_MaterialAssets.find( current );
        if ( baseIt == m_MaterialAssets.end() )
            return false;
        // The base is read for its parameters AND its textures, and a released shell has neither. This is
        // the terrain's path to its material, so without it a terrain drawn after a scene round trip loses
        // its authored surface silently.
        (void)EnsureLoaded( baseIt->second );
        auto* base = dynamic_cast<Assets::SurfaceMaterialAsset*>( baseIt->second.get() );
        if ( !base )
            return false;

        // Unlike CreateRuntimeInstance this carries TEXTURES as well as params. It can: the consumer binds
        // them onto its own material by sampler name, so there is no per-instance descriptor set to need.
        const auto append = [&out]( const Assets::MaterialData& data )
        {
            for ( const auto& p : data.Params )
                out.Params.emplace_back( p.Name, p.Value );
            // Textures, cloud assets and shader refs alike, each as its folded handle: the consumers bind by
            // slot name (a sampler, ApplyCloudAssetRef) and never see a GUID or a path.
            data.ForEachSlotHandle( [&out]( const std::string& name, uint64_t handle )
                                    { out.Textures.emplace_back( name, handle ); } );
        };

        append( base->Data() );
        for ( auto it = chain.rbegin(); it != chain.rend(); ++it )
            append( ( *it )->Data() );
        return true;
    }

    std::string MaterialService::ShaderNameOf( const Assets::AssetHandle& handle ) const
    {
        // Same chain walk as ResolveOverrides — an instance names no program of its own, so the answer is
        // always the base's. Depth-capped for the same cycle reason.
        Assets::AssetHandle current = handle;
        for ( int depth = 0; depth < 8; ++depth )
        {
            auto it = m_MaterialAssets.find( current );
            if ( it == m_MaterialAssets.end() )
                return {};
            (void)EnsureLoaded( it->second );
            auto* surf = dynamic_cast<Assets::SurfaceMaterialAsset*>( it->second.get() );
            if ( !surf )
                return {};
            const auto parentId = surf->Data().InstanceParentId();
            if ( !parentId.has_value() )
            {
                return surf->GetShaderName();
            }
            const auto parent = GetAssetHandleByExternal( *parentId );
            if ( parent.IsNull() || parent == current )
                return surf->GetShaderName();
            current = parent;
        }
        return {};
    }

    void MaterialService::Clear()
    {
        // Was an empty body. The graveyard goes with the rest: at shutdown there is no next frame to
        // collect it, and its materials own descriptor pools that must not outlive the device.
        m_Materials.clear();
        m_BuiltToAsset.clear();
        m_MaterialAssets.clear();
        m_ExternalToInternal.clear();
        m_Graveyard.clear();
    }

    void MaterialService::Invalidate( const Assets::AssetHandle& handle )
    {
        auto it = m_Materials.find( handle );
        if ( it == m_Materials.end() )
            return;
        // Keep the materials alive until CollectGarbage(): the frame being recorded (and frames
        // in flight) may still reference their descriptor pools — destroying them now invalidates
        // the command buffer (-> device lost).
        //
        // EVERY cell, not the static forward one: an asset whose shader changed is a different material on
        // all of them, and a surviving skinned or G-buffer variant would keep drawing the old shader with
        // no way left to notice — the graveyard is the only thing that retires it.
        //
        // The reverse index is dropped HERE and not in CollectGarbage: a graveyarded material is still
        // alive (frames in flight reference its pools) but it is no longer THE material for this asset,
        // and GetVariant answering from it would hand a render pass a sibling of a material that is
        // about to be destroyed.
        for ( auto& variant : it->second )
            if ( variant )
            {
                m_BuiltToAsset.erase( variant.get() );
                m_Graveyard.push_back( std::move( variant ) );
            }
        m_Materials.erase( it );
        ++m_InvalidationVersion; // cached instance sets rebuild on their next system tick
    }

    void MaterialService::Release( const Assets::AssetHandle& handle )
    {
        // The built materials first, through the graveyard — Invalidate already writes that correctly
        // (reverse index dropped now, destruction deferred to a safe point) and it bumps the stamp, which
        // is what makes every cached runtime instance of the dying material rebuild instead of holding a
        // pointer into it.
        Invalidate( handle );

        const auto it = m_MaterialAssets.find( handle );
        if ( it == m_MaterialAssets.end() )
            return;

        // The external id comes off the ASSET rather than being assumed equal to the handle. For a file
        // material the two are the same value by construction (SurfaceMaterialAsset::Load adopts the
        // in-file MaterialId as both), but an imported material's ids genuinely diverge, and an entry left
        // behind would keep resolving a material that no longer exists for as long as the editor runs.
        if ( it->second )
            m_ExternalToInternal.erase( it->second->GetMaterialUUID() );
        m_MaterialAssets.erase( it );
    }

    void MaterialService::CollectGarbage()
    {
        if ( m_Graveyard.empty() )
            return;
        // Safe point: no frame is being recorded (caller guarantees frame start) and idle-wait
        // retires every in-flight frame that could reference the dying descriptor pools.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();
        m_Graveyard.clear();
    }

    Graphic::Material* MaterialService::GetByExternalHandle( const Common::UUID& handle ) const
    {
        auto it = m_ExternalToInternal.find( handle );
        if ( it == m_ExternalToInternal.end() )
        {
            return nullptr;
        }

        return Get( it->second );
    }

    Assets::AssetHandle MaterialService::GetAssetHandleByExternal( const Common::UUID& uuid ) const
    {
        auto it = m_ExternalToInternal.find( uuid );
        if ( it != m_ExternalToInternal.end() )
            return it->second;

        // Identity fallback: file materials ADOPT their header GUID's handle as the asset handle,
        // so for them external id == internal handle. This makes resolution independent of the
        // order the external->internal map fills in (the map stays authoritative for imported
        // materials whose ids genuinely diverge).
        const Assets::AssetHandle asHandle{ uuid };
        if ( m_MaterialAssets.count( asHandle ) || m_Materials.count( asHandle ) )
            return asHandle;

        return Common::UUID::Null();
    }

} // namespace Desert::Runtime
