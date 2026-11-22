#include <Engine/Core/Serialize/ComponentSerializerKit.hpp>

namespace Desert::Core::Serialize
{
    using namespace Builtins;

    // Resolves reflected AssetHandle fields to/from on-disk PATHS (backward-compatible with the old
    // per-component serializers). Dispatches by the field's PROPERTY(Asset<...>) type name. Only a few
    // asset types exist, so this is a small hand-written table (not codegen). Captures `mgr` by ref —
    // only used within the (de)serialize call that builds it.
    //
    // AT NAMESPACE SCOPE rather than in the anonymous namespace above, because SceneSettings is reflected
    // like a component and serialized like one, but is not one: SceneSerializer writes it directly, and
    // could not reach a resolver that only existed inside this file.
    namespace
    {

        // The level's Default Pawn (SceneSettings), LOADED HERE, not at Play: Play instantiates it on the
        // frame the button is pressed, and a prefab still reading from disk then would be a refused Play for
        // a level that names a perfectly good pawn. One loader for both spellings the resolver meets — the
        // GUID's registry row and the locator path — so a prefab is loaded one way whichever resolved it.
        uint64_t LoadScenePrefab( Assets::AssetManager& m, const std::filesystem::path& full )
        {
            auto a = m.FindByPath<Assets::PrefabAsset>( full );
            if ( !a )
                a = m.CreateAsset<Assets::PrefabAsset>( full, /*loadAfterCreate=*/false );
            if ( !a )
                return 0;
            if ( !a->IsReadyForUse() )
            {
                if ( const auto loaded = Assets::LoadThroughLoader( m, a ); !loaded )
                {
                    LOG_ERROR( "[Scene] Prefab '{}' named by the scene could not be loaded: {}", full.string(),
                               loaded.GetError() );
                    return 0;
                }
            }
            return static_cast<uint64_t>( a->GetMetadata().Handle );
        }
    } // namespace

