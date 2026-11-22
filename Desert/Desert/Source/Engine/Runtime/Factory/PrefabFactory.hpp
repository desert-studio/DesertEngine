#pragma once

#include <Engine/ECS/Entity.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include <unordered_set>

namespace Desert::Core
{
    class Scene;
} // namespace Desert::Core

namespace Desert::Runtime::Factory
{

    // A PACKED PREFAB INSTANCE (UE APackedLevelActor): its root carries this, and one InstancedStaticMesh child
    // per group of the prefab's packed form (Assets::PackedPrefabSer). RUNTIME ONLY, like PrefabInstanceComponent:
    // the scene states the mode (EntityData::PrefabPacked) and the prefab file states the form, so nothing here is
    // written. The ISM instances are WORLD matrices (MeshECSSystem), so the groups are re-placed whenever the
    // root's world moves, and rebuilt whenever the prefab's form changes (RefreshPackedInstances).
    struct PackedPrefabComponent
    {
        Assets::Asset<Assets::PrefabAsset>             Prefab; // the source whose packed form is drawn
        std::shared_ptr<const Assets::PackedPrefabSer> Form;   // the form the groups below were built from
        const Assets::AssetManager*                    Manager = nullptr;
        std::vector<ECS::Entity>                       Groups; // the ISM children, one per Form group, in order
        glm::mat4   PlacedAt{ 0.0f };                          // root world the instances were composed with
        std::string ReportedRefusal;                           // the source checksum already refused on the log
    };

    class PrefabFactory
    {
    public:
        // Build one instance of @p prefab in @p scene and hand back its root.
        //
        // @p pathPrefix is the address of the entity this instantiation hangs under, in the sense
        // PrefabInstanceComponent::SourcePath documents: empty at the top, and the enclosing record's own
        // path when a nested prefab is being built. It is an ARGUMENT and not a member because the nesting
        // is a recursion and the prefix is the only thing that differs between its levels — deriving it
        // from the scene afterwards would mean reading back a fact the recursion already knows.
        //
        // @p rootId is the identity the INSTANCE ROOT is created under. Every entity of an instance is
        // minted a fresh uuid on purpose — two instances of one prefab must not collide — but the root
        // is the one the SCENE FILE names, and minting it too meant a `.desce` got a different id for
        // the same instance on every save: measured as the ONLY difference between two consecutive
        // saves of the witness scene, which is churn in version control and a parent link that means a
        // different entity each time the file is written. The scene loader passes the id the record
        // states; a fresh instantiation passes nothing and gets a minted one, as before.
        static ECS::Entity Instantiate( const Assets::PrefabAsset& prefab, Core::Scene& scene,
                                        const Assets::AssetManager&       assetManager,
                                        std::unordered_set<Common::UUID>& stack,
                                        const std::vector<Common::UUID>&  pathPrefix = {},
                                        std::optional<Common::UUID>       rootId     = {} );

        // THE PREFAB A NESTING RECORD NAMES (EntityData::PrefabPath), found in @p assetManager or registered
        // there, and loaded. Nothing registers a nested prefab but the reader of the file that nests it, so this
        // is the ONE way both the instancer and a reader of a prefab's body without instancing it (the Default
        // Pawn's capsule, Core::DefaultPawnCapsule) reach it. Refuses with the reason; never a silent null.
        [[nodiscard]] static Common::ResultStr<Assets::Asset<Assets::PrefabAsset>>
        ResolveNested( const Assets::AssetManager& assetManager, const std::string& path );

        // Every entity of a finished instance, keyed by PrefabPathKey(SourcePath). This is how an override
        // finds the entity it belongs to, and it is a walk of the LIVE subtree rather than a map handed
        // back by Instantiate so that the caller can index an instance it did not create itself (the
        // editor's "apply overrides to the selected instance" is exactly that).
        static std::unordered_map<std::string, ECS::Entity> IndexInstance( ECS::Entity instanceRoot );

        // Put @p overrides back onto a finished instance. @p indexed comes from IndexInstance; @p prefix
        // is prepended to each override's Path, which is what makes a nested prefab's own overrides
        // (stored relative to that prefab) address the right entities of the outer instance.
        //
        // Returns the number of overrides whose entity was NOT found. An override that matches nothing is
        // a user's edit that has quietly stopped being applied — the loader says so by name rather than
        // succeeding on the ones it did find.
        static std::size_t ApplyOverrides( const std::vector<Assets::PrefabOverrideData>&      overrides,
                                           const std::unordered_map<std::string, ECS::Entity>& indexed,
                                           std::span<const Common::UUID>                       prefix,
                                           const Assets::AssetManager&                         assetManager );

        // ONE PACKED INSTANCE of @p prefab: its root (the root record's tag and transform, a PrefabComponent and a
        // PackedPrefabComponent) and one InstancedStaticMesh child per group of the prefab's packed form, loaded
        // by ComponentRegistry exactly as an authored ISM is. @p rootId as Instantiate's. Refuses, with the
        // packer's reason, a prefab whose form is Refused - the caller places it unpacked and says so.
        [[nodiscard]] static Common::ResultStr<ECS::Entity>
        InstantiatePacked( const Assets::Asset<Assets::PrefabAsset>& prefab, Core::Scene& scene,
                           const Assets::AssetManager& assetManager, std::optional<Common::UUID> rootId = {} );

        // Once per frame, at the top of Scene::OnUpdate: every packed instance whose prefab's form changed (the
        // source was committed, reloaded or re-captured: a new checksum) is rebuilt from the new form, and every
        // one whose root's world moved has its instances composed again. A form that can no longer be packed is
        // named on the log once per checksum and the instance keeps the groups it has.
        static void RefreshPackedInstances( Core::Scene& scene );
    };
} // namespace Desert::Runtime::Factory
