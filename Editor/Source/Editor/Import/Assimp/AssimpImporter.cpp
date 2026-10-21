#include "AssimpImporter.hpp"
#include "../TextureImporter.hpp"
#include "SourceAlphaMode.hpp"
#include "SourceMaterialAdapter.hpp"
#include "EmbeddedSourceTexture.hpp"
#include "SourceTexturePath.hpp"
#include "VertexStreams.hpp"

#include <Engine/Assets/TextureSourceAsset.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>
#include <functional>
#include <optional>

#include <glm/gtc/quaternion.hpp>

#include <assimp/Importer.hpp>
#include <assimp/LogStream.hpp>
#include <assimp/DefaultLogger.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>
#include <Engine/Geometry/MikkTangents.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/UUID.hpp>
#include <Common/Core/Constants.hpp>

#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Animation/TrackEditing.hpp>

#include <cmath>
#include <format>

#include <Editor/Import/CookPaths.hpp>
#include <Editor/Import/ImportManager.hpp>
#include <Editor/Import/ImportUnits.hpp>
#include <Editor/Import/ImportResult.hpp>

struct aiNode;
struct aiAnimation;
struct aiNodeAnim;
struct aiScene;

namespace Assimp
{
    class Importer;
}

namespace Desert::Editor
{
    namespace
    {
        /**
         * @brief An exporter's `mTicksPerSecond` (a double) as the exact rational the time model wants.
         *
         * WHY A SEARCH AND NOT A CAST. Every rate a real exporter states is either a whole number (24, 25,
         * 30, 60, 1000 for glTF milliseconds) or one of the two broadcast rationals, and both of those are
         * IRRATIONAL as decimals: 30000/1001 is 29.97002997..., so `{ 2997, 100 }` is a different rate that
         * drifts one frame every sixteen minutes. Recognising them by name is the difference between
         * storing the rate and storing a rounding of it.
         *
         * Anything else falls back to a millionths approximation — which is reported, because a rate this
         * function had to approximate is exactly the case whose keys will not land on the project grid.
         */
        [[nodiscard]] Animation::FrameRate RationalFromRate( double rate )
        {
            if ( !( rate > 0.0 ) || !std::isfinite( rate ) )
            {
                return Animation::FrameRate{ 25, 1 };
            }

            const double rounded = std::round( rate );
            if ( std::fabs( rate - rounded ) < 1.0e-9 && rounded < 2.0e9 )
            {
                return Animation::FrameRate{ static_cast<int32_t>( rounded ), 1 };
            }

            for ( const Animation::FrameRate broadcast :
                  { Animation::FrameRate{ 30000, 1001 }, Animation::FrameRate{ 24000, 1001 },
                    Animation::FrameRate{ 60000, 1001 }, Animation::FrameRate{ 48000, 1001 } } )
            {
                if ( std::fabs( rate - broadcast.AsDouble() ) < 1.0e-6 )
                {
                    return broadcast;
                }
            }

            return Animation::FrameRate{ static_cast<int32_t>( std::llround( rate * 1000000.0 ) ), 1000000 };
        }

        /**
         * @brief The cadence a clip without a stated frame rate was authored on: the standard rate nearest to
         * the finest spacing of its distinct key times (Fox Walk: 18 keys 1/24 s apart -> 24 fps). A clip
         * with a single key time has no spacing and shows the project's own 24.
         */
        [[nodiscard]] Animation::FrameRate CadenceOfKeys( const aiAnimation& anim )
        {
            const double        ticksPerSecond = anim.mTicksPerSecond != 0.0 ? anim.mTicksPerSecond : 1.0;
            std::vector<double> seconds;
            for ( uint32_t c = 0; c < anim.mNumChannels; ++c )
            {
                const aiNodeAnim& channel = *anim.mChannels[c];
                for ( uint32_t k = 0; k < channel.mNumPositionKeys; ++k )
                    seconds.push_back( channel.mPositionKeys[k].mTime / ticksPerSecond );
                for ( uint32_t k = 0; k < channel.mNumRotationKeys; ++k )
                    seconds.push_back( channel.mRotationKeys[k].mTime / ticksPerSecond );
                for ( uint32_t k = 0; k < channel.mNumScalingKeys; ++k )
                    seconds.push_back( channel.mScalingKeys[k].mTime / ticksPerSecond );
            }
            std::sort( seconds.begin(), seconds.end() );
            double finest = 0.0;
            for ( size_t i = 1; i < seconds.size(); ++i )
            {
                const double step = seconds[i] - seconds[i - 1];
                if ( step > 1.0e-5 && ( finest == 0.0 || step < finest ) )
                    finest = step;
            }
            if ( finest == 0.0 )
                return Animation::FrameRate{ 24, 1 };
            const double rate = 1.0 / finest;
            for ( const Animation::FrameRate standard :
                  { Animation::FrameRate{ 12, 1 }, Animation::FrameRate{ 15, 1 },
                    Animation::FrameRate{ 24000, 1001 }, Animation::FrameRate{ 24, 1 },
                    Animation::FrameRate{ 25, 1 }, Animation::FrameRate{ 30000, 1001 },
                    Animation::FrameRate{ 30, 1 }, Animation::FrameRate{ 48, 1 }, Animation::FrameRate{ 50, 1 },
                    Animation::FrameRate{ 60000, 1001 }, Animation::FrameRate{ 60, 1 },
                    Animation::FrameRate{ 90, 1 }, Animation::FrameRate{ 120, 1 } } )
                if ( std::fabs( rate - standard.AsDouble() ) < standard.AsDouble() * 1.0e-3 )
                    return standard;
            return RationalFromRate( std::round( rate ) );
        }
    } // namespace

    class ScopedAssimpLogger
    {
    public:
        ScopedAssimpLogger()
        {
            Assimp::DefaultLogger::create( "", Assimp::Logger::VERBOSE );
            auto* logger = Assimp::DefaultLogger::get();

            logger->attachStream( new ErrorStream, Assimp::Logger::Err );
            logger->attachStream( new WarnStream, Assimp::Logger::Warn );
            logger->attachStream( new InfoStream, Assimp::Logger::Info );
            logger->attachStream( new DebugStream, Assimp::Logger::DEBUGGING );
        }

        ~ScopedAssimpLogger()
        {
            Assimp::DefaultLogger::kill();
        }

    private:
        class ErrorStream : public Assimp::LogStream
        {
        public:
            void write( const char* message ) override
            {
                LOG_ERROR( "Assimp: {}", message );
            }
        };

        class WarnStream : public Assimp::LogStream
        {
        public:
            void write( const char* message ) override
            {
                LOG_WARN( "Assimp: {}", message );
            }
        };

        class InfoStream : public Assimp::LogStream
        {
        public:
            void write( const char* message ) override
            {
                LOG_INFO( "Assimp: {}", message );
            }
        };

        class DebugStream : public Assimp::LogStream
        {
        public:
            void write( const char* message ) override
            {
                LOG_DEBUG( "Assimp: {}", message );
            }
        };
    };

    using namespace Desert::Assets::Serialization;

    static glm::mat4 ConvertMatrix( const aiMatrix4x4& matrix )
    {
        glm::mat4 result;
        result[0][0] = matrix.a1;
        result[1][0] = matrix.a2;
        result[2][0] = matrix.a3;
        result[3][0] = matrix.a4;
        result[0][1] = matrix.b1;
        result[1][1] = matrix.b2;
        result[2][1] = matrix.b3;
        result[3][1] = matrix.b4;
        result[0][2] = matrix.c1;
        result[1][2] = matrix.c2;
        result[2][2] = matrix.c3;
        result[3][2] = matrix.c4;
        result[0][3] = matrix.d1;
        result[1][3] = matrix.d2;
        result[2][3] = matrix.d3;
        result[3][3] = matrix.d4;
        return result;
    }

    static aiNode* FindNodeRecursive( aiNode* node, const std::string& name )
    {
        if ( name == node->mName.C_Str() )
            return node;

        for ( uint32_t i = 0; i < node->mNumChildren; ++i )
        {
            if ( auto* found = FindNodeRecursive( node->mChildren[i], name ) )
                return found;
        }

        return nullptr;
    }

    static glm::mat4 GetFullLocalTransform( aiNode*                                          node,
                                            const std::unordered_map<std::string, uint32_t>& boneMapping )
    {
        glm::mat4 transform = ConvertMatrix( node->mTransformation );

        aiNode* parent = node->mParent;

        while ( parent )
        {
            if ( boneMapping.contains( parent->mName.C_Str() ) )
                break;

            transform = ConvertMatrix( parent->mTransformation ) * transform;
            parent    = parent->mParent;
        }

        return transform;
    }

