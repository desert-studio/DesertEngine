#include <Engine/Core/SceneAssetRoots.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Core
{
    namespace
    {
        // The reason string a root carries. Built once per component kind rather than per entity: the set
        // holds the FIRST reason for a handle and a popular asset is named by hundreds of entities, so a
        // per-entity string would be hundreds of allocations to keep one of them.
        void MarkSlots( Assets::AssetRootSet& roots, const std::vector<Assets::AssetHandle>& slots,
                        const char* why )
        {
            for ( const Assets::AssetHandle& slot : slots )
                roots.Mark( slot, why );
        }
    } // namespace

    void CollectAssetRoots( Scene& scene, Assets::AssetRootSet& roots )
    {
        // A NON-const registry and plainly-spelled component types. The Scene is taken by reference for
        // that reason alone — nothing here writes. This engine's vendored entt does not accept `const T`
        // as a view argument (it asserts `is_same_v<const T, T>`), and its const-registry views require
        // exactly that spelling, so a read-only pass has to be written this way; every other read-only
        // walk in the engine is.
        auto& registry = scene.GetRegistry();

        // ── THE SCENE'S OWN SETTINGS ──────────────────────────────────────────────────────────────────
        roots.Mark( scene.GetSettings().SplashSprite, "the scene's splash sprite" );

        // ── GEOMETRY ──────────────────────────────────────────────────────────────────────────────────
        //
        // The mesh AND its per-entity material slots. The slots are marked directly rather than reached
        // through the mesh's own default material list, because an entity may override them — and because
        // a mesh that is registered but not yet parsed cannot report its defaults at all (the evictor's
        // Expand deliberately reads no unloaded asset).
        for ( const auto entity : registry.view<ECS::StaticMeshComponent>() )
        {
            const auto& mesh = registry.get<ECS::StaticMeshComponent>( entity );
            roots.Mark( mesh.MeshHandle, "a StaticMeshComponent draws it" );
            MarkSlots( roots, mesh.MaterialSlots, "a StaticMeshComponent draws with it" );
        }

        for ( const auto entity : registry.view<ECS::SkinnedMeshComponent>() )
        {
            const auto& mesh = registry.get<ECS::SkinnedMeshComponent>( entity );
            roots.Mark( mesh.MeshHandle, "a SkinnedMeshComponent draws it" );
            MarkSlots( roots, mesh.MaterialSlots, "a SkinnedMeshComponent draws with it" );
        }

        for ( const auto entity : registry.view<ECS::InstancedStaticMeshComponent>() )
        {
            const auto& mesh = registry.get<ECS::InstancedStaticMeshComponent>( entity );
            roots.Mark( mesh.MeshHandle, "an InstancedStaticMeshComponent draws it" );
            MarkSlots( roots, mesh.MaterialSlots, "an InstancedStaticMeshComponent draws with it" );
        }

        for ( const auto entity : registry.view<ECS::LandscapeMaterialComponent>() )
            roots.Mark( registry.get<ECS::LandscapeMaterialComponent>( entity ).Data.Material,
                        "a landscape is surfaced with it" );

        // A generic shader material's texture overrides. `uint64_t` rather than AssetHandle in the
        // component, which is why this loop cannot be folded into the others.
        for ( const auto entity : registry.view<ECS::MaterialComponent>() )
        {
            for ( const auto& texture : registry.get<ECS::MaterialComponent>( entity ).Textures )
                roots.Mark( Assets::AssetHandle( texture.TextureHandle ),
                            "a MaterialComponent overrides a texture slot with it" );
        }

        // ── THE WORLD ─────────────────────────────────────────────────────────────────────────────────
        for ( const auto entity : registry.view<ECS::SkyboxComponent>() )
            roots.Mark( registry.get<ECS::SkyboxComponent>( entity ).SkyboxHandle,
                        "a SkyboxComponent is the scene's environment" );

        for ( const auto entity : registry.view<ECS::PrefabComponent>() )
            roots.Mark( registry.get<ECS::PrefabComponent>( entity ).Prefab,
                        "a PrefabComponent was instantiated from it" );

        for ( const auto entity : registry.view<ECS::TextComponent>() )
            roots.Mark( registry.get<ECS::TextComponent>( entity ).Font,
                        "a world-space TextComponent is set in it" );

        // THE RIG IS A ROOT LIKE ANY OTHER SLOT. Nothing else names a `.derig` — a rig is named only by
        // an entity's ControlRigComponent — so without this row the first eviction sweep after a scene
        // load drops it, AnimationECSSystem finds a handle the manager has never heard of, and the
        // character silently poses from its clip alone. That is the shape the UI theme hit (Ю13) and the
        // one the string tables hit; it is cheaper to write the row than to debug the symptom.
        for ( const auto entity : registry.view<ECS::ControlRigComponent>() )
        {
            roots.Mark( registry.get<ECS::ControlRigComponent>( entity ).Data.Rig, "an entity is posed by it" );
        }

        // THE RETARGET IS A ROOT FOR THE RIG'S REASON AND BY THE SAME MECHANISM. Nothing else names a
        // `.retarget`. It is also the row that keeps the SOURCE rig alive: the source `SkeletonAsset` is
        // reachable only through `RetargetAsset`'s own dependency, exactly as a `.skeleton` is reachable
        // only through the `.skmesh` that names it, so dropping the retarget drops the rig behind it and
        // nothing can bring either back.
        for ( const auto entity : registry.view<ECS::RetargetComponent>() )
        {
            roots.Mark( registry.get<ECS::RetargetComponent>( entity ).Data.Retarget,
                        "an entity plays a foreign clip through it" );
        }

        // THE ANIM GRAPH IS A ROOT FOR THE RIG'S REASON AND BY THE SAME MECHANISM. Nothing else names a
        // `.danimgraph` — a graph is named only by an entity's AnimationComponent — so without this row the
        // first eviction sweep after a scene load drops it, AnimationECSSystem finds a handle the manager
        // has never heard of, and the character silently plays its single `CurrentClip` while the file says
        // it has a state machine. That is the shape the UI theme hit (Ю13), the string tables hit, and the
        // rig would have hit; it is cheaper to write the row than to debug the symptom a fourth time.
        for ( const auto entity : registry.view<ECS::AnimationComponent>() )
        {
            roots.Mark( registry.get<ECS::AnimationComponent>( entity ).GraphAsset,
                        "an entity is animated by it" );
        }

        // ── THE INTERFACE ─────────────────────────────────────────────────────────────────────────────
        //
        // Every UI element that names an asset. They are separate components rather than one, so this is
        // one loop each; the census in Desert/Tests/Engine/AssetRoots is what keeps the list complete when
        // the eighth element type is added.
        for ( const auto entity : registry.view<ECS::UICanvasComponent>() )
        {
            const auto& canvas = registry.get<ECS::UICanvasComponent>( entity ).Data;
            roots.Mark( canvas.Sprite, "a UI canvas draws it as its background" );
            // THE THEME IS A ROOT LIKE ANY OTHER SLOT (Ю13). Without this row the evictor would drop a
            // theme that no OTHER asset names — nothing does; a theme is named only by a canvas — and the
            // whole interface would silently fall back to the elements' own colours mid-session, which
            // looks exactly like a theme system that stopped working.
            roots.Mark( canvas.Theme, "a UI canvas is themed by it" );
        }

        for ( const auto entity : registry.view<ECS::UIPanelComponent>() )
        {
            const auto& panel = registry.get<ECS::UIPanelComponent>( entity ).Data;
            roots.Mark( panel.Sprite, "a UI panel draws it" );
            roots.Mark( panel.Video, "a UI panel plays it" );
        }

        for ( const auto entity : registry.view<ECS::UIIconComponent>() )
            roots.Mark( registry.get<ECS::UIIconComponent>( entity ).Data.Icon, "a UI icon draws it" );

        for ( const auto entity : registry.view<ECS::UIImageComponent>() )
            roots.Mark( registry.get<ECS::UIImageComponent>( entity ).Data.Sprite, "a UI image draws it" );

        for ( const auto entity : registry.view<ECS::UITextComponent2D>() )
            roots.Mark( registry.get<ECS::UITextComponent2D>( entity ).Data.Font, "a UI label is set in it" );

        for ( const auto entity : registry.view<ECS::UIButtonComponent>() )
        {
            const auto& button = registry.get<ECS::UIButtonComponent>( entity ).Data;
            roots.Mark( button.Sprite, "a UI button draws it" );
            roots.Mark( button.HoverSprite, "a UI button draws it on hover" );
            roots.Mark( button.PressedSprite, "a UI button draws it while pressed" );
        }
    }

    Assets::AssetRootSet CollectAssetRootsFromLiveScenes()
    {
        Assets::AssetRootSet roots;

        // WHICH WORLDS ANSWERED, AND WITH HOW MUCH. Called only when a sweep is due, so this is one line
        // per eviction rather than one per frame — and it is the line that separates the two ways a small
        // root set happens: a genuinely small scene, and a walk that visited the wrong worlds. Without it
        // "17 roots, then 5 roots, same scene name" is unreadable, which is exactly the state the first
        // measured session left this in.
        std::string census;
        for ( Scene* scene : Scene::LiveScenes() )
        {
            if ( scene == nullptr )
                continue;

            const std::size_t before = roots.Size();
            CollectAssetRoots( *scene, roots );

            if ( !census.empty() )
                census += ", ";
            census += "'" + scene->GetSceneName() + "' (+" + std::to_string( roots.Size() - before ) + ")";
        }

        LOG_INFO( "[Assets] roots: {} live scene(s) named {} asset(s) -- {}", Scene::LiveScenes().size(),
                  roots.Size(), census.empty() ? std::string( "no live scene" ) : census );

        return roots;
    }

} // namespace Desert::Core
