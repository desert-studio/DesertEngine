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
} // namespace Desert::Editor::ThumbnailPose
