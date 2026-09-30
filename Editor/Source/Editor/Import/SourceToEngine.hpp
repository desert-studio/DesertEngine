#pragma once
#include <Common/Core/Math/AABB.hpp>

#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <glm/glm.hpp>

#include <optional>
#include <vector>

namespace Desert::Editor
{
    // SOURCE AXES -> THE ENGINE'S (+Y up, centimetres), the one mapping of the import options Uniform Scale and
    // Up Axis (UE FFbxImportUI ImportUniformScale / the axis conversion). FromFile and Y: the importer already
    // resolved the file's own hierarchy into Y-up, so only the scale remains; Z: +Z up becomes +Y up
    // (x, y, z) -> (x, z, -y), a rotation, so winding and handedness are kept. A uniform scale times a rotation.
    glm::mat4 SourceToEngine( const Assets::MeshImportSettings& settings );

    // A source-space box in the engine's space: the box of its eight corners under SourceToEngine(settings). The
    // box a placed mesh has, whichever side applies the mapping (the deriver for a static mesh, the import for a
    // skinned one) - what the import record states (ImportRecord.hpp `Bounds`).
    Common::Math::AABB SourceToEngineBounds( const Common::Math::AABB&         box,
                                             const Assets::MeshImportSettings& settings );

    // THE SAME OPTIONS ON A SKINNED FILE (UE applies FBX Import Options to skeletal meshes, their skeleton and
    // their animations alike). A static mesh keeps its source untransformed and has the mapping applied by the
    // deriver (MeshDeriver); a skinned mesh, its skeleton and its clips are written in render form at import,
    // so the mapping C = SourceToEngine(settings) is baked here, consistently for all three:
    //   - vertices, submesh boxes and morph position deltas: C p; normals, tangents, bitangents, morph normal
    //     deltas: the rotation of C;
    //   - every bone matrix (local bind, inverse bind) and submesh transform M: C M C^-1, so the skinned result
    //     global * offset * v is C times the source's (a scale lands in the translations, not in the bones);
    //   - every animation key in a bone's local frame the same way: positions and tangents C t, rotations
    //     q_C q q_C^-1, scales re-ordered along the axes C rotates (C only ever rotates by quarter turns).
    // Identity settings change nothing.
    void ApplySourceToEngine( const Assets::MeshImportSettings&         settings,
                              Assets::Serialization::MeshAssetData*     mesh,
                              Assets::Serialization::SkeletonAssetData* skeleton,
                              std::vector<Animation::AnimationClip>&    animations );
} // namespace Desert::Editor