    static void BuildSkeletonHierarchy( aiNode* root, std::unordered_map<std::string, uint32_t>& boneMapping,
                                        SkeletonAssetData& skeletonData )
    {
        for ( auto& [boneName, boneIndex] : boneMapping )
        {
            aiNode* node = FindNodeRecursive( root, boneName );
            if ( !node )
                continue;

            aiNode* parentNode = node->mParent;

            auto& bone = skeletonData.Bones[boneIndex];

            if ( parentNode )
            {
                auto it = boneMapping.find( parentNode->mName.C_Str() );

                if ( it != boneMapping.end() )
                {
                    bone.ParentBoneID = it->second;
                }
                else
                {
                    bone.ParentBoneID = std::nullopt;
                }
            }
            else
            {
                bone.ParentBoneID = std::nullopt;
            }

            bone.LocalBindTransform = GetFullLocalTransform( node, boneMapping );
        }
    }

    // THE SOURCE'S NODE HIERARCHY IS APPLIED ONCE, HERE (THM1l-b12). A skinned vertex is drawn as
    // jointGlobal * inverseBind * v (glTF 2.0 skins; assimp's mOffsetMatrix is the inverse bind) and the mesh
    // node's own transform is ignored, so the stored vertices are NOT the rest shape whenever the joints sit
    // under non-joint nodes that turn them — CesiumMan's Z_UP and Armature nodes: its vertices lie along Z
    // while the character drawn stands along Y, so its bounds, its Approx Size and its placement all read a
    // body lying down. UE's import bakes the same into the reference pose. The rest shape is
    // sum(w * G_b * O_b) * v; baking it into the vertices and stating O_b = inverse(G_b) makes the stored
    // vertices the drawn shape, and every animated pose is unchanged: G(t) * inverse(G_b) * v' = G(t) * O_b * v
    // wherever the bones under a vertex agree on G_b * O_b, which a bound skin does. A RIGID mesh's vertices
    // (the RIGID branch of ProcessScene) are written already in that rest space, so `rigid` marks them to be left
    // as they are; they take the same O_b = inverse(G_b) below and follow their bone from there.
    static void BakeBindPose( SkeletonAssetData& skeleton, MeshAssetData& mesh, const std::vector<char>& rigid )
    {
        const std::size_t                       n = skeleton.Bones.size();
        std::vector<glm::mat4>                  global( n, glm::mat4( 1.0f ) );
        std::vector<char>                       known( n, 0 );
        std::function<glm::mat4( std::size_t )> globalOf = [&]( std::size_t i ) -> glm::mat4
        {
            if ( known[i] == 0 )
            {
                const auto& bone = skeleton.Bones[i];
                global[i]        = bone.ParentBoneID && *bone.ParentBoneID < n
                                        ? globalOf( *bone.ParentBoneID ) * bone.LocalBindTransform
                                        : bone.LocalBindTransform;
                known[i]         = 1;
            }
            return global[i];
        };
        std::vector<glm::mat4> rest( n );
        for ( std::size_t i = 0; i < n; ++i )
            rest[i] = globalOf( i ) * skeleton.Bones[i].OffsetMatrix;

        for ( std::size_t vi = 0; vi < mesh.SkinnedVertices.size(); ++vi )
        {
            if ( vi < rigid.size() && rigid[vi] != 0 )
                continue;
            auto&     v = mesh.SkinnedVertices[vi];
            glm::mat4 m( 0.0f );
            float     sum = 0.0f;
            for ( int k = 0; k < 4; ++k )
                if ( v.BoneWeights[k] > 0.0f && v.BoneIDs[k] < n )
                {
                    m += v.BoneWeights[k] * rest[v.BoneIDs[k]];
                    sum += v.BoneWeights[k];
                }
            if ( sum <= 0.0f )
                continue;
            m /= sum;
            const glm::mat3 r      = glm::mat3( m );
            const glm::mat3 normal = glm::transpose( glm::inverse( r ) );
            const auto      unit   = []( const glm::vec3& d )
            { return glm::length( d ) > 0.0f ? glm::normalize( d ) : d; };
            v.Position  = glm::vec3( m * glm::vec4( v.Position, 1.0f ) );
            v.Normal    = unit( normal * v.Normal );
            v.Tangent   = unit( r * v.Tangent );
            v.Bitangent = unit( r * v.Bitangent );
            for ( auto& morph : mesh.MorphTargets )
            {
                if ( vi < morph.DeltaPositions.size() )
                    morph.DeltaPositions[vi] = r * morph.DeltaPositions[vi];
                if ( vi < morph.DeltaNormals.size() )
                    morph.DeltaNormals[vi] = normal * morph.DeltaNormals[vi];
            }
        }
        for ( std::size_t i = 0; i < n; ++i )
            skeleton.Bones[i].OffsetMatrix = glm::inverse( globalOf( i ) );

        for ( auto& sub : mesh.Submeshes )
        {
            if ( sub.VertexCount == 0 || sub.VertexOffset + sub.VertexCount > mesh.SkinnedVertices.size() )
                continue;
            glm::vec3 mn( std::numeric_limits<float>::max() );
            glm::vec3 mx( std::numeric_limits<float>::lowest() );
            for ( std::size_t i = sub.VertexOffset; i < sub.VertexOffset + sub.VertexCount; ++i )
            {
                mn = glm::min( mn, mesh.SkinnedVertices[i].Position );
                mx = glm::max( mx, mesh.SkinnedVertices[i].Position );
            }
            sub.BoundingBox = { mn, mx };
        }
    }

    // A BONE'S CHANNEL IS LOCAL TO ITS NODE'S PARENT, and the skeleton's local bind (GetFullLocalTransform)
    // folds the non-bone nodes between the bone and its parent bone into it. The keys must be stated in the
    // same frame, or a clip drops those nodes the moment it plays (CesiumMan's root turn). Rigid transforms
    // with a uniform scale — what a node chain above a rig is — keep TRS keys TRS.
    static void FoldNonBoneAncestors( const glm::mat4& pre, Animation::Timeline::TransformChannel& channel )
    {
        if ( pre == glm::mat4( 1.0f ) )
            return;
        const float     scale = glm::length( glm::vec3( pre[0] ) );
        const glm::quat turn  = glm::quat_cast( glm::mat3( pre ) / ( scale > 0.0f ? scale : 1.0f ) );
        // The importer keys the components of one source key together (the channel loop below), so key i of X, Y
        // and Z (and W for rotation) is one vector.
        auto& t = channel.Translation;
        for ( std::size_t k = 0; k < t.X.Keys.size(); ++k )
        {
            const glm::vec3 v =
                 glm::vec3( pre * glm::vec4( t.X.Keys[k].Value, t.Y.Keys[k].Value, t.Z.Keys[k].Value, 1.0f ) );
            t.X.Keys[k].Value = v.x;
            t.Y.Keys[k].Value = v.y;
            t.Z.Keys[k].Value = v.z;
        }
        auto& r = channel.Rotation;
        for ( std::size_t k = 0; k < r.X.Keys.size(); ++k )
        {
            const glm::quat q =
                 turn * glm::quat( r.W.Keys[k].Value, r.X.Keys[k].Value, r.Y.Keys[k].Value, r.Z.Keys[k].Value );
            r.X.Keys[k].Value = q.x;
            r.Y.Keys[k].Value = q.y;
            r.Z.Keys[k].Value = q.z;
            r.W.Keys[k].Value = q.w;
        }
        for ( auto* component : { &channel.Scale.X, &channel.Scale.Y, &channel.Scale.Z } )
            for ( auto& key : component->Keys )
                key.Value *= scale;
    }

    static glm::vec4 GetColor( aiMaterial* mat, const char* key, unsigned type, unsigned idx, glm::vec4 def )
    {
        aiColor4D color;
        if ( aiGetMaterialColor( mat, key, type, idx, &color ) == AI_SUCCESS )
        {
            return { color.r, color.g, color.b, color.a };
        }
        return def;
    }

    static float GetFloat( aiMaterial* mat, const char* key, unsigned type, unsigned idx, float def )
    {
        float value;
        if ( aiGetMaterialFloat( mat, key, type, idx, &value ) == AI_SUCCESS )
            return value;
        return def;
    }