    Reflection::AssetResolver MakeAssetResolver( const Assets::AssetManager& mgr )
    {
        Reflection::AssetResolver r;

        r.ToPath = []( uint64_t handle, const std::string& type ) -> std::string
        {
            if ( handle == 0 )
                return "";

            // TWELVE PER-TYPE LOOKUPS STOOD HERE AND THERE IS NOW ONE, which is the second half of
            // GAP_ANALYSIS T2.4: the cooked registry is the source of BOTH answers.
            //
            // Each of the twelve was the same shape — `mgr.FindByHandle<SomeAsset>( handle )` and then
            // `GetMetadata().Filepath` bent into whatever string that type stores. That arrangement
            // needed the asset REGISTERED, and the only thing that registered assets wholesale was the
            // boot's directory walk; with the walk gone it would have been twelve branches one step
            // from returning "" — which a scene saves as an empty slot and reads back as unset, losing
            // the reference with nothing anywhere saying so. `ContentRegistry::KeyForHandle` answers
            // from a FILE instead, once, for every type, with no AssetManager involved at all (the
            // lambda no longer captures one).
            //
            // WHAT DID NOT COLLAPSE IS THE SPELLING, and it must not: the string a `.desce` stores is
            // a FORMAT, and changing it is a corpus migration rather than an edit. Three forms serve
            // all twelve types, `StoredAssetForm` below names which type takes which, and each one is
            // byte-identical to what its branch produced. So the IDENTITY question has one answer and
            // only the rendering differs — twelve lookups became three renderings.
            const std::optional<StoredAssetForm> form = StoredFormFor( type );
            if ( !form )
            {
                // AND ANYTHING ELSE REFUSES, LOUDLY. The refusal cannot save the field — there is no
                // form to write it in — but it turns a slot that quietly loses its value into one line
                // naming the type that needs one. Desert/Tests/Engine/AssetResolverCensus catches the
                // same omission earlier, at the moment the field is declared; this catches everything
                // the census cannot see, including a type that reaches the resolver from somewhere else.
                LOG_ERROR( "[Scene] asset type '{0}' has no branch in Core::MakeAssetResolver, so a "
                           "reference of that type cannot be written to a scene: the slot will save as "
                           "empty and load as unset. Add a row for it beside the others in "
                           "ComponentRegistry.cpp.",
                           type );
                return "";
            }

            const std::string key = Assets::ContentRegistry::KeyForHandle( handle );
            if ( key.empty() )
            {
                LOG_ERROR( "[Scene] handle {0} is set on a '{1}' slot and nothing in this project names "
                           "it — neither the cooked asset registry nor anything this session derived. "
                           "The slot is being written out EMPTY and the reference is lost. If the file "
                           "is on disk, the registry is stale: restart the editor (it gathers the content roots "
                           "at start).",
                           handle, type );
                return "";
            }

            std::string stored = RenderStoredForm( *form, key );
            if ( stored.empty() )
                LOG_ERROR( "[Scene] the '{0}' at '{1}' lies outside every content root, and a '{0}' reference "
                           "is stored only as a project key: the slot is written EMPTY rather than as a path "
                           "that names one machine. Move the file under the assets root.",
                           type, key );
            return stored;
        };

        // THE SAVE DIRECTION, like ToPath above: it only reads what is already loaded and never resolves a
        // reference, so it registers nothing. It is the ONE handle-to-GUID answer for every reference a
        // scene stores by GUID (reflected skybox and texture fields, and the mesh and material slots the
        // hand-written serializers below write), so the loaded-asset-then-registry rule exists once.
        r.ToGuid = [&mgr]( uint64_t handle, const std::string& type ) -> std::string
        {
            if ( handle == 0 )
                return {};
            const Common::UUID                        id( handle );
            std::optional<Common::Content::AssetGuid> loaded;
            if ( type == "SkyboxAsset" )
            {
                if ( const auto a = mgr.FindByHandle<Assets::SkyboxAsset>( id ) )
                    loaded = a->Guid();
            }
            else if ( type == "TextureAsset" )
            {
                if ( const auto a = mgr.FindByHandle<Assets::TextureAsset>( id ) )
                    loaded = a->Guid();
            }
            else if ( type == "PrefabAsset" )
            {
                if ( const auto a = mgr.FindByHandle<Assets::PrefabAsset>( id ) )
                    loaded = a->Guid();
            }
            else if ( type == "FoliageTypeAsset" )
            {
                if ( const auto a = mgr.FindByHandle<Assets::FoliageTypeAsset>( id ) )
                    loaded = a->Guid();
            }
            else if ( type == "MaterialAsset" )
            {
                if ( const auto a = mgr.FindByHandle<Assets::SurfaceMaterialAsset>( id ) )
                    loaded = a->Data().Guid();
            }
            else if ( type == "StaticMeshAsset" || type == "SkinnedMeshAsset" )
            {
                if ( const auto a = mgr.FindByHandle<Assets::MeshAsset>( id ) )
                    loaded = a->Guid();
            }
            return GuidTextOrRegistry( handle, loaded, type.c_str() );
        };

        r.FromPath = [&mgr]( const std::string& path, const std::string& type ) -> uint64_t
        {
            if ( path.empty() )
                return 0;
            auto& m = const_cast<Assets::AssetManager&>( mgr );

            if ( type == "SkyboxAsset" )
            {
                // The path names a registry ROW, and the row is what is required — the same route as the GUID
                // spelling below, so a skybox is created and requested one way whichever spelling a scene uses.
                const Assets::AssetHandle handle = Runtime::SkyboxHandleAtPath( path );
                if ( static_cast<uint64_t>( handle ) == 0 )
                {
                    LOG_ERROR(
                         "[Skybox] Skybox path '{0}' named by a component is no skybox row of the content "
                         "registry, so the scene has NO environment: the sky draws black and the lit "
                         "materials get no ambient. Skyboxes are the Skybox-kind `.detex` files under '{1}'; "
                         "re-point the Skybox component.",
                         path, Common::Constants::Path::ASSETS_PATH.string() );
                    return 0;
                }
                return Runtime::RequireSkybox( handle ) ? static_cast<uint64_t>( handle ) : 0;
            }
            if ( type == "MaterialAsset" )
            {
                // The stored reference is the content key (or an absolute path outside the project), expanded
                // ONCE, here, by FileForStoredReference; the bare assets-relative spelling is refused (PATH1).
                const std::filesystem::path full = FileForStoredReference( path, type );
                if ( full.empty() )
                    return 0;

                // FIND, ELSE CREATE, THEN REGISTER — and the "then" is outside both branches on purpose.
                // Registering only what this parse CREATED is the defect this shape retires; see
                // AssetReferenceResolve.hpp for why the two routes are asserted to agree rather than
                // asserted one at a time. The create-on-miss is still needed: an editor `.demat` outside
                // the Materials root is not preloaded, and it must survive a cold restart rather than only
                // an in-session reload.
                const auto a = ResolveSceneReference(
                     [&] { return mgr.FindByPath<Assets::MaterialAsset>( full ); },
                     [&]
                     {
                         return Assets::Asset<Assets::MaterialAsset>(
                              m.CreateAsset<Assets::SurfaceMaterialAsset>( full,
                                                                           /*loadAfterCreate=*/false ) );
                     },
                     []( const Assets::Asset<Assets::MaterialAsset>& material, ReferenceOrigin )
                     { Runtime::EnsureMaterialRegistered( material ); } );
                return a ? static_cast<uint64_t>( a->GetMetadata().Handle ) : 0;
            }
            if ( type == "TextureAsset" )
            {
                // The reference itself is resolved in TextureSlot.cpp — every spelling accepted,
                // create-on-miss, and a logged reason instead of the silent 0 this branch used to
                // return. (The parenthesis that stood here said the create-on-miss was ALSO the
                // spelling-independent lookup FindByPath was not. It no longer is: both entry points
                // answer on the same identity — see Desert/Tests/Engine/AssetPathIdentity.)
                const uint64_t resolved = TextureSlotFromPath( m, path );
                if ( resolved == 0 )
                    return 0;

                Runtime::EnsureTextureRegistered( mgr, resolved );
                return resolved;
            }
            // The read side of the volume branch is gone with the write side above, and for the same
            // reason: no reflected field names that asset type any more. A cloud type's own volume is
            // bound in Assets::CloudTypeAsset::ResolveDependencies, from the path inside the type's
            // file, which is where a reference to a `.dcnv` now lives.
            // THE READ-SIDE "CloudTypeAsset" AND "CloudLayoutAsset" BRANCHES ARE GONE WITH THE WRITE
            // SIDE ABOVE (O1). What replaced their register-on-load duty: CloudTypeService and CloudLayoutService
            // create the named file from its registry row on first use (AL1-2, AL1-7), and the Material Editor's
            // drop target registers an out-of-library file the moment it is bound. A file outside the
            // library that only a `.demat` names is NOT re-registered on the next launch — the renderer
            // and the services say so loudly, once, with the handle — which is the named cost of the
            // deletion rather than an oversight.
            if ( type == "CloudModellingVolumeAsset" )
            {
                const std::filesystem::path full = FileForStoredReference( path, type );
                if ( full.empty() )
                    return 0;

                auto a = mgr.FindByPath<Assets::CloudModellingVolumeAsset>( full );
                if ( !a )
                    a = m.CreateAsset<Assets::CloudModellingVolumeAsset>( full,
                                                                          /*loadAfterCreate=*/false );
                if ( !a )
                    return 0;

                // ANNOUNCED, NOT READ — AND THIS LINE USED TO BE A 4 MiB BLOCKING READ INSIDE THE SCENE
                // DESERIALISER. It said `if ( !a->IsReadyForUse() && !a->Load() ) return 0;`, which in the
                // editor runs AFTER the boot, so `SyncLoadLedger` counted every one of them as an
                // in-frame load: the very hitch the detector exists to name. It was invisible because the
                // preloader had almost always read the file already, so the branch was almost never
                // taken — the eager preload was hiding a blocking read, not avoiding one, and removing
                // the preload is exactly what would have exposed it.
                //
                // The hero cloud's own renderer asks for the body through `RequireBody` when it comes to
                // draw it, and waits on a three-state answer. Deserialising a scene must establish WHICH
                // asset a field names; it has never been the right place to read one.
                Runtime::ResourceRegistry::GetCloudModellingService()->Announce( a );
                return static_cast<uint64_t>( a->GetMetadata().Handle );
            }
            if ( type == "ControlRigAsset" )
            {
                const std::filesystem::path full = FileForStoredReference( path, type );
                if ( full.empty() )
                    return 0;

                auto a = mgr.FindByPath<Assets::ControlRigAsset>( full );
                if ( !a )
                {
                    a = m.CreateAsset<Assets::ControlRigAsset>( full,
                                                                /*loadAfterCreate=*/false );
                }
                if ( !a )
                {
                    return 0;
                }
                // LOADED HERE AND NOT LEFT TO THE FIRST FRAME. A rig that is not ready is a rig
                // AnimationECSSystem cannot build a stage from, and the entity would pose from its clip
                // alone while the scene file plainly names a rig — the silent shape this whole task exists
                // to avoid. There is no service registry for rigs: the stage is per entity and per
                // skeleton, so it is built by the ECS system rather than registered globally.
                if ( !a->IsReadyForUse() )
                {
                    if ( const auto loaded = Assets::LoadThroughLoader( m, a ); !loaded )
                    {
                        LOG_ERROR( "[Animation] Control rig '{}' named by the scene could not be loaded: {}",
                                   full.string(), loaded.GetError() );
                        return 0;
                    }
                }
                return static_cast<uint64_t>( a->GetMetadata().Handle );
            }
            if ( type == "RetargetAsset" )
            {
                const std::filesystem::path full = FileForStoredReference( path, type );
                if ( full.empty() )
                    return 0;

                auto a = mgr.FindByPath<Assets::RetargetAsset>( full );
                if ( !a )
                {
                    a = m.CreateAsset<Assets::RetargetAsset>( full,
                                                              /*loadAfterCreate=*/false );
                }
                if ( !a )
                {
                    return 0;
                }
                // LOADED HERE AND NOT LEFT TO THE FIRST FRAME, for the rig's reason one branch up — and
                // with one more reason of its own: loading is what runs `ResolveDependencies`, and that is
                // what binds the SOURCE rig. A retarget whose source rig is unbound is a retarget
                // AnimationECSSystem cannot build, and the character would pose from its clip on the wrong
                // proportions while the scene file plainly names a retarget.
                if ( !a->IsReadyForUse() )
                {
                    if ( const auto loaded = Assets::LoadThroughLoader( m, a ); !loaded )
                    {
                        LOG_ERROR( "[Animation] Retarget '{}' named by the scene could not be loaded: {}",
                                   full.string(), loaded.GetError() );
                        return 0;
                    }
                }
                return static_cast<uint64_t>( a->GetMetadata().Handle );
            }
            if ( type == "FoliageTypeAsset" )
            {
                // The locator half of a procedural volume's type list {Guid, Path}: ResolveGuidRef reaches it
                // only when the GUID's registry row did not, and checks the file found here IS that type.
                const std::filesystem::path full = FileForStoredReference( path, type );
                if ( full.empty() )
                    return 0;
                auto a = mgr.FindByPath<Assets::FoliageTypeAsset>( full );
                if ( !a )
                    a = m.CreateAsset<Assets::FoliageTypeAsset>( full, /*loadAfterCreate=*/false );
                return a ? static_cast<uint64_t>( a->GetMetadata().Handle ) : 0;
            }
            if ( type == "FractureAsset" )
            {
                // A destructible's rest collection (DestructibleData::Fracture). ANNOUNCED, NOT READ, for the
                // cloud body's reason above: FractureService reads the `.dfrac` when Play asks for it, and the
                // asset's constructor adopts the handle its envelope GUID names, so the service's registry row
                // and this handle are one identity.
                const std::filesystem::path full = FileForStoredReference( path, type );
                if ( full.empty() )
                    return 0;
                auto a = mgr.FindByPath<Assets::FractureAsset>( full );
                if ( !a )
                    a = m.CreateAsset<Assets::FractureAsset>( full, /*loadAfterCreate=*/false );
                return a ? static_cast<uint64_t>( a->GetMetadata().Handle ) : 0;
            }
            if ( type == "WaterWavesAsset" )
            {
                // A water body's wave set (WaterBodyData::WaterWaves). Announced, not read, as the fracture
                // above: WaterWavesService reads the `.dwaves` when Play gathers the bodies, and the asset's
                // constructor adopts the handle its header GUID names.
                const std::filesystem::path full = FileForStoredReference( path, type );
                if ( full.empty() )
                    return 0;
                auto a = mgr.FindByPath<Assets::WaterWavesAsset>( full );
                if ( !a )
                    a = m.CreateAsset<Assets::WaterWavesAsset>( full, /*loadAfterCreate=*/false );
                return a ? static_cast<uint64_t>( a->GetMetadata().Handle ) : 0;
            }
            if ( type == "WorldParametersAsset" )
            {
                // The scene's world parameter collection (SceneSettings::WorldParameters). Announced, not read, as
                // the fracture above: WorldParametersService reads the `.dworldparams` when the world's instance
                // adopts it, and the asset's constructor adopts the handle its header GUID names.
                const std::filesystem::path full = FileForStoredReference( path, type );
                if ( full.empty() )
                    return 0;
                auto a = mgr.FindByPath<Assets::WorldParametersAsset>( full );
                if ( !a )
                    a = m.CreateAsset<Assets::WorldParametersAsset>( full, /*loadAfterCreate=*/false );
                return a ? static_cast<uint64_t>( a->GetMetadata().Handle ) : 0;
            }
            if ( type == "WeatherPresetAsset" )
            {
                // A weather zone's preset (ECS::WeatherZoneData::Preset). Announced, not read, as the collection
                // above: WeatherPresetService reads the `.dweatherpreset` when the world's weather first needs it.
                const std::filesystem::path full = FileForStoredReference( path, type );
                if ( full.empty() )
                    return 0;
                auto a = mgr.FindByPath<Assets::WeatherPresetAsset>( full );
                if ( !a )
                    a = m.CreateAsset<Assets::WeatherPresetAsset>( full, /*loadAfterCreate=*/false );
                return a ? static_cast<uint64_t>( a->GetMetadata().Handle ) : 0;
            }
            if ( type == "PhysicsAsset" )
            {
                // A ragdoll's physics asset (RagdollData::PhysicsAsset). Announced, not read, as the fracture
                // above: PhysicsAssetService reads the `.dephysasset` when Play asks for it, and the asset's
                // constructor adopts the handle its envelope GUID names.
                const std::filesystem::path full = FileForStoredReference( path, type );
                if ( full.empty() )
                    return 0;
                auto a = mgr.FindByPath<Assets::PhysicsAsset>( full );
                if ( !a )
                    a = m.CreateAsset<Assets::PhysicsAsset>( full, /*loadAfterCreate=*/false );
                return a ? static_cast<uint64_t>( a->GetMetadata().Handle ) : 0;
            }
            if ( type == "SoundAsset" )
            {
                // The locator half of the Audio Source's {Guid, Path}: the `.desound` the path names states
                // the GUID, and ResolveGuidRef checks it is the one the scene states. Nothing is loaded: the
                // audio file is streamed when a voice starts.
                const std::filesystem::path full = FileForStoredReference( path, type );
                if ( full.empty() )
                    return 0;
                const auto sound = Assets::SoundAsset::ReadFile( full );
                if ( !sound )
                {
                    LOG_ERROR( "[Audio] {}", sound.GetError() );
                    return 0;
                }
                return static_cast<uint64_t>( Common::Content::HandleForGuid( sound.GetValue().Guid ) );
            }
            if ( type == "IesProfileAsset" )
            {
                // The locator half of a local light's IES {Guid, Path}: the `.dies` the path names states the
                // GUID, and ResolveGuidRef checks it is the one the scene states. Nothing is held: the light
                // collectors read the profile into the atlas on the first frame that asks (IesProfileService).
                const std::filesystem::path full = FileForStoredReference( path, type );
                if ( full.empty() )
                    return 0;
                const auto profile = Assets::IesProfileAsset::ReadFile( full );
                if ( !profile )
                {
                    LOG_ERROR( "[IES] {}", profile.GetError() );
                    return 0;
                }
                return static_cast<uint64_t>( Common::Content::HandleForGuid( profile.GetValue().Guid ) );
            }
            if ( type == "SparseVolumeAsset" )
            {
                // The locator half of a heterogeneous volume's baked source {Guid, Path}: only the `.dsvol`'s
                // envelope header is read (the bricks are the renderer's residency's to load, on first sight).
                const std::filesystem::path full = FileForStoredReference( path, type );
                if ( full.empty() )
                    return 0;
                const Common::Content::AssetGuid guid = Assets::SparseVolumeAsset::ReadGuid( full );
                if ( guid.IsNull() )
                {
                    LOG_ERROR( "[HeterogeneousVolume] '{}' has no readable SparseVolume envelope header",
                               full.string() );
                    return 0;
                }
                return static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) );
            }
            if ( type == "VFXSystemAsset" )
            {
                // A VFX component's `.dfx` system. Announced, not read, as the wave set above: VFXSystemService
                // requests the read from the registry row on the first tick that asks for it, and the asset's
                // constructor adopts the handle its header GUID names, so the row and this shell agree.
                const std::filesystem::path full = FileForStoredReference( path, type );
                if ( full.empty() )
                    return 0;
                auto a = mgr.FindByPath<Assets::VFXSystemAsset>( full );
                if ( !a )
                    a = m.CreateAsset<Assets::VFXSystemAsset>( full, /*loadAfterCreate=*/false );
                return a ? static_cast<uint64_t>( a->GetMetadata().Handle ) : 0;
            }
            if ( type == "PrefabAsset" )
            {
                // The locator half of the Default Pawn's {Guid, Path}: ResolveGuidRef reaches it only when
                // the GUID's registry row did not, and checks the file found here IS that prefab.
                const std::filesystem::path full = FileForStoredReference( path, type );
                if ( full.empty() )
                    return 0;
                return LoadScenePrefab( m, full );
            }
            if ( type == "AnimGraphAsset" )
            {
                const std::filesystem::path full = FileForStoredReference( path, type );
                if ( full.empty() )
                    return 0;

                auto a = mgr.FindByPath<Assets::AnimGraphAsset>( full );
                if ( !a )
                {
                    a = m.CreateAsset<Assets::AnimGraphAsset>( full,
                                                               /*loadAfterCreate=*/false );
                }
                if ( !a )
                {
                    return 0;
                }
                // LOADED HERE AND NOT LEFT TO THE FIRST FRAME, for the rig's reason one branch up: a graph
                // that is not ready is a graph AnimationECSSystem cannot hand to the entity, and the
                // character would fall back to its single `CurrentClip` while the scene file plainly names
                // a state machine — the silent shape §5.1 exists to end.
                if ( !a->IsReadyForUse() )
                {
                    if ( const auto loaded = Assets::LoadThroughLoader( m, a ); !loaded )
                    {
                        LOG_ERROR( "[Animation] Anim graph '{}' named by the scene could not be loaded: {}",
                                   full.string(), loaded.GetError() );
                        return 0;
                    }
                }
                return static_cast<uint64_t>( a->GetMetadata().Handle );
            }
            if ( type == "UIThemeAsset" )
            {
                const std::filesystem::path full = FileForStoredReference( path, type );
                if ( full.empty() )
                    return 0;

                auto a = mgr.FindByPath<Assets::UIThemeAsset>( full );
                if ( !a )
                    a = m.CreateAsset<Assets::UIThemeAsset>( full,
                                                             /*loadAfterCreate=*/false );
                if ( !a )
                    return 0;
                // REQUESTED HERE, NOT READ (AL1-7): the scene names the theme, so its read starts during the
                // scene load and lands on a worker while ContentGate holds the loading screen. The shell
                // exists already, so a theme outside the shipped library is requested the same way; a
                // read that fails is reported by UIThemeService with the file's path.
                (void)Runtime::ResourceRegistry::GetUIThemeService()->Get( a->GetMetadata().Handle );
                return static_cast<uint64_t>( a->GetMetadata().Handle );
            }
            // The three SERVICE-REGISTRY types. `path` here is the file's ROOT-TAGGED KEY (I10), so it
            // is resolved against the roots as they stand in THIS process before the service — whose
            // registry is path-keyed — is asked to register it. Registration is idempotent and returns
            // the deterministic handle; the icon service bakes its SDF on first draw.
            if ( type == "FontAsset" )
            {
                return Runtime::ResourceRegistry::GetFontService()->RegisterFont( ServicePathForKey( path ) );
            }
            if ( type == "VideoAsset" )
            {
                return Runtime::ResourceRegistry::GetVideoService()->RegisterVideo( ServicePathForKey( path ) );
            }
            if ( type == "IconAsset" )
            {
                return Runtime::ResourceRegistry::GetIconService()->RegisterIcon( ServicePathForKey( path ) );
            }
            // Meshes: find, else cook-create as the concrete type + register + load.
            if ( type == "StaticMeshAsset" || type == "SkinnedMeshAsset" || type == "MeshAsset" )
            {
                // FIND, ELSE CREATE, THEN REGISTER — the same shape as the material branch above and for
                // the same reason. What the found route USED to skip is the whole of this task: Ф5 made
                // `FindByPath` answer on the spelling-independent identity, so the preloaded shell is now
                // RETURNED BY THE LOOKUP instead of arriving through create dressed as something fresh —
                // which moved the common case onto the one route that registered nothing.
                //
                // NO `Load()` ANYWHERE HERE, and that order was a defect of its own. `MeshService::Register`
                // parses before it builds (Г15), so a `Load()` after it could only ever run once an empty
                // mesh had been cached under a live handle for the life of the process.
                // The locator is the stable key ToPath writes (StoredAssetForm::StableKey); the key expands to
                // the file it names under its root, so the scene names no machine.
                const std::filesystem::path file = Common::AssetHandle::PathForStableKey( path );
                const auto                  a    = ResolveSceneReference(
                     [&] { return mgr.FindByPath<Assets::MeshAsset>( file ); },
                     [&]
                     {
                         return type == "SkinnedMeshAsset"
                                                         ? Assets::Asset<Assets::MeshAsset>( m.CreateAsset<Assets::SkinnedMeshAsset>(
                                            file, /*loadAfterCreate=*/false ) )
                                                         : Assets::Asset<Assets::MeshAsset>( m.CreateAsset<Assets::StaticMeshAsset>(
                                            file, /*loadAfterCreate=*/false ) );
                     },
                     [&m]( const Assets::Asset<Assets::MeshAsset>& mesh, ReferenceOrigin )
                     { Runtime::EnsureMeshRegistered( mesh, m ); } );
                return a ? static_cast<uint64_t>( a->GetMetadata().Handle ) : 0;
            }

