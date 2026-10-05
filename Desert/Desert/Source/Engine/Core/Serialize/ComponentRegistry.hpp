#pragma once

#include <Engine/ECS/Entity.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>

#include <Common/Json/Document.hpp>

#include <functional>
#include <string>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Core::Serialize
{
    // One registered component (de)serializer. The unifying mechanism behind entity serialization:
    // every serializable component contributes one of these instead of a hand-written branch in
    // EntitySerializer. Reflected components (light/camera data blocks) get an auto-generated handler
    // driven by the reflection registry; asset-bearing components (mesh/skybox) provide a custom handler
    // because they map asset handles <-> file paths, which reflection cannot know about.
    struct ComponentSerializer
    {
        std::string Key; // JSON key under the entity's "Components" object

        std::function<bool( ECS::Entity )>                                                   Has;
        std::function<Common::Json::Value( ECS::Entity, const Assets::AssetManager& )>       Serialize;
        // `block` carries its place in the scene ("Entities[id=4127].Light"); a wrong-typed value is appended to
        // `issues` with its full path and the caller reports the entity's issues in one line.
        std::function<void( ECS::Entity, const Common::Json::Node& block, const Assets::AssetManager&,
                            Common::Json::Issues& issues )>
             Deserialize;
    };

    // Process-wide table of component serializers, built once. Adding a new serializable component means
    // registering it here (see ComponentRegistry.cpp) — not editing EntitySerializer.
    class ComponentRegistry
    {
    public:
        static const ComponentRegistry& Get();

        const std::vector<ComponentSerializer>& All() const
        {
            return m_Serializers;
        }

    private:
        ComponentRegistry();

        void Register( ComponentSerializer serializer );
        void RegisterBuiltins();

        std::vector<ComponentSerializer> m_Serializers;
    };

    // The one place an AssetResolver is built (the invariant is stated on AssetResolver itself). Exposed
    // because SceneSettings is reflected like a component but is NOT one, so it is serialized straight
    // from SceneSerializer — and until this was exposed that call had no resolver, which meant the
    // scene's own `SplashSprite` was the last field in the engine still written as a raw 64-bit number.
    Reflection::AssetResolver MakeAssetResolver( const Assets::AssetManager& mgr );

    // SaveMaterialComponentToJson / LoadMaterialComponentFromJson USED TO BE DECLARED HERE and are gone.
    // They were the "MVP" `.demat` writer from before a material became an ASSET: their own comment still
    // promised "full asset-system integration is a later milestone", and that milestone arrived —
    // Assets::MaterialData is the on-disk canon, SurfaceMaterialAsset reads it and MaterialService owns
    // it. Nothing had called either function for as long as that has been true.
    //
    // They are deleted rather than fixed because they were a SECOND, unreachable copy of the component
    // serializer twenty lines above — including its Path+Guid double write, which is what this change came
    // to remove. Repairing an unreachable copy leaves two paths, one of which no test can execute (§4.1).
} // namespace Desert::Core::Serialize
