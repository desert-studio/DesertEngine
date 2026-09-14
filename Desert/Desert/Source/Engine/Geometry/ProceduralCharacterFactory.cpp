#include "ProceduralCharacterFactory.hpp"

#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/BoneInfo.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Common/Core/Units.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>

namespace Desert::Geometry
{
    namespace
    {
        // ---- Skeleton joints (index order is the bone array order; parents reference these indices) ----
        enum Joint : uint32_t
        {
            Hips = 0,
            Spine,
            Chest,
            Neck,
            Head,
            ShoulderL,
            ElbowL,
            HandL,
            ShoulderR,
            ElbowR,
            HandR,
            HipL,
            KneeL,
            FootL,
            HipR,
            KneeR,
            FootR,
            JointCount
        };

        struct JointDef
        {
            const char* Name;
            int         Parent;     // -1 = root
            glm::vec3   BindWorld;  // T/A-pose position in mesh space (origin at feet, +Y up, meters)
        };

        // A-pose: arms hang straight down so EVERY segment box is axis-aligned (no orientation maths needed).
        constexpr JointDef kJoints[JointCount] = {
            { "Hips", -1, { 0.00f, 0.95f, 0.0f } },     { "Spine", Hips, { 0.00f, 1.15f, 0.0f } },
            { "Chest", Spine, { 0.00f, 1.40f, 0.0f } }, { "Neck", Chest, { 0.00f, 1.55f, 0.0f } },
            { "Head", Neck, { 0.00f, 1.65f, 0.0f } },

            { "Shoulder.L", Chest, { 0.18f, 1.45f, 0.0f } },
            { "Elbow.L", ShoulderL, { 0.18f, 1.15f, 0.0f } },
            { "Hand.L", ElbowL, { 0.18f, 0.88f, 0.0f } },

            { "Shoulder.R", Chest, { -0.18f, 1.45f, 0.0f } },
            { "Elbow.R", ShoulderR, { -0.18f, 1.15f, 0.0f } },
            { "Hand.R", ElbowR, { -0.18f, 0.88f, 0.0f } },

            { "Hip.L", Hips, { 0.09f, 0.92f, 0.0f } },
            { "Knee.L", HipL, { 0.09f, 0.50f, 0.0f } },
            { "Foot.L", KneeL, { 0.09f, 0.06f, 0.0f } },

            { "Hip.R", Hips, { -0.09f, 0.92f, 0.0f } },
            { "Knee.R", HipR, { -0.09f, 0.50f, 0.0f } },
            { "Foot.R", KneeR, { -0.09f, 0.06f, 0.0f } },
        };

        // ---- Smooth body: TAPERED CAPSULES (limb/torso segments) + SPHERE joints, each rigid-skinned 100% to
        // one joint. Replaces the old box mannequin so the character reads as ROUNDED, not blocky. The SKELETON
        // is unchanged (same joints/signature), so the procedural idle/walk/run/jump clips still map onto it. ----
        constexpr float kPi = 3.14159265358979323846f;

        // A tapered cylinder between two joints (endpoints = their bind positions), rigid-skinned to SkinBone.
        struct SegmentDef
        {
            uint32_t JointA;   // start
            uint32_t JointB;   // end
            float    RadiusA;  // radius at JointA
            float    RadiusB;  // radius at JointB
            uint32_t SkinBone; // bone this segment skins to
        };

        constexpr std::array<SegmentDef, 12> kSegments = { {
            { Hips, Spine, 0.15f, 0.15f, Hips },   // lower torso
            { Spine, Chest, 0.15f, 0.18f, Chest }, // chest (widening)
            { Chest, Neck, 0.14f, 0.055f, Chest }, // upper chest -> neck
            { Neck, Head, 0.05f, 0.05f, Neck },    // neck

            { ShoulderL, ElbowL, 0.058f, 0.05f, ShoulderL }, // upper arm L
            { ElbowL, HandL, 0.048f, 0.04f, ElbowL },        // forearm L
            { ShoulderR, ElbowR, 0.058f, 0.05f, ShoulderR }, // upper arm R
            { ElbowR, HandR, 0.048f, 0.04f, ElbowR },        // forearm R

            { HipL, KneeL, 0.09f, 0.07f, HipL },    // thigh L
            { KneeL, FootL, 0.065f, 0.05f, KneeL }, // shin L
            { HipR, KneeR, 0.09f, 0.07f, HipR },    // thigh R
            { KneeR, FootR, 0.065f, 0.05f, KneeR }, // shin R
        } };

        // Spheres round off the joints (shoulders/elbows/knees/head/hips) so segments blend, not butt-join.
        struct SphereDef
        {
            uint32_t Joint;
            float    Radius;
        };