            // The read half of the same refusal, for the reason the write half states at length: a bare
            // `return 0` here is indistinguishable from "the scene named nothing", so a type with no
            // branch loaded as unset on every open and the file's value was silently discarded.
            LOG_ERROR( "[Scene] asset type '{0}' has no branch in Core::MakeAssetResolver, so the "
                       "reference '{1}' this scene states cannot be resolved: the slot loads as unset "
                       "and the value in the file is discarded. Add a branch for it beside the others "
                       "in ComponentRegistry.cpp.",
                       type, path );
            return 0;
        };

        // Asset-database path: a stable GUID stored in the scene resolves directly through the
        // AssetManager (assets adopt their persisted/path-derived handles on load), surviving
        // file renames that break path references. Returns 0 for unknown GUIDs so the caller
        // falls back to FromPath.
        r.FromGuid = [&mgr, fromPath = r.FromPath]( uint64_t guid, const std::string& type ) -> uint64_t
        {
            if ( guid == 0 )
                return 0;
            const Common::UUID handle( guid );

            if ( type == "MaterialAsset" )
            {
                auto a = mgr.FindByHandle<Assets::MaterialAsset>( handle );
                if ( !a )
                    return 0;
                // The SAME registration the path branch performs, through the SAME helper: a reference is
                // registered whichever spelling the scene used to make it. This is also where the old
                // `!svc->Get( handle )` guard lived, which BUILT the runtime material for every material a
                // scene named, during parsing, to decide whether it needed registering.
                Runtime::EnsureMaterialRegistered( a );
                return guid;
            }
            if ( type == "TextureAsset" )
            {
                if ( mgr.FindByHandle<Assets::TextureAsset>( handle ) )
                {
                    // The SAME registration the path branch performs. It was absent here entirely.
                    Runtime::EnsureTextureRegistered( mgr, guid );
                    return guid;
                }

                // DC §1.4. A path that will not resolve breaks LOUDLY and with the filename in it
                // (TextureSlotFromPath prints the name, its expansion and the roots it searched); an
                // identifier that will not resolve used to break silently and as a bare 0, and the slot
                // then reads as "the artist left it empty". Now that the handle is the ONLY spelling of a
                // texture reference in a component, its miss owes the same message — and a number is
                // exactly the kind of name a human cannot search for, so the message has to carry the
                // search: where cooked textures come from, and what actually mints the number.
                LOG_ERROR( "[Textures] Texture handle {0} named by a component resolves to no registered "
                           "texture, so the slot stays EMPTY. Texture assets (.detex) are scanned from '{1}' and "
                           "each carries its handle in its header, so a handle that has stopped resolving "
                           "means that asset was deleted or is outside the scanned root. Re-point the slot.",
                           guid, Common::Constants::Path::ASSETS_PATH.string() );
                return 0;
            }
            if ( type == "StaticMeshAsset" || type == "SkinnedMeshAsset" || type == "MeshAsset" )
            {
                // A shell already on record is registered; otherwise MeshService discovers the mesh from its
                // registry row (AL1-5: no mesh shell is created at startup). Nothing is read here — the scene
                // open waits for the scene's meshes afterwards (MeshService::AwaitResident).
                if ( auto a = mgr.FindByHandle<Assets::MeshAsset>( handle ) )
                {
                    // The same const_cast `FromPath` makes at its head, for the same reason: a resolver is
                    // handed the registry as const, and registering against it is a write.
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                    Runtime::EnsureMeshRegistered( a, const_cast<Assets::AssetManager&>( mgr ) );
                    return guid;
                }
                return Runtime::DiscoverMesh( handle ) ? guid : 0;
            }
            if ( type == "PrefabAsset" )
            {
                // BY GUID through the content registry: a prefab's handle IS HandleForGuid of its header GUID
                // (FO-9), so the row under that handle is the prefab, wherever it has since moved.
                if ( mgr.FindByHandle<Assets::PrefabAsset>( handle ) )
                    return guid;
                const std::string key = Assets::ContentRegistry::KeyForHandle( guid );
                if ( key.empty() )
                    return 0;
                // Loaded through the locator half above (FromPath's PrefabAsset branch), which owns the one
                // mutable view of the manager: the row's key IS the stored spelling, handed over as is.
                return fromPath( key, type ) == guid ? guid : 0;
            }
            if ( type == "FoliageTypeAsset" )
            {
                // BY GUID through the content registry, the Foliage block's way: a type's handle IS
                // HandleForGuid of its header GUID, so the row under that handle is the type wherever it moved.
                if ( mgr.FindByHandle<Assets::FoliageTypeAsset>( handle ) )
                    return guid;
                const std::string key = Assets::ContentRegistry::KeyForHandle( guid );
                if ( key.empty() )
                    return 0;
                return fromPath( key, type ) == guid ? guid : 0;
            }
            if ( type == "WaterWavesAsset" )
            {
                // BY GUID through the content registry, as the foliage type: a wave set's handle IS HandleForGuid
                // of its header GUID, so the row under that handle is the `.dwaves` wherever it has since moved.
                if ( mgr.FindByHandle<Assets::WaterWavesAsset>( handle ) )
                    return guid;
                const std::string key = Assets::ContentRegistry::KeyForHandle( guid );
                if ( key.empty() )
                    return 0;
                return fromPath( key, type ) == guid ? guid : 0;
            }
            if ( type == "SoundAsset" )
            {
                // A sound's handle IS HandleForGuid of its header GUID, so the registry row under it is the
                // `.desound`, wherever it has since moved; nothing is loaded until a voice starts.
                return Assets::ContentRegistry::KeyForHandle( guid ).empty() ? 0 : guid;
            }
            if ( type == "IesProfileAsset" || type == "SparseVolumeAsset" )
            {
                // An IES profile's / a sparse volume's handle IS HandleForGuid of its header GUID, as a sound's:
                // the registry row under it is the file wherever it has since moved.
                return Assets::ContentRegistry::KeyForHandle( guid ).empty() ? 0 : guid;
            }
            if ( type == "SkyboxAsset" )
            {
                // Created from its content-registry row and requested (AL1-9): no boot stage has created the
                // shell, and RequireSkybox's error names the handle when the registry has no skybox under it.
                return Runtime::RequireSkybox( handle ) ? guid : 0;
            }
            return 0;
        };

        return r;
    }
} // namespace Desert::Core::Serialize
