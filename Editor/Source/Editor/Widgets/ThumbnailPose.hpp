#pragma once

#include <Editor/Widgets/ThumbnailSubject.hpp>

#include <Common/Core/ResultStr.hpp>

#include <string>

namespace Desert::Assets
{
    class AssetManager;
}

// A SKINNED MESH'S PICTURE IS THE MESH IN ITS BIND POSE (THM1n-6; UE: USkeletalMesh's thumbnail renders the
// reference pose). The static route cannot photograph one: a skinned mesh's static buffer is empty by design,
// so ThumbnailSubject::ResolveMesh refuses it as "no drawable geometry". This resolver finds the same handle
// and readiness the static one does, and the capture poses it (AssetThumbnailRenderer::RequestPose).
// A file of its own because ThumbnailSubject belongs to the UI tree.
namespace Desert::Editor::ThumbnailPose
{
    /// The `.skmesh` at @p skinnedPath, registered and built, ready for a pose capture. The picture is filed
    /// under the file itself (a `.skmesh` is its own cooked form, so CookedPath == @p skinnedPath) and judged
    /// against it. Answers Pending — never a refusal — while the read is on a worker. Refuses, naming why:
    /// the file cannot be created as a skinned mesh, the build is not drawable, or the built mesh is not skinned.
    [[nodiscard]] Common::ResultStr<ThumbnailSubject::Mesh> ResolveSkinnedMesh( Assets::AssetManager& manager,
                                                                                const std::string& skinnedPath );

    /// THE ONE ROUTE OF A POSED PICTURE (THM-FIXB; UE: each class its own thumbnail renderer, all three on the
    /// skeletal-mesh scene). By the subject's kind:
    ///   * `.skmesh`   — ResolveSkinnedMesh: the mesh itself, bind pose;
    ///   * `.skeleton` — its preview mesh (ContentRegistry::PreviewMeshRow of the skeleton's Rig tag), bind pose;
    ///   * `.anim`     — its skeleton's preview mesh, the clip (read on a worker) at its middle frame.
    /// The picture is filed under @p subjectPath (CookedPath == @p subjectPath) and judged against it, so a
    /// skeleton's and a clip's pictures never collide with their mesh's. Pending while the mesh or the clip is
    /// read; a refusal names the kind and why (no rig tag, no mesh on the rig, a clip that will not read).
    [[nodiscard]] Common::ResultStr<ThumbnailSubject::Mesh> ResolvePoseSubject( Assets::AssetManager& manager,
                                                                                const std::string&    subjectPath );
} // namespace Desert::Editor::ThumbnailPose