    // Deterministic material GUID from a stable key. Re-importing the same source yields the SAME GUID, so a
    // mesh submesh's reference (the GUID's handle) survives re-cooks, as a random one would not. The two
    // halves are two keyed folds, so no second derivation rule exists beside AssetHandle::FromKey.
    //
    // The FNV-1a loop this used to hold was one of THREE hand-written copies of the same derivation
    // (here, TextureImporter, and Common::AssetHandle::FromKey). Three copies of one rule is how two of
    // them drift; there is now one, and it lives with the handle type.
    static Common::Content::AssetGuid StableMaterialGuid( const std::string& key )
    {
        return { static_cast<uint64_t>( Common::AssetHandle::FromKey( key ) ),
                 static_cast<uint64_t>( Common::AssetHandle::FromKey( "guid-lo:" + key ) ) };
    }

    // Extract every source material into its source dictionary, bound to a template's layout at write, NORMAL
    // and OPACITY maps included, and stamps a stable MaterialId.
    static std::vector<ImportedMaterial>
    ExtractMaterials( const aiScene* scene, const std::filesystem::path& sourcePath, ImportPass pass )
    {
        std::vector<ImportedMaterial> result;

        for ( uint32_t i = 0; i < scene->mNumMaterials; ++i )
        {
            aiMaterial* mat = scene->mMaterials[i];

            ImportedMaterial out;
            aiString         name;
            mat->Get( AI_MATKEY_NAME, name );
            out.Name = name.length > 0 ? std::string( name.C_Str() ) : ( "Material_" + std::to_string( i ) );

            // Keyed on the mesh's place in the project, NOT on its file stem — see CookPaths::MaterialKey
            // for what the stem-only key merged and why the repository is one same-named file away from it.
            out.Guid = StableMaterialGuid( CookPaths::MaterialKey( sourcePath, out.Name, i ) );

            // The source's own dictionary (MAT1b adapters); which template takes it, and which of its keys land
            // where, is decided at SerializeMaterialAsset by the templates' Import rows. Where the texture a
            // reference names lives - on disk beside the source, or embedded in it and derived to a file there:
            // EmbeddedSourceTexture.hpp.
            SourceMaterialRead read =
                 ReadSourceMaterial( *mat, SourceFormatOf( sourcePath ), out.Name,
                                     [&]( const std::string& refText )
                                     {
                                         const auto resolved =
                                              ResolveSourceTexture( *scene, sourcePath, refText, pass );
                                         if ( !resolved.IsSuccess() )
                                         {
                                             LOG_ERROR( "{}", resolved.GetError() );
                                             return std::filesystem::path{};
                                         }
                                         if ( resolved.GetValue().Extracted == PackOutcome::Written )
                                             LOG_INFO( "[Import][Tex] embedded texture '{}' written to '{}'",
                                                       refText, resolved.GetValue().Path.generic_string() );
                                         return resolved.GetValue().Path;
                                     } );
            out.Source               = std::move( read.Material );
            const SourceAlpha& alpha = read.Alpha;
            if ( !alpha.Warning.empty() )
            {
                LOG_WARN( "{} (in '{}')", alpha.Warning, sourcePath.generic_string() );
            }
            else if ( alpha.Kind == SourceAlphaKind::BlendAsMask )
            {
                LOG_WARN( "[Import][Material] '{}' in '{}' states alphaMode BLEND; surface materials have no "
                          "translucent blend mode, so it is drawn MASKED at cutoff {} (the albedo's alpha)",
                          out.Name, sourcePath.generic_string(), alpha.AlphaCutoff );
            }
            else if ( alpha.Kind == SourceAlphaKind::Mask )
            {
                LOG_INFO( "[Import][Material] '{}' {}, cutoff {} (the albedo's alpha)", out.Name,
                          alpha.AlphaMode.empty() ? "base colour alpha cuts out" : "alphaMode MASK",
                          alpha.AlphaCutoff );
            }
            else if ( alpha.Kind == SourceAlphaKind::OpacityMap && !alpha.AlphaMode.empty() &&
                      alpha.AlphaMode != "OPAQUE" )
            {
                LOG_INFO( "[Import][Material] '{}' has an opacity map; it is the mask over alphaMode {}", out.Name,
                          alpha.AlphaMode );
            }

            result.push_back( std::move( out ) );
        }

        return result;
    }

