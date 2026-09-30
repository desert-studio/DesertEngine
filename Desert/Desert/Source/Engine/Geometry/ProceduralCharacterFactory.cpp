#include "ProceduralCharacterFactory.hpp"
#include "ProceduralCharacterSkeleton.hpp"

#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Units.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <memory>
#include <vector>

namespace Desert::Geometry
{
    namespace
    {
        // ---- Smooth body: TAPERED CAPSULES (limb/torso segments) + SPHERE joints, each rigid-skinned 100% to
        // one bone, laid over the bind pose of the rig the ASSET states (ProceduralCharacterSkeleton). ----
        constexpr float kPi = 3.14159265358979323846f;

        // A tapered cylinder (side surface) p0(radius r0) -> p1(radius r1), rigid-skinned to `bone`. Normals
        // point radially outward; triangles are wound CCW-outward (matches the mesh convention).
        void AppendCylinder( std::vector<SkinnedVertex>& verts, std::vector<Index>& indices, const glm::vec3& p0,
                             const glm::vec3& p1, float r0, float r1, uint32_t bone, int radial = 12 )
        {
            glm::vec3   axis = p1 - p0;
            const float len  = glm::length( axis );
            if ( len < 1e-5f )
                return;
            axis /= len;

            // Right-handed perpendicular basis (cross(u, w) == axis).
            const glm::vec3 up = ( std::abs( axis.y ) < 0.99f ) ? glm::vec3( 0, 1, 0 ) : glm::vec3( 1, 0, 0 );
            const glm::vec3 u  = glm::normalize( glm::cross( up, axis ) );
            const glm::vec3 w  = glm::cross( axis, u );

            const uint32_t base   = static_cast<uint32_t>( verts.size() );
            const int      stride = radial + 1;
            for ( int ring = 0; ring < 2; ++ring )
            {
                const glm::vec3 c = ( ring == 0 ) ? p0 : p1;
                const float     r = ( ring == 0 ) ? r0 : r1;
                for ( int s = 0; s <= radial; ++s )
                {
                    const float     a   = static_cast<float>( s ) / radial * 2.0f * kPi;
                    const glm::vec3 dir = std::cos( a ) * u + std::sin( a ) * w;
                    SkinnedVertex   v{};
                    v.StaticVertex.Position  = c + r * dir;
                    v.StaticVertex.Normal    = glm::normalize( dir );
                    v.StaticVertex.Tangent   = axis;
                    v.StaticVertex.Bitangent = glm::cross( v.StaticVertex.Normal, axis );
                    v.StaticVertex.TexCoord  = { static_cast<float>( s ) / radial, static_cast<float>( ring ) };
                    v.BoneIDs[0]             = bone;
                    v.BoneWeights[0]         = 1.0f;
                    verts.push_back( v );
                }
            }

            for ( int s = 0; s < radial; ++s )
            {
                const uint32_t a = base + s, b = base + s + 1;
                const uint32_t c = base + stride + s, d = base + stride + s + 1;
                indices.push_back( { a, d, c } ); // outward
                indices.push_back( { a, b, d } ); // outward
            }
        }

        // A UV sphere centered at `center`, rigid-skinned to `bone`. Outward normals + CCW-outward winding.
        void AppendSphere( std::vector<SkinnedVertex>& verts, std::vector<Index>& indices,
                           const glm::vec3& center, float radius, uint32_t bone, int stacks = 10, int slices = 14 )
        {
            const uint32_t base   = static_cast<uint32_t>( verts.size() );
            const int      stride = slices + 1;
            for ( int i = 0; i <= stacks; ++i )
            {
                const float theta = kPi * static_cast<float>( i ) / stacks; // 0 = +Y pole
                for ( int j = 0; j <= slices; ++j )
                {
                    const float     phi = 2.0f * kPi * static_cast<float>( j ) / slices;
                    const glm::vec3 n( std::sin( theta ) * std::cos( phi ), std::cos( theta ),
                                       std::sin( theta ) * std::sin( phi ) );
                    SkinnedVertex v{};
                    v.StaticVertex.Position  = center + radius * n;
                    v.StaticVertex.Normal    = n;
                    v.StaticVertex.Tangent   = glm::vec3( -std::sin( phi ), 0.0f, std::cos( phi ) );
                    v.StaticVertex.Bitangent = glm::cross( n, v.StaticVertex.Tangent );
                    v.StaticVertex.TexCoord  = { static_cast<float>( j ) / slices,
                                                 static_cast<float>( i ) / stacks };
                    v.BoneIDs[0]             = bone;
                    v.BoneWeights[0]         = 1.0f;
                    verts.push_back( v );
                }
            }

            for ( int i = 0; i < stacks; ++i )
                for ( int j = 0; j < slices; ++j )
                {
                    const uint32_t a = base + i * stride + j, b = base + i * stride + j + 1;
                    const uint32_t c = base + ( i + 1 ) * stride + j, d = base + ( i + 1 ) * stride + j + 1;
                    indices.push_back( { a, b, d } ); // outward
                    indices.push_back( { a, d, c } ); // outward
                }
        }

