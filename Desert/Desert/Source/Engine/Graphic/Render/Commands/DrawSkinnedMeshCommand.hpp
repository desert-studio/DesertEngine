#pragma once

#include "../RenderCommand.hpp"
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/AnimationUpdateRate.hpp>

#include <Engine/Graphic/Materials/MaterialInstance.hpp>

namespace Desert
{
    class Mesh;
    class SkinnedMesh;
} // namespace Desert

namespace Desert::Graphic::Render
{
    struct DrawSkinnedMeshCommand : RenderCommand
    {
        // The entity that owns the draw (entt id incl. version): GpuSceneRegistry keys this draw's
        // GpuScene primitive by it (MotionKey), so the velocity of a moving object is its own.
        uint32_t             Entity;
        Desert::SkinnedMesh* Mesh;
        // CO-OWNED, not a copy of bare pointers: the copy this used to make kept the ARRAY safe and left
        // every MaterialInstance in it owned by an ECS component that Lua can destroy before this command
        // runs. See Graphic::MaterialSlotBinding (A8-3).
        Graphic::MaterialSlotBindingPtr MaterialSlot;
        glm::mat4                       Transform;
        // CO-OWNED, never copied per frame: the pooled buffer MeshECSSystem refills only on a new pose.
        std::shared_ptr<const std::vector<glm::mat4>> BoneMatrices;
        // The entity's MorphComponent weights and revision; empty weights = no morph.
        std::vector<float>              MorphWeights;
        uint64_t                        MorphRevision = 0;
        // The animated instance's LOD / update-rate feedback (null: not animated).
        std::shared_ptr<Animation::SkinnedLodFeedback> LodFeedback;
        bool                            Outlined    = false;
        bool                            CastShadows = true;
        // SkinnedMeshComponent::ReceivesDecals (UE bReceivesDecals; off by default on a skinned mesh).
        bool                            ReceivesDecals = false;

        DrawSkinnedMeshCommand( uint32_t entity, Desert::SkinnedMesh* mesh,
                                Graphic::MaterialSlotBindingPtr materialSlot, const glm::mat4& transform,
                                std::shared_ptr<const std::vector<glm::mat4>> bones,
                                std::vector<float>                            morphWeights,
                                uint64_t morphRevision, bool outlined, bool castShadows,
                                std::shared_ptr<Animation::SkinnedLodFeedback> lodFeedback = nullptr,
                                bool receivesDecals = false )
             : Entity( entity ), Mesh( mesh ), MaterialSlot( std::move( materialSlot ) ), Transform( transform ),
               BoneMatrices( std::move( bones ) ), MorphWeights( std::move( morphWeights ) ), MorphRevision( morphRevision ),
               LodFeedback( std::move( lodFeedback ) ), Outlined( outlined ), CastShadows( castShadows ),
               ReceivesDecals( receivesDecals )
        {
        }

        void Prepare() override;

        void Execute( SceneRenderer& renderer ) override;
    };
} // namespace Desert::Graphic::Render