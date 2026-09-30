#include "LayeredBlendPerBone.hpp"

#include <Engine/Animation/Skeleton.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <format>
#include <optional>

// The Layered Blend Per Bone node's two pure halves: the per-bone table (built when the node or the skeleton
// changes) and the per-frame blend. No state, no clock: the caller owns both — an Animator's stage today,
// the pose graph's own evaluation when it evaluates poses.
namespace Desert::Animation::Graph
{
    namespace
    {
        /// Per bone: how many levels below `filterBone` it sits, or -1 when it is not under it at all.
        /// One pass in resolve order, parent before child, so a bone's parent is final when it is reached.
        std::vector<int32_t> DepthsBelow( const Skeleton& skeleton, uint32_t filterBone )
        {
            std::vector<int32_t> depth( skeleton.GetBones().size(), -1 );
            depth[filterBone] = 0;
            for ( const uint32_t bone : skeleton.GetResolveOrder() )
            {
                const uint32_t parent = skeleton.ResolveParent( bone );
                if ( bone != filterBone && parent != Skeleton::NO_PARENT && depth[parent] >= 0 )
                    depth[bone] = depth[parent] + 1;
            }
            return depth;
        }

        /// The weight a filter of `blendDepth` gives a bone `levels` below its bone (the header's table).
        float RampWeight( int32_t blendDepth, int32_t levels )
        {
            if ( blendDepth <= 0 )
                return 1.0F;
            return std::min( 1.0F, static_cast<float>( levels + 1 ) / static_cast<float>( blendDepth ) );
        }

        /// The curve `name` in `pose`, or nothing.
        std::optional<float> CurveOf( const GraphPose& pose, const std::string& name )
        {
            for ( size_t i = 0; i < pose.CurveNames.size() && i < pose.CurveValues.size(); ++i )
                if ( pose.CurveNames[i] == name )
                    return pose.CurveValues[i];
            return std::nullopt;
        }

        /// Mesh-space rotation and scale of every bone of `pose` (rotation and scale compose down the
        /// chain independently of translation, which a rotation/scale blend never reads).
        void MeshRotationScale( const Skeleton& skeleton, const LocalPose& pose, std::vector<glm::quat>& rotation,
                                std::vector<glm::vec3>& scale )
        {
            rotation.assign( pose.Size(), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ) );
            scale.assign( pose.Size(), glm::vec3( 1.0F ) );
            for ( const uint32_t bone : skeleton.GetResolveOrder() )
            {
                const uint32_t parent = skeleton.ResolveParent( bone );
                if ( parent == Skeleton::NO_PARENT )
                {
                    rotation[bone] = pose[bone].Rotation;
                    scale[bone]    = pose[bone].Scale;
                    continue;
                }
                rotation[bone] = glm::normalize( rotation[parent] * pose[bone].Rotation );
                scale[bone]    = scale[parent] * pose[bone].Scale;
            }
        }

        void BlendCurves( CurveBlendOption option, const GraphPose& base, std::span<const GraphPose> layers,
                          std::span<const float> layerWeights, GraphPose& out )
        {
            // The sources in order, base first at weight 1; a layer at weight 0 is absent (the short circuit).
            struct Source
            {
                const GraphPose* Pose;
                float            Weight;
            };
            std::vector<Source> sources{ Source{ &base, 1.0F } };
            if ( option != CurveBlendOption::UseBasePose )
                for ( size_t i = 0; i < layers.size(); ++i )
                    if ( const float w = std::clamp( layerWeights[i], 0.0F, 1.0F ); w > 0.0F )
                        sources.push_back( Source{ &layers[i], w } );

            std::vector<std::string> names;
            for ( const Source& source : sources )
                for ( const std::string& name : source.Pose->CurveNames )
                    if ( std::find( names.begin(), names.end(), name ) == names.end() )
                        names.push_back( name );

            out.CurveNames.clear();
            out.CurveValues.clear();
            for ( const std::string& name : names )
            {
                std::optional<float> value;
                float                sum = 0.0F, weightSum = 0.0F;
                for ( const Source& source : sources )
                {
                    const auto v = CurveOf( *source.Pose, name );
                    if ( !v )
                        continue;
                    sum += *v * source.Weight;
                    weightSum += source.Weight;
                    switch ( option )
                    {
                        case CurveBlendOption::Override:
                        case CurveBlendOption::UseBasePose:
                            value = *v;
                            break;
                        case CurveBlendOption::DoNotOverride:
                            value = value ? *value : *v;
                            break;
                        case CurveBlendOption::UseMaxValue:
                            value = value ? std::max( *value, *v ) : *v;
                            break;
                        case CurveBlendOption::UseMinValue:
                            value = value ? std::min( *value, *v ) : *v;
                            break;
                        case CurveBlendOption::NormalizeByWeight:
                        case CurveBlendOption::BlendByWeight:
                            break;
                    }
                }
                if ( option == CurveBlendOption::BlendByWeight )
                    value = sum;
                else if ( option == CurveBlendOption::NormalizeByWeight )
                    value = weightSum > 0.0F ? sum / weightSum : 0.0F;
                out.CurveNames.push_back( name );
                out.CurveValues.push_back( value.value_or( 0.0F ) );
            }
        }
    } // namespace

    Common::ResultStr<std::vector<PerBoneBlendWeight>> BuildPerBoneWeights( const LayeredBlendPerBoneNode& node,
                                                                            const Skeleton& skeleton )
    {
        using Table            = std::vector<PerBoneBlendWeight>;
        const size_t boneCount = skeleton.GetBones().size();
        Table        table( boneCount );

        for ( size_t layer = 0; layer < node.Layers.size(); ++layer )
        {
            // This layer's reach alone first: includes take the widest weight a bone gets from any of them,
            // then exclusions cut their branches out — whatever order the filters were listed in.
            std::vector<float> reach( boneCount, -1.0F );
            std::vector<bool>  excluded( boneCount, false );
            for ( const BranchFilter& filter : node.Layers[layer].Filters )
            {
                const auto bone = skeleton.FindBoneIndex( filter.BoneName );
                if ( !bone )
                    return Common::MakeError<Table>(
                         std::format( "Layered Blend Per Bone: layer {} filters on bone '{}', which the skeleton "
                                      "does not have; the filter would be a layer that blends nothing",
                                      layer, filter.BoneName ) );

                const std::vector<int32_t> depth = DepthsBelow( skeleton, *bone );
                for ( size_t b = 0; b < boneCount; ++b )
                {
                    if ( depth[b] < 0 )
                        continue;
                    if ( filter.BlendDepth < 0 )
                        excluded[b] = true;
                    else
                        reach[b] = std::max( reach[b], RampWeight( filter.BlendDepth, depth[b] ) );
                }
            }

            // The LATER layer wins a bone both reach (UE's per-bone SourceIndex).
            for ( size_t b = 0; b < boneCount; ++b )
                if ( reach[b] >= 0.0F && !excluded[b] )
                    table[b] = PerBoneBlendWeight{ static_cast<int32_t>( layer ), reach[b] };
        }
        return Common::MakeSuccess( std::move( table ) );
    }

    Common::BoolResultStr BlendLayeredPerBone( const LayeredBlendPerBoneNode&      node,
                                               std::span<const PerBoneBlendWeight> weights,
                                               const Skeleton& skeleton, const GraphPose& base,
                                               std::span<const GraphPose> layers,
                                               std::span<const float> layerWeights, GraphPose& out )
    {
        const size_t boneCount = skeleton.GetBones().size();
        if ( layers.size() != node.Layers.size() || layerWeights.size() != node.Layers.size() )
            return Common::MakeError<bool>(
                 std::format( "Layered Blend Per Bone has {} layer(s) and was given {} pose(s) and {} weight(s)",
                              node.Layers.size(), layers.size(), layerWeights.size() ) );
        if ( weights.size() != boneCount || base.Pose.Size() != boneCount )
            return Common::MakeError<bool>(
                 std::format( "Layered Blend Per Bone: the skeleton has {} bone(s), the per-bone table {} and the "
                              "base pose {}; the table is built for another skeleton",
                              boneCount, weights.size(), base.Pose.Size() ) );
        for ( size_t i = 0; i < layers.size(); ++i )
            if ( layers[i].Pose.Size() != boneCount )
                return Common::MakeError<bool>(
                     std::format( "Layered Blend Per Bone: layer {}'s pose has {} bone(s), the skeleton {}", i,
                                  layers[i].Pose.Size(), boneCount ) );

        // The effective alpha of bone b: its mask weight times its layer's weight; 0 leaves the base as is.
        const auto alphaOf = [&]( size_t b ) -> float
        {
            const int32_t layer = weights[b].Layer;
            if ( layer < 0 )
                return 0.0F;
            return weights[b].Weight * std::clamp( layerWeights[static_cast<size_t>( layer )], 0.0F, 1.0F );
        };

        out.Pose = base.Pose;

        const bool                          meshSpace = node.MeshSpaceRotationBlend || node.MeshSpaceScaleBlend;
        std::vector<glm::quat>              baseRot, outRot;
        std::vector<glm::vec3>              baseScale, outScale;
        std::vector<std::vector<glm::quat>> layerRot( layers.size() );
        std::vector<std::vector<glm::vec3>> layerScale( layers.size() );
        if ( meshSpace )
        {
            MeshRotationScale( skeleton, base.Pose, baseRot, baseScale );
            for ( size_t i = 0; i < layers.size(); ++i )
                if ( layerWeights[i] > 0.0F )
                    MeshRotationScale( skeleton, layers[i].Pose, layerRot[i], layerScale[i] );
            outRot.assign( boneCount, glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ) );
            outScale.assign( boneCount, glm::vec3( 1.0F ) );
        }

        // Resolve order, so a mesh-space bone reads its parent's OUTPUT rotation: the layer says where the
        // bone points in the mesh, and the local rotation is whatever gets it there under the blended parent.
        for ( const uint32_t b : skeleton.GetResolveOrder() )
        {
            const float    alpha  = alphaOf( b );
            const uint32_t parent = skeleton.ResolveParent( b );
            if ( alpha > 0.0F )
            {
                const auto           layer      = static_cast<size_t>( weights[b].Layer );
                const BoneTransform& layerLocal = layers[layer].Pose[b];
                BoneTransform        blended    = Blend( base.Pose[b], layerLocal, alpha );
                // Mesh space is only ever read when a mesh-space flag is set, and then outRot/outScale exist.
                const bool      root      = parent == Skeleton::NO_PARENT || !meshSpace;
                const glm::quat parentRot = root ? glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ) : outRot[parent];
                const glm::vec3 parentScl = root ? glm::vec3( 1.0F ) : outScale[parent];
                if ( node.MeshSpaceRotationBlend )
                {
                    const glm::quat mesh = glm::slerp( baseRot[b], layerRot[layer][b], alpha );
                    blended.Rotation     = glm::normalize( glm::inverse( parentRot ) * mesh );
                }
                if ( node.MeshSpaceScaleBlend )
                {
                    const glm::vec3 mesh = glm::mix( baseScale[b], layerScale[layer][b], alpha );
                    blended.Scale        = mesh / glm::max( glm::abs( parentScl ), glm::vec3( 1e-6F ) );
                }
                out.Pose[b] = blended;
            }
            if ( meshSpace )
            {
                const bool root = parent == Skeleton::NO_PARENT;
                outRot[b] = root ? out.Pose[b].Rotation : glm::normalize( outRot[parent] * out.Pose[b].Rotation );
                outScale[b] = root ? out.Pose[b].Scale : outScale[parent] * out.Pose[b].Scale;
            }
        }

        BlendCurves( node.CurveBlend, base, layers, layerWeights, out );

        // Root motion: by the layer's weight ON THE ROOT BONE, or by its layer weight alone.
        out.RootMotion = base.RootMotion;
        if ( node.BlendRootMotionBasedOnRootBone )
        {
            const auto& order = skeleton.GetResolveOrder();
            if ( !order.empty() )
                if ( const float alpha = alphaOf( order.front() ); alpha > 0.0F )
                    out.RootMotion =
                         Blend( base.RootMotion,
                                layers[static_cast<size_t>( weights[order.front()].Layer )].RootMotion, alpha );
        }
        else
        {
            for ( size_t i = 0; i < layers.size(); ++i )
                if ( const float w = std::clamp( layerWeights[i], 0.0F, 1.0F ); w > 0.0F )
                    out.RootMotion = Blend( out.RootMotion, layers[i].RootMotion, w );
        }
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Animation::Graph
