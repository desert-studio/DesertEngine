#pragma once

#include <memory>

namespace Desert::Assets
{
    class AssetManager;
} // namespace Desert::Assets

namespace Desert::Animation
{
    class AnimationLibrary;
} // namespace Desert::Animation

namespace Desert::Editor
{
    class DocumentHost;
    class SceneWorkspace;

    // WHICH EDITOR OPENS WHICH KIND OF SUBJECT, AND HOW A PATH BECOMES ONE OF THEM. Fills
    // DocumentHost::SubjectEditors() with every asset editor (factory + presence test + name + icon) and every
    // path opener, in one fixed order. UE: each asset editor registers its AssetTypeActions with the
    // AssetTools registry at module startup; here the editors' registrations are one function called once
    // from EditorLayer::OnAttach.
    //
    // Every argument is a member of the caller and is captured BY REFERENCE in the registered factories, so
    // the caller must outlive the registry (it does: the registry is a member of the same host). The smart
    // pointers are read at open time, not at registration, exactly as the factories always did.
    void RegisterAssetEditors( DocumentHost& documents, SceneWorkspace& workspace,
                               std::shared_ptr<Assets::AssetManager>&              assetManager,
                               const std::unique_ptr<Animation::AnimationLibrary>& animationLibrary );
} // namespace Desert::Editor