        // Owns the humanoid skeleton for the process lifetime (SkinnedMesh only holds a const Skeleton*).
        std::unique_ptr<Animation::Skeleton> s_Skeleton;
        // NOT AN OPTIONAL, AND IT NEVER MEANT ONE. `RegisterProcedural` returns a handle, not a maybe, so
        // after BuildOnce() there is no state in which this is absent — the optional was doing duty as the
        // "already built" flag and every reader paid for it with an unguarded dereference.
        Assets::AssetHandle                  s_Handle{ static_cast<uint64_t>( 0 ) };
        bool                                 s_Built = false;

        void BuildOnce()
        {
            if ( s_Built )
            {
                return;
            }

            // Whatever happens below, this runs once: a rig that does not read is reported once, by name.
            s_Built = true;
            s_Skeleton = LoadHumanoidSkeleton();
            if ( !s_Skeleton )
                return;

            // The body is laid over the rig's COMPONENT-SPACE BIND: the file's LocalBindTransform chain, the
            // same one its OffsetMatrix inverts. Positions are in world units (cm), as the file is.
            const auto& bones = s_Skeleton->GetBones();
            std::vector<glm::mat4> bind;
            s_Skeleton->ResolveComponentSpace( [&]( uint32_t i ) { return bones[i].LocalBindTransform; }, bind );

            // Every bone the body names must be in the rig; one that is not is refused by name, and the
            // humanoid then has no mesh rather than a limb skinned to a wrong bone.
            bool                  complete = true;
            const auto            BoneOf   = [&]( const char* name ) -> uint32_t
            {
                if ( const auto index = s_Skeleton->FindBoneIndex( name ) )
                    return *index;
                LOG_ERROR( "[Geometry] the built-in humanoid's skeleton '{}' has no bone '{}'; the procedural "
                           "humanoid has no mesh.",
                           HumanoidSkeletonFile().generic_string(), name );
                complete = false;
                return 0;
            };
            const auto PositionOf = [&]( uint32_t bone ) { return glm::vec3( bind[bone][3] ); };

            std::vector<SkinnedVertex> verts;
            std::vector<Index>         indices;
            constexpr float            M = Common::Units::UnitsPerMetre; // radii are authored in metres
            for ( const HumanoidSegment& seg : HumanoidSegments() )
            {
                const uint32_t a = BoneOf( seg.BoneA ), b = BoneOf( seg.BoneB ), skin = BoneOf( seg.SkinBone );
                if ( complete )
                    AppendCylinder( verts, indices, PositionOf( a ), PositionOf( b ), seg.RadiusA * M,
                                    seg.RadiusB * M, skin );
            }
            for ( const HumanoidSphere& sph : HumanoidSpheres() )
            {
                const uint32_t bone = BoneOf( sph.Bone );
                if ( complete )
                    AppendSphere( verts, indices, PositionOf( bone ), sph.Radius * M, bone );
            }
            // Feet: a short capsule from each ankle forward (+Z), skinned to the foot bone.
            for ( const char* footName : HumanoidFeet() )
            {
                const uint32_t foot = BoneOf( footName );
                if ( complete )
                    AppendCylinder( verts, indices, PositionOf( foot ),
                                    PositionOf( foot ) + glm::vec3( 0.0f, -0.02f, 0.17f ) * M, 0.055f * M,
                                    0.045f * M, foot );
            }
            if ( !complete )
            {
                s_Skeleton.reset();
                return;
            }

            // Single submesh covering the whole body.
            Submesh sub{};
            sub.Name        = "Humanoid";
            sub.VertexOffset = 0;
            sub.VertexCount  = static_cast<uint32_t>( verts.size() );
            sub.IndexOffset  = 0;
            sub.IndexCount   = static_cast<uint32_t>( indices.size() * 3 );
            sub.Transform    = glm::mat4( 1.0f );
            sub.BoundingBox  = { glm::vec3( -30.0f, 0.0f, -20.0f ), glm::vec3( 30.0f, 190.0f, 20.0f ) };

            auto mesh = std::make_shared<SkinnedMesh>( verts, indices, std::vector<Submesh>{ sub },
                                                       s_Skeleton.get(), std::vector<MeshVertexStreams>{} );
            s_Handle = Runtime::ResourceRegistry::GetMeshService()->RegisterProcedural( mesh );
        }
    } // namespace

    Assets::AssetHandle ProceduralCharacterFactory::GetHumanoidMesh()
    {
        BuildOnce();
        return s_Handle;
    }

    const Animation::Skeleton* ProceduralCharacterFactory::GetHumanoidSkeleton()
    {
        BuildOnce();
        return s_Skeleton.get();
    }
} // namespace Desert::Geometry
