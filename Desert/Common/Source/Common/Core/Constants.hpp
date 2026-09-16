#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace Common::Constants
{
    namespace Path
    {
        // Layout (UE-like): engine/editor resources (Shaders, Fonts) live under the shared Resources/
        // tree next to the binary's working directory; all USER CONTENT lives under the PROJECT's assets
        // root. By default (no project) the content paths point at Resources/Assets/ — the built-in
        // sandbox; opening a .deproj calls SetProjectRoot() and REMAPS every content path (and the
        // Cooked/ intermediate tree) into the project folder. Engine resources are never remapped.
        //
        // This file used to hold each content path as its own mutable global, all seventeen rewritten
        // one assignment at a time by SetProjectRoot — seventeen chances to forget one, and nothing but
        // the order of calls keeping them consistent. Now the only stored state is the PROJECT ROOT
        // PAIR, and every directory is DERIVED from it through one census below: the relation
        // "path = root / relative part" holds for all of them at once, by construction, and a new
        // directory cannot exist outside the census because the census row is what brings it into being.
        //
        // What is deliberately NOT here: a configuration file. The owner asked whether these belong in
        // one, and the answer after counting is that there is nothing to put in it — the single real
        // per-project setting, the assets root name, already lives in the .deproj (ProjectFile
        // AssetsRoot), and the folder names below that root are the engine's layout, not a user's
        // choice. Turning them into fourteen .deproj fields nobody asked for would be dead settings.
        // REVISIT CONDITION: the day a project genuinely needs to rename or relocate one of these
        // folders, that folder's census row grows a .deproj override — not before.

        // --- Engine / editor resources (SHARED, never remapped) ---
        inline const std::filesystem::path RESOURCE_PATH  = "Resources/";
        inline const std::filesystem::path SHADERDIR_PATH = "Resources/Shaders/";
        inline const std::filesystem::path FONTS_PATH     = "Resources/Fonts/";
        // Built-in vector icons (.svg, imported into SDF at first use — see Runtime::IconService).
        inline const std::filesystem::path ICONS_PATH = "Resources/Icons/";

        // --- The census of project-derived directories ---

        // Every directory that moves with the project, by name. A path that is not a row here cannot be
        // project content: Dir() takes this enum, the storage is sized by it, and the spec table below
        // is index-matched to it — adding a directory WITHOUT extending all three does not compile,
        // which is the point. (The old shape allowed an eighteenth variable that SetProjectRoot did not
        // know about; this shape cannot express one.)
        enum class ContentDir : std::size_t
        {
            Assets,
            Mesh,
            Material,
            Texture,
            Skybox,
            Scene,
            Prefab,
            Script,
            Collections,
            Localization,
            CloudNoise,
            CloudType,
            CloudVolume,
            CloudLayout,
            UITheme,
            ControlRig,
            Cooked,
            MeshCooked,
            TextureCooked,
            COUNT
        };

        inline constexpr std::size_t CONTENT_DIR_COUNT = static_cast<std::size_t>( ContentDir::COUNT );

        // The two roots a project-derived directory can hang off. Assets = <projectDir>/<assetsRoot from
        // the .deproj>; Cooked = <projectDir>/Cooked — generated intermediates, deliberately OUTSIDE the
        // assets tree so a content scan never offers a cooked twin as authorable content.
        enum class DirRoot : unsigned char
        {
            Assets,
            Cooked,
        };

        // The Cooked tree's own name under the project directory. One spelling, used by the derivation
        // and by the relation test; not a setting (see the refusal above).
        inline constexpr std::string_view COOKED_DIR_NAME = "Cooked";

        struct ContentDirSpec
        {
            std::string_view Rel;  // relative part under the root; "" names the root itself
            DirRoot          Root; // which root the relative part is joined to
        };

        // Index-matched to ContentDir. The relative parts here are the ONE spelling of the project
        // layout — the engine reads through Dir()/the named views, and anything that scaffolds these
        // folders on disk must take its names from these rows rather than carry its own list.
        //
        // Clouds/ commentary, kept from the seventeen-variable era because the layout it explains is
        // unchanged: cloud noise volumes (`.dcnv`) get their own folder rather than Textures/ because a
        // volume is not a texture to this engine (no importer, no cooked twin, no 2D preview), and
        // mixing it into the texture scan would offer it in every texture slot. Types (`.decloudtype`),
        // sculpted volumes (`.dcmv`) and painted layouts (`.dclayout`) are SUBFOLDERS of Clouds/ because
        // the four are one body of content — an artist who opens Clouds/ should see every half of what
        // makes a sky — while each is scanned separately so no kind can be offered in another's slot.
        inline constexpr std::array<ContentDirSpec, CONTENT_DIR_COUNT> CONTENT_DIRS = { {
             /* Assets        */ { "", DirRoot::Assets },
             /* Mesh          */ { "Meshes/", DirRoot::Assets },
             /* Material      */ { "Materials/", DirRoot::Assets },
             /* Texture       */ { "Textures/", DirRoot::Assets },
             /* Skybox        */ { "Textures/HDR/", DirRoot::Assets },
             /* Scene         */ { "Scenes/", DirRoot::Assets },
             /* Prefab        */ { "Prefabs/", DirRoot::Assets },
             /* Script        */ { "Scripts/", DirRoot::Assets },
             /* Collections   */ { "Collections/", DirRoot::Assets },
             /* Localization  */ { "Localization/", DirRoot::Assets },
             /* CloudNoise    */ { "Clouds/", DirRoot::Assets },
             /* CloudType     */ { "Clouds/Types/", DirRoot::Assets },
             /* CloudVolume   */ { "Clouds/Volumes/", DirRoot::Assets },
             /* CloudLayout   */ { "Clouds/Layouts/", DirRoot::Assets },
             // UI themes (`.detheme`) get their own folder under UI/ rather than sitting loose in the
             // assets root: a theme is scanned separately so no other kind can be offered in a canvas's
             // theme slot, and UI/ is where the rest of the UI's own content will land.
             /* UITheme       */ { "UI/Themes/", DirRoot::Assets },
             // Control rigs (`.derig`) get their own folder for the theme's reason: a rig is scanned
             // separately so no other kind can be offered in an entity's rig slot, and nothing else is a
             // rig. Beside Animations/ rather than inside it because a rig is not a clip — it is the thing
             // a clip's control tracks are authored against.
             /* ControlRig    */ { "Rigs/", DirRoot::Assets },
             /* Cooked        */ { "", DirRoot::Cooked },
             /* MeshCooked    */ { "Meshes/", DirRoot::Cooked },
             /* TextureCooked */ { "Textures/", DirRoot::Cooked },
        } };

        // --- Compile-time guards over the census (relations, not values — the Д27 pattern) ---

        namespace Detail
        {
            constexpr const ContentDirSpec& Spec( ContentDir d ) noexcept
            {
                return CONTENT_DIRS[static_cast<std::size_t>( d )];
            }

            constexpr bool EveryRelIsRelativeAndSlashTerminated() noexcept
            {
                for ( const auto& spec : CONTENT_DIRS )
                {
                    if ( !spec.Rel.empty() && ( spec.Rel.front() == '/' || spec.Rel.back() != '/' ) )
                        return false;
                }
                return true;
            }

            constexpr bool EachRootIsNamedExactlyOnce() noexcept
            {
                std::size_t assetsRoots = 0, cookedRoots = 0;
                for ( const auto& spec : CONTENT_DIRS )
                {
                    if ( spec.Rel.empty() )
                        ( spec.Root == DirRoot::Assets ? assetsRoots : cookedRoots ) += 1;
                }
                return assetsRoots == 1 && cookedRoots == 1;
            }
        } // namespace Detail

        static_assert( Detail::EveryRelIsRelativeAndSlashTerminated(),
                       "a census row must be a relative part ending in '/' (or \"\" for the root row) — an "
                       "absolute or unterminated part would silently change what root / rel concatenates to" );
        static_assert( Detail::EachRootIsNamedExactlyOnce(),
                       "exactly one census row must name each root itself (Rel == \"\"), or the roots have "
                       "no path of their own to read" );

        // The Clouds/ containment is a RELATION the comments used to merely describe: the three cloud
        // kinds live inside the noise volumes' folder so an artist sees one body of content. Moving one
        // out (or renaming Clouds/ in one row and not the others) keeps every line individually
        // plausible and stops compiling here.
        static_assert(
             Detail::Spec( ContentDir::CloudType ).Rel.starts_with( Detail::Spec( ContentDir::CloudNoise ).Rel ) &&
                  Detail::Spec( ContentDir::CloudVolume )
                       .Rel.starts_with( Detail::Spec( ContentDir::CloudNoise ).Rel ) &&
                  Detail::Spec( ContentDir::CloudLayout )
                       .Rel.starts_with( Detail::Spec( ContentDir::CloudNoise ).Rel ),
             "cloud types, sculpted volumes and painted layouts are one body of content and must "
             "stay inside the cloud noise volumes' folder" );
        static_assert( Detail::Spec( ContentDir::MeshCooked ).Root == DirRoot::Cooked &&
                            Detail::Spec( ContentDir::TextureCooked ).Root == DirRoot::Cooked,
                       "cooked twins are generated intermediates and must stay under the Cooked root, or a "
                       "content scan will offer them as authorable assets" );

        // --- The stored state and the derivation ---

        // The ONLY mutable state: where the project is, and what its assets root is called. Everything
        // else in this namespace is a pure function of these two.
        struct ProjectRootState
        {
            std::filesystem::path ProjectDir; // "" = no project (the built-in sandbox)
            std::filesystem::path AssetsRoot; // from the .deproj; the sandbox uses SANDBOX_ASSETS_ROOT
        };

        // The built-in sandbox's assets root, chosen so the no-project layout stays byte-identical to
        // the historical `Resources/Assets/` tree.
        inline constexpr std::string_view SANDBOX_ASSETS_ROOT = "Resources/Assets";

        namespace Detail
        {
            // Derives every census row from the root pair. THE only writer of the storage below —
            // per-row assignment (the old shape, and the old defect surface) is not expressible.
            inline std::array<std::filesystem::path, CONTENT_DIR_COUNT> Derive( const ProjectRootState& state )
            {
                const std::filesystem::path assets = ( state.ProjectDir / state.AssetsRoot ).lexically_normal();
                const std::filesystem::path cooked = ( state.ProjectDir / COOKED_DIR_NAME ).lexically_normal();

                std::array<std::filesystem::path, CONTENT_DIR_COUNT> dirs;
                for ( std::size_t i = 0; i < CONTENT_DIR_COUNT; ++i )
                {
                    const ContentDirSpec& spec = CONTENT_DIRS[i];
                    const auto&           root = spec.Root == DirRoot::Assets ? assets : cooked;
                    dirs[i]                    = root / spec.Rel; // Rel "" yields the root with a trailing '/'
                }
                return dirs;
            }

            inline ProjectRootState s_ProjectRoot{ "", std::filesystem::path( SANDBOX_ASSETS_ROOT ) };
            inline std::array<std::filesystem::path, CONTENT_DIR_COUNT> s_Dirs = Derive( s_ProjectRoot );
        } // namespace Detail

        // Where a project-derived directory currently is. The reference stays valid across remaps (the
        // storage is stable; SetProjectRoot assigns into it), which is what lets long-lived tables hold
        // `const std::filesystem::path*` and follow a project switch for free.
        inline const std::filesystem::path& Dir( ContentDir d ) noexcept
        {
            return Detail::s_Dirs[static_cast<std::size_t>( d )];
        }

        inline const ProjectRootState& CurrentProjectRoot() noexcept
        {
            return Detail::s_ProjectRoot;
        }

        // THE INVERSE OF Derive(), read through ONE census row: given a path to a file that lies inside
        // Dir(d), the ROOT that row hangs off — i.e. what the derivation must have been handed for Dir(d)
        // to contain this file.
        //
        // WHY IT EXISTS. Four defects in one day were the same sentence: a path resolved from the process's
        // WORKING DIRECTORY rather than from the file being worked on. The migration tool wrote a scene's
        // new material into a second `Resources/Assets/` tree at whatever directory it was launched from,
        // leaving two differing files with one name under one relative path in two roots; before it, scene
        // material paths went to disk absolute, a committed `.tex` carried one machine's home directory,
        // and a thumbnail key named a checkout. The answer is the same every time and it is a RELATION, not
        // a patch at each site: derive the root from the FILE, never from where the process is standing.
        //
        // Derive() answers that forwards, from a root that must already be known; this answers it backwards
        // from a path that IS known. Both read the same census row, so a layout change reaches the two
        // directions in one edit and they cannot come to disagree — the reason this lives here rather than
        // in the one tool that needed it first, which would have spelled `Scenes/` a second time.
        //
        // LEXICAL, and deliberately so: it consults no disk, so a caller can ask about a file it is only
        // about to create, and the answer cannot change with what happens to exist. The LAST occurrence of
        // the row's components wins, so `<root>/Scenes/Autosave/x.desce` resolves against the project's own
        // `Scenes/` and not against some `Scenes` further up a developer's home directory.
        //
        // An EMPTY path is a real answer and means "the root is the working directory" — it is what a
        // caller-relative spelling like `Scenes/x.desce` honestly implies, and joining onto it reproduces
        // that same caller-relative form rather than inventing an absolute one.
        //
        // std::nullopt in two cases, both of which are "no answer" rather than a guess: the path does not
        // lie under the row's relative part at all, and the two rows that name a root ITSELF (Rel == ""),
        // where a file somewhere inside a root says nothing about where that root begins.
        inline std::optional<std::filesystem::path> RootForContentPath( ContentDir                   d,
                                                                        const std::filesystem::path& contentPath )
        {
            const ContentDirSpec& spec = CONTENT_DIRS[static_cast<std::size_t>( d )];
            if ( spec.Rel.empty() )
                return std::nullopt;

            // A trailing separator makes the last component an empty string ("Scenes/" -> {"Scenes",""}),
            // and matching on it would match everywhere; "." carries no location at all.
            const auto Components = []( const std::filesystem::path& p )
            {
                std::vector<std::filesystem::path> parts;
                for ( const auto& part : p.lexically_normal() )
                {
                    if ( !part.empty() && part != "." )
                        parts.push_back( part );
                }
                return parts;
            };

            const std::vector<std::filesystem::path> rel = Components( std::filesystem::path( spec.Rel ) );
            const std::vector<std::filesystem::path> dir = Components( contentPath.parent_path() );
            if ( rel.empty() || dir.size() < rel.size() )
                return std::nullopt;

            std::size_t rootLength = dir.size() + 1; // > dir.size() means "no occurrence found"
            for ( std::size_t start = 0; start + rel.size() <= dir.size(); ++start )
            {
                if ( std::equal( rel.begin(), rel.end(), dir.begin() + static_cast<std::ptrdiff_t>( start ) ) )
                    rootLength = start;
            }
            if ( rootLength > dir.size() )
                return std::nullopt;

            std::filesystem::path root;
            for ( std::size_t i = 0; i < rootLength; ++i )
                root /= dir[i];
            return root;
        }

        // Points every content path at <projectDir>/<assetsRoot>/... and the cooked tree at
        // <projectDir>/Cooked/. Must be called BEFORE any subsystem reads the paths (the editor does it
        // while parsing --project, before the engine spins up). assetsRoot comes from the .deproj.
        inline void SetProjectRoot( const std::filesystem::path& projectDir,
                                    const std::filesystem::path& assetsRoot )
        {
            Detail::s_ProjectRoot = ProjectRootState{ projectDir, assetsRoot };
            Detail::s_Dirs        = Detail::Derive( Detail::s_ProjectRoot );
        }

        // Back to the no-project sandbox. Exists for tests, which used to restore the globals one
        // assignment at a time — the one write path this file no longer offers.
        inline void ResetToSandbox()
        {
            SetProjectRoot( "", std::filesystem::path( SANDBOX_ASSETS_ROOT ) );
        }

        // --- Named views (the spellings the codebase reads) ---
        //
        // References into the derived storage, const so the census cannot be bypassed: the only way to
        // move one of these is to move the project root they are all derived from. Taking the ADDRESS of
        // a view is supported and survives remaps — AssetHandle's root table, the runtime scan roots and
        // the packager's tree census all do exactly that.
        inline const std::filesystem::path& ASSETS_PATH         = Dir( ContentDir::Assets );
        inline const std::filesystem::path& MESH_PATH           = Dir( ContentDir::Mesh );
        inline const std::filesystem::path& MATERIAL_PATH       = Dir( ContentDir::Material );
        inline const std::filesystem::path& TEXTUREDIR_PATH     = Dir( ContentDir::Texture );
        inline const std::filesystem::path& SKYBOX_PATH         = Dir( ContentDir::Skybox );
        inline const std::filesystem::path& SCENE_PATH          = Dir( ContentDir::Scene );
        inline const std::filesystem::path& PREFAB_PATH         = Dir( ContentDir::Prefab );
        inline const std::filesystem::path& SCRIPT_PATH         = Dir( ContentDir::Script );
        inline const std::filesystem::path& COLLECTIONS_PATH    = Dir( ContentDir::Collections );
        inline const std::filesystem::path& LOCALIZATION_PATH   = Dir( ContentDir::Localization );
        inline const std::filesystem::path& CLOUD_NOISE_PATH    = Dir( ContentDir::CloudNoise );
        inline const std::filesystem::path& CLOUD_TYPE_PATH     = Dir( ContentDir::CloudType );
        inline const std::filesystem::path& CLOUD_VOLUME_PATH   = Dir( ContentDir::CloudVolume );
        inline const std::filesystem::path& CLOUD_LAYOUT_PATH   = Dir( ContentDir::CloudLayout );
        inline const std::filesystem::path& UI_THEME_PATH       = Dir( ContentDir::UITheme );
        inline const std::filesystem::path& CONTROL_RIG_PATH   = Dir( ContentDir::ControlRig );
        inline const std::filesystem::path& COOKED_PATH         = Dir( ContentDir::Cooked );
        inline const std::filesystem::path& MESH_PATH_COOKED    = Dir( ContentDir::MeshCooked );
        inline const std::filesystem::path& TEXTURE_PATH_COOKED = Dir( ContentDir::TextureCooked );
    } // namespace Path

    namespace Extensions
    {
        // `constexpr string_view` across the board, for the reason the two mesh extensions below state
        // at length: the compiler can then assert the relations. Extensions that had NO reader anywhere
        // (the three SPIRV_BINARY_EXTENSION_* spellings and MESH_SERIALIZBLE_EXTENSION — a name whose
        // own typo went unnoticed for years precisely because nothing read it) were deleted rather than
        // migrated; a constant without a reader is a trap, not documentation.
        constexpr std::string_view SCENE_EXTENSION    = ".desce";
        constexpr std::string_view MATERIAL_EXTENSION = ".demat";
        constexpr std::string_view PREFAB_EXTENSION   = ".deprefab";
        // st = STatic, sk = SKinned. These two were SWAPPED from the day they were written, and nothing
        // caught it because nothing read them: AssetPreloader carried its own literal arrays and was
        // right, so the cooker, the loaders and the tests all agreed with each other and disagreed with
        // this file in silence. The bug could only surface the moment someone trusted these names — i.e.
        // it was a trap armed for a future reader, not a defect anyone could observe.
        // AssetPreloader now consumes these, so the two spellings are one value again.
        //
        // They are `constexpr string_view` and not `const std::string` so the agreement between the NAME
        // and the LETTERS can be asserted by the compiler below. A runtime test would only fail once
        // someone ran it; this cannot be compiled wrong, which is the right strength for a value whose
        // only failure mode is a human reading two similar spellings too quickly.
        constexpr std::string_view STATIC_MESH  = ".stmesh";
        constexpr std::string_view SKINNED_MESH = ".skmesh";

        // The relation, not the values: whatever these become, `st` must mean STatic and `sk` SKinned.
        // Swapping the two right-hand sides above keeps every individual line looking correct — that is
        // exactly how they were wrong in the first place — and stops compiling here.
        static_assert( STATIC_MESH.starts_with( ".st" ), "STATIC_MESH must be the .st* spelling" );
        static_assert( SKINNED_MESH.starts_with( ".sk" ), "SKINNED_MESH must be the .sk* spelling" );
        static_assert( STATIC_MESH != SKINNED_MESH, "the two mesh extensions must stay distinct" );

        // Every extension names a distinct format and reads as one to std::filesystem::path::extension().
        static_assert( SCENE_EXTENSION.starts_with( '.' ) && MATERIAL_EXTENSION.starts_with( '.' ) &&
                            PREFAB_EXTENSION.starts_with( '.' ) && STATIC_MESH.starts_with( '.' ) &&
                            SKINNED_MESH.starts_with( '.' ),
                       "an extension without its leading dot never matches path::extension()" );
        static_assert( SCENE_EXTENSION != MATERIAL_EXTENSION && SCENE_EXTENSION != PREFAB_EXTENSION &&
                            MATERIAL_EXTENSION != PREFAB_EXTENSION,
                       "two formats sharing one extension would make every by-extension scan ambiguous" );
    } // namespace Extensions
} // namespace Common::Constants
