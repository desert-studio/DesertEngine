#pragma once

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Desert::Editor::Commands
{
    /**
     * @brief WHY A FOLD EXISTS AT ALL, AND WHY ITS DECISION IS A FREE FUNCTION.
     *
     * Measured on the world-scale scene (`Tools/WorldGen`, 50 179 entities): the ECS mesh walk costs
     * ~55 ms a frame against 22.6 ms of GPU, because it visits every entity. Auto-batching already
     * folds those 50 179 draws into ~25 — it saves DRAW CALLS, and the walk has already happened by
     * the time it runs. An InstancedStaticMeshComponent is the other saving: N repeated props become
     * ONE entity carrying N world matrices, so the walk visits one.
     *
     * The component, its serialization, its draw command and its shadow flag all existed. What did
     * not exist was any way to PRODUCE one from a selection — and nobody types five hundred matrices
     * by hand, so the instance list was a feature with no author.
     *
     * ── WHY THE DECISION IS HERE, SEPARATE FROM THE MUTATION ────────────────────────────────────
     *
     * Everything below is std + glm: no entt, no ImGui, no renderer. That is what lets a suite ask
     * the questions that actually matter — does it refuse a mixed selection, does it keep the world
     * transforms in order, does it refuse to silently drop a property an ISM cannot carry — without
     * a device. The editor half only gathers the facts and performs the swap.
     *
     * ── AND WHY IT REFUSES RATHER THAN FOLDING WHAT IT CAN ──────────────────────────────────────
     *
     * A fold DESTROYS the source entities. An ISM carries a mesh, materials, world matrices and one
     * shadow flag, and nothing else: an entity's outline, forced LOD, LOD bias, receive-shadows,
     * hidden submeshes, script, collider or children have no home in it. Folding "the ones that fit"
     * would be the middle-link defect this project keeps finding — both ends look right and something
     * in between is quietly gone. So a blocker on ANY member of the selection refuses the whole
     * operation and names the entity and the property, which is a message a person can act on.
     */

    /// What two meshes must share before one instanced draw can stand for both.
    struct FoldMeshIdentity
    {
        Common::AssetHandle                            Mesh;      ///< null when @ref Primitive is used
        std::optional<Desert::Geometry::PrimitiveType> Primitive; ///< a built-in shape instead of an asset
        std::vector<Common::AssetHandle>               Materials; ///< the material slot list, in order
        bool                                           CastShadows = true;

        [[nodiscard]] bool SameAs( const FoldMeshIdentity& other ) const;

        /// For the refusal message: "the Cube primitive", "mesh 1234...", "no mesh".
        [[nodiscard]] std::string Describe() const;
    };

    /// One selected entity, as the planner needs to see it.
    struct FoldCandidate
    {
        Common::UUID     Entity;
        std::string      Name; ///< the tag, for refusal messages
        FoldMeshIdentity Identity;
        glm::mat4        World{ 1.0f }; ///< instance transforms are WORLD-space (MeshECSSystem)

        /// Things this entity carries that an ISM cannot: "a forced LOD", "children", "a Script".
        /// Non-empty => the whole fold is refused, naming this entity and these words.
        std::vector<std::string> Blockers;
    };

    /// What the editor half then performs.
    struct FoldPlan
    {
        FoldMeshIdentity          Identity;
        std::vector<Common::UUID> Sources;            ///< to be destroyed, in selection order
        std::vector<glm::mat4>    InstanceTransforms; ///< one per source, in the SAME order
    };

    /**
     * @brief Decide whether these entities can become one instanced draw.
     *
     * Refuses (with a message naming names and counts) when: fewer than two candidates are given,
     * any candidate carries a blocker, or the candidates do not all share one mesh identity.
     */
    [[nodiscard]] Common::ResultStr<FoldPlan> PlanInstanceFold( const std::vector<FoldCandidate>& candidates );
} // namespace Desert::Editor::Commands
