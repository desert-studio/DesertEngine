#pragma once

#include <Engine/Assets/AssetManager.hpp>

#include <memory>

namespace Desert::Animation
{
    class AnimationLibrary;
}

namespace Desert::Core
{
    class StartupProgress;
}

namespace Desert::Assets
{
    // THE CONTENT A HOST LOADS BEFORE ITS FIRST FRAME, AND NOTHING ELSE (AL1 plan §2.4(a)).
    //
    // What stood here was `AssetPreloader`: a class whose stages created a shell for every file of a kind at
    // boot — textures, materials, meshes, skeletons, clips, the cloud kinds, rigs, retargets and finally
    // skyboxes — so that a reference could be answered by looking in the manager. AL1-1…AL1-9 moved every one
    // of those kinds onto its content-registry row: a service creates the shell from `ContentRegistry::RowOf`
    // the first time something names the handle and reads it through AsyncAssetLoader. The class was deleted
    // with its last scan (skyboxes, AL1-9); what is left is the three things a host needs before it can draw
    // at all, and each is a free function both hosts call, because a class with one state and no invariant
    // is the shape that let a stage exist that no host called (`PreloadCloudLayouts`).
    //
    // `Desert/Tests/Editor/BootContentCensus` holds both hosts to calling every function declared here and
    // forbids a `Preload*` declaration anywhere in the engine, editor or runtime sources.

    /// Every `.shader` row (the engine's Shaders/ and the project's Content/Shaders/, one kind) created and
    /// registered with the ShaderService, compiling its programs. A registration the service refuses (a program
    /// name another file owns) is logged naming the file and is not counted.
    /// SYNCHRONOUS AND FIRST, which is §2.4(a)'s case: the render systems' default materials resolve their
    /// shaders in their constructors, so these must exist before any of them is built. A material's own
    /// shader graph is still compiled on demand when the material is. @p progress (the editor's; null in the
    /// Runtime) gets the three shader phases begun on it and is asked StopRequested before each program; a stop
    /// ends the compile there and leaves the rest unregistered (the splash's close button).
    /// REFUSED when no loaded shader is the `Default Surface` template (or the project's DefaultSurfaceTemplate
    /// names none): the engine's slot-less meshes and every new material are authored on it, so an engine without
    /// it is refused at its start, naming the role, rather than carrying a null default into the renderer (the
    /// "no config = error" rule). A stop request ends the compile with success — the host is closing.
    [[nodiscard]] Common::BoolResultStr CompileShaders( const std::shared_ptr<AssetManager>& manager,
                                                        Core::StartupProgress*               progress = nullptr );

    /// The animation library indexed from the registry's clip rows plus the built-in procedural clips.
    /// Nothing is read: a clip is loaded when an animator first names it (AL1-6). Also the re-index behind
    /// "Rebuild Cooked Assets". A refusal (clip rows exist and none reached the library) is logged.
    void IndexAnimationClips( AssetManager& manager, Animation::AnimationLibrary& library );

    /// The current language's string tables requested through AsyncAssetLoader (AL1-7b); the loading screen
    /// waits on them through ContentGate. A project with no `Localization/` folder requests nothing.
    void RequestStringTables( const std::weak_ptr<AssetManager>& manager );
} // namespace Desert::Assets
