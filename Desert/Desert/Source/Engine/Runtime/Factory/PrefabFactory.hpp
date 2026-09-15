#pragma once

#include <Engine/ECS/Entity.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>

#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Runtime::Factory
{

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
    };
} // namespace Desert::Runtime::Factory
