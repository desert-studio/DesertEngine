#pragma once

#include <Common/Core/Constants.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <string_view>

namespace Common::Content
{
    // THE CENSUS OF WHAT THIS ENGINE CALLS CONTENT: every kind of file a scan can bring into the
    // project, where it lives, and what extension names it.
    //
    // ── WHY IT EXISTS, AND WHAT IT REPLACED ───────────────────────────────────────────────────────
    //
    // These seventeen (root, extension) pairs were sixteen `ProcessAssetFiles<T>( ROOT, EXTENSIONS, ... )`
    // call sites in `AssetPreloader.cpp` and nowhere else. That was survivable while the boot walked
    // the directories itself — the call site WAS the list. It stopped being survivable with the cooked
    // registry (GAP_ANALYSIS T2.4): the cook now enumerates content and the boot reads a file, so the
    // producer and the consumer are in different programs, and a kind known to one and not the other
    // is content that exists in the editor and is absent from the shipped game. That is precisely the
    // shape `PackagedContentTrees.hpp` was written for after the packager forgot fonts and icons and
    // no `.ttf` reached a built game.
    //
    // ── WHAT THE ROW IS, AND WHAT IT IS NOT ───────────────────────────────────────────────────────
    //
    // A row says WHERE a kind is enumerated from and WHAT names it. It says nothing about the C++ class
    // that loads it, the priority it loads at, or whether it loads eagerly — all three are properties
    // of the call site and two of them differ between kinds that share everything else here. Putting
    // them in this table would mean a template parameter in a constexpr row, which is not expressible,
    // and would turn a census anyone can read into a dispatch mechanism.
    //
    // ── WHY IT IS IN `Common` AND NOT BESIDE THE LOADER ───────────────────────────────────────────
    //
    // Four programs have to agree on this list and only one of them links the engine: the editor's cook
    // writes the rows, both hosts read them, the standalone `AssetRegistryTool` bootstraps a project
    // that has never been cooked (Tools link `Common` only — see Tools/PakTool/premake5.lua), and the
    // cook gate in CI holds the list against the content tree. A census one of them could not include
    // would be a census with a copy, which is the shape this file exists to end.
    //
    // It carries NO asset class and no priority. Those are properties of the call site — a template
    // argument is not data — and keeping them out is what lets the census live below the engine.
    //
    // ── THE ROOT IS A POINTER TO THE LIVE CONSTANT ────────────────────────────────────────────────
    //
    // Taking the address of one of `Constants::Path`'s named views is supported and survives
    // `SetProjectRoot` (Constants.hpp says so beside them, and `AssetHandle`'s own root table and the
    // packager's tree census both do it). A row therefore follows a project switch for free, where a
    // copied path would have frozen the sandbox's layout into this header.
    enum class ContentKind : std::size_t
    {
        StaticMesh,
        SkinnedMesh,
        Skeleton,
        Animation,
        Texture,
        Material,
        Skybox,
        Shader,
        CloudNoiseVolume,
        CloudType,
        CloudModellingVolume,
        CloudLayout,
        UITheme,
        ControlRig,
        AnimGraph,
        Retarget,
        StringTable,
        COUNT,
    };

    inline constexpr std::size_t CONTENT_KIND_COUNT = static_cast<std::size_t>( ContentKind::COUNT );

    struct ContentKindSpec
    {
        // The name written into the registry's `kind` column and read back out of it. One word, and
        // it is the ONE spelling: `Common::Utils::AssetRegistry` holds it as an opaque string because
        // `Common` must not learn the engine's content taxonomy, so this array is where the taxonomy
        // and its spelling meet.
        std::string_view Name;
        // Lower case, with the dot, as `std::filesystem::path::extension()` produces it.
        std::string_view Extension;
        // The live constant the kind is enumerated from.
        const std::filesystem::path* Root;
    };

    // Index-matched to ContentKind. Adding an enumerator without a row here does not compile, which is
    // the entire reason the enum carries COUNT.
    inline std::array<ContentKindSpec, CONTENT_KIND_COUNT> ContentKinds()
    {
        namespace P = Constants::Path;
        namespace E = Constants::Extensions;
        return { {
             /* StaticMesh           */ { "StaticMesh", E::STATIC_MESH, &P::MESH_PATH_COOKED },
             /* SkinnedMesh          */ { "SkinnedMesh", E::SKINNED_MESH, &P::MESH_PATH_COOKED },
             /* Skeleton             */ { "Skeleton", ".skeleton", &P::MESH_PATH_COOKED },
             /* Animation            */ { "Animation", ".anim", &P::MESH_PATH_COOKED },
             /* Texture              */ { "Texture", ".tex", &P::TEXTURE_PATH_COOKED },
             // Materials are editable CONTENT (the project's Materials/ dir): imported per-mesh
             // subfolders and editor-created files both land there, in the unified .demat format.
             /* Material             */ { "Material", E::MATERIAL_EXTENSION, &P::MATERIAL_PATH },
             /* Skybox               */ { "Skybox", ".hdr", &P::SKYBOX_PATH },
             /* Shader               */ { "Shader", ".shader", &P::SHADERDIR_PATH },
             // The four cloud kinds. CloudNoiseVolume's root is `Clouds/` itself, which CONTAINS the
             // other three roots — that is not a mistake to tidy up: the extension is what separates
             // them, and narrowing this root would change which files the engine finds.
             /* CloudNoiseVolume     */ { "CloudNoiseVolume", ".dcnv", &P::CLOUD_NOISE_PATH },
             /* CloudType            */ { "CloudType", ".decloudtype", &P::CLOUD_TYPE_PATH },
             /* CloudModellingVolume */ { "CloudModellingVolume", ".dcmv", &P::CLOUD_VOLUME_PATH },
             /* CloudLayout          */ { "CloudLayout", ".dclayout", &P::CLOUD_LAYOUT_PATH },
             /* UITheme              */ { "UITheme", ".detheme", &P::UI_THEME_PATH },
             /* ControlRig           */ { "ControlRig", ".derig", &P::CONTROL_RIG_PATH },
             /* AnimGraph            */ { "AnimGraph", ".danimgraph", &P::ANIM_GRAPH_PATH },
             /* Retarget             */ { "Retarget", ".retarget", &P::RETARGET_PATH },
             /* StringTable          */ { "StringTable", ".destrings", &P::LOCALIZATION_PATH },
        } };
    }

    inline const ContentKindSpec& KindSpec( ContentKind kind )
    {
        // A function-local static rather than a namespace-scope one, for `AssetPathIndex`'s reason:
        // the rows hold references into `Constants::Path`'s derived storage, which is itself an inline
        // variable, and first-use construction is the one spelling with no initialisation order to get
        // wrong.
        static const std::array<ContentKindSpec, CONTENT_KIND_COUNT> kinds = ContentKinds();
        return kinds[static_cast<std::size_t>( kind )];
    }

    inline std::string_view KindName( ContentKind kind )
    {
        return KindSpec( kind ).Name;
    }
} // namespace Common::Content
