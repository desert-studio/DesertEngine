#pragma once

// THE HELPERS EVERY HAND-WRITTEN COMPONENT SERIALIZER SHARES (BUILD-MOD-5). Internal to Core/Serialize: the
// serializers that used to be one 2400-line ComponentRegistry.cpp are split by subject into
// *ComponentSerializers.cpp beside it, and this is what they had in common. ComponentRegistry.cpp itself includes
// only the declarations (BuiltinComponentSerializers.hpp), so it compiles in seconds and names no component
// header.

#include <Engine/Core/Serialize/ComponentRegistration.hpp>
#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Core/Serialize/AssetReferenceResolve.hpp>
#include <Engine/Core/Serialize/AuthoredComponentIO.hpp>
#include <Engine/Core/Serialize/GenericBlock.hpp>
#include <Engine/Core/Serialize/StoredAssetForm.hpp>
#include <Engine/Core/Serialize/TextureSlot.hpp>
#include <Engine/World/Landscape/LandscapeTileFiles.hpp>

#include <Common/Core/ByteText.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Json/Document.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>

#include <cstring>
#include <format>
#include <array>
#include <optional>
#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AssetRefSerialization.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/MaterialAsset.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>
#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Assets/CloudModellingVolumeAsset.hpp>
#include <Engine/Assets/AnimGraphAsset.hpp>
#include <Engine/Assets/ControlRigAsset.hpp>
#include <Engine/Assets/RetargetAsset.hpp>
#include <Engine/Assets/SoundAsset.hpp>
#include <Engine/Assets/IesProfileAsset.hpp>
#include <Engine/Assets/SparseVolumeAsset.hpp>
#include <Engine/Assets/FoliageTypeAsset.hpp>
#include <Engine/Assets/WaterWavesAsset.hpp>
#include <Engine/Assets/WorldParametersAsset.hpp>
#include <Engine/Assets/WeatherPresetAsset.hpp>
#include <Engine/Assets/PhysicsAsset.hpp>
#include <Engine/Assets/VFXSystemAsset.hpp>
#include <Engine/Assets/LevelSequenceAsset.hpp>
#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Assets/UIThemeAsset.hpp>
#include <Engine/Assets/LandscapeLayerInfoAsset.hpp>
#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/ECS/CubeGridBlockoutComponent.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>

#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>

#include <algorithm>
#include <Engine/Runtime/Services/CloudModelling/CloudModellingService.hpp>
#include <Engine/Runtime/Services/Font/FontService.hpp>
#include <Engine/Runtime/Services/Icon/IconService.hpp>
#include <Engine/Runtime/Services/UITheme/UIThemeService.hpp>
#include <Engine/Runtime/Services/Video/VideoService.hpp>
#include <Engine/Animation/Timeline/Player.hpp>
#include <Engine/ECS/AnimationComponent.hpp>
#include <Engine/ECS/FolderComponent.hpp>
#include <Engine/ECS/InstancedStaticMeshComponent.hpp>
#include <Engine/ECS/LevelSequenceComponent.hpp>
#include <Engine/ECS/LockComponent.hpp>
#include <Engine/ECS/MaterialComponent.hpp>
#include <Engine/ECS/MorphComponent.hpp>
#include <Engine/ECS/SkinnedMeshComponent.hpp>
#include <Engine/ECS/SocketAttachmentComponent.hpp>
#include <Engine/ECS/StaticMeshComponent.hpp>
#include <Engine/ECS/TagComponent.hpp>
#include <Engine/ECS/TextComponent.hpp>
#include <Engine/ECS/VisibilityComponent.hpp>
#include <Engine/Scripting/ScriptComponent.hpp>
#include <Engine/Scripting/ScriptProperty.hpp>
#include <Engine/UI/Ecs/UIAnimComponent.hpp>
#include <Engine/UI/Ecs/UIRenderTextureComponent.hpp>
#include <Engine/World/Foliage/FoliageComponent.hpp>
#include <Engine/World/Landscape/LandscapeComponent.hpp>
#include <Engine/World/Landscape/LandscapeSplineComponent.hpp>
#include <Engine/World/Landscape/LandscapeTileComponent.hpp>
#include <Engine/World/Partition/AlwaysLoadedComponent.hpp>
#include <UI/Args/UIImageArgs.hpp>
#include <Common/Json/ReadWrite.hpp>
#include <Common/Json/DocumentStruct.hpp>

#include <Engine/Core/Serialize/BuiltinComponentSerializers.hpp>

