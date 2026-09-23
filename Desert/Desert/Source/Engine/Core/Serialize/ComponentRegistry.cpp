#include "ComponentRegistry.hpp"
#include <Engine/Core/Serialize/AssetReferenceResolve.hpp>
#include <Engine/Core/Serialize/AuthoredComponentIO.hpp>
#include <Engine/Core/Serialize/GenericBlock.hpp>
#include <Engine/Core/Serialize/StoredAssetForm.hpp>
#include <Engine/Core/Serialize/TextureSlot.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <cstring>
#include <optional>

#include <Engine/ECS/Components.hpp>
#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/MaterialAsset.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>
#include <Engine/Assets/CloudModellingVolumeAsset.hpp>
#include <Engine/Assets/AnimGraphAsset.hpp>
#include <Engine/Assets/ControlRigAsset.hpp>
#include <Engine/Assets/RetargetAsset.hpp>
#include <Engine/Assets/UIThemeAsset.hpp>
#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/Geometry/EditMesh.hpp>
#include <Engine/Geometry/EditMeshSerialization.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>

#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>

#include <rflcpp/rfl/json.hpp>

#include <limits>

namespace Desert::Core::Serialize
{
    namespace
    {
        // ── THE FILE FORM OF A SERVICE-REGISTRY REFERENCE (I10) ──────────────────────────────────────
        //
        // Fonts, vector icons and videos are not AssetManager assets: FontService, IconService and
        // VideoService each own a handle<->path registry keyed on AssetHandle::FromCookedPath. So a
        // scene cannot store a handle for them (nothing would resolve it on the next launch before the
        // service has scanned) and it used to store the PATH the service happened to hold.
        //
        // WHY THAT WAS WRONG, and it is the same sentence I9 removed from the script slot. A packaged
        // game remaps ASSETS_PATH to <package>/Assets/, so a path written from the development tree
        // names a directory that does not exist there. It survived until now only because every font
        // and icon this repository ships names the ENGINE trees Resources/Fonts and Resources/Icons,
        // which the packager stores under their own dev-time relative paths and SetProjectRoot never
        // remaps. Both scan roots also accept the PROJECT'S OWN assets tree
        // (Runtime/Services/ServiceScanRoots.hpp), and a `.ttf` or `.svg` dropped in from there took
        // exactly the broken route.
        //
        // The file therefore stores the ROOT-TAGGED KEY — the same form every other content reference
        // in a `.desce` uses, and precisely the string the service's own handle is the FNV of. These two
        // functions are the ONLY place the conversion happens, so no site can hold half of it: the
        // service registries stay path-keyed in memory and every editor picker keeps handing them paths.
        std::string ServiceKeyForPath( const std::string& path )
        {
            // An empty path is "this slot names nothing" and must stay empty rather than become a bare
            // tag — the read side below turns a non-empty string into a registration attempt.
            return path.empty() ? std::string() : Common::AssetHandle::StableKeyForPath( path );
        }

        std::string ServicePathForKey( const std::string& key )
        {
            // PathForStableKey hands an untagged string back verbatim, so a key from outside the root
            // table behaves exactly as the old bare path did instead of silently becoming something
            // else. Nothing in this repository has one; a file hand-edited to hold one still works
            // where it worked before.
            return key.empty() ? std::string() : Common::AssetHandle::PathForStableKey( key ).generic_string();
        }

        // `ToGeneric`/`FromGeneric` STOOD HERE and are gone: they are `WriteBlock`/`ReadBlock` in
        // Engine/Core/Serialize/GenericBlock.hpp now, which that header explains. Two things changed
        // with the move and both are the point — the read takes `rfl::DefaultIfMissing`, so a block
        // short of one field no longer costs the entity the WHOLE component, and both directions log
        // their refusal with the component's key instead of returning an empty answer in silence. They
        // moved to a header because nothing defined in this translation unit can be tested: it links
        // the AssetManager and through it the renderer.

        // Builds a handler for a component whose serializable payload is a reflected data block. Adding a
        // PROPERTY field to that block automatically extends serialization — no code change here.
        template <class TComponent, class TData>
        ComponentSerializer MakeReflected( std::string key, std::string typeName, TData TComponent::*member )
        {
            ComponentSerializer s;
            s.Key = std::move( key );
            s.Has = []( ECS::Entity e ) { return e.HasComponent<TComponent>(); };

            // The AssetResolver is passed on BOTH directions, exactly as MakeReflectedSelf passes it. It
            // is a no-op for a component with no PROPERTY(Asset<...>) field, and load-bearing for the ones
            // that have one (UIImage's Sprite, UIText's Font, UIPanel's Video). Without it an AssetHandle
            // field is written as a raw 64-bit id and read back as one: the id is minted at load time from
            // the file path, so it does not survive a restart, and the component comes back pointing at
            // nothing with no error anywhere.
            s.Serialize = [member, typeName]( ECS::Entity e, const Assets::AssetManager& mgr ) -> rfl::Generic
            {
                const auto* type = Reflection::ReflectionRegistry::Get().Find( typeName );
                if ( !type )
                    return rfl::Generic( rfl::Generic::Object{} );
                const auto& comp     = e.GetComponent<TComponent>();
                auto        resolver = MakeAssetResolver( mgr );
                return Reflection::SerializeReflected( *type, &( comp.*member ), &resolver );
            };

            s.Deserialize =
                 [member, typeName]( ECS::Entity e, const rfl::Generic& g, const Assets::AssetManager& mgr )
            {
                const auto* type = Reflection::ReflectionRegistry::Get().Find( typeName );
                if ( !type )
                    return;
                auto obj = g.to_object();
                if ( !obj.has_value() )
                    return;
                auto& comp =
                     e.HasComponent<TComponent>() ? e.GetComponent<TComponent>() : e.AddComponent<TComponent>();
                auto resolver = MakeAssetResolver( mgr );
                Reflection::DeserializeReflected( *type, &( comp.*member ), obj.value(), &resolver );
            };

            return s;
        }

        // A marker component (no data, e.g. FolderComponent): presence IS the state. Serializes as an empty
        // object; deserialize just re-adds it.
        template <class TComponent>
        ComponentSerializer MakeMarker( std::string key )
        {
            ComponentSerializer s;
            s.Key       = std::move( key );
            s.Has       = []( ECS::Entity e ) { return e.HasComponent<TComponent>(); };
            s.Serialize = []( ECS::Entity, const Assets::AssetManager& ) -> rfl::Generic
            { return rfl::Generic( rfl::Generic::Object{} ); };
            s.Deserialize = []( ECS::Entity e, const rfl::Generic&, const Assets::AssetManager& )
            {
                if ( !e.HasComponent<TComponent>() )
                    e.AddComponent<TComponent>();
            };
            return s;
        }

        // A component whose ENTIRE state is one bool (VisibilityComponent::Visible). It gets its own maker
        // rather than reusing MakeMarker above, because MakeMarker WOULD COMPILE AND WOULD BE WRONG: it
        // writes an empty object and re-adds the component on load, so the value comes back at the
        // struct's default. `Visible = false` would round-trip to `true` — the presence would survive and
        // the only thing the component carries would not, which is the defect this registration came to
        // fix wearing the shape of a fix.
        //
        // A record that carries the component but not its field is REPORTED and left at the default: an
        // entity is drawn either way, so a silent choice here is a scene that renders differently from
        // the file it claims to be.
        template <class TComponent>
        ComponentSerializer MakeFlag( std::string key, std::string field, bool TComponent::*member )
        {
            ComponentSerializer s;
            s.Key       = std::move( key );
            s.Has       = []( ECS::Entity e ) { return e.HasComponent<TComponent>(); };
            s.Serialize = [field, member]( ECS::Entity e, const Assets::AssetManager& ) -> rfl::Generic
            {
                rfl::Generic::Object object;
                object[field] = rfl::Generic( e.GetComponent<TComponent>().*member );
                return rfl::Generic( object );
            };
            s.Deserialize =
                 [key = s.Key, field, member]( ECS::Entity e, const rfl::Generic& g, const Assets::AssetManager& )
            {
                auto& comp =
                     e.HasComponent<TComponent>() ? e.GetComponent<TComponent>() : e.AddComponent<TComponent>();

                const auto object = g.to_object();
                if ( !object.has_value() )
                {
                    LOG_WARN( "[Scene] component '{0}' is not an object; '{1}' kept its default.", key, field );
                    return;
                }
                const auto value = object.value().get( field );
                if ( !value.has_value() )
                {
                    LOG_WARN( "[Scene] component '{0}' carries no '{1}'; it kept its default.", key, field );
                    return;
                }
                const auto flag = value.value().to_bool();
                if ( !flag.has_value() )
                {
                    LOG_WARN( "[Scene] component '{0}' has a non-boolean '{1}'; it kept its default.", key,
                              field );
                    return;
                }
                comp.*member = flag.value();
            };
            return s;
        }

