#include "ProceduralCharacterFactory.hpp"
#include "ProceduralCharacterSkeleton.hpp"

#include <Engine/Animation/ProceduralCharacterAnimations.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/Serialization/AnimationClipWrite.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Units.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <format>
#include <limits>
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
        void AppendCylinder( std::vector<Assets::Serialization::SkinnedVertexData>& verts,
                             std::vector<Assets::Serialization::IndexData>& indices, const glm::vec3& p0,
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
                    Assets::Serialization::SkinnedVertexData v{};
                    v.Position               = c + r * dir;
                    v.Normal                 = glm::normalize( dir );
                    v.Tangent                = axis;
                    v.Bitangent              = glm::cross( v.Normal, axis );
                    v.TexCoord               = { static_cast<float>( s ) / static_cast<float>( radial ),
                                                 static_cast<float>( ring ) };
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
        void AppendSphere( std::vector<Assets::Serialization::SkinnedVertexData>& verts,
                           std::vector<Assets::Serialization::IndexData>& indices, const glm::vec3& center,
                           float radius, uint32_t bone, int stacks = 10, int slices = 14 )
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
                    Assets::Serialization::SkinnedVertexData v{};
                    v.Position  = center + radius * n;
                    v.Normal    = n;
                    v.Tangent   = glm::vec3( -std::sin( phi ), 0.0f, std::cos( phi ) );
                    v.Bitangent = glm::cross( n, v.Tangent );
                    v.TexCoord               = { static_cast<float>( j ) / static_cast<float>( slices ),
                                                 static_cast<float>( i ) / static_cast<float>( stacks ) };
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
    } // namespace

    std::filesystem::path HumanoidMeshFile()
    {
        return HumanoidSkeletonFile().parent_path() / "Humanoid.skmesh";
    }

    std::filesystem::path HumanoidClipFile( std::string_view clipName )
    {
        return HumanoidSkeletonFile().parent_path() / std::format( "Humanoid_{}.anim", clipName );
    }

    Common::ResultStr<Assets::Serialization::MeshAssetData>
    ProceduralCharacterFactory::BuildHumanoidMesh( const Animation::Skeleton&        skeleton,
                                                   const Common::Content::AssetGuid& skeletonGuid )
    {
        using Result = Assets::Serialization::MeshAssetData;
        if ( skeletonGuid.IsNull() )
            return Common::MakeError<Result>( "the humanoid mesh states no skeleton: the skeleton GUID is null" );

        // The body is laid over the rig's COMPONENT-SPACE BIND: the file's LocalBindTransform chain, the same
        // one its OffsetMatrix inverts. Positions are in world units (cm), as the file is.
        const auto&            bones = skeleton.GetBones();
        std::vector<glm::mat4> bind;
        skeleton.ResolveComponentSpace( [&]( uint32_t i ) { return bones[i].LocalBindTransform; }, bind );

        // Every bone the body names must be in the rig; one that is not is refused by name, and no mesh is
        // written rather than a limb skinned to a wrong bone.
        std::string missing;
        const auto  BoneOf = [&]( const char* name ) -> uint32_t
        {
            if ( const auto index = skeleton.FindBoneIndex( name ) )
                return *index;
            missing = name;
            return 0;
        };
        const auto PositionOf = [&]( uint32_t bone ) { return glm::vec3( bind[bone][3] ); };

        Result data;
        data.IsSkinned = true;
        data.Guid      = kHumanoidMeshGuid;
        data.Skeleton  = skeletonGuid;
        auto& verts    = data.SkinnedVertices;
        auto& indices  = data.Indices;

        constexpr float M = Common::Units::UnitsPerMetre; // radii are authored in metres
        for ( const HumanoidSegment& seg : HumanoidSegments() )
        {
            const uint32_t a    = BoneOf( seg.BoneA );
            const uint32_t b    = BoneOf( seg.BoneB );
            const uint32_t skin = BoneOf( seg.SkinBone );
            AppendCylinder( verts, indices, PositionOf( a ), PositionOf( b ), seg.RadiusA * M, seg.RadiusB * M,
                            skin );
        }
        for ( const HumanoidSphere& sph : HumanoidSpheres() )
        {
            const uint32_t bone = BoneOf( sph.Bone );
            AppendSphere( verts, indices, PositionOf( bone ), sph.Radius * M, bone );
        }
        // Feet: a short capsule from each ankle forward (+Z), skinned to the foot bone.
        for ( const char* footName : HumanoidFeet() )
        {
            const uint32_t foot = BoneOf( footName );
            AppendCylinder( verts, indices, PositionOf( foot ),
                            PositionOf( foot ) + glm::vec3( 0.0f, -0.02f, 0.17f ) * M, 0.055f * M, 0.045f * M,
                            foot );
        }
        if ( !missing.empty() )
            return Common::MakeFormattedError<Result>(
                 "the humanoid's skeleton '{}' has no bone '{}' the body names",
                 HumanoidSkeletonFile().generic_string(), missing );

        // One submesh covering the whole body, bounded by its own vertices.
        Assets::Serialization::SubmeshData sub{};
        sub.Name         = "Humanoid";
        sub.VertexOffset = 0;
        sub.VertexCount  = static_cast<uint32_t>( verts.size() );
        sub.IndexOffset  = 0;
        sub.IndexCount   = static_cast<uint32_t>( indices.size() * 3 );
        sub.Transform    = glm::mat4( 1.0f );
        glm::vec3 lo( std::numeric_limits<float>::max() );
        glm::vec3 hi( std::numeric_limits<float>::lowest() );
        for ( const auto& v : verts )
        {
            lo = glm::min( lo, v.Position );
            hi = glm::max( hi, v.Position );
        }
        sub.BoundingBox = { lo, hi };
        data.Submeshes.push_back( std::move( sub ) );
        return Common::MakeSuccess( std::move( data ) );
    }

    Common::ResultStr<std::vector<std::filesystem::path>> ProceduralCharacterFactory::WriteEngineAssets()
    {
        using Result = std::vector<std::filesystem::path>;

        // The rig and its identity come from the skeleton FILE: its bones, and the GUID its header states.
        const std::filesystem::path skeletonFile = HumanoidSkeletonFile();
        auto                        read         = Assets::Serialization::ReadSkeletonFile( skeletonFile );
        if ( !read )
            return Common::MakeFormattedError<Result>( "the humanoid's skeleton does not read: {}",
                                                       read.GetError() );
        auto                       rigData = read.ExtractValue();
        Common::Content::AssetGuid skeletonGuid;
        if ( rigData.Header )
            if ( const auto stated = Common::Content::AssetGuidFromText( rigData.Header->Guid ); stated )
                skeletonGuid = stated.GetValue();
        if ( skeletonGuid.IsNull() )
            return Common::MakeFormattedError<Result>( "the humanoid's skeleton '{}' states no GUID in its header",
                                                       skeletonFile.generic_string() );
        const Animation::Skeleton skeleton( std::move( rigData.Bones ) );

        Result written;

        auto mesh = BuildHumanoidMesh( skeleton, skeletonGuid );
        if ( !mesh )
            return Common::MakeError<Result>( mesh.GetError() );
        const std::string bytes = Assets::Serialization::EncodeMeshBinary( mesh.GetValue() );
        if ( const auto ok = Common::Utils::FileSystem::WriteContentToFileAtomic( HumanoidMeshFile(), bytes );
             !ok )
            return Common::MakeFormattedError<Result>( "'{}' was not written: {}",
                                                       HumanoidMeshFile().generic_string(), ok.GetError() );
        written.push_back( HumanoidMeshFile() );

        for ( const HumanoidClipAsset& clipAsset : kHumanoidClips )
        {
            auto clip = Animation::ProceduralCharacterAnimations::Build( skeleton, clipAsset.Name );
            if ( !clip )
                return Common::MakeError<Result>( clip.GetError() );
            Animation::AnimationClip bound   = clip.ExtractValue();
            bound.Skeleton                   = skeletonGuid;
            const std::filesystem::path file = HumanoidClipFile( clipAsset.Name );
            if ( const auto ok = Assets::Serialization::SaveClipToFile( file, bound, clipAsset.Guid ); !ok )
                return Common::MakeError<Result>( ok.GetError() );
            written.push_back( file );
        }
        return Common::MakeSuccess( std::move( written ) );
    }
} // namespace Desert::Geometry