        constexpr std::array<SphereDef, 10> kJointSpheres = { {
            { Hips, 0.155f }, { Chest, 0.175f }, { Head, 0.13f }, { Neck, 0.055f },
            { ShoulderL, 0.062f }, { ElbowL, 0.05f }, { KneeL, 0.067f },
            { ShoulderR, 0.062f }, { ElbowR, 0.05f }, { KneeR, 0.067f },
        } };

        glm::vec3 JointPos( uint32_t joint )
        {
            // The rig is authored in metres (a 1.8 m humanoid reads better as a table); one world unit is a
            // centimetre, so every position — bind pose AND the segment radii below — scales up by 100.
            return kJoints[joint].BindWorld * Common::Units::UnitsPerMetre;
        }

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

        std::vector<Animation::BoneInfo> BuildBones()
        {
            std::vector<Animation::BoneInfo> bones( JointCount );
            for ( uint32_t i = 0; i < JointCount; ++i )
            {
                const JointDef& j = kJoints[i];

                // THROUGH JointPos, like the mesh: the rig is authored in metres and a world unit is a
                // centimetre, so the bind pose has to be scaled by exactly the same 100 the vertices are.
                // It used to use the raw (metre) values, which left the skeleton 100x smaller than the mesh
                // it skins. In BIND pose that cancels out — the render path multiplies chainGlobal by
                // OffsetMatrix, which is its own inverse — so a standing character looked right. As soon as
                // a CLIP played, each bone rotated about a pivot 100x closer to the origin than the vertices
                // it moves, and the body flew apart. (The bone overlay and the bone gizmo, both drawn at
                // chainGlobal, were mis-scaled for the same reason.)
                const glm::vec3 world = JointPos( i );
                const glm::vec3 parentWorld =
                     ( j.Parent >= 0 ) ? JointPos( static_cast<uint32_t>( j.Parent ) ) : glm::vec3( 0.0f );

                bones[i].Name      = j.Name;
                // No rotation in the bind pose, so the local transform is just the offset from the parent.
                bones[i].LocalBindTransform = glm::translate( glm::mat4( 1.0f ), world - parentWorld );
                bones[i].OffsetMatrix       = glm::mat4( 1.0f ); // recomputed below
                if ( j.Parent >= 0 )
                    bones[i].ParentBoneID = static_cast<uint32_t>( j.Parent );
            }
            return bones;
        }

        // Owns the humanoid skeleton for the process lifetime (SkinnedMesh only holds a const Skeleton*).
        std::unique_ptr<Animation::Skeleton> s_Skeleton;
        // NOT AN OPTIONAL, AND IT NEVER MEANT ONE. `RegisterProcedural` returns a handle, not a maybe, so
        // after BuildOnce() there is no state in which this is absent — the optional was doing duty as the
        // "already built" flag and every reader paid for it with an unguarded dereference.
        Assets::AssetHandle                  s_Handle{ static_cast<uint64_t>( 0 ) };
        bool                                 s_Built     = false;
        uint64_t                             s_Signature = 0;

        void BuildOnce()
        {
            if ( s_Built )
                return;

            s_Skeleton = std::make_unique<Animation::Skeleton>( BuildBones() );
            s_Skeleton->RecomputeOffsetMatrices(); // signature is name/parent based, so this stays valid
            s_Signature = s_Skeleton->GetSignature();

            std::vector<SkinnedVertex> verts;
            std::vector<Index>         indices;
            constexpr float            M = Common::Units::UnitsPerMetre; // radii are authored in metres too
            for ( const SegmentDef& seg : kSegments )
                AppendCylinder( verts, indices, JointPos( seg.JointA ), JointPos( seg.JointB ), seg.RadiusA * M,
                                seg.RadiusB * M, seg.SkinBone );
            for ( const SphereDef& sph : kJointSpheres )
                AppendSphere( verts, indices, JointPos( sph.Joint ), sph.Radius * M, sph.Joint );
            // Feet: a short capsule from each ankle forward (+Z), skinned to the foot joint.
            for ( uint32_t foot : { static_cast<uint32_t>( FootL ), static_cast<uint32_t>( FootR ) } )
                AppendCylinder( verts, indices, JointPos( foot ),
                                JointPos( foot ) + glm::vec3( 0.0f, -0.02f, 0.17f ) * M, 0.055f * M, 0.045f * M,
                                foot );

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
                                                       s_Skeleton.get() );
            s_Handle = Runtime::ResourceRegistry::GetMeshService()->RegisterProcedural( mesh );
            s_Built   = true;
        }
    } // namespace

    Assets::AssetHandle ProceduralCharacterFactory::GetHumanoidMesh()
    {
        BuildOnce();
        return s_Handle;
    }

    uint64_t ProceduralCharacterFactory::GetHumanoidSkeletonSignature()
    {
        BuildOnce();
        return s_Signature;
    }

    const Animation::Skeleton* ProceduralCharacterFactory::GetHumanoidSkeleton()
    {
        BuildOnce();
        return s_Skeleton.get();
    }
} // namespace Desert::Geometry