        // A component with no reflected `Data` block whose whole authored state is mapped by hand in
        // Engine/Core/Serialize/AuthoredComponentIO.hpp. The mapping lives there and not here for one
        // reason: this translation unit links the AssetManager and through it the renderer, so nothing
        // in it can be round-tripped by a test, and a serializer nobody round-trips is how a field goes
        // missing between two halves that each look right. `WriteComponent`/`ReadComponent` overload on
        // the component type, so this template is the whole of the wiring.
        //
        // A payload that is not an object is REPORTED and the component is left as it was, rather than
        // reset: the entity keeps whatever the editor already had, which is the state the user can see.
        template <class TComponent>
        ComponentSerializer MakeAuthored( std::string key )
        {
            ComponentSerializer s;
            s.Key       = std::move( key );
            s.Has       = []( ECS::Entity e ) { return e.HasComponent<TComponent>(); };
            s.Serialize = []( ECS::Entity e, const Assets::AssetManager& ) -> rfl::Generic
            { return rfl::Generic( WriteComponent( e.GetComponent<TComponent>() ) ); };
            s.Deserialize = [key = s.Key]( ECS::Entity e, const rfl::Generic& g, const Assets::AssetManager& )
            {
                const auto object = g.to_object();
                if ( !object.has_value() )
                {
                    LOG_WARN( "[Scene] component '{0}' is not an object; it kept its current values.", key );
                    return;
                }
                auto& comp =
                     e.HasComponent<TComponent>() ? e.GetComponent<TComponent>() : e.AddComponent<TComponent>();
                ReadComponent( object.value(), comp );
            };
            return s;
        }

        // ScriptComponent has no reflected data block (reflection can't do std::string/variant lists), so it
        // gets a manual serializer via a reflect-cpp mirror: the .lua reference + the exposed-property values.
        struct ScriptPropSer
        {
            std::string Name;
            int         Type   = 0;
            double      Number = 0.0;
            bool        Bool   = false;
            std::string Str;
        };
        // `ScriptKey` and not `Path`, because the value stopped being a path at scene v16 (I9): it is the
        // root-tagged key ScriptSlot documents. Renaming the JSON field with the value is the point — a
        // key called `Path` reads as something you may hand to std::filesystem, which is exactly the
        // mistake the migration exists to undo, and the loader refuses anything below v16 anyway so there
        // is no file in which both spellings can be present.
        struct ScriptSlotSer
        {
            std::string                ScriptKey;
            std::vector<ScriptPropSer> Props;
        };
        // One entity runs a LIST of scripts (single-script legacy format removed).
        struct ScriptCompSer
        {
            std::optional<std::vector<ScriptSlotSer>> Scripts;
        };

        // Rebuild a ScriptSlot's properties from its serialized form.
        auto loadProps = []( const std::vector<ScriptPropSer>& src )
        {
            std::vector<Scripting::ScriptProperty> out;
            for ( const auto& p : src )
            {
                Scripting::ScriptProperty prop;
                prop.Name   = p.Name;
                prop.Type   = static_cast<Scripting::PropertyType>( p.Type );
                prop.Number = p.Number;
                prop.Bool   = p.Bool;
                prop.Str    = p.Str;
                out.push_back( prop );
            }
            return out;
        };

        ComponentSerializer MakeScript()
        {
            ComponentSerializer s;
            s.Key = "Script";
            s.Has = []( ECS::Entity e ) { return e.HasComponent<ECS::ScriptComponent>(); };

            s.Serialize = [key = s.Key]( ECS::Entity e, const Assets::AssetManager& ) -> rfl::Generic
            {
                const auto&                sc = e.GetComponent<ECS::ScriptComponent>();
                ScriptCompSer              ser;
                std::vector<ScriptSlotSer> slots;
                for ( const auto& slot : sc.Scripts )
                {
                    ScriptSlotSer ss;
                    ss.ScriptKey = slot.ScriptKey;
                    for ( const auto& p : slot.Properties )
                        ss.Props.push_back( { p.Name, static_cast<int>( p.Type ), p.Number, p.Bool, p.Str } );
                    slots.push_back( std::move( ss ) );
                }
                ser.Scripts = std::move( slots );
                return WriteBlock( ser, key );
            };

            s.Deserialize = [key = s.Key]( ECS::Entity e, const rfl::Generic& g, const Assets::AssetManager& )
            {
                auto ser = ReadBlock<ScriptCompSer>( g, key );
                if ( !ser )
                    return;
                auto& sc = e.HasComponent<ECS::ScriptComponent>() ? e.GetComponent<ECS::ScriptComponent>()
                                                                  : e.AddComponent<ECS::ScriptComponent>();
                sc.Scripts.clear();
                if ( ser->Scripts )
                {
                    for ( const auto& ss : *ser->Scripts )
                    {
                        ECS::ScriptSlot slot;
                        slot.ScriptKey  = ss.ScriptKey;
                        slot.Started    = false;
                        slot.Properties = loadProps( ss.Props );
                        sc.Scripts.push_back( std::move( slot ) );
                    }
                }
            };
            return s;
        }

        // THE THREE `Ensure*Registered` HELPERS THAT STOOD HERE NOW LIVE IN
        // Engine/Runtime/Services/AssetServiceRegistration.hpp, unchanged in behaviour.
        //
        // They moved because two OTHER files had independently written the same find-else-create and each
        // got a different part of it wrong (the import's reuse path registered nothing; the thumbnail's
        // reuse path registered nothing AND then blamed the consequence on the wrong cause). A rule
        // enforced by three careful copies is the shape Ф5 paid forty-four call sites for; this one is
        // enforced by there being one implementation to call.
    } // namespace