    static ImportResult ProcessScene( const aiScene* scene, ImportManager& manager,
                                      const std::filesystem::path& sourcePath, ImportPass pass )
    {
        ImportResult result;

        MeshAssetData     meshData;
        SkeletonAssetData skeletonData;

        // Resolve the material's texture references RELATIVE TO THE SOURCE FILE's own folder (how FBX/glTF
        // store them, e.g. Poly Haven's "textures/<name>.jpg" sits next to the .fbx). The old code looked in
        // a hardcoded <assets root>/Textures/<stem>/ and never found them.
        const auto materialData = ExtractMaterials( scene, sourcePath, pass );

        std::unordered_map<std::string, uint32_t> boneMapping;

        // EVERY NODE THAT REFERENCES A MESH IS ITS OWN INSTANCE (UE: the FBX static mesh import bakes each
        // mesh-bearing node into the combined mesh, FbxStaticMeshImport ImportStaticMeshAsSingle over every node;
        // with Combine Meshes off each node is its own UStaticMesh, FbxFactory RecursiveImportNode). A source
        // shares one aiMesh between nodes that place the same geometry (Bistro's string lights, its repeated
        // chairs): mesh index -> every (node, WORLD transform) that places it, in node-walk order. FBX keeps the
        // real orientation/placement (and the exporter's axis conversion, e.g. Blender's Z-up -> our Y-up) in the
        // NODE hierarchy, NOT the raw vertices, so each placement's world transform is baked into its own copy of
        // the STATIC vertices below (without it a Blender FBX imports rotated ~90 degrees about X). Skinned meshes
        // are NOT baked and NOT copied: their bind/bone hierarchy (BuildSkeletonHierarchy) already carries the
        // node transforms, so a skinned mesh keeps its first placement only. A mesh no node references keeps one
        // placement at the identity, unnamed. In a source with a skin, a mesh without one is a RIGID part of the
        // skeletal mesh (the RIGID branch below) and every placement of it is its own rigid copy.
        struct MeshPlacement
        {
            uint32_t      Mesh = 0;
            glm::mat4     World{ 1.0f };
            std::string   Node;
            const aiNode* Source = nullptr;
        };
        // One entry per submesh of the imported mesh: mesh-index major, then the mesh's placements in node-walk
        // order, so a source that shares no mesh keeps the submesh order it always had.
        std::vector<MeshPlacement> placements;
        {
            std::vector<std::vector<MeshPlacement>>                meshPlacements( scene->mNumMeshes );
            std::function<void( const aiNode*, const glm::mat4& )> walk =
                 [&]( const aiNode* node, const glm::mat4& parent )
            {
                const glm::mat4 world = parent * ConvertMatrix( node->mTransformation );
                for ( unsigned i = 0; i < node->mNumMeshes; ++i )
                {
                    const unsigned mi = node->mMeshes[i];
                    if ( mi >= meshPlacements.size() )
                        continue;
                    if ( scene->mMeshes[mi]->HasBones() && !meshPlacements[mi].empty() )
                        continue;
                    meshPlacements[mi].push_back( { mi, world, node->mName.C_Str(), node } );
                }
                for ( unsigned i = 0; i < node->mNumChildren; ++i )
                    walk( node->mChildren[i], world );
            };
            if ( scene->mRootNode )
                walk( scene->mRootNode, glm::mat4( 1.0f ) );
            for ( uint32_t mi = 0; mi < scene->mNumMeshes; ++mi )
            {
                if ( meshPlacements[mi].empty() )
                    meshPlacements[mi].push_back( { mi, glm::mat4( 1.0f ), {} } );
                placements.insert( placements.end(), meshPlacements[mi].begin(), meshPlacements[mi].end() );
            }
        }

        // A SOURCE WITH A SKIN IS ONE SKELETAL MESH, ITS RIGID PARTS INCLUDED (UE: FbxSkeletalMeshImport with
        // bImportMeshesInBoneHierarchy — a mesh that hangs under a bone of the skeleton is skinned to that bone
        // with weight 1, its vertices taken at the node's global transform). RobotExpressive is built that way:
        // only its hands carry a skin, the head, torso, arms and legs are meshes parented to bones under the
        // RobotArmature node (scale 100, -90 degrees about X); taken as static vertices they were written to an
        // array a skeletal mesh never draws, and only the hands showed. The skin's joints are therefore registered
        // before any mesh (in the order the mesh loop met them, which the skeleton's bone order and signature were
        // made from), so a rigid mesh met first already finds its bone. A joint is matched to its node by the name
        // the skin states, as BuildSkeletonHierarchy matches it.
        bool hasBones = false;
        for ( uint32_t mi = 0; mi < scene->mNumMeshes; ++mi )
        {
            const aiMesh* mesh = scene->mMeshes[mi];
            if ( !mesh->HasBones() )
                continue;
            hasBones = true;
            for ( uint32_t b = 0; b < mesh->mNumBones; ++b )
            {
                const std::string name = mesh->mBones[b]->mName.C_Str();
                if ( boneMapping.contains( name ) )
                    continue;
                boneMapping[name] = static_cast<uint32_t>( skeletonData.Bones.size() );
                Desert::Animation::BoneInfo bone;
                bone.Name               = name;
                bone.OffsetMatrix       = ConvertMatrix( mesh->mBones[b]->mOffsetMatrix );
                bone.LocalBindTransform = glm::mat4( 1.0f );
                skeletonData.Bones.push_back( bone );
            }
        }
        std::unordered_map<const aiNode*, uint32_t> jointOfNode;
        if ( hasBones && scene->mRootNode )
            for ( const auto& [name, index] : boneMapping )
                if ( const aiNode* node = FindNodeRecursive( scene->mRootNode, name ) )
                    jointOfNode[node] = index;
        // The bone a rigid mesh follows: the nearest joint at or above its node.
        const auto rigidBoneOf = [&]( const aiNode* node ) -> std::optional<uint32_t>
        {
            for ( ; node != nullptr; node = node->mParent )
                if ( const auto it = jointOfNode.find( node ); it != jointOfNode.end() )
                    return it->second;
            return std::nullopt;
        };
        // One flag per skinned vertex: 1 where the vertex belongs to a rigid mesh, already in the rest space.
        std::vector<char> rigidVertices;

        // ============================
        // Mesh extraction
        // ============================

        // Blendshape/morph deltas collected per submesh, then merged (by name) into global morph targets
        // after the mesh loop — a named blendshape can span several submeshes, and each contributes its
        // deltas at its own vertex offset into the final mesh-wide vertex array.
        struct MorphContribution
        {
            std::string            Name;
            uint32_t               VertexOffset;
            std::vector<glm::vec3> DeltaPos;
            std::vector<glm::vec3> DeltaNorm; // empty => no normal deltas
        };
        std::vector<MorphContribution> morphContribs;

        // COLOR_0 and TEXCOORD_1 become the asset's optional streams, one entry per vertex in the order the
        // vertex arrays below are filled.
        const SceneVertexStreams streams = StreamsOf( *scene );

        for ( const MeshPlacement& placement : placements )
        {
            aiMesh* mesh = scene->mMeshes[placement.Mesh];

            const bool meshHasBones = mesh->HasBones();

            std::optional<uint32_t> rigidBone;
            if ( hasBones && !meshHasBones )
            {
                rigidBone = rigidBoneOf( placement.Source );
                if ( !rigidBone )
                {
                    // UE's skeletal mesh import takes the skinned meshes and the meshes in the bone hierarchy;
                    // a mesh outside both is not part of the character.
                    LOG_WARN(
                         "[Import] '{}': mesh '{}' (node '{}') hangs under no bone of the skeleton and has no "
                         "skin, so it is not part of the skeletal mesh.",
                         sourcePath.string(), mesh->mName.C_Str(), placement.Node );
                    continue;
                }
            }
            const bool skinnedVertices = meshHasBones || rigidBone.has_value();

            SubmeshData submesh;
            submesh.Name = mesh->mName.C_Str();

            submesh.VertexOffset =
                 skinnedVertices ? meshData.SkinnedVertices.size() : meshData.StaticVertices.size();

            submesh.IndexOffset    = meshData.Indices.size() * 3;
            submesh.VertexCount    = mesh->mNumVertices;
            submesh.IndexCount     = mesh->mNumFaces * 3;
            submesh.Transform      = glm::mat4( 1.0f );
            submesh.MaterialGuid   = materialData[mesh->mMaterialIndex].Guid;

            // ============================
            // VERTICES
            // ============================

            if ( !skinnedVertices )
            {
                // -------- STATIC -------- (bake the node world transform so orientation/placement match the
                // DCC tool; normals use the 3x3 part, renormalized to survive any scale. Tangents are generated
                // over the baked vertices below.)
                const glm::mat4 world     = placement.World;
                const glm::mat3 normalMat = glm::mat3( world );

                for ( uint32_t i = 0; i < mesh->mNumVertices; ++i )
                {
                    StaticVertexData v{};

                    const glm::vec3 p = glm::vec3(
                         world * glm::vec4( mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z, 1.0f ) );
                    v.Position = { p.x, p.y, p.z };

                    if ( mesh->HasNormals() )
                        v.Normal = glm::normalize(
                             normalMat * glm::vec3( mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z ) );

                    if ( mesh->HasTextureCoords( 0 ) )
                        v.TexCoord = { mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y };

                    meshData.StaticVertices.push_back( v );
                }
                AppendVertexStreams( *mesh, streams, meshData.Colors, meshData.UV1 );
            }
            else if ( rigidBone )
            {
                // -------- RIGID (a mesh in the bone hierarchy) -------- the node's global transform puts the
                // vertices where the bind pose draws them, the same rest space BakeBindPose takes the skinned
                // vertices to; one bone, full weight.
                const glm::mat4 world      = placement.World;
                const glm::mat3 normalMat  = glm::transpose( glm::inverse( glm::mat3( world ) ) );
                const auto      unit       = []( const glm::vec3& d )
                { return glm::length( d ) > 0.0f ? glm::normalize( d ) : d; };

                rigidVertices.resize( meshData.SkinnedVertices.size(), 0 );
                for ( uint32_t i = 0; i < mesh->mNumVertices; ++i )
                {
                    SkinnedVertexData v{};
                    v.Position = glm::vec3( world * glm::vec4( mesh->mVertices[i].x, mesh->mVertices[i].y,
                                                               mesh->mVertices[i].z, 1.0f ) );
                    if ( mesh->HasNormals() )
                        v.Normal = unit( normalMat * glm::vec3( mesh->mNormals[i].x, mesh->mNormals[i].y,
                                                                mesh->mNormals[i].z ) );
                    if ( mesh->HasTextureCoords( 0 ) )
                        v.TexCoord = { mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y };
                    v.BoneIDs[0]     = *rigidBone;
                    v.BoneWeights[0] = 1.0f;
                    meshData.SkinnedVertices.push_back( v );
                }
                rigidVertices.resize( meshData.SkinnedVertices.size(), 1 );
                AppendVertexStreams( *mesh, streams, meshData.Colors, meshData.UV1 );
            }
            else
            {
                // -------- SKINNED --------
                std::vector<SkinnedVertexData> tempVertices( mesh->mNumVertices );

                for ( uint32_t i = 0; i < mesh->mNumVertices; ++i )
                {
                    auto& v = tempVertices[i];

                    v.Position = { mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z };

                    if ( mesh->HasNormals() )
                        v.Normal = { mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z };

                    if ( mesh->HasTextureCoords( 0 ) )
                        v.TexCoord = { mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y };
                }

                // ============================
                // BONES + WEIGHTS
                // ============================

                for ( uint32_t b = 0; b < mesh->mNumBones; ++b )
                {
                    aiBone*     aiBone = mesh->mBones[b];
                    std::string name   = aiBone->mName.C_Str();

                    uint32_t boneIndex;

                    if ( !boneMapping.contains( name ) )
                    {
                        boneIndex         = static_cast<uint32_t>( skeletonData.Bones.size() );
                        boneMapping[name] = boneIndex;

                        Desert::Animation::BoneInfo bone;
                        bone.Name               = name;
                        bone.OffsetMatrix       = ConvertMatrix( aiBone->mOffsetMatrix );
                        bone.LocalBindTransform = glm::mat4( 1.0f );

                        skeletonData.Bones.push_back( bone );
                    }
                    else
                    {
                        boneIndex = boneMapping[name];
                    }

                    for ( uint32_t w = 0; w < aiBone->mNumWeights; ++w )
                    {
                        const aiVertexWeight& weight = aiBone->mWeights[w];

                        uint32_t vertexID = weight.mVertexId;
                        float    value    = weight.mWeight;

                        auto& v = tempVertices[vertexID];

                        int minIndex = 0;
                        for ( int i = 1; i < 4; ++i )
                        {
                            if ( v.BoneWeights[i] < v.BoneWeights[minIndex] )
                                minIndex = i;
                        }

                        if ( value > v.BoneWeights[minIndex] )
                        {
                            v.BoneIDs[minIndex]     = boneIndex;
                            v.BoneWeights[minIndex] = value;
                        }
                    }
                }

                // No renormalisation here: the read flags carry aiProcess_LimitBoneWeights, which cuts every
                // vertex with more than four influences to its four strongest and rescales those to sum to one
                // (LimitBoneWeightsProcess.cpp); a vertex with four or fewer keeps the file's own weights.
                for ( auto& v : tempVertices )
                {
                    meshData.SkinnedVertices.push_back( v );
                }
                AppendVertexStreams( *mesh, streams, meshData.Colors, meshData.UV1 );
            }

            const size_t firstMorph = morphContribs.size();

            // ============================
            // MORPH TARGETS (blendshapes)
            // ============================
            // Assimp exposes FBX/glTF blendshapes as aiAnimMesh entries — each stores the FULL morphed
            // vertex positions, so the per-vertex delta is (morphed - base). Static meshes bake the node
            // world transform into base positions, so the delta (a direction) uses only the 3x3 part, and so
            // do rigid meshes of a skeletal mesh; skinned meshes keep local space (their base verts are
            // untransformed too) until BakeBindPose turns both.
            for ( uint32_t a = 0; a < mesh->mNumAnimMeshes; ++a )
            {
                const aiAnimMesh* anim = mesh->mAnimMeshes[a];
                if ( !anim || !anim->HasPositions() )
                    continue;

                MorphContribution mc;
                mc.Name = anim->mName.length > 0 ? std::string( anim->mName.C_Str() )
                                                 : ( "Morph_" + std::to_string( a ) );
                mc.VertexOffset    = submesh.VertexOffset;
                const bool hasNorm = anim->mNormals != nullptr && mesh->HasNormals();
                mc.DeltaPos.resize( mesh->mNumVertices );
                if ( hasNorm )
                    mc.DeltaNorm.resize( mesh->mNumVertices );

                const glm::mat3 dirMat = meshHasBones ? glm::mat3( 1.0f ) : glm::mat3( placement.World );
                for ( uint32_t i = 0; i < mesh->mNumVertices; ++i )
                {
                    const glm::vec3 dp( anim->mVertices[i].x - mesh->mVertices[i].x,
                                        anim->mVertices[i].y - mesh->mVertices[i].y,
                                        anim->mVertices[i].z - mesh->mVertices[i].z );
                    mc.DeltaPos[i] = dirMat * dp;
                    if ( hasNorm )
                    {
                        const glm::vec3 dn( anim->mNormals[i].x - mesh->mNormals[i].x,
                                            anim->mNormals[i].y - mesh->mNormals[i].y,
                                            anim->mNormals[i].z - mesh->mNormals[i].z );
                        mc.DeltaNorm[i] = dirMat * dn;
                    }
                }
                morphContribs.push_back( std::move( mc ) );
            }

            // ============================
            // INDICES
            // ============================

            for ( uint32_t i = 0; i < mesh->mNumFaces; ++i )
            {
                auto& face = mesh->mFaces[i];

                meshData.Indices.push_back( { face.mIndices[0], face.mIndices[1], face.mIndices[2] } );
            }

            // ============================
            // BOUNDING BOX
            // ============================

            if ( mesh->mNumVertices > 0 )
            {
                // Compute the AABB from the SAME space the cooked vertices end up in: static meshes have the
                // node world transform baked in (above), so the box must be baked too — otherwise it's stale
                // (wrong size/position) and everything that frames by it (thumbnail FitTarget, picking, cull)
                // misbehaves: the preview camera ends up inside/off the mesh. Skinned verts aren't baked.
                const glm::mat4 boxXf = meshHasBones ? glm::mat4( 1.0f ) : placement.World;
                glm::vec3       aabbMin( std::numeric_limits<float>::max() );
                glm::vec3       aabbMax( -std::numeric_limits<float>::max() );

                for ( uint32_t i = 0; i < mesh->mNumVertices; ++i )
                {
                    const glm::vec3 pos = glm::vec3(
                         boxXf * glm::vec4( mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z, 1.0f ) );
                    aabbMin = glm::min( aabbMin, pos );
                    aabbMax = glm::max( aabbMax, pos );
                }

                submesh.BoundingBox = { aabbMin, aabbMax };
            }

            // ============================
            // TANGENTS (MikkTSpace)
            // ============================
            // The engine's one generator (Engine/Geometry/MikkTangents.hpp) over the vertices as stored: static
            // and rigid ones with the node transform baked, skinned ones in mesh space (BakeBindPose turns the
            // frame with them). A vertex whose corners disagree is split, and every per-vertex array of this
            // submesh (vertices, colour / UV1 streams, rigid flags, morph deltas) and its triangles follow the
            // split. Without UV0 there is no tangent space to derive; the frame stays zero, as no normal map
            // can be sampled on such a mesh.
            if ( mesh->HasTextureCoords( 0 ) && mesh->mNumFaces > 0 )
            {
                const size_t           first = submesh.VertexOffset;
                const size_t           count = mesh->mNumVertices;
                std::vector<glm::vec3> positions( count );
                std::vector<glm::vec3> normals( count );
                std::vector<glm::vec2> uvs( count );
                const auto             gather = [&]( const auto& vertices )
                {
                    for ( size_t i = 0; i < count; ++i )
                    {
                        positions[i] = vertices[first + i].Position;
                        normals[i]   = vertices[first + i].Normal;
                        uvs[i]       = vertices[first + i].TexCoord;
                    }
                };
                if ( skinnedVertices )
                    gather( meshData.SkinnedVertices );
                else
                    gather( meshData.StaticVertices );

                const size_t            firstTriangle = meshData.Indices.size() - mesh->mNumFaces;
                std::vector<glm::uvec3> triangles( mesh->mNumFaces );
                for ( size_t f = 0; f < triangles.size(); ++f )
                {
                    const IndexData& tri = meshData.Indices[firstTriangle + f];
                    triangles[f]         = glm::uvec3( tri.V1, tri.V2, tri.V3 );
                }

                Geometry::IndexedMikkTangents frames;
                // MikkTSpace fails only when its working set cannot be allocated.
                if ( !Geometry::ComputeIndexedMikkTangents( positions, normals, uvs, triangles, frames ) )
                    throw std::bad_alloc();

                const auto follow = [&]( auto& values, size_t offset )
                {
                    using Value = typename std::decay_t<decltype( values )>::value_type;
                    std::vector<Value> remapped;
                    remapped.reserve( frames.SourceVertex.size() );
                    for ( const uint32_t source : frames.SourceVertex )
                        remapped.push_back( values[offset + source] );
                    values.resize( offset );
                    values.insert( values.end(), remapped.begin(), remapped.end() );
                };
                const auto writeFrames = [&]( auto& vertices )
                {
                    follow( vertices, first );
                    for ( size_t k = 0; k < frames.SourceVertex.size(); ++k )
                    {
                        vertices[first + k].Tangent   = frames.Tangents[k];
                        vertices[first + k].Bitangent = frames.Bitangents[k];
                    }
                };
                if ( skinnedVertices )
                    writeFrames( meshData.SkinnedVertices );
                else
                    writeFrames( meshData.StaticVertices );
                if ( streams.Colors )
                    follow( meshData.Colors, first );
                if ( streams.UV1 )
                    follow( meshData.UV1, first );
                if ( skinnedVertices && rigidVertices.size() >= first + count )
                    follow( rigidVertices, first );
                for ( size_t m = firstMorph; m < morphContribs.size(); ++m )
                {
                    follow( morphContribs[m].DeltaPos, 0 );
                    if ( !morphContribs[m].DeltaNorm.empty() )
                        follow( morphContribs[m].DeltaNorm, 0 );
                }
                for ( size_t f = 0; f < triangles.size(); ++f )
                    meshData.Indices[firstTriangle + f] = { frames.Triangles[f].x, frames.Triangles[f].y,
                                                            frames.Triangles[f].z };
                submesh.VertexCount = static_cast<uint32_t>( frames.SourceVertex.size() );
            }

            meshData.Submeshes.push_back( submesh );
            result.SubmeshNodes.push_back( placement.Node );
        }

        // ============================
        // FINALIZE
        // ============================

        meshData.IsSkinned = hasBones;

        // Merge the per-submesh morph contributions (by name) into mesh-wide morph targets whose delta
        // arrays are index-aligned with the final vertex array (zero where a target doesn't touch a vertex).
        if ( !morphContribs.empty() )
        {
            const size_t totalVerts =
                 meshData.IsSkinned ? meshData.SkinnedVertices.size() : meshData.StaticVertices.size();
            std::unordered_map<std::string, size_t> morphIndex;
            for ( auto& mc : morphContribs )
            {
                size_t idx;
                if ( auto it = morphIndex.find( mc.Name ); it != morphIndex.end() )
                {
                    idx = it->second;
                }
                else
                {
                    idx                 = meshData.MorphTargets.size();
                    morphIndex[mc.Name] = idx;
                    MorphTargetData mt;
                    mt.Name = mc.Name;
                    mt.DeltaPositions.assign( totalVerts, glm::vec3( 0.0f ) );
                    mt.DeltaNormals.assign( totalVerts, glm::vec3( 0.0f ) );
                    meshData.MorphTargets.push_back( std::move( mt ) );
                }

                auto&      mt      = meshData.MorphTargets[idx];
                const bool hasNorm = !mc.DeltaNorm.empty();
                for ( size_t i = 0; i < mc.DeltaPos.size(); ++i )
                {
                    const size_t g = static_cast<size_t>( mc.VertexOffset ) + i;
                    if ( g < mt.DeltaPositions.size() )
                        mt.DeltaPositions[g] = mc.DeltaPos[i];
                    if ( hasNorm && g < mt.DeltaNormals.size() )
                        mt.DeltaNormals[g] = mc.DeltaNorm[i];
                }
            }
            LOG_INFO( "[Import] {} morph target(s) across {} contribution(s).", meshData.MorphTargets.size(),
                      morphContribs.size() );
        }

        if ( hasBones )
        {

            BuildSkeletonHierarchy( scene->mRootNode, boneMapping, skeletonData );
            BakeBindPose( skeletonData, meshData, rigidVertices );

            // The bones' hash: import pre-selects an existing .skeleton by it (ImportManager::Import); the mesh's
            // Skeleton GUID is set there, once the rig is known to be an existing skeleton or a new one.
            skeletonData.Signature = Animation::Skeleton::ComputeSignature( skeletonData.Bones );
        }

        if ( !meshData.StaticVertices.empty() || !meshData.SkinnedVertices.empty() )
            result.Mesh = meshData;

        if ( hasBones )
            result.Skeleton = skeletonData;

        // ============================
        // SKINLESS ANIMATION FILE (e.g. Mixamo "without skin"): no mesh -> no mesh bones, so the bone set
        // above is empty and every animation channel would be dropped. Reconstruct the skeleton from the
        // animation channels' node names + the node hierarchy, so the clips carry data and get a skeleton
        // SIGNATURE that matches the character (the signature is order-independent + playback binds by NAME).
        // ============================
        if ( !hasBones && scene->mNumAnimations > 0 && scene->mRootNode )
        {
            for ( uint32_t i = 0; i < scene->mNumAnimations; ++i )
            {
                const aiAnimation* anim = scene->mAnimations[i];
                for ( uint32_t c = 0; c < anim->mNumChannels; ++c )
                {
                    const std::string name = anim->mChannels[c]->mNodeName.C_Str();
                    if ( name.empty() || boneMapping.contains( name ) )
                        continue;
                    if ( !FindNodeRecursive( scene->mRootNode, name ) )
                        continue; // animated node must exist in the hierarchy

                    const uint32_t idx = static_cast<uint32_t>( skeletonData.Bones.size() );
                    boneMapping[name]  = idx;

                    Desert::Animation::BoneInfo bone;
                    bone.Name = name;
                    bone.OffsetMatrix       = glm::mat4( 1.0f ); // unused for playback (target skeleton's bind is used)
                    bone.LocalBindTransform = glm::mat4( 1.0f ); // filled by BuildSkeletonHierarchy
                    skeletonData.Bones.push_back( bone );
                }
            }

            if ( !skeletonData.Bones.empty() )
            {
                BuildSkeletonHierarchy( scene->mRootNode, boneMapping, skeletonData );
                skeletonData.Signature = Animation::Skeleton::ComputeSignature( skeletonData.Bones );
            }
        }

        // ============================
        // ANIMATIONS (��� ���������)
        // ============================

        for ( uint32_t i = 0; i < scene->mNumAnimations; ++i )
        {
            aiAnimation* anim = scene->mAnimations[i];

            // THE CLIP IS BUILT AS THE ENGINE BUILDS ONE (ANIM 6): one Timeline::Sequence, a Bone binding and one
            // whole-clip Absolute Transform section per animated bone (TrackEditing's AddBoneTrack /
            // ChannelForKey, the path an edit takes), so the import writes through the same BuildAssetDataFromClip
            // every other producer does and has no clip format of its own.
            Animation::AnimationClip clip;
            clip.AnimationName =
                 anim->mName.length > 0 ? std::string( anim->mName.C_Str() ) : std::format( "Animation_{}", i );

            // ---- the tick grid, and what crossing it costs ------------------------------------------
            //
            // assimp already counts time in TICKS — `aiAnimation::mTicksPerSecond` is the exporter's own
            // rate — so this boundary is a change of tick GRID and not a change of unit. What it used to
            // be was `(float)mTime` per key: exact for every integral tick below 2^24 (which is what an
            // FBX frame index or a glTF millisecond is), and silently inexact for anything else. Rounding
            // is not the problem; an unreported rounding is.
            //
            // PROJECT_TICK_RATE is 24000 precisely so this is an integer multiply for every rate an
            // exporter realistically states: 30 -> x800, 25 -> x960, 24 -> x1000, 1000 (glTF ms) -> x24.
            const double sourceRateValue          = anim->mTicksPerSecond != 0.0 ? anim->mTicksPerSecond : 25.0;
            const Animation::FrameRate sourceRate = RationalFromRate( sourceRateValue );
            // glTF has no frame rate: key times are SECONDS (assimp reports them as milliseconds, 1000 ticks/s).
            // 1000 is a unit, not the cadence the clip was authored on, so the display grid comes from the keys.
            const bool timeIsContinuous = SourceFormatOf( sourcePath ) == "gltf";

            clip.Sequence.TickRate = Animation::PROJECT_TICK_RATE;
            // The file's own rate becomes the DISPLAY grid: it is the cadence the animation was authored
            // on, which is exactly what an artist should see on the ruler and snap to.
            const Animation::FrameRate displayRate = timeIsContinuous ? CadenceOfKeys( *anim ) : sourceRate;
            clip.Sequence.DisplayRate              = displayRate;
            if ( timeIsContinuous )
                LOG_INFO(
                     "[Import] clip '{}': key times in seconds (glTF); authored cadence from the keys: {}/{} fps",
                     clip.AnimationName, displayRate.Numerator, displayRate.Denominator );

            std::size_t roundedKeys   = 0;
            int64_t     worstMicro    = 0;
            const auto  toProjectTick = [&]( double sourceTick ) -> int32_t
            {
                // A fractional source tick (glTF: 1/24 s = 41.666... ms) is converted as a time, not rounded to
                // a whole source tick first — that moved every 24 fps glTF key by up to half a millisecond.
                // The file stores float seconds, so a key within the float's own precision counts as exact.
                if ( std::fabs( sourceTick - std::round( sourceTick ) ) > 1.0e-6 )
                {
                    const double exact =
                         sourceTick * Animation::PROJECT_TICK_RATE.AsDouble() / sourceRate.AsDouble();
                    const double  rounded = std::round( exact );
                    const int64_t micro   = std::llround( std::fabs( exact - rounded ) * 1.0e6 );
                    if ( micro > std::llround( std::fabs( exact ) * 0.25 ) + 1 )
                    {
                        ++roundedKeys;
                        worstMicro = std::max( worstMicro, micro );
                    }
                    return static_cast<int32_t>( rounded );
                }
                const auto converted = Animation::ConvertTick(
                     Animation::FrameNumber{ static_cast<int32_t>( llround( sourceTick ) ) }, sourceRate,
                     Animation::PROJECT_TICK_RATE );
                if ( !converted.Exact )
                {
                    ++roundedKeys;
                    worstMicro =
                         std::max( worstMicro, converted.RoundedAwayMicro < 0 ? -converted.RoundedAwayMicro
                                                                              : converted.RoundedAwayMicro );
                }
                return converted.Ticks.Value;
            };

            // The clip's Skeleton reference is the import's to set (ImportManager::Import): the parser knows the
            // rig's bones, not which registered .skeleton they are.
            clip.Sequence.Start = Animation::FrameNumber{ 0 };
            clip.Sequence.End   = Animation::FrameNumber{ toProjectTick( anim->mDuration ) };

            for ( uint32_t c = 0; c < anim->mNumChannels; ++c )
            {
                aiNodeAnim* channel = anim->mChannels[c];

                const std::string bone = channel->mNodeName.C_Str();
                // The bone must exist in the rig this file also produced, otherwise the channel names a bone
                // no skeleton here has and its keys are unreachable — BuildClipFromAssetData refuses those.
                if ( !boneMapping.contains( bone ) )
                    continue;

                Animation::Timeline::Track&            track = Animation::AddBoneTrack( clip.Sequence, bone );
                Animation::Timeline::TransformChannel& ch =
                     Animation::ChannelForKey( clip.Sequence, track, clip.Sequence.Start );
                const auto key = []( int32_t tick, float value )
                {
                    Animation::ScalarKey out;
                    out.Tick  = Animation::FrameNumber{ tick };
                    out.Value = value;
                    return out;
                };

                for ( uint32_t p = 0; p < channel->mNumPositionKeys; ++p )
                {
                    const int32_t tick = toProjectTick( channel->mPositionKeys[p].mTime );
                    const auto&   v    = channel->mPositionKeys[p].mValue;
                    ch.Translation.X.Keys.push_back( key( tick, v.x ) );
                    ch.Translation.Y.Keys.push_back( key( tick, v.y ) );
                    ch.Translation.Z.Keys.push_back( key( tick, v.z ) );
                }

                for ( uint32_t r = 0; r < channel->mNumRotationKeys; ++r )
                {
                    const int32_t tick = toProjectTick( channel->mRotationKeys[r].mTime );
                    const auto&   q    = channel->mRotationKeys[r].mValue;
                    ch.Rotation.X.Keys.push_back( key( tick, q.x ) );
                    ch.Rotation.Y.Keys.push_back( key( tick, q.y ) );
                    ch.Rotation.Z.Keys.push_back( key( tick, q.z ) );
                    ch.Rotation.W.Keys.push_back( key( tick, q.w ) );
                }

                for ( uint32_t k = 0; k < channel->mNumScalingKeys; ++k )
                {
                    const int32_t tick = toProjectTick( channel->mScalingKeys[k].mTime );
                    const auto&   v    = channel->mScalingKeys[k].mValue;
                    ch.Scale.X.Keys.push_back( key( tick, v.x ) );
                    ch.Scale.Y.Keys.push_back( key( tick, v.y ) );
                    ch.Scale.Z.Keys.push_back( key( tick, v.z ) );
                }

                if ( aiNode* node = FindNodeRecursive( scene->mRootNode, bone ) )
                    FoldNonBoneAncestors( GetFullLocalTransform( node, boneMapping ) *
                                               glm::inverse( ConvertMatrix( node->mTransformation ) ),
                                          ch );
            }

            // SAID OUT LOUD OR NOT SAID AT ALL. A clip whose rate divides 24000 reports nothing; one that
            // does not gets one line naming how many keys moved and by how much, so the cost is a number
            // in the log rather than a difference somebody finds in a frame months later.
            if ( roundedKeys > 0 )
            {
                LOG_WARN( "[Import] clip '{}' is authored at {}/{} ticks per second, which does not divide "
                          "the project's {} — {} key time(s) were rounded onto the project grid, the worst "
                          "by {} millionths of a tick ({} ns).",
                          clip.AnimationName, sourceRate.Numerator, sourceRate.Denominator,
                          Animation::PROJECT_TICK_RATE.Numerator, roundedKeys, worstMicro,
                          worstMicro * 1000 / 24 / 1000 );
            }

            result.Animations.push_back( std::move( clip ) );
        }

        // Only the materials a mesh draws with. assimp appends its own default material to a glTF whose
        // primitives all name one (Fox: fox_material + an unnamed default that became "Material_1"); a
        // material nothing references is not content of this file. Filtered here, after the submeshes took
        // their GUIDs by index, so every kept material keeps its index-derived GUID.
        std::vector<bool> referenced( scene->mNumMaterials, false );
        for ( uint32_t m = 0; m < scene->mNumMeshes; ++m )
            if ( scene->mMeshes[m]->mMaterialIndex < scene->mNumMaterials )
                referenced[scene->mMeshes[m]->mMaterialIndex] = true;
        for ( uint32_t m = 0; m < materialData.size(); ++m )
            if ( referenced[m] )
                result.Materials.push_back( materialData[m] );

        return result;
    }

