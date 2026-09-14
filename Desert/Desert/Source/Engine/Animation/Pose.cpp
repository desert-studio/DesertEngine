#include "Pose.hpp"

#include <Engine/Animation/Skeleton.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/compatibility.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>

namespace Desert::Animation
{
    glm::mat4 BoneTransform::ToMatrix() const
    {
        return glm::translate( glm::mat4( 1.0F ), Translation ) * glm::toMat4( Rotation ) *
               glm::scale( glm::mat4( 1.0F ), Scale );
    }

    Common::ResultStr<BoneTransform> BoneTransform::FromMatrix( const glm::mat4& matrix )
    {
        const glm::mat3 basis( matrix );
        const float     det = glm::determinant( basis );

        // <= 0 rather than < 0: a zero determinant means at least one axis collapsed, and normalising it
        // below would divide by zero and hand back a quaternion of NaNs. Both are refusals for the same
        // reason — the matrix does not describe a translation, a rotation and a positive scale.
        if ( det <= 0.0F )
        {
            return Common::MakeFormattedError<BoneTransform>(
                 "a bone transform with determinant {} cannot be decomposed into translation/rotation/scale: "
                 "it is mirrored or degenerate, and taking the scale as the length of each basis column "
                 "(which is what this used to do) turns the mirror into a rotation that does not exist.",
                 det );
        }

        BoneTransform out;
        out.Translation = glm::vec3( matrix[3] );
        out.Scale       = glm::vec3( glm::length( basis[0] ), glm::length( basis[1] ), glm::length( basis[2] ) );

        glm::mat3 rotation;
        rotation[0]  = basis[0] / out.Scale.x;
        rotation[1]  = basis[1] / out.Scale.y;
        rotation[2]  = basis[2] / out.Scale.z;
        out.Rotation = glm::quat_cast( rotation );

        return Common::MakeSuccess( out );
    }

    BoneTransform Blend( const BoneTransform& from, const BoneTransform& to, float alpha )
    {
        BoneTransform out;
        out.Translation = glm::mix( from.Translation, to.Translation, alpha );
        out.Rotation    = glm::slerp( from.Rotation, to.Rotation, alpha );
        out.Scale       = glm::mix( from.Scale, to.Scale, alpha );
        return out;
    }

    Common::ResultStr<LocalPose> LocalPose::FromBindPose( const Skeleton& skeleton )
    {
        const auto& bones = skeleton.GetBones();
        LocalPose   pose( bones.size() );

        for ( size_t i = 0; i < bones.size(); ++i )
        {
            auto decomposed = BoneTransform::FromMatrix( bones[i].LocalBindTransform );
            if ( !decomposed.IsSuccess() )
            {
                return Common::MakeFormattedError<LocalPose>( "bind pose of bone {} ('{}'): {}", i, bones[i].Name,
                                                              decomposed.GetError() );
            }
            pose[i] = decomposed.GetValue();
        }

        return Common::MakeSuccess( std::move( pose ) );
    }

    ComponentPose::ComponentPose( const Skeleton& skeleton, const LocalPose& local )
         : m_Skeleton( skeleton ), m_Local( local ), m_Global( local.Size(), glm::mat4( 1.0F ) ),
           m_Converted( local.Size(), 0 )
    {
    }

    const glm::mat4& ComponentPose::Get( uint32_t bone )
    {
        static const glm::mat4 identity( 1.0F );
        if ( bone >= m_Global.size() )
        {
            return identity;
        }

        if ( m_Converted[bone] )
            return m_Global[bone];

        // Walk UP to the first converted ancestor collecting the chain, then come back DOWN multiplying.
        // Iterative rather than recursive on purpose: the recursive form this replaces overflowed the stack
        // on a parent cycle, and the skeleton's resolve order has already rejected cycles by treating the
        // bones in one as roots — but a bone count in the thousands would still be a deep stack for a
        // property that costs nothing to have.
        m_Chain.clear();
        uint32_t walk = bone;
        while ( true )
        {
            m_Chain.push_back( walk );
            const uint32_t parent = m_Skeleton.ResolveParent( walk );
            if ( parent == Skeleton::NO_PARENT || m_Converted[parent] )
            {
                break;
            }
            walk = parent;
        }

        for ( auto it = m_Chain.rbegin(); it != m_Chain.rend(); ++it )
        {
            const uint32_t  index       = *it;
            const uint32_t  parent      = m_Skeleton.ResolveParent( index );
            const glm::mat4 localMatrix = m_Local[index].ToMatrix();
            m_Global[index]    = parent == Skeleton::NO_PARENT ? localMatrix : m_Global[parent] * localMatrix;
            m_Converted[index] = 1;
        }

        return m_Global[bone];
    }

    void ComponentPose::ConvertAll()
    {
        const auto& order = m_Skeleton.GetResolveOrder();
        for ( const uint32_t index : order )
        {
            if ( index >= m_Global.size() )
                continue;
            if ( m_Converted[index] )
                continue;
            const uint32_t  parent      = m_Skeleton.ResolveParent( index );
            const glm::mat4 localMatrix = m_Local[index].ToMatrix();
            m_Global[index]    = parent == Skeleton::NO_PARENT ? localMatrix : m_Global[parent] * localMatrix;
            m_Converted[index] = 1;
        }
    }

    void ComponentPose::Invalidate()
    {
        std::fill( m_Converted.begin(), m_Converted.end(), static_cast<uint8_t>( 0 ) );
    }

    void ComponentPose::Reset( size_t boneCount )
    {
        m_Global.assign( boneCount, glm::mat4( 1.0F ) );
        m_Converted.assign( boneCount, 0 );
    }

    void ComponentPose::WriteSkinningMatrices( std::vector<glm::mat4>& out )
    {
        ConvertAll();

        const auto&  bones = m_Skeleton.GetBones();
        const size_t n     = m_Global.size();
        if ( out.size() != n )
            out.assign( n, glm::mat4( 1.0F ) );
        for ( size_t i = 0; i < n; ++i )
            out[i] = m_Global[i] * bones[i].OffsetMatrix;
    }
} // namespace Desert::Animation