    // Resolves reflected AssetHandle fields to/from on-disk PATHS (backward-compatible with the old
    // per-component serializers). Dispatches by the field's PROPERTY(Asset<...>) type name. Only a few
    // asset types exist, so this is a small hand-written table (not codegen). Captures `mgr` by ref —
    // only used within the (de)serialize call that builds it.
    //
    // AT NAMESPACE SCOPE rather than in the anonymous namespace above, because SceneSettings is reflected
    // like a component and serialized like one, but is not one: SceneSerializer writes it directly, and
    // could not reach a resolver that only existed inside this file.
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
                           "is on disk, the registry is stale: run 'AssetRegistryTool cook'.",
                           handle, type );
                return "";
            }

            return RenderStoredForm( *form, key );
        };

        r.FromPath = [&mgr]( const std::string& path, const std::string& type ) -> uint64_t
        {
            if ( path.empty() )
                return 0;
            auto& m = const_cast<Assets::AssetManager&>( mgr );

            if ( type == "SkyboxAsset" )
            {
                auto a = mgr.FindByPath<Assets::SkyboxAsset>( path );
                if ( !a )
                    a = m.CreateAsset<Assets::SkyboxAsset>( Assets::AssetPriority::Medium, path );
                if ( a )
                {
                    if ( !a->IsReadyForUse() )
                        a->Load();
                    if ( !Runtime::ResourceRegistry::GetSkyboxService()->Get( a->GetMetadata().Handle ) )
                        Runtime::ResourceRegistry::GetSkyboxService()->Register( a );
                    return static_cast<uint64_t>( a->GetMetadata().Handle );
                }
                return 0;
            }
            if ( type == "MaterialAsset" )
            {
                // BOTH FORMS ARE ACCEPTED, on the terms the cloud branches below state: a file written
                // by ToPath above (or by the v7 -> v8 migration) carries a path relative to the assets
                // root, because a material ships with the project; a material an artist points at
                // outside the project carries an absolute one. Joining the relative form to the root
                // HERE means it happens exactly once.
                //
                // THE JOIN IS THE EXPANSION OF A REFERENCE AND NOT A LOOKUP FIX — it used to be both,
                // and that half is now retired. `FindByPath` compared filepaths VERBATIM while
                // `CreateAsset` deduplicated on the spelling-independent stable key, so a scene naming a
                // preloaded material by any other spelling missed and went the create-and-register way
                // round; this join was one of three hand-written detours around that. The registry
                // answers both questions the same way now (Desert/Tests/Engine/AssetPathIdentity). What
                // the join still does, and must: an identity is derived RELATIVE TO THE CONTENT ROOTS, so
                // a bare `Materials/M.demat` would otherwise be resolved against the process's working
                // directory, land under no root, and be a different asset from the same file under the
                // assets root. Deleting it would break the reference this branch exists to read.
                const std::filesystem::path named( path );
                const std::filesystem::path full =
                     named.is_absolute() ? named
                                         : ( Common::Constants::Path::ASSETS_PATH / named ).lexically_normal();

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
                              m.CreateAsset<Assets::SurfaceMaterialAsset>( Assets::AssetPriority::High, full ) );
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
            // SIDE ABOVE (O1). What replaced their register-on-load duty: AssetPreloader::PreloadCloudTypes
            // / PreloadCloudLayouts registers the library directories at startup, and the Material Editor's
            // drop target registers an out-of-library file the moment it is bound. A file outside the
            // library that only a `.demat` names is NOT re-registered on the next launch — the renderer
            // and the services say so loudly, once, with the handle — which is the named cost of the
            // deletion rather than an oversight.
            if ( type == "CloudModellingVolumeAsset" )
            {
                // Both forms accepted, for the reason the cloud type's branch gives above.
                const std::filesystem::path named( path );
                const std::filesystem::path full =
                     named.is_absolute() ? named
                                         : ( Common::Constants::Path::ASSETS_PATH / named ).lexically_normal();

                auto a = mgr.FindByPath<Assets::CloudModellingVolumeAsset>( full );
                if ( !a )
                    a = m.CreateAsset<Assets::CloudModellingVolumeAsset>( Assets::AssetPriority::Medium, full,
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
                // Both forms accepted, for the reason the branches above give.
                const std::filesystem::path named( path );
                const std::filesystem::path full =
                     named.is_absolute() ? named
                                         : ( Common::Constants::Path::ASSETS_PATH / named ).lexically_normal();

                auto a = mgr.FindByPath<Assets::ControlRigAsset>( full );
                if ( !a )
                {
                    a = m.CreateAsset<Assets::ControlRigAsset>( Assets::AssetPriority::Medium, full );
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
                    if ( const auto loaded = a->Load(); !loaded )
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
                // Both forms accepted, for the reason the branches above give.
                const std::filesystem::path named( path );
                const std::filesystem::path full =
                     named.is_absolute() ? named
                                         : ( Common::Constants::Path::ASSETS_PATH / named ).lexically_normal();

                auto a = mgr.FindByPath<Assets::RetargetAsset>( full );
                if ( !a )
                {
                    a = m.CreateAsset<Assets::RetargetAsset>( Assets::AssetPriority::Medium, full );
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
                    if ( const auto loaded = a->EnsureLoaded( m ); !loaded )
                    {
                        LOG_ERROR( "[Animation] Retarget '{}' named by the scene could not be loaded: {}",
                                   full.string(), loaded.GetError() );
                        return 0;
                    }
                }
                return static_cast<uint64_t>( a->GetMetadata().Handle );
            }
            if ( type == "AnimGraphAsset" )
            {
                // Both forms accepted, for the reason the branches above give.
                const std::filesystem::path named( path );
                const std::filesystem::path full =
                     named.is_absolute() ? named
                                         : ( Common::Constants::Path::ASSETS_PATH / named ).lexically_normal();

                auto a = mgr.FindByPath<Assets::AnimGraphAsset>( full );
                if ( !a )
                {
                    a = m.CreateAsset<Assets::AnimGraphAsset>( Assets::AssetPriority::Medium, full );
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
                    if ( const auto loaded = a->Load(); !loaded )
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
                // Both forms accepted, for the reason the branches above give.
                const std::filesystem::path named( path );
                const std::filesystem::path full =
                     named.is_absolute() ? named
                                         : ( Common::Constants::Path::ASSETS_PATH / named ).lexically_normal();

                auto a = mgr.FindByPath<Assets::UIThemeAsset>( full );
                if ( !a )
                    a = m.CreateAsset<Assets::UIThemeAsset>( Assets::AssetPriority::Medium, full );
                if ( !a )
                    return 0;
                if ( !a->IsReadyForUse() && !a->Load() )
                    return 0;
                // REGISTERED HERE AND NOT ONLY IN THE PRELOADER, because a theme an author points at
                // outside the shipped library is never scanned: without this the canvas would hold a
                // valid handle the service has never heard of, and every element would draw its local
                // colours while the scene file plainly names a theme.
                if ( const auto registered = Runtime::ResourceRegistry::GetUIThemeService()->Register( a );
                     !registered )
                    LOG_ERROR( "[UI] Theme '{}' named by the scene could not be registered: {}", full.string(),
                               registered.GetError() );
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
                const auto a = ResolveSceneReference(
                     [&] { return mgr.FindByPath<Assets::MeshAsset>( path ); },
                     [&]
                     {
                         return type == "SkinnedMeshAsset"
                                     ? Assets::Asset<Assets::MeshAsset>( m.CreateAsset<Assets::SkinnedMeshAsset>(
                                            Assets::AssetPriority::High, path ) )
                                     : Assets::Asset<Assets::MeshAsset>( m.CreateAsset<Assets::StaticMeshAsset>(
                                            Assets::AssetPriority::High, path ) );
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
        r.FromGuid = [&mgr]( uint64_t guid, const std::string& type ) -> uint64_t
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
                           "texture, so the slot stays EMPTY. Cooked textures are scanned from '{1}'; a "
                           "texture's handle is AssetHandle::FromCookedPath of its SOURCE image, so a "
                           "handle that has stopped resolving usually means the image was renamed or moved "
                           "since this was saved. Re-point the slot at the texture in its new place.",
                           guid, Common::Constants::Path::TEXTURE_PATH_COOKED.string() );
                return 0;
            }
            if ( type == "StaticMeshAsset" || type == "SkinnedMeshAsset" || type == "MeshAsset" )
            {
                auto a = mgr.FindByHandle<Assets::MeshAsset>( handle );
                if ( !a )
                    return 0;
                // The SAME registration the path branch performs, through the SAME helper. The guard that
                // stood here was `!svc->GetAsset( handle )`, which PARSES the `.stmesh` through
                // EnsureLoaded before answering — asking "is it registered?" at the price of registering.
                // The const_cast is the same one `FromPath` makes at its head and for the same reason: a
                // resolver is handed the registry as const, and registering against it is a write.
                Runtime::EnsureMeshRegistered( a, const_cast<Assets::AssetManager&>( mgr ) );
                return guid;
            }
            if ( type == "SkyboxAsset" )
            {
                return mgr.FindByHandle<Assets::SkyboxAsset>( handle ) ? guid : 0;
            }
            return 0;
        };

        return r;
    }

    namespace
    {
        // Like MakeReflected, but reflects the WHOLE component (no Data sub-member) and threads an
        // AssetResolver so AssetHandle fields round-trip as paths. Used for asset-bearing components that
        // are now fully reflected (Skybox) instead of hand-mapped.
        template <class TComponent>
        ComponentSerializer MakeReflectedSelf( std::string key, std::string typeName )
        {
            ComponentSerializer s;
            s.Key = std::move( key );
            s.Has = []( ECS::Entity e ) { return e.HasComponent<TComponent>(); };

            s.Serialize = [typeName]( ECS::Entity e, const Assets::AssetManager& mgr ) -> rfl::Generic
            {
                const auto* type = Reflection::ReflectionRegistry::Get().Find( typeName );
                if ( !type )
                    return rfl::Generic( rfl::Generic::Object{} );
                auto resolver = MakeAssetResolver( mgr );
                return Reflection::SerializeReflected( *type, &e.GetComponent<TComponent>(), &resolver );
            };

            s.Deserialize = [typeName]( ECS::Entity e, const rfl::Generic& g, const Assets::AssetManager& mgr )
            {
                const auto* type = Reflection::ReflectionRegistry::Get().Find( typeName );
                if ( !type )
                    return;
                auto obj = g.to_object();
                if ( !obj.has_value() )
                    return;
                auto  resolver = MakeAssetResolver( mgr );
                auto& comp =
                     e.HasComponent<TComponent>() ? e.GetComponent<TComponent>() : e.AddComponent<TComponent>();
                Reflection::DeserializeReflected( *type, &comp, obj.value(), &resolver );
            };

            return s;
        }

        // ONE ASSET REFERENCE, TWO SPELLINGS, ONE ANSWER. A mesh or material reference travels as a stable
        // GUID and as a path: the GUID is the rename-safe one and is tried first, the path is what an older
        // file carries and is the fallback. Zero means "this scene named nothing that resolves here", which
        // the caller distinguishes from a live handle.
        //
        // Extracted rather than written inline for the third time: the static and skinned mesh blocks each
        // spell this out, the instanced one now needs it too, and three copies of a fallback ORDER is three
        // places for the order to differ.
        uint64_t ResolveAssetRef( const Reflection::AssetResolver& resolver, const std::optional<uint64_t>& guid,
                                  const std::optional<std::string>& path, const char* type )
        {
            uint64_t handle = 0;
            if ( guid )
            {
                handle = resolver.FromGuid( *guid, type );
            }
            if ( handle == 0 && path )
            {
                handle = resolver.FromPath( *path, type );
            }
            return handle;
        }

        // The same answer for one slot of a material list, where either list may be absent or shorter.
        uint64_t ResolveSlotRef( const Reflection::AssetResolver&               resolver,
                                 const std::optional<std::vector<uint64_t>>&    guids,
                                 const std::optional<std::vector<std::string>>& paths, std::size_t slot )
        {
            const std::optional<uint64_t> guid =
                 ( guids && slot < guids->size() ) ? std::optional<uint64_t>( ( *guids )[slot] ) : std::nullopt;
            const std::optional<std::string> path =
                 ( paths && slot < paths->size() ) ? std::optional<std::string>( ( *paths )[slot] ) : std::nullopt;
            return ResolveAssetRef( resolver, guid, path, "MaterialAsset" );
        }
    } // namespace

    const ComponentRegistry& ComponentRegistry::Get()
    {
        static ComponentRegistry instance;
        return instance;
    }

    ComponentRegistry::ComponentRegistry()
    {
        RegisterBuiltins();
    }

    void ComponentRegistry::Register( ComponentSerializer serializer )
    {
        m_Serializers.push_back( std::move( serializer ) );
    }

    // Cognitive complexity 212 against a threshold of 19, and the lambda for one component 22: both are
    // TRUE and PRE-EXISTING. This function is the engine's whole component table -- twenty-odd registration
    // blocks, each a pair of lambdas -- and the analyser reports a function-level finding for any edit
    // anywhere inside it, so a field added to one component cannot land without this line or a split that
    // is a task of its own. Named in Г26's report as debt rather than hidden.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    void ComponentRegistry::RegisterBuiltins()
    {
        // ---- Static Mesh (asset-bearing: handle <-> path) ----
        {
            ComponentSerializer s;
            s.Key = "StaticMesh";
            s.Has = []( ECS::Entity e ) { return e.HasComponent<ECS::StaticMeshComponent>(); };

            s.Serialize = [key = s.Key]( ECS::Entity                 entity,
                                         const Assets::AssetManager& assetManager ) -> rfl::Generic
            {
                const auto&                    smc = entity.GetComponent<ECS::StaticMeshComponent>();
                Assets::StaticMeshComponentSer meshSer;

                // Asset refs go through the single shared resolver (same code path as the reflected
                // components), instead of duplicating FindByHandle/FindByPath here.
                auto resolver = MakeAssetResolver( assetManager );
                if ( smc.MeshHandle )
                {
                    if ( auto p = resolver.ToPath( static_cast<uint64_t>( smc.MeshHandle ), "StaticMeshAsset" );
                         !p.empty() )
                        meshSer.MeshPath = p;
                    // GUID = the stable handle itself (asset-database identity); rename-safe.
                    meshSer.MeshGuid = static_cast<uint64_t>( smc.MeshHandle );
                }

                if ( !smc.MaterialSlots.empty() )
                {
                    meshSer.MaterialPaths = std::vector<std::string>{};
                    meshSer.MaterialGuids = std::vector<uint64_t>{};
                    for ( auto handle : smc.MaterialSlots )
                    {
                        meshSer.MaterialPaths->push_back(
                             resolver.ToPath( static_cast<uint64_t>( handle ), "MaterialAsset" ) );
                        meshSer.MaterialGuids->push_back( static_cast<uint64_t>( handle ) );
                    }
                }
                meshSer.Primitive = smc.Primitive;

                // Rendering controls: write only non-default values (absent = default on load).
                if ( smc.OutlineDraw )
                    meshSer.OutlineDraw = smc.OutlineDraw;
                if ( smc.ForcedLOD >= 0 )
                    meshSer.ForcedLOD = smc.ForcedLOD;
                if ( smc.LODBias != 0 )
                    meshSer.LODBias = smc.LODBias;
                if ( !smc.CastShadows )
                    meshSer.CastShadows = smc.CastShadows;
                if ( !smc.ReceiveShadows )
                    meshSer.ReceiveShadows = smc.ReceiveShadows;
                if ( smc.HiddenSubmeshes != 0 )
                    meshSer.HiddenSubmeshes = smc.HiddenSubmeshes;

                // The SOURCE is saved, never the render mesh derived from it (see EditableMesh.hpp).
                if ( smc.EditableMesh )
                    meshSer.EditMesh = Geometry::ToSerialized( *smc.EditableMesh );

                return WriteBlock( meshSer, key );
            };

            s.Deserialize = [key = s.Key]( ECS::Entity entity, const rfl::Generic& g,
                                           const Assets::AssetManager& assetManager )
            {
                auto parsed = ReadBlock<Assets::StaticMeshComponentSer>( g, key );
                if ( !parsed.has_value() )
                    return;
                const auto& meshData = parsed.value();

                auto& smc      = entity.AddComponent<ECS::StaticMeshComponent>();
                auto  resolver = MakeAssetResolver( assetManager );

                // GUID first (rename-safe asset-database reference), path as fallback/back-compat.
                uint64_t meshHandle = 0;
                if ( meshData.MeshGuid )
                    meshHandle = resolver.FromGuid( *meshData.MeshGuid, "StaticMeshAsset" );
                if ( meshHandle == 0 && meshData.MeshPath )
                    meshHandle = resolver.FromPath( *meshData.MeshPath, "StaticMeshAsset" );
                if ( meshHandle != 0 )
                    smc.MeshHandle = Common::UUID( meshHandle );

                const size_t slotCount = meshData.MaterialGuids
                                              ? meshData.MaterialGuids->size()
                                              : ( meshData.MaterialPaths ? meshData.MaterialPaths->size() : 0 );
                if ( slotCount > 0 )
                {
                    smc.MaterialSlots.clear();
                    for ( size_t i = 0; i < slotCount; ++i )
                    {
                        uint64_t h = 0;
                        if ( meshData.MaterialGuids && i < meshData.MaterialGuids->size() )
                            h = resolver.FromGuid( ( *meshData.MaterialGuids )[i], "MaterialAsset" );
                        if ( h == 0 && meshData.MaterialPaths && i < meshData.MaterialPaths->size() )
                            h = resolver.FromPath( ( *meshData.MaterialPaths )[i], "MaterialAsset" );
                        smc.MaterialSlots.push_back( Common::UUID( h ) );
                    }
                }
                smc.Primitive = meshData.Primitive;

                smc.OutlineDraw     = meshData.OutlineDraw.value_or( smc.OutlineDraw );
                smc.ForcedLOD       = meshData.ForcedLOD.value_or( smc.ForcedLOD );
                smc.LODBias         = meshData.LODBias.value_or( smc.LODBias );
                smc.CastShadows     = meshData.CastShadows.value_or( smc.CastShadows );
                smc.ReceiveShadows  = meshData.ReceiveShadows.value_or( smc.ReceiveShadows );
                smc.HiddenSubmeshes = meshData.HiddenSubmeshes.value_or( smc.HiddenSubmeshes );

                if ( meshData.EditMesh )
                {
                    // A refusal DROPS the geometry, and says so with the entity's name: the component still
                    // loads (asset handle, materials, flags), so the entity falls back to what it names
                    // besides the edited mesh rather than the whole scene failing to open.
                    const std::string tag    = entity.HasComponent<ECS::TagComponent>()
                                                    ? entity.GetComponent<ECS::TagComponent>().Tag
                                                    : std::string( "Entity" );
                    auto              loaded = Geometry::FromSerialized( *meshData.EditMesh );
                    if ( !loaded.IsSuccess() )
                    {
                        LOG_ERROR( "[Scene] entity '{0}': its edited mesh could not be read and was DROPPED: {1}",
                                   tag, loaded.GetError() );
                    }
                    else if ( auto set = ECS::SetEditableMesh(
                                   smc, std::make_shared<const Geometry::EditMesh>( loaded.ExtractValue() ) );
                              !set.IsSuccess() )
                    {
                        LOG_ERROR(
                             "[Scene] entity '{0}': its edited mesh was read but not built, and was DROPPED: {1}",
                             tag, set.GetError() );
                    }
                }
            };

            Register( std::move( s ) );
        }

        // ---- Instanced Static Mesh (UE-style ISM: asset/primitive mesh + N world matrices) ----
        {
            ComponentSerializer s;
            s.Key = "InstancedStaticMesh";
            s.Has = []( ECS::Entity e ) { return e.HasComponent<ECS::InstancedStaticMeshComponent>(); };

            s.Serialize = [key = s.Key]( ECS::Entity                 entity,
                                         const Assets::AssetManager& assetManager ) -> rfl::Generic
            {
                const auto& ism = entity.GetComponent<ECS::InstancedStaticMeshComponent>();
                Assets::InstancedStaticMeshComponentSer ser;

                auto resolver = MakeAssetResolver( assetManager );
                if ( ism.MeshHandle )
                {
                    if ( auto p = resolver.ToPath( static_cast<uint64_t>( ism.MeshHandle ), "StaticMeshAsset" );
                         !p.empty() )
                        ser.MeshPath = p;
                    // GUID = the stable handle itself (asset-database identity); rename-safe.
                    ser.MeshGuid = static_cast<uint64_t>( ism.MeshHandle );
                }

                if ( !ism.MaterialSlots.empty() )
                {
                    ser.MaterialPaths = std::vector<std::string>{};
                    ser.MaterialGuids = std::vector<uint64_t>{};
                    for ( auto handle : ism.MaterialSlots )
                    {
                        ser.MaterialPaths->push_back(
                             resolver.ToPath( static_cast<uint64_t>( handle ), "MaterialAsset" ) );
                        ser.MaterialGuids->push_back( static_cast<uint64_t>( handle ) );
                    }
                }
                ser.Primitive = ism.Primitive;
                if ( !ism.CastShadows )
                    ser.CastShadows = ism.CastShadows;
                if ( !ism.InstanceTransforms.empty() )
                {
                    std::vector<std::array<float, 16>> flat;
                    flat.reserve( ism.InstanceTransforms.size() );
                    for ( const auto& m : ism.InstanceTransforms )
                    {
                        std::array<float, 16> a{};
                        std::memcpy( a.data(), &m[0][0], sizeof( float ) * 16 );
                        flat.push_back( a );
                    }
                    ser.InstanceTransforms = std::move( flat );
                }

                return WriteBlock( ser, key );
            };

            s.Deserialize = [key = s.Key]( ECS::Entity entity, const rfl::Generic& g,
                                           const Assets::AssetManager& assetManager )
            {
                auto parsed = ReadBlock<Assets::InstancedStaticMeshComponentSer>( g, key );
                if ( !parsed.has_value() )
                    return;
                const auto& data = parsed.value();

                auto& ism      = entity.AddComponent<ECS::InstancedStaticMeshComponent>();
                auto  resolver = MakeAssetResolver( assetManager );

                // GUID first (rename-safe asset-database reference), path as fallback/back-compat --
                // the same order the static path uses, and it did not before Г26.
                const uint64_t meshHandle =
                     ResolveAssetRef( resolver, data.MeshGuid, data.MeshPath, "StaticMeshAsset" );
                if ( meshHandle != 0 )
                {
                    ism.MeshHandle = Common::UUID( meshHandle );
                }

                size_t slotCount = 0;
                if ( data.MaterialGuids )
                {
                    slotCount = data.MaterialGuids->size();
                }
                else if ( data.MaterialPaths )
                {
                    slotCount = data.MaterialPaths->size();
                }
                if ( slotCount > 0 )
                {
                    ism.MaterialSlots.clear();
                    for ( size_t i = 0; i < slotCount; ++i )
                    {
                        ism.MaterialSlots.emplace_back(
                             ResolveSlotRef( resolver, data.MaterialGuids, data.MaterialPaths, i ) );
                    }
                }
                ism.Primitive   = data.Primitive;
                ism.CastShadows = data.CastShadows.value_or( ism.CastShadows );
                if ( data.InstanceTransforms.has_value() )
                {
                    ism.InstanceTransforms.clear();
                    ism.InstanceTransforms.reserve( data.InstanceTransforms->size() );
                    for ( const auto& a : *data.InstanceTransforms )
                    {
                        glm::mat4 m( 1.0f );
                        std::memcpy( &m[0][0], a.data(), sizeof( float ) * 16 );
                        ism.InstanceTransforms.push_back( m );
                    }
                }
            };

            Register( std::move( s ) );
        }

        // ---- Material (generic data-driven: shader name + param overrides + texture refs as paths) ----
        {
            ComponentSerializer s;
            s.Key = "Material";
            s.Has = []( ECS::Entity e ) { return e.HasComponent<ECS::MaterialComponent>(); };

            s.Serialize = [key = s.Key]( ECS::Entity entity,
                                         const Assets::AssetManager& /*assetManager*/ ) -> rfl::Generic
            {
                const auto&                  mc = entity.GetComponent<ECS::MaterialComponent>();
                Assets::MaterialComponentSer ser;
                ser.ShaderName = mc.ShaderName;

                if ( !mc.Params.empty() )
                {
                    std::vector<Assets::MaterialParamSer> ps;
                    for ( const auto& p : mc.Params )
                        ps.push_back( { p.Name, p.Value } );
                    ser.Params = std::move( ps );
                }

                if ( !mc.Textures.empty() )
                {
                    // The handle alone (DC §4.2). This used to write the resolved path AND the handle into
                    // every entry; see MaterialTextureSer for why that pairing is worse than either half.
                    std::vector<Assets::MaterialTextureSer> ts;
                    for ( const auto& t : mc.Textures )
                        ts.push_back( { t.Name, t.TextureHandle } );
                    ser.Textures = std::move( ts );
                }

                return WriteBlock( ser, key );
            };

            s.Deserialize = [key = s.Key]( ECS::Entity entity, const rfl::Generic& g,
                                           const Assets::AssetManager& assetManager )
            {
                auto parsed = ReadBlock<Assets::MaterialComponentSer>( g, key );
                if ( !parsed.has_value() )
                    return;
                const auto& data = parsed.value();

                auto& mc      = entity.AddComponent<ECS::MaterialComponent>();
                mc.ShaderName = data.ShaderName;

                if ( data.Params.has_value() )
                    for ( const auto& p : *data.Params )
                        mc.Params.push_back( { p.Name, p.Value } );

                if ( data.Textures.has_value() )
                {
                    auto resolver = MakeAssetResolver( assetManager );
                    for ( const auto& t : *data.Textures )
                    {
                        // ONE reader for one written value. The path fallback that used to sit under this
                        // line is gone with the field it read (DC §4.2) — and it was the dangerous half:
                        // a handle that resolves to nothing is a slot that ends up empty and says so,
                        // while a stale path that still resolves puts the WRONG texture on the surface.
                        //
                        // FromGuid names what it could not find, so a miss here is not silent; that is the
                        // §1.4 obligation this reference class acquired when it became the only spelling.
                        mc.Textures.push_back( { t.Name, resolver.FromGuid( t.TextureHandle, "TextureAsset" ) } );
                    }
                }
            };

            Register( std::move( s ) );
        }

        // ---- Skinned Mesh (asset-bearing) ----
        {
            ComponentSerializer s;
            s.Key = "SkinnedMesh";
            s.Has = []( ECS::Entity e ) { return e.HasComponent<ECS::SkinnedMeshComponent>(); };

            s.Serialize = [key = s.Key]( ECS::Entity                 entity,
                                         const Assets::AssetManager& assetManager ) -> rfl::Generic
            {
                const auto&                     smc = entity.GetComponent<ECS::SkinnedMeshComponent>();
                Assets::SkinnedMeshComponentSer meshSer;

                auto resolver = MakeAssetResolver( assetManager );
                if ( smc.MeshHandle )
                {
                    if ( auto p = resolver.ToPath( static_cast<uint64_t>( smc.MeshHandle ), "SkinnedMeshAsset" );
                         !p.empty() )
                        meshSer.MeshPath = p;
                    meshSer.MeshGuid = static_cast<uint64_t>( smc.MeshHandle );
                }

                if ( !smc.MaterialSlots.empty() )
                {
                    meshSer.MaterialPaths = std::vector<std::string>{};
                    meshSer.MaterialGuids = std::vector<uint64_t>{};
                    for ( auto handle : smc.MaterialSlots )
                    {
                        meshSer.MaterialPaths->push_back(
                             resolver.ToPath( static_cast<uint64_t>( handle ), "MaterialAsset" ) );
                        meshSer.MaterialGuids->push_back( static_cast<uint64_t>( handle ) );
                    }
                }

                // Rendering controls: write only non-default values (absent = default on load), exactly
                // like the static mesh above.
                if ( !smc.CastShadows )
                    meshSer.CastShadows = smc.CastShadows;

                return WriteBlock( meshSer, key );
            };

            s.Deserialize = [key = s.Key]( ECS::Entity entity, const rfl::Generic& g,
                                           const Assets::AssetManager& assetManager )
            {
                auto parsed = ReadBlock<Assets::SkinnedMeshComponentSer>( g, key );
                if ( !parsed.has_value() )
                    return;
                const auto& meshData = parsed.value();

                auto& smc      = entity.AddComponent<ECS::SkinnedMeshComponent>();
                auto  resolver = MakeAssetResolver( assetManager );

                uint64_t meshHandle = 0;
                if ( meshData.MeshGuid )
                    meshHandle = resolver.FromGuid( *meshData.MeshGuid, "SkinnedMeshAsset" );
                if ( meshHandle == 0 && meshData.MeshPath )
                    meshHandle = resolver.FromPath( *meshData.MeshPath, "SkinnedMeshAsset" );
                if ( meshHandle != 0 )
                    smc.MeshHandle = Common::UUID( meshHandle );

                const size_t slotCount = meshData.MaterialGuids
                                              ? meshData.MaterialGuids->size()
                                              : ( meshData.MaterialPaths ? meshData.MaterialPaths->size() : 0 );
                if ( slotCount > 0 )
                {
                    smc.MaterialSlots.clear();
                    for ( size_t i = 0; i < slotCount; ++i )
                    {
                        uint64_t h = 0;
                        if ( meshData.MaterialGuids && i < meshData.MaterialGuids->size() )
                            h = resolver.FromGuid( ( *meshData.MaterialGuids )[i], "MaterialAsset" );
                        if ( h == 0 && meshData.MaterialPaths && i < meshData.MaterialPaths->size() )
                            h = resolver.FromPath( ( *meshData.MaterialPaths )[i], "MaterialAsset" );
                        smc.MaterialSlots.push_back( Common::UUID( h ) );
                    }
                }

                smc.CastShadows = meshData.CastShadows.value_or( smc.CastShadows );
            };

            Register( std::move( s ) );
        }

        // ---- UI Anim (custom: the reflected path has no vector-of-struct support; the playhead is
        //      runtime-only and never written) ----
        {
            ComponentSerializer s;
            s.Key       = "UIAnim";
            s.Has       = []( ECS::Entity e ) { return e.HasComponent<ECS::UIAnimComponent>(); };
            s.Serialize = [key = s.Key]( ECS::Entity e, const Assets::AssetManager& ) -> rfl::Generic
            {
                const auto&                d = e.GetComponent<ECS::UIAnimComponent>().Data;
                Assets::UIAnimComponentSer ser;
                ser.Duration = d.Duration;
                ser.Loop     = d.Loop;
                ser.Playing  = d.Playing;
                ser.Tracks.reserve( d.Tracks.size() );
                for ( const auto& tr : d.Tracks )
                {
                    Assets::UIAnimTrackSer ts;
                    ts.Property = static_cast<int>( tr.Property );
                    ts.Keys.reserve( tr.Keys.size() );
                    for ( const auto& k : tr.Keys )
                        ts.Keys.push_back( { k.Time, k.Value, static_cast<int>( k.Easing ) } );
                    ser.Tracks.push_back( std::move( ts ) );
                }
                return WriteBlock( ser, key );
            };
            s.Deserialize = [key = s.Key]( ECS::Entity e, const rfl::Generic& g, const Assets::AssetManager& )
            {
                auto parsed = ReadBlock<Assets::UIAnimComponentSer>( g, key );
                if ( !parsed.has_value() )
                    return;
                const auto& d    = parsed.value();
                auto&       ac   = e.HasComponent<ECS::UIAnimComponent>() ? e.GetComponent<ECS::UIAnimComponent>()
                                                                          : e.AddComponent<ECS::UIAnimComponent>();
                ac.Data.Duration = d.Duration;
                ac.Data.Loop     = d.Loop;
                ac.Data.Playing  = d.Playing;
                ac.Data.Time     = 0.0f;
                ac.Data.Tracks.clear();
                ac.Data.Tracks.reserve( d.Tracks.size() );
                for ( const auto& ts : d.Tracks )
                {
                    ECS::UIAnimTrack tr;
                    tr.Property = static_cast<ECS::UITweenProperty>( ts.Property );
                    tr.Keys.reserve( ts.Keys.size() );
                    for ( const auto& k : ts.Keys )
                        tr.Keys.push_back( { k.Time, k.Value, static_cast<ECS::UIEasing>( k.Easing ) } );
                    std::sort( tr.Keys.begin(), tr.Keys.end(),
                               []( const ECS::UIAnimKey& a, const ECS::UIAnimKey& b )
                               { return a.Time < b.Time; } );
                    ac.Data.Tracks.push_back( std::move( tr ) );
                }
            };
            Register( std::move( s ) );
        }

        // ---- Text (custom: only the authored fields; the glyph mesh is transient) ----
        {
            ComponentSerializer s;
            s.Key       = "Text";
            s.Has       = []( ECS::Entity e ) { return e.HasComponent<ECS::TextComponent>(); };
            s.Serialize = [key = s.Key]( ECS::Entity e, const Assets::AssetManager& ) -> rfl::Generic
            {
                const auto& tc = e.GetComponent<ECS::TextComponent>();
                // The font is an asset HANDLE in memory and persists as the ROOT-TAGGED KEY its handle is
                // the FNV of — the same form the reflected UIText.Font slot above writes, through the
                // same two functions, so the world-space and UI text routes cannot drift apart.
                const std::string fontKey =
                     ServiceKeyForPath( Runtime::ResourceRegistry::GetFontService()->PathForHandle(
                          static_cast<uint64_t>( tc.Font ) ) );
                Assets::TextComponentSer ser{ tc.Text,     fontKey, tc.Color, tc.Size, tc.EmissiveIntensity,
                                              tc.Billboard };
                return WriteBlock( ser, key );
            };
            s.Deserialize = [key = s.Key]( ECS::Entity e, const rfl::Generic& g, const Assets::AssetManager& )
            {
                auto parsed = ReadBlock<Assets::TextComponentSer>( g, key );
                if ( !parsed.has_value() )
                    return;
                const auto& d        = parsed.value();
                auto&       tc       = e.HasComponent<ECS::TextComponent>() ? e.GetComponent<ECS::TextComponent>()
                                                                            : e.AddComponent<ECS::TextComponent>();
                tc.Text              = d.Text;
                // Path -> stable handle (registers it so the handle resolves at render time). Empty stays null,
                // which the render path falls back to the default font for.
                tc.Font = Assets::AssetHandle(
                     Runtime::ResourceRegistry::GetFontService()->RegisterFont( ServicePathForKey( d.Font ) ) );
                tc.Color             = d.Color;
                tc.Size              = d.Size;
                tc.EmissiveIntensity = d.EmissiveIntensity;
                tc.Billboard         = d.Billboard;
            };
            Register( std::move( s ) );
        }

        // ---- Animation (manual: playback settings + the `.danimgraph` this entity plays) ----
        {
            ComponentSerializer s;
            s.Key       = "Animation";
            s.Has       = []( ECS::Entity e ) { return e.HasComponent<ECS::AnimationComponent>(); };
            s.Serialize = [key = s.Key]( ECS::Entity e, const Assets::AssetManager& assetManager ) -> rfl::Generic
            {
                const auto&                   ac = e.GetComponent<ECS::AnimationComponent>();
                Assets::AnimationComponentSer ser;
                ser.CurrentClip   = ac.CurrentClip;
                ser.Playing       = ac.Playing;
                ser.Loop          = ac.Loop;
                ser.PlaybackSpeed = ac.PlaybackSpeed;

                // THE HANDLE, NOT THE GRAPH. `Animation::Graph::Serialize(*ac.Graph)` stood here and put
                // the whole state machine inside the entity; the file is the graph's identity now and the
                // scene only names it. A handle the resolver cannot place writes NOTHING rather than an
                // empty string: absence is how this format says "no state machine".
                if ( ac.GraphAsset )
                {
                    auto resolver = MakeAssetResolver( assetManager );
                    if ( auto path = resolver.ToPath( static_cast<uint64_t>( ac.GraphAsset ), "AnimGraphAsset" );
                         !path.empty() )
                    {
                        ser.Graph = std::move( path );
                    }
                }
                return WriteBlock( ser, key );
            };
            s.Deserialize =
                 [key = s.Key]( ECS::Entity e, const rfl::Generic& g, const Assets::AssetManager& assetManager )
            {
                auto parsed = ReadBlock<Assets::AnimationComponentSer>( g, key );
                if ( !parsed.has_value() )
                    return;
                const auto& d = parsed.value();
                auto& ac = e.HasComponent<ECS::AnimationComponent>() ? e.GetComponent<ECS::AnimationComponent>()
                                                                     : e.AddComponent<ECS::AnimationComponent>();
                ac.CurrentClip = d.CurrentClip;
                ac.Playing     = d.Playing;
                ac.Loop        = d.Loop;
                ac.PlaybackSpeed = d.PlaybackSpeed;

                // ONLY THE HANDLE IS SET HERE. The graph OBJECT is AnimationECSSystem's to hand over
                // (SyncAnimGraph), from the asset, so that every entity naming one file ends up pointing
                // at one object — parsing it here would give each entity a copy and put us back where
                // GraphJson was. FromPath loads the asset, so a graph the scene names and the project
                // cannot produce is reported by the resolver rather than becoming a silently empty slot.
                if ( d.Graph.has_value() && !d.Graph->empty() )
                {
                    auto resolver = MakeAssetResolver( assetManager );
                    ac.GraphAsset = Assets::AssetHandle( resolver.FromPath( *d.Graph, "AnimGraphAsset" ) );
                }
            };
            Register( std::move( s ) );
        }

        // ---- Reflected data blocks (auto-serialized via reflection) ----
        Register( MakeReflected<ECS::CameraComponent, ECS::CameraData>( "Camera", "CameraData",
                                                                        &ECS::CameraComponent::Data ) );
        Register( MakeReflected<ECS::DirectionLightComponent, ECS::DirectionalLightData>(
             "DirectionLight", "DirectionalLightData", &ECS::DirectionLightComponent::Data ) );
        Register( MakeReflected<ECS::PointLightComponent, ECS::PointLightData>(
             "PointLight", "PointLightData", &ECS::PointLightComponent::Data ) );
        Register( MakeReflected<ECS::SpotLightComponent, ECS::SpotLightData>( "SpotLight", "SpotLightData",
                                                                              &ECS::SpotLightComponent::Data ) );
        Register( MakeReflected<ECS::TerrainComponent, ECS::TerrainData>( "Terrain", "TerrainData",
                                                                          &ECS::TerrainComponent::Data ) );
        Register( MakeReflected<ECS::TwoBoneIKComponent, ECS::TwoBoneIKData>( "TwoBoneIK", "TwoBoneIKData",
                                                                              &ECS::TwoBoneIKComponent::Data ) );
        Register( MakeReflected<ECS::ControlRigComponent, ECS::ControlRigData>(
             "ControlRig", "ControlRigData", &ECS::ControlRigComponent::Data ) );
        Register( MakeReflected<ECS::RetargetComponent, ECS::RetargetData>( "Retarget", "RetargetData",
                                                                            &ECS::RetargetComponent::Data ) );
        Register( MakeReflected<ECS::ColliderComponent, ECS::ColliderData>( "Collider", "ColliderData",
                                                                            &ECS::ColliderComponent::Data ) );
        Register( MakeReflected<ECS::RigidBodyComponent, ECS::RigidBodyData>( "RigidBody", "RigidBodyData",
                                                                              &ECS::RigidBodyComponent::Data ) );
        Register( MakeReflected<ECS::CharacterControllerComponent, ECS::CharacterControllerData>(
             "CharacterController", "CharacterControllerData", &ECS::CharacterControllerComponent::Data ) );
        Register( MakeReflected<ECS::AudioSourceComponent, ECS::AudioSourceData>(
             "AudioSource", "AudioSourceData", &ECS::AudioSourceComponent::Data ) );
        Register( MakeReflected<ECS::ParticleEmitterComponent, ECS::ParticleEmitterData>(
             "ParticleEmitter", "ParticleEmitterData", &ECS::ParticleEmitterComponent::Data ) );
        Register( MakeReflected<ECS::UICanvasComponent, ECS::UICanvasData>( "UICanvas", "UICanvasData",
                                                                            &ECS::UICanvasComponent::Data ) );
        Register( MakeReflected<ECS::UILayoutComponent, ECS::UILayoutData>( "UILayout", "UILayoutData",
                                                                            &ECS::UILayoutComponent::Data ) );
        Register( MakeReflected<ECS::UIPanelComponent, ECS::UIPanelData>( "UIPanel", "UIPanelData",
                                                                          &ECS::UIPanelComponent::Data ) );
        Register( MakeReflected<ECS::UITextComponent2D, ECS::UITextData>( "UIText", "UITextData",
                                                                          &ECS::UITextComponent2D::Data ) );
        Register( MakeReflected<ECS::UIButtonComponent, ECS::UIButtonData>( "UIButton", "UIButtonData",
                                                                            &ECS::UIButtonComponent::Data ) );
        Register( MakeReflected<ECS::UIIconComponent, ECS::UIIconData>( "UIIcon", "UIIconData",
                                                                        &ECS::UIIconComponent::Data ) );
        Register( MakeReflected<ECS::UIRenderTextureComponent, ECS::UIRenderTextureData>(
             "UIRenderTexture", "UIRenderTextureData", &ECS::UIRenderTextureComponent::Data ) );

        Register( MakeReflected<ECS::UIBindingComponent, ECS::UIBindingData>( "UIBinding", "UIBindingData",
                                                                              &ECS::UIBindingComponent::Data ) );
        Register( MakeReflected<ECS::UIScreenComponent, ECS::UIScreenData>( "UIScreen", "UIScreenData",
                                                                            &ECS::UIScreenComponent::Data ) );
        Register( MakeReflected<ECS::UIScreenStackComponent, ECS::UIScreenStackData>(
             "UIScreenStack", "UIScreenStackData", &ECS::UIScreenStackComponent::Data ) );
        Register( MakeReflected<ECS::UITweenComponent, ECS::UITweenData>( "UITween", "UITweenData",
                                                                          &ECS::UITweenComponent::Data ) );
        Register( MakeReflected<ECS::UIPointerEventsComponent, ECS::UIPointerEventsData>(
             "UIPointerEvents", "UIPointerEventsData", &ECS::UIPointerEventsComponent::Data ) );
        Register( MakeReflected<ECS::UIDraggableComponent, ECS::UIDraggableData>(
             "UIDraggable", "UIDraggableData", &ECS::UIDraggableComponent::Data ) );
        Register( MakeReflected<ECS::UIDropTargetComponent, ECS::UIDropTargetData>(
             "UIDropTarget", "UIDropTargetData", &ECS::UIDropTargetComponent::Data ) );
        Register( MakeReflected<ECS::UIImageComponent, ECS::UIImageData>( "UIImage", "UIImageData",
                                                                          &ECS::UIImageComponent::Data ) );
        Register( MakeReflected<ECS::UILayoutGroupComponent, ECS::UILayoutGroupData>(
             "UILayoutGroup", "UILayoutGroupData", &ECS::UILayoutGroupComponent::Data ) );
        Register( MakeReflected<ECS::UIProgressBarComponent, ECS::UIProgressBarData>(
             "UIProgressBar", "UIProgressBarData", &ECS::UIProgressBarComponent::Data ) );
        Register( MakeReflected<ECS::UIStyleComponent, ECS::UIStyleData>( "UIStyle", "UIStyleData",
                                                                          &ECS::UIStyleComponent::Data ) );
        Register( MakeReflected<ECS::UIToggleComponent, ECS::UIToggleData>( "UIToggle", "UIToggleData",
                                                                            &ECS::UIToggleComponent::Data ) );
        Register( MakeReflected<ECS::UISliderComponent, ECS::UISliderData>( "UISlider", "UISliderData",
                                                                            &ECS::UISliderComponent::Data ) );
        Register( MakeReflected<ECS::UIScrollViewComponent, ECS::UIScrollViewData>(
             "UIScrollView", "UIScrollViewData", &ECS::UIScrollViewComponent::Data ) );
        Register( MakeReflected<ECS::UIListViewComponent, ECS::UIListViewData>(
             "UIListView", "UIListViewData", &ECS::UIListViewComponent::Data ) );
        Register( MakeReflected<ECS::UIInputFieldComponent, ECS::UIInputFieldData>(
             "UIInputField", "UIInputFieldData", &ECS::UIInputFieldComponent::Data ) );
        Register( MakeReflected<ECS::UIDropdownComponent, ECS::UIDropdownData>(
             "UIDropdown", "UIDropdownData", &ECS::UIDropdownComponent::Data ) );
        // Overlays (Ю12). An overlay canvas and a trigger are ORDINARY scene data — that is the whole point
        // of the shape: nothing is spawned at runtime, so a tooltip, a menu, a dialog and a toast stack
        // survive a save and a reload because they are entities like any other.
        Register( MakeReflected<ECS::UIOverlayComponent, ECS::UIOverlayData>( "UIOverlay", "UIOverlayData",
                                                                              &ECS::UIOverlayComponent::Data ) );
        Register( MakeReflected<ECS::UIOverlayTriggerComponent, ECS::UIOverlayTriggerData>(
             "UIOverlayTrigger", "UIOverlayTriggerData", &ECS::UIOverlayTriggerComponent::Data ) );

        // ---- Marker components (presence is the state) ----
        Register( MakeMarker<ECS::FolderComponent>( "Folder" ) );
        // The authoring lock. Serialized for the reason Components.hpp gives: a lock that does not
        // survive a reload protects nothing. No version bump — an added key is what ForeignKeys is for.
        Register( MakeMarker<ECS::LockComponent>( "Lock" ) );

        // ---- Single-flag components ----
        // The outliner's eye, for the same reason the lock beside it is here: a hidden object that comes
        // back visible on the next load is a setting the user made and the file never kept, and nothing
        // says so — the entity simply reappears. It was the lock's defect, already live for visibility,
        // in the same panel and one row up. No version bump: an added key is what ForeignKeys is for.
        Register(
             MakeFlag<ECS::VisibilityComponent>( "Visibility", "Visible", &ECS::VisibilityComponent::Visible ) );

        // ---- Hand-mapped authored blocks (no reflected Data; see AuthoredComponentIO.hpp) ----
        // U13. Five components with a full Details editor and no row in this table: everything the
        // artist typed into them was discarded by the next load, and by every Ctrl+C, every delete-undo,
        // every prefab instancing and every Play/Stop, because all of those are this same registry.
        // Desert/Tests/Engine/ComponentPersistence now derives "is authored in Details" from the editor's
        // own registration source and refuses to let a sixth one exist. No version bump: an added key is
        // what ForeignKeys is for, and no scene in this repository carries these blocks yet — nothing
        // ever wrote one.
        Register( MakeAuthored<ECS::FoliageComponent>( "Foliage" ) );
        Register( MakeAuthored<ECS::LocomotionComponent>( "Locomotion" ) );
        Register( MakeAuthored<ECS::MorphComponent>( "Morph" ) );
        Register( MakeAuthored<ECS::SocketAttachmentComponent>( "SocketAttachment" ) );
        Register( MakeAuthored<ECS::ProjectileComponent>( "Projectile" ) );

        // ---- Skybox (now FULLY REFLECTED via RA3) ----
        // No more hand-written SkyboxComponentSer / field mapping: the whole component reflects, and its
        // SkyboxHandle round-trips as a path through the AssetResolver. It now carries the HDR path ONLY —
        // the procedural sky lives under "SkyAtmosphere".
        Register( MakeReflectedSelf<ECS::SkyboxComponent>( "Skybox", "SkyboxComponent" ) );

        // ---- Sky / fog ----
        // Data-block components, so one line each is the whole of save/load, duplicate and undo.
        Register( MakeReflected<ECS::SkyAtmosphereComponent, ECS::SkyAtmosphereData>(
             "SkyAtmosphere", "SkyAtmosphereData", &ECS::SkyAtmosphereComponent::Data ) );
        Register( MakeReflected<ECS::ExponentialHeightFogComponent, ECS::ExponentialHeightFogData>(
             "ExponentialHeightFog", "ExponentialHeightFogData", &ECS::ExponentialHeightFogComponent::Data ) );
        Register( MakeReflected<ECS::VolumetricCloudComponent, ECS::VolumetricCloudData>(
             "VolumetricCloud", "VolumetricCloudData", &ECS::VolumetricCloudComponent::Data ) );
        Register( MakeReflected<ECS::HeroCloudComponent, ECS::HeroCloudData>( "HeroCloud", "HeroCloudData",
                                                                              &ECS::HeroCloudComponent::Data ) );

        // ---- Script (manual: .lua path + exposed-property values) ----
        Register( MakeScript() );
    }

} // namespace Desert::Core::Serialize
