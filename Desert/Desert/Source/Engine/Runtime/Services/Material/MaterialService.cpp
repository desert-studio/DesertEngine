#include "MaterialService.hpp"
#include <Engine/Core/FrameManager.hpp>

#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/Materials/Properties/Texture2DProperty.hpp>
#include <Engine/Graphic/MaterialPipelineStates.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/Renderer.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Runtime
{
    namespace
    {
        // Every `Texture2D` slot the template's manifest declares gets a value on every application: the
        // image the `.demat` names for it, or — for a slot the file does not name, has just stopped naming,
        // whose texture is still being read, or whose handle nobody has — the slot's schema default
        // (`setSlot(name, nullptr)`). THE LOOP IS OVER THE SCHEMA, NOT OVER THE FILE (М9): walking the file
        // visits only the slots it mentions, and this runs again over a LIVE material when the `.demat`
        // changes (AssetHotReload), so a slot the file stopped naming would keep drawing its old texture.
        // One function for every template: a slot a shader adds is bound by name, with no line to add here.
        template <class SetSlot>
        void BindManifestTextures( const Core::Formats::ShaderProgramMeta& meta,
                                   const Assets::SurfaceMaterialAsset& asset, const std::string& shaderName,
                                   SetSlot&& setSlot )
        {
            const auto& data = asset.Data();
            Core::Formats::ForEachMaterialTextureSlot(
                 meta, [&data]( const std::string& name ) { return data.GetTexture( name ); },
                 [&]( const Core::Formats::ShaderParam& param, uint64_t handle )
                 {
                     if ( handle == 0 )
                     {
                         setSlot( param.Name, nullptr ); // an empty slot is an authored decision
                         return;
                     }

                     auto* textures = ResourceRegistry::GetTextureService();
                     if ( auto* tex = textures->Get( Common::UUID( handle ) ) )
                     {
                         if ( auto* image = ResourceRegistry::GetImageService()->Resolve( tex->GetImageHandle() );
                              image != nullptr )
                         {
                             auto* img = dynamic_cast<Graphic::Image2D*>( image );
                             if ( img == nullptr )
                                 LOG_ERROR(
                                      "[Materials] '{0}' binds texture handle {1} in its '{2}' slot, and that "
                                      "texture's image is not a 2D image, so '{3}' samples the slot's "
                                      "schema default instead.",
                                      asset.GetMetadata().Filepath.string(), handle, param.Name, shaderName );
                             setSlot( param.Name, img );
                             return;
                         }
                     }

                     // PENDING IS NOT MISSING (AL1-4): the slot shows its default until the texture lands,
                     // and the texture service rebuilds this material then, bound.
                     setSlot( param.Name, nullptr );
                     if ( textures->Require( Common::UUID( handle ) ).IsPending() )
                     {
                         textures->RebuildWhenReady( Common::UUID( handle ), asset.GetMetadata().Handle );
                         return;
                     }

                     // DC §1.4: a `.demat` names its textures by number and by nothing else, so a reference
                     // that stops resolving produces a surface that is merely untextured — nothing in the log
                     // a search can start from. This is the only place that knows the material, the slot and
                     // the number together.
                     LOG_ERROR( "[Materials] '{0}' names texture handle {1} in its '{2}' slot and no texture "
                                "with that handle is registered, so '{3}' samples that slot's schema default "
                                "instead. A texture's handle is HandleForGuid of its .detex header GUID, which "
                                "the slot names, so this means that texture asset is not in the project "
                                "(deleted, or never imported); re-import it or re-assign the slot.",
                                asset.GetMetadata().Filepath.string(), handle, param.Name, shaderName );
                 } );
        }

        // Every Texture2D slot of the schema gets its sampling state on every application, for the same
        // reason BindManifestTextures walks the schema: a .demat that stops stating a clamp must go back to
        // the template's state on hot reload, not keep the clamp.
        void BindManifestSamplers( const Core::Formats::ShaderProgramMeta& meta, const Assets::MaterialData& data,
                                   const Graphic::Material& material )
        {
            for ( const auto& param : meta.Params )
            {
                if ( !param.IsTexture || param.IsCubeTexture || param.IsAssetRef() )
                    continue;
                if ( auto* prop = material.Get<Graphic::Texture2DProperty>( param.Name ) )
                    prop->SetSamplerState( data.SlotSampler( param.Name, param.Sampler ) );
            }
        }
    } // namespace

    void ApplySurfaceAsset( Graphic::DataDrivenMaterial& material, const Assets::SurfaceMaterialAsset& asset )
    {
        // Seed schema defaults, then overlay the asset's persisted parameter values.
        material.ApplyDefaults();

        const auto& data = asset.Data();
        for ( const auto& p : data.Params )
            material.SetParamRaw( p.Name, p.Value );
        // The template's `Surface { TwoSided }` is the material's DEFAULT (UE: the parent material's TwoSided);
        // the asset states it only to override. The parser publishes it as the default cell's Cull None
        // (DShaderParser: Meta.State = the default cell's state), so every draw path — including the batched
        // static one, whose pipeline is built from the pass shader and not from the template's cells — reaches
        // it through the same CullPermutation.
        const bool templateTwoSided = material.GetSchema().State.Cull == Core::Formats::StateCull::None;
        material.SetTwoSided( data.TwoSided.value_or( templateTwoSided ) );

        // `MaterialData::Textures` holds the sampler slots only since MATL 3 (the cloud material's asset
        // slots have a list of their own), but the SHADER SCHEMA still says what each name is: a `Texture2D`
        // sampler, a `TextureCube` (bound by MaterialSkybox, not here), or a non-texture asset reference a
        // different service consumes. Asking the schema is what lets the miss below be an ERROR, not noise.
        const auto& schema   = material.GetSchema();
        const auto  paramFor = [&schema]( const std::string& name ) -> const Core::Formats::ShaderParam*
        {
            for ( const auto& p : schema.Params )
                if ( p.Name == name )
                    return &p;
            return nullptr;
        };

        BindManifestTextures( schema, asset, material.GetShaderName(),
                              [&material]( const std::string& name, Graphic::Image2D* image )
                              { material.SetTexture( name, image ); } );
        BindManifestSamplers( schema, data, material );

        // The file's side of the same relation: a name the material carries that the shader no longer
        // declares. It cannot be found by the loop above (which only walks names the shader HAS), and it
        // is the one thing that loop can no longer report.
        if ( !schema.Params.empty() )
        {
            for ( const auto& t : data.Textures )
            {
                if ( t.Guid.empty() || paramFor( t.Name ) != nullptr )
                    continue;

                LOG_WARN( "[Materials] '{0}' carries a value for '{1}', which the shader '{2}' does not "
                          "declare. The value is ignored — the slot was renamed or removed from the "
                          "shader since this material was authored.",
                          asset.GetMetadata().Filepath.string(), t.Name, material.GetShaderName() );
            }
        }
    }

    std::shared_ptr<Graphic::DataDrivenMaterial> CreateSurfaceMaterial( const Assets::MaterialAsset* asset,
                                                                        Graphic::MeshVertexPath      path,
                                                                        Graphic::MeshPass            pass )
    {
        if ( asset == nullptr )
            return nullptr;

        // ROUTED BY THE TEMPLATE'S HANDLE (its GUID identity), never by its name: the name is display text
        // and renaming a shader must not change which template draws it.
        if ( asset->GetShaderHandle().IsNull() )
        {
            LOG_ERROR( "[Materials] Material '{}' has no resolved surface template (its \"Shader\" GUID is "
                       "missing or names no loaded shader) — it draws nothing; there is no default template.",
                       asset->GetMetadata().Filepath.generic_string() );
            return nullptr;
        }

        // ONE PATH FOR EVERY TEMPLATE. There used to be a branch here: the template declaring `Role
        // PBRSurface` was built as the C++ PBR class and every other one as a DataDrivenMaterial,
        // with two appliers that had to be kept saying the same thing. A material is one template cell's
        // descriptor sets plus a parameter row, whatever the template shades like; the scene's part of the
        // draw is declared by the template's resources (Core::Formats::MaterialLayout::SceneReads), so the
        // C++ class has nothing left to add.
        //
        // THERE IS DELIBERATELY NO DOMAIN CHECK HERE: this function does not know its consumer. A Terrain
        // material is legitimately built by the terrain, the Material Editor and the File Explorer; the
        // refusal lives in MeshRenderer::DrawGenericMeshes, which asks Core::Formats::DrawnByMeshPath().
        const std::string templateName = asset->GetShaderName();
        const auto        cell         = Graphic::SurfaceCellShader( templateName, path, pass );
        if ( !cell )
        {
            LOG_WARN( "[Materials] Material '{}' uses the template '{}', which has no ({} x {}) cell — a DSL "
                      "surface carries no skinning, instancing or G-buffer stage. The mesh asking for it "
                      "falls back to the default surface material; assign a material whose template has "
                      "that cell, or author one for '{}'.",
                      asset->GetMetadata().Filepath.generic_string(), templateName,
                      Graphic::MeshVertexPathName( path ), Graphic::MeshPassName( pass ), templateName );
            return nullptr;
        }

        const std::string& shaderName = *cell;
        auto               material   = std::make_shared<Graphic::DataDrivenMaterial>( shaderName );
        // ON LOAD, not at the first draw (AL1-12, UE's PSO precache): every renderer starts this shader's
        // pipeline compile on a worker from its next frame, so it has usually landed before the mesh is seen.
        // The request is for the GENERIC path's pipeline; a mesh-table cell is never drawn there (the mesh
        // system routes it to the batched path by the same MeshCellPath question), so asking for one built a
        // forward pipeline of a G-buffer shader nobody draws with, and validation flagged its unused outputs.
        if ( !Graphic::MeshCellPath( shaderName ) )
            Graphic::MaterialPipelineRequests::Get().Request( shaderName );
        if ( const auto* surface = dynamic_cast<const Assets::SurfaceMaterialAsset*>( asset ) )
            ApplySurfaceAsset( *material, *surface );
        return material;
    }

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
        auto material = CreateSurfaceMaterial( materialAsset.get(), Graphic::MeshVertexPath::Static,
                                               Graphic::MeshPass::Forward );

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

    void MaterialService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_Assets = assets;
    }

    void MaterialService::StartRead( const Assets::AssetHandle&        handle,
                                     std::vector<Assets::AssetHandle>& awaited ) const
    {
        if ( handle.IsNull() )
            return;
        // A copy of the pointer, not the iterator: the request below may discover and rehash.
        const auto it = FindOrDiscover( handle );
        if ( it == m_MaterialAssets.end() )
            return;
        const std::shared_ptr<Assets::MaterialAsset> shell = it->second;
        if ( !RequestIfUnread( shell ) )
            awaited.emplace_back( shell->GetMetadata().Handle );
    }

    std::unordered_map<Assets::AssetHandle, std::shared_ptr<Assets::MaterialAsset>>::iterator
    MaterialService::FindOrDiscover( const Assets::AssetHandle& handle ) const
    {
        if ( const auto it = m_MaterialAssets.find( handle ); it != m_MaterialAssets.end() )
            return it;
        if ( handle.IsNull() || m_ReportedMissing.contains( handle ) || m_Assets.expired() )
            return m_MaterialAssets.end();

        auto created = Assets::CreateFromRegistryRow<Assets::SurfaceMaterialAsset>(
             m_Assets, handle, Common::Content::ContentKind::Material );
        if ( !created )
        {
            m_ReportedMissing.insert( handle );
            LOG_ERROR( "[MaterialService] {}", created.GetError() );
            return m_MaterialAssets.end();
        }
        // The external id IS the handle for every file-backed material (SurfaceMaterialAsset adopts both
        // from the header GUID in its constructor), so the mesh->material link needs no load to resolve.
        const auto& asset                              = created.GetValue();
        m_ExternalToInternal[asset->GetMaterialUUID()] = handle;
        return m_MaterialAssets.emplace( handle, asset ).first;
    }

    bool MaterialService::RequestIfUnread( const std::shared_ptr<Assets::MaterialAsset>& asset ) const
    {
        if ( asset->IsReadyForUse() )
            return true;
        const auto handle = asset->GetMetadata().Handle;
        if ( m_Requests.contains( handle ) )
            return false;
        // BOTH DELEGATES (T2.3). The completion bumps the invalidation version, so every cached instance
        // set that drew nothing for this material asks again on its next tick.
        m_Requests[handle] = Assets::AsyncAssetLoader::Get().Request(
             asset,
             [this, handle]( const Assets::Asset<Assets::AssetBase>& loaded, const Assets::LoadOutcome outcome,
                             const std::string& error )
             {
                 m_Requests.erase( handle );
                 if ( outcome != Assets::LoadOutcome::Loaded )
                 {
                     LOG_ERROR( "[MaterialService] '{}' could not be read: {}",
                                loaded->GetMetadata().Filepath.string(), error );
                     return;
                 }
                 // The shader is resolved by name through the manager, which a worker may not touch.
                 if ( const auto manager = m_Assets.lock() )
                     loaded->ResolveDependencies( *manager );
                 ++m_InvalidationVersion;
             },
             [this, handle] { m_Requests.erase( handle ); } );
        return false;
    }

    Common::BoolResultStr
    MaterialService::EnsureLoaded( const std::shared_ptr<Assets::MaterialAsset>& asset ) const
    {
        if ( !asset )
            return Common::MakeError<bool>( "MaterialService: a null material shell cannot be loaded" );
        if ( asset->IsReadyForUse() )
            return BOOLSUCCESS;

        // ONE LOADING PATH (AL1-4, plan §2.4(c)). The walks that need the data NOW (CreateRuntimeInstance,
        // ResolveOverrides, ShaderHandleOf, the editor) go through the same request Get() starts, finished on
        // this thread by FlushOne: no second read of a file a worker is already reading, the shader resolved
        // by the one completion, and SyncLoadLedger counting it as the synchronous load it is.
        const auto handle = asset->GetMetadata().Handle;
        if ( !RequestIfUnread( asset ) )
            Assets::AsyncAssetLoader::Get().FlushOne( handle );

        if ( !asset->IsReadyForUse() )
        {
            // Loudly, and then the caller decides. A material that cannot be read is a surface that will
            // draw with the shader's own defaults, and the ONLY place that knows which file it was is here.
            LOG_ERROR( "[MaterialService] '{}' could not be read. Anything drawn with it falls back to the "
                       "shader's default parameters.",
                       asset->GetMetadata().Filepath.string() );
            return Common::MakeFormattedError<bool>( "material '{}' could not be read",
                                                     asset->GetMetadata().Filepath.string() );
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
            if ( auto ait = FindOrDiscover( current ); ait != m_MaterialAssets.end() )
            {
                // The chain is read out of the asset's DATA, so the asset has to have some. A released
                // shell answers IsInstance() with false and the walk stops at the instance instead of
                // resolving to its parent -- the surface then draws with the instance's own (empty)
                // material rather than the base it overrides. NOT READ HERE (AL1-4): Get runs in a frame,
                // so an unread shell starts its read and answers Pending (nullptr) until it lands.
                if ( !RequestIfUnread( ait->second ) )
                    return nullptr;
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
        if ( auto ait = FindOrDiscover( current ); ait != m_MaterialAssets.end() )
        {
            // THE LINE THE ROUND TRIP WAS MISSING. Building from a released shell produces a material with
            // the shader's default parameters and no textures -- a white surface where a green one was,
            // with nothing in the log. See MaterialService::EnsureLoaded.
            if ( !RequestIfUnread( ait->second ) )
                return nullptr;

            auto material = CreateSurfaceMaterial( ait->second.get(), path, pass );
            if ( !material )
                return nullptr; // CreateSurfaceMaterial named the material and the cell it refused
            auto* raw           = material.get();
            m_BuiltToAsset[raw] = current;
            raw->ClaimOwnership( Graphic::ResourceOwner::AssetService, current );
            m_Materials[current][slot] = std::move( material );
            return raw;
        }
        return nullptr;
    }

    Graphic::DataDrivenMaterial* MaterialService::GetVariant( const Graphic::Material* built,
                                                              Graphic::MeshVertexPath  path,
                                                              Graphic::MeshPass        pass ) const
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
        // Every material the service builds is a DataDrivenMaterial (CreateSurfaceMaterial); a sibling that is
        // not one was not built here, and is refused by its cell rather than drawn as the wrong class.
        Graphic::Material* sibling = Get( it->second, path, pass );
        auto*              surface = dynamic_cast<Graphic::DataDrivenMaterial*>( sibling );
        if ( sibling != nullptr && surface == nullptr )
            LOG_ERROR(
                 "[MaterialService] the ({} x {}) sibling of a service material is not a DataDrivenMaterial; "
                 "refusing it",
                 Graphic::MeshVertexPathName( path ), Graphic::MeshPassName( pass ) );
        return surface;
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
            auto ait = FindOrDiscover( current );
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
        {
            for ( const auto& p : ( *it )->Data().Params )
                instance->SetParamFromVec4( p.Name, p.Value );
            // The childmost instance that states TwoSided wins, as its parameters do.
            if ( ( *it )->Data().TwoSided.has_value() )
                instance->SetTwoSidedOverride( ( *it )->Data().TwoSided );
        }
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
            auto ait = FindOrDiscover( current );
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

        auto baseIt = FindOrDiscover( current );
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

    MaterialService::MaterialTemplate MaterialService::ShaderHandleOf( const Assets::AssetHandle& handle ) const
    {
        // Same chain walk as ResolveOverrides — an instance names no program of its own, so the answer is
        // always the base's. Depth-capped for the same cycle reason.
        Assets::AssetHandle current = handle;
        for ( int depth = 0; depth < 8; ++depth )
        {
            auto it = FindOrDiscover( current );
            if ( it == m_MaterialAssets.end() )
                return MaterialTemplate{};
            (void)EnsureLoaded( it->second );
            auto* surf = dynamic_cast<Assets::SurfaceMaterialAsset*>( it->second.get() );
            if ( !surf )
                return MaterialTemplate{};
            const auto parentId = surf->Data().InstanceParentId();
            if ( !parentId.has_value() )
            {
                return MaterialTemplate{ surf->GetShaderHandle(), surf->GetShaderName() };
            }
            const auto parent = GetAssetHandleByExternal( *parentId );
            if ( parent.IsNull() || parent == current )
                return MaterialTemplate{ surf->GetShaderHandle(), surf->GetShaderName() };
            current = parent;
        }
        return MaterialTemplate{};
    }

    void MaterialService::Clear()
    {
        // Was an empty body. The graveyard goes with the rest: at shutdown there is no next frame to
        // collect it, and its materials own descriptor pools that must not outlive the device.
        m_Materials.clear();
        m_BuiltToAsset.clear();
        m_MaterialAssets.clear();
        m_Requests.clear();
        m_ReportedMissing.clear();
        m_ExternalToInternal.clear();
        m_Graveyard.Clear();
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
                m_Graveyard.Park( std::move( variant ),
                                  Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() );
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
        // Safe point: no frame is being recorded (caller guarantees frame start). Only the materials whose
        // last possible frame has retired are destroyed; the rest wait for a later frame start.
        const auto& frames = Engine::FrameManager::GetInstance();
        m_Graveyard.Collect( frames.GetAbsoluteFrameCount(), frames.GetMaxFramesInFlight() );
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
        if ( m_Materials.contains( asHandle ) || FindOrDiscover( asHandle ) != m_MaterialAssets.end() )
            return asHandle;

        return Common::UUID::Null();
    }

} // namespace Desert::Runtime