    // Did the file say what its unit is? FBX states it in GlobalSettings::UnitScaleFactor, which assimp
    // copies into the scene metadata under that name and in CENTIMETRES PER FILE UNIT — 1.0 is a
    // centimetre file, 100.0 a metre one. The value is written as a float; the double branch is there
    // because aiMetadata stores the two as distinct types and reading the wrong one silently answers
    // "the file said nothing", which is the one answer that must never be a guess.
    static bool ReadStatedUnitScale( const aiScene& scene, float& centimetresPerUnit )
    {
        if ( scene.mMetaData == nullptr )
            return false;

        float asFloat = 0.0f;
        if ( scene.mMetaData->Get( "UnitScaleFactor", asFloat ) )
        {
            centimetresPerUnit = asFloat;
            return true;
        }

        double asDouble = 0.0;
        if ( scene.mMetaData->Get( "UnitScaleFactor", asDouble ) )
        {
            centimetresPerUnit = static_cast<float>( asDouble );
            return true;
        }

        return false;
    }

    // THE FILE'S LIGHT SOURCES (BISTRO-LIGHTS; ports UE FbxLightImport.cpp CreateLight / Interchange
    // FbxLight.cpp AddLightsRecursively): one entry per aiLight, at its node's world transform (the same root-down
    // composition the meshes are baked with in ProcessScene, so a light stands where the file put it among them).
    // assimp's FBX reader (FBXConverter.cpp ConvertLight) folds Intensity / 100 into the colour, points the light
    // down its node's -Y, and turns DecayStart into attenuation terms; those are read back into the radius UE
    // takes from DecayStart. @p centimetresPerUnit is the file unit the geometry was scaled by: DecayStart is
    // stated in file units and assimp's scaling step does not touch lights.
    static std::vector<ImportedLight> ReadSceneLights( const aiScene& scene, const std::string& fileName,
                                                       const float centimetresPerUnit )
    {
        std::vector<ImportedLight> lights;
        for ( uint32_t i = 0; i < scene.mNumLights; ++i )
        {
            const aiLight& source = *scene.mLights[i];
            ImportedLight  light;
            light.Name = source.mName.C_Str();
            switch ( source.mType )
            {
                case aiLightSource_DIRECTIONAL:
                    light.Type = ImportedLightType::Directional;
                    break;
                case aiLightSource_SPOT:
                    light.Type             = ImportedLightType::Spot;
                    light.InnerConeDegrees = glm::degrees( source.mAngleInnerCone );
                    light.OuterConeDegrees = glm::degrees( source.mAngleOuterCone );
                    break;
                case aiLightSource_POINT:
                    light.Type = ImportedLightType::Point;
                    break;
                default:
                    // UE FbxLightImport: "default: LightClass = APointLight" - an area or volume light the engine
                    // has no type for is imported as a point light at its node, and the log says so.
                    LOG_WARN( "[Import] {}: light '{}' is of a type the engine has no light for (assimp type {}); "
                              "imported as a point light, as UE does",
                              fileName, light.Name, static_cast<int>( source.mType ) );
                    light.Type = ImportedLightType::Point;
                    break;
            }

            const aiNode* node = scene.mRootNode->FindNode( source.mName );
            glm::mat4     world( 1.0f );
            for ( const aiNode* at = node; at != nullptr; at = at->mParent )
                world = ConvertMatrix( at->mTransformation ) * world;
            if ( node == nullptr )
                LOG_WARN( "[Import] {}: light '{}' names no node of the file, so it stands at the file's origin",
                          fileName, light.Name );
            light.Position = glm::vec3(
                 world * glm::vec4( source.mPosition.x, source.mPosition.y, source.mPosition.z, 1.0f ) );
            const glm::vec3 direction = glm::vec3(
                 world * glm::vec4( source.mDirection.x, source.mDirection.y, source.mDirection.z, 0.0f ) );
            if ( glm::length( direction ) <= 1e-6f )
            {
                LOG_WARN( "[Import] {}: light '{}' has no direction under its node's transform; it is skipped",
                          fileName, light.Name );
                continue;
            }
            light.Direction = glm::normalize( direction );
            light.Radiance  = glm::vec3( source.mColorDiffuse.r, source.mColorDiffuse.g, source.mColorDiffuse.b );

            // DecayStart back out of assimp's terms: None -> constant = DecayStart; Linear -> linear = 2 / start;
            // Quadratic -> quadratic = 2 / start^2 (FBXConverter.cpp ConvertLight). UE: AttenuationRadius =
            // DecayStart, and no decay = an unbounded radius.
            if ( light.Type != ImportedLightType::Directional )
            {
                std::optional<float> decayStart;
                if ( source.mAttenuationLinear > 0.0f )
                    decayStart = 2.0f / source.mAttenuationLinear;
                else if ( source.mAttenuationQuadratic > 0.0f )
                    decayStart = std::sqrt( 2.0f / source.mAttenuationQuadratic );
                if ( decayStart && std::isfinite( *decayStart ) && *decayStart > 0.0f )
                    light.Radius = *decayStart * centimetresPerUnit;
            }
            LOG_INFO( "[Import] {}: light '{}' ({}) at ({}, {}, {}) cm", fileName, light.Name,
                      light.Type == ImportedLightType::Directional ? "directional"
                      : light.Type == ImportedLightType::Spot      ? "spot"
                                                                   : "point",
                      light.Position.x, light.Position.y, light.Position.z );
            lights.push_back( std::move( light ) );
        }
        return lights;
    }