namespace Desert::Core::Serialize::Builtins
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
    inline std::string ServiceKeyForPath( const std::string& path )
    {
        // An empty path is "this slot names nothing" and must stay empty rather than become a bare
        // tag — the read side below turns a non-empty string into a registration attempt.
        return path.empty() ? std::string() : Common::AssetHandle::StableKeyForPath( path );
    }

    inline std::string ServicePathForKey( const std::string& key )
    {
        // PathForStableKey reads a content key, a synthetic scheme:// key or an absolute path, and
        // refuses a bare relative string by name (answering empty: the slot names nothing).
        return key.empty() ? std::string() : Common::AssetHandle::PathForStableKey( key ).generic_string();
    }

    // A STORED CONTENT REFERENCE EXPANDED TO THE FILE IT NAMES. The one spelling is the root-tagged key
    // ("assets:Materials/M.demat", Common::AssetHandle::StableKeyForPath; UE's /Game/... package path),
    // expanded by its exact inverse; an absolute path names a file genuinely outside the project and is
    // taken as given. The bare root-relative spelling ("Materials/M.demat") that used to be joined to
    // ASSETS_PATH here is GONE: the corpus was restated in the key form (PATH1), and a bare string is
    // refused, naming it, rather than resolved against a root the reader guesses.
    inline std::filesystem::path FileForStoredReference( const std::string& stored, const std::string& type )
    {
        if ( Common::AssetHandle::IsProjectRelativeKey( stored ) )
            return Common::AssetHandle::PathForStableKey( stored );
        const std::filesystem::path named( stored );
        if ( named.is_absolute() )
            return named.lexically_normal();
        LOG_ERROR( "[Scene] '{0}' reference '{1}' is neither a content key (\"{2}:<path>\") nor an absolute "
                   "path, so it names no file: the slot loads unset. Re-point the slot, or restate the "
                   "reference as \"{2}:{1}\" if it is relative to the assets root.",
                   type, stored, Common::AssetHandle::AssetsTag() );
        return {};
    }

    // The scene a UIRenderTexture names, as SCNE 31 states it: the `.desce` header GUID (read through the
    // VFS first, like every identity) and the file's stable key for the reader.
    inline Common::ResultStr<Assets::AssetGuidRef> SceneRefForPath( const std::string&          path,
                                                                    const Assets::AssetRefSite& site )
    {
        return Assets::WriteAssetGuidRef( Assets::ReadTextHeaderGuid( path ), std::filesystem::path( path ),
                                          site );
    }

    // BY GUID through the content registry, which indexes scenes by their header GUID (AF6f): a scene
    // moved or renamed keeps its GUID, so `Path` is only named in the error and never looked up.
    inline Common::ResultStr<std::string> ScenePathForRef( const Assets::AssetGuidRef& ref,
                                                           const Assets::AssetRefSite& site )
    {
        return Assets::ResolveAssetGuidRef(
             ref,
             []( const Common::Content::AssetGuid& guid ) -> std::optional<std::string>
             {
                 const Common::Utils::AssetRegistryEntry* row =
                      Assets::ContentRegistry::Get().FindByGuidReference( guid, {} );
                 if ( row == nullptr || row->Kind != "Scene" )
                     return std::nullopt;
                 return Common::AssetHandle::PathForStableKey( row->Key ).generic_string();
             },
             site );
    }

    inline std::string EntityContext( ECS::Entity entity )
    {
        return std::format( "entity '{}'", entity.GetComponent<ECS::TagComponent>().Tag );
    }

    // A marker component (no data, e.g. FolderComponent): presence IS the state. Serializes as an empty
    // object; deserialize just re-adds it.
    template <class TComponent>
    ComponentSerializer MakeMarker( std::string key )
    {
        ComponentSerializer s;
        s.Key       = std::move( key );
        s.Has       = []( ECS::Entity e ) { return e.HasComponent<TComponent>(); };
        s.Serialize = []( ECS::Entity, const Assets::AssetManager& ) -> Common::Json::Value
        { return { Common::Json::Object{} }; };
        s.Deserialize =
             []( ECS::Entity e, const Common::Json::Node&, const Assets::AssetManager&, Common::Json::Issues& )
        {
            if ( !e.HasComponent<TComponent>() )
                e.AddComponent<TComponent>();
        };
        return s;
    }

    // A component whose ENTIRE state is one bool (VisibilityComponent: IsVisible / ECS::SetVisible). It gets its
    // own maker rather than reusing MakeMarker above, because MakeMarker WOULD COMPILE AND WOULD BE WRONG: it
    // writes an empty object and re-adds the component on load, so the value comes back at the
    // struct's default. `Visible = false` would round-trip to `true` — the presence would survive and
    // the only thing the component carries would not, which is the defect this registration came to
    // fix wearing the shape of a fix.
    //
    // A record that carries the component but not its field is REPORTED and left at the default: an
    // entity is drawn either way, so a silent choice here is a scene that renders differently from
    // the file it claims to be.
    // The value is read through `get` and written through `set`, the component's one writer (ECS::SetVisible),
    // so a loaded flag reaches the render-state dirty set like any other write.
    template <class TComponent>
    ComponentSerializer MakeFlag( std::string key, std::string field, bool ( TComponent::*get )() const,
                                  bool ( *set )( entt::registry&, entt::entity, bool ) )
    {
        ComponentSerializer s;
        s.Key       = std::move( key );
        s.Has       = []( ECS::Entity e ) { return e.HasComponent<TComponent>(); };
        s.Serialize = [field, get]( ECS::Entity e, const Assets::AssetManager& ) -> Common::Json::Value
        {
            Common::Json::Object object;
            object[field] = Common::Json::Value( ( e.GetComponent<TComponent>().*get )() );
            return { object };
        };
        s.Deserialize = [field, get, set]( ECS::Entity e, const Common::Json::Node& g, const Assets::AssetManager&,
                                           Common::Json::Issues& issues )
        {
            if ( !e.HasComponent<TComponent>() )
                e.AddComponent<TComponent>();

            if ( !g.ExpectKind( Common::Json::Kind::Object, issues ) )
                return;
            const auto value = g.Find( field );
            if ( !value )
            {
                issues.push_back(
                     { g.Where().Key( field ).ToString(), "bool", "nothing (the member is absent)" } );
                return;
            }
            bool flag = ( e.GetComponent<TComponent>().*get )();
            value->ReadValue( flag, issues );
            set( *e.GetRegistry(), e.GetHandle(), flag );
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
        s.Serialize = []( ECS::Entity e, const Assets::AssetManager& ) -> Common::Json::Value
        { return Common::Json::Value( WriteComponent( e.GetComponent<TComponent>() ) ); };
        s.Deserialize = []( ECS::Entity e, const Common::Json::Node& block, const Assets::AssetManager&,
                            Common::Json::Issues& issues )
        {
            if ( block.ExpectKind( Common::Json::Kind::Object, issues ) )
            {
                auto& comp =
                     e.HasComponent<TComponent>() ? e.GetComponent<TComponent>() : e.AddComponent<TComponent>();
                ReadComponent( block, comp, issues );
            }
        };
        return s;
    }

    using Reflection::ResolveGuidRef;

    inline uint64_t ResolveSlotRef( const Reflection::AssetResolver&               resolver,
                                    const std::optional<std::vector<std::string>>& guids,
                                    const std::optional<std::vector<std::string>>& paths, std::size_t slot )
    {
        const std::string text = ( guids && slot < guids->size() ) ? ( *guids )[slot] : std::string();
        const std::string path = ( paths && slot < paths->size() ) ? ( *paths )[slot] : std::string();
        return ResolveGuidRef( resolver, text, path, "MaterialAsset", std::format( "material slot {}", slot ) );
    }

    inline uint64_t ResolveMeshRef( const Reflection::AssetResolver&  resolver,
                                    const std::optional<std::string>& guid, const std::optional<std::string>& path,
                                    const char* type )
    {
        return ResolveGuidRef( resolver, guid.value_or( std::string() ), path.value_or( std::string() ), type,
                               std::string( type ) + " reference" );
    }

    // The GUID text a reference is saved under: the header GUID the loaded asset states, else the cooked
    // registry row of its file. A handle with neither is an asset with no identity to save; it is NAMED
    // and saved empty.
    inline std::string GuidTextOrRegistry( uint64_t                                         handle,
                                           const std::optional<Common::Content::AssetGuid>& loaded,
                                           const char*                                      kind )
    {
        if ( loaded && !loaded->IsNull() )
            return Common::Content::AssetGuidToText( *loaded );
        if ( const auto guid = Assets::ContentRegistry::GuidForHandle( handle ) )
            return Common::Content::AssetGuidToText( *guid );
        LOG_ERROR( "[ComponentRegistry] {0} handle {1} has no header GUID in the loaded asset or the content "
                   "registry - its reference is saved empty",
                   kind, handle );
        return {};
    }

    // Absent (not "") when the mesh has no identity, so the file carries no MeshGuid key at all.
    inline std::optional<std::string> MeshGuidText( const Reflection::AssetResolver& resolver, uint64_t handle,
                                                    const char* type )
    {
        std::string text = resolver.ToGuid( handle, type );
        if ( text.empty() )
            return std::nullopt;
        return text;
    }
} // namespace Desert::Core::Serialize::Builtins
