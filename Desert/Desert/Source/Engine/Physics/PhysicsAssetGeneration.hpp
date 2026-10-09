#pragma once

#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/Physics/PhysicsAssetFormat.hpp>

#include <Common/Core/ResultStr.hpp>

#include <span>

namespace Desert::Animation
{
    class Skeleton;
}

namespace Desert::Physics
{
    /**
     * @brief "Create Physics Asset" from a skeletal mesh — UE FPhysicsAssetUtils::CreateFromSkeletalMesh with
     * FPhysAssetCreateParams { GeomType = EFG_Sphyl, VertWeight = EVW_DominantWeight, bCreateConstraints,
     * AngularConstraintMode = ACM_Limited }.
     *
     * WHICH BONES GET A BODY. Every vertex belongs to the bone with its heaviest weight (dominant weight). Bones
     * are visited children first; a bone whose vertices span less than MinBoneSizeCm along their longest axis
     * gets no body and hands its vertices to its parent (UE merges a too-small bone into its parent), so a
     * finger chain too small to simulate thickens the hand instead of vanishing. A bone with no vertices gets
     * no body.
     *
     * THE CAPSULE RUNS ALONG THE BONE. Its axis is the direction from the bone to the first child bone that
     * carries vertices or a body below it (a leaf uses the direction to its vertices' centroid). The vertices,
     * taken into that axis' frame, give the cylinder's extent along it and the radius around it (the largest
     * distance from the axis, so every vertex is inside); Length = extent - 2 * radius, never negative.
     *
     * FRAMES ARE THE RUNTIME'S. Body Center / Rotation are in the bone's bind frame with its scale removed —
     * the frame BuildRagdollDesc puts the body in — and the capsule's local +Z is the axis. Vertices are in
     * the mesh's component space, which is the skeleton's bind component space.
     *
     * JOINTS. One per body whose nearest ancestor bone with a body exists (UE: the constraint joins a body to
     * its nearest bodied ancestor; a root body stays unjointed). The joint sits at the child bone's origin, its
     * twist axis (+X of the frame) along the child's capsule axis, every limit DefaultLimitDegrees.
     */
    struct PhysicsAssetGenerationSettings
    {
        float MinBoneSizeCm       = 5.0f;  ///< UE FPhysAssetCreateParams::MinBoneSize (bones under it merge up)
        float DefaultLimitDegrees = 45.0f; ///< swing1, swing2 and twist of every generated joint (UE ACM_Limited)
    };

    [[nodiscard]] Common::ResultStr<PhysicsAssetData>
    GeneratePhysicsAsset( const Animation::Skeleton& skeleton, std::span<const SkinnedVertex> vertices,
                          const Common::Content::AssetGuid&     skeletonGuid,
                          const PhysicsAssetGenerationSettings& settings = {} );
} // namespace Desert::Physics