    Common::ResultStr<ImportContentKind> AssimpImporter::Probe( const std::filesystem::path& path )
    {
        static const ScopedAssimpLogger logger;
        Assimp::Importer                importer;
        // No post-processing: the kind is the scene's meshes and bones as the file states them. A relative
        // source is read off the project (FullPath), the root every cooked output of it is written under.
        const aiScene* scene = importer.ReadFile( Common::Constants::Path::FullPath( path ).string(), 0 );
        if ( scene == nullptr || scene->mRootNode == nullptr )
            return Common::MakeFormattedError<ImportContentKind>( "'{}' could not be read: {}", path.string(),
                                                                  importer.GetErrorString() );
        if ( scene->mNumMeshes == 0 )
            return Common::MakeSuccess( ImportContentKind::Animation );
        for ( uint32_t i = 0; i < scene->mNumMeshes; ++i )
            if ( scene->mMeshes[i]->HasBones() )
                return Common::MakeSuccess( ImportContentKind::SkeletalMesh );
        return Common::MakeSuccess( ImportContentKind::StaticMesh );
    }

    ImportResult AssimpImporter::Import( const std::filesystem::path& path, ImportManager& manager,
                                         const Assets::SourceImportSettings& settings, ImportPass pass )
    {
        static ScopedAssimpLogger logger;
        Assimp::Importer          importer;

        // READ FIRST, SCALE SECOND, and aiProcess_GlobalScale is deliberately NOT in this set.
        //
        // assimp normalises every file to METRES: its FBX reader calls SetFileScale( UnitScaleFactor *
        // 0.01 ) (Editor/ThirdParty/assimp/code/AssetLib/FBX/FBXImporter.cpp:180), so running the scaling
        // step with the default factor divided every centimetre-authored FBX by 100. Measured on
        // base.fbx, a 190 cm humanoid: 1.1542 x 1.8983 x 0.3784 with the step on, 115.42 x 189.83 x 37.84
        // with it off. This engine is centimetres (Common::Units), so we read the file, ask it what its
        // unit is (ImportUnits::Resolve), and only then run assimp's OWN scaling step with the factor
        // that lands the geometry in centimetres. Scaling it ourselves is not the cheaper option:
        // ScaleProcess also scales bone offset matrices, node translations and animation position keys,
        // and a second implementation of that is a second thing to keep in agreement.
        const uint32_t readFlags = aiProcess_Triangulate | aiProcess_GenNormals |
                                   aiProcess_JoinIdenticalVertices | aiProcess_LimitBoneWeights;

        // The source is read where its outputs are written: a relative path is off the project (FullPath),
        // never off the process's working directory (ENG-ROOT).
        const aiScene* scene = importer.ReadFile( Common::Constants::Path::FullPath( path ).string(), readFlags );

        if ( !scene || !scene->mRootNode )
        {
            throw std::runtime_error( "Failed to import: " + path.string() );
        }

        const std::string fileName = path.filename().string();

        float                    statedCentimetresPerUnit = 0.0f;
        const bool               stated = ReadStatedUnitScale( *scene, statedCentimetresPerUnit );
        const ImportUnits::Scale unit =
             ImportUnits::Resolve( path.extension().string(), stated, statedCentimetresPerUnit,
                                   Assets::MeshFileUnitCentimetres( settings.FileUnit ) );

        switch ( unit.From )
        {
            case ImportUnits::Source::StatedBySettings:
                if ( stated && statedCentimetresPerUnit != unit.CentimetresPerUnit )
                    LOG_WARN( "[Import] {}: the file states 1 unit = {} cm and the import settings say {} ({}); "
                              "the settings win",
                              fileName, statedCentimetresPerUnit, unit.CentimetresPerUnit,
                              Assets::MeshFileUnitName( settings.FileUnit ) );
                LOG_INFO( "[Import] {}: 1 file unit = {} cm ({}); geometry scaled by {}", fileName,
                          unit.CentimetresPerUnit, ImportUnits::Describe( unit.From ), unit.CentimetresPerUnit );
                break;
            case ImportUnits::Source::StatedByFile:
            case ImportUnits::Source::FixedByFormat:
                LOG_INFO( "[Import] {}: 1 file unit = {} cm ({}); geometry scaled by {}", fileName,
                          unit.CentimetresPerUnit, ImportUnits::Describe( unit.From ), unit.CentimetresPerUnit );
                break;
            case ImportUnits::Source::AssumedCentimetres:
                // Not a silent guess: the file is about to be treated as centimetres and the log says so,
                // because a model that turns up 100x wrong is only debuggable if this line exists.
                LOG_WARN( "[Import] {}: {} (an OBJ never states one) — taking 1 file unit = 1 cm. Set the "
                          "import's File Unit if the model was authored in another unit.",
                          fileName, ImportUnits::Describe( unit.From ) );
                break;
            case ImportUnits::Source::StatedButUnusable:
                LOG_ERROR( "[Import] {}: {} ({}); taking 1 file unit = 1 cm instead.", fileName,
                           ImportUnits::Describe( unit.From ), statedCentimetresPerUnit );
                break;
        }

        // The metres-per-file-unit assimp itself worked out while reading. BaseImporter::ReadFile sets it
        // unconditionally, so it is already here whether or not the scaling step ran.
        const float assimpMetresPerUnit = importer.GetPropertyFloat( AI_CONFIG_APP_SCALE_KEY, 1.0f );

        if ( !ImportUnits::IsUsableScale( assimpMetresPerUnit ) )
        {
            throw std::runtime_error( "Refusing to import " + path.string() + ": the reader reports " +
                                      std::to_string( assimpMetresPerUnit ) +
                                      " metres per file unit, which is not a usable scale." );
        }

        importer.SetPropertyFloat(
             AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY,
             ImportUnits::GlobalScaleFactor( unit.CentimetresPerUnit, assimpMetresPerUnit ) );

        scene = importer.ApplyPostProcessing( aiProcess_GlobalScale );

        if ( scene == nullptr || scene->mRootNode == nullptr )
        {
            throw std::runtime_error( "Failed to scale to centimetres on import: " + path.string() );
        }

        ImportResult result = ProcessScene( scene, manager, path, pass );
        result.Lights       = ReadSceneLights( *scene, fileName, unit.CentimetresPerUnit );
        return result;
    }

} // namespace Desert::Editor