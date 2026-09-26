#pragma once

#include "Mesh/MeshAsset.hpp"
#include "Mesh/SurfaceMaterialAsset.hpp"
#include "Skybox/SkyboxAsset.hpp"

#include <Engine/Animation/AnimationLibrary.hpp>
#include <Engine/Assets/ItemProgress.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Assets
{
    class AssetPreloader
    {
    public:
        // THE LIBRARY IS A CONSTRUCTOR PARAMETER, not something a host fills afterwards, and that is the
        // decision this class exists to carry. Every other content kind here is scanned and then published
        // to its service in the same function; animation clips were the one exception — scanned here,
        // published by the editor layer in a loop of its own and by the runtime layer not at all — and the
        // exception is the whole defect. A REFERENCE rather than a pointer: a preloader with no library to
        // fill is not a state anything should be able to construct.
        AssetPreloader( const std::shared_ptr<AssetManager>& assetManager,
                        Animation::AnimationLibrary&         animationLibrary );

        // A `PreloadAllAssets()` USED TO SIT HERE and it is why the painted layout was dead for a
        // month. It called all seven preloads in one line, so the class LOOKED like it had an entry
        // point — and nothing had called it since 2023: both layers list the preloads themselves, in an
        // order they need to control. PreloadCloudLayouts was added to that dead function and to nowhere
        // else, so every `.dclayout` in the project went unregistered and every painted sky rendered
        // procedurally with one error line nobody read. Deleted rather than fixed: a second way to start
        // the asset layer, which nobody runs, is the thing that hid the omission.
        //
        // What replaced it as a guard is Desert/Tests/Editor/AssetPreloadCensus, which asserts that every
        // Preload* declared below is called by both EditorLayer and RuntimeLayer.

        // Re-process + re-register cooked meshes/textures/materials (Rebuild Cooked Assets). Re-registering
        // reloads texture pixels from source and rebuilds runtime materials; callers must idle the GPU
        // first and clear cached per-entity material instances after. Skips shaders/skyboxes (no IBL re-bake).
        void ReloadCooked();

        // Individual stages — public so the editor's staged startup loader can run them one per frame
        // behind a progress overlay. ORDER MATTERS: shaders must be loaded before any render system is
        // constructed (default PBR materials resolve their shader in the constructor).
        // SIX ASSET KINDS UNDER THREE ROOTS, and the name says so because it used to say "meshes" and
        // walked static meshes, cooked textures, animations, skeletons, project materials and skinned
        // meshes. A name that promises less than the code does is how a reader comes to believe there is
        // a second scan somewhere for the other five.
        //
        // Kept as ONE function rather than split into honest halves, and that is a decision with a
        // reason: the six scans must all precede the three register loops at its tail, and those loops
        // carry a stated order dependency (textures before materials, or a runtime material rebuilds
        // against images that are about to be replaced). Splitting it would move that ordering
        // constraint out into two startup sequences in two different layers, where nothing states it.
        //
        // @p progress names each registry row as it is created (the splash's item line).
        void PreloadCookedAssetsAndMaterials( const ItemProgress& progress = {} );
        // The number of registry rows `PreloadCookedAssetsAndMaterials` works through — the splash weighs
        // the stage by it before the stage begins.
        static std::size_t CookedAssetRowCount();
        void               PreloadSkyboxes();
        // @p progress names each shader program as it is compiled; `ShaderRowCount` is how many there are.
        // @p stop is asked before each program; true ends the preload there, the rest unregistered.
        void               PreloadShaders( const ItemProgress& progress = {}, const StopRequested& stop = {} );
        static std::size_t ShaderRowCount();
        // Cloud noise volumes (`.dcnv`). Scanned so the type asset's slot can offer them by name and so a
        // type that names one finds it already loaded; no GPU work happens here, the renderer uploads.
        void PreloadCloudNoiseVolumes();
        // Cloud types (`.decloudtype`). MUST run after PreloadCloudNoiseVolumes: a type names its noise
        // volume by path and binds it in ResolveDependencies, which the AssetManager calls the moment the
        // type is created — a volume that is not in the manager yet resolves to nothing and the type
        // renders with the default edge instead of its own.
        void PreloadCloudTypes();
        // Sculpted hero-cloud bodies (`.dcmv`). Independent of the two above — a body names no other
        // asset and no other asset names it — so its position in the order is free; it is last because
        // a scene without one still has a sky and this is the stage a project may have nothing in.
        void PreloadCloudModellingVolumes();
        // Painted cloud layouts (`.dclayout`). Independent of the three above — a layout names no other
        // asset and no other asset names it — so its position in the order is free; it is last because a
        // scene without one still has a sky, which is the state every shipped scene is in.
        void PreloadCloudLayouts();
        // UI themes (`.detheme`). MUST run after the font scan the FontService does on demand is
        // reachable — it is, because the service registers a font path the moment it is asked — and it is
        // independent of every cloud stage above. A canvas without a theme still draws, which is the
        // state every scene authored before themes existed is in.
        void PreloadUIThemes();
        // String tables (`.destrings`). Independent of everything above — a table names no other asset and
        // no other asset names it. Loading one PUBLISHES it to the process's localisation lookup, which is
        // why there is no register loop beside this call the way the cloud stages have one.
        void PreloadStringTables();
        // Control rigs (`.derig`). Independent of everything above — a rig names no other asset and no
        // other asset names it. Scanned rather than left to the scene's own on-demand load so the
        // entity's rig slot can OFFER them by name: a picker that can only show what a scene already
        // names is a picker that can never be used to pick a different rig.
        void PreloadControlRigs();

        // Anim graphs (`.danimgraph`). Independent of every other preload: a graph names no asset and is
        // named by a component slot, so nothing orders it against the rigs or the clips.
        void PreloadAnimGraphs();

        // Retargets (`.retarget`). AFTER the cooked scan in both layers, and that ordering is the one thing
        // this preload has that the two above do not: a `.retarget` names its SOURCE RIG by signature, and
        // `RetargetAsset::ResolveDependencies` can only find that rig among the `SkeletonAsset`s the cooked
        // scan has registered. Run before it, and every retarget in the project binds to nothing — and
        // because the dependency is only re-resolved on a later `EnsureLoaded`, it would recover only by
        // accident. That is the `PreloadCloudLayouts` shape one step along: not an uncalled function, but a
        // function called at a moment that cannot work.
        void PreloadRetargets();

    private:
        std::weak_ptr<AssetManager> m_AssetManager;

        // Non-owning: the library belongs to the layer, which outlives its preloader. Never null — the
        // constructor takes a reference.
        Animation::AnimationLibrary* m_AnimationLibrary;
    };
} // namespace Desert::Assets