#pragma once

#include <Common/Core/Constants.hpp>

#include "../IAssetImporter.hpp"
#include "../Assimp/AssimpImporter.hpp"
#include "BlendConvertScript.hpp"

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace Desert::Editor
{
    // Blender (.blend) support. Assimp's native .blend loader reads raw SDNA and is unreliable on modern
    // Blender files, so — like UE/Unity/Godot — we don't parse .blend directly: we CONVERT it to an
    // intermediate FBX by driving Blender headless (`blender --background ... --python convert.py`), then hand
    // that FBX to the normal AssimpImporter. The cooked outputs end up keyed off the ORIGINAL .blend path
    // (ImportManager serializes the returned ImportResult against the source it was given), so the .blend is a
    // first-class source just like an .fbx. REQUIRES Blender installed (set BLENDER_PATH or install to the
    // default Program Files location).
    class BlendImporter : public IAssetImporter
    {
    public:
        ImportResult Import( const std::filesystem::path& blendPath, ImportManager& manager ) override
        {
            const std::filesystem::path fbx = ConvertToFbx( blendPath );
            if ( fbx.empty() )
                return {}; // conversion failed (no Blender / export error) — already logged

            AssimpImporter assimp;
            return assimp.Import( fbx, manager );
        }

    private:
        // Where converted FBXs (and their copied textures) live — a generated intermediate tree, per-source
        // subfolder so texture copies from different .blends never collide.
        static std::filesystem::path IntermediateFbx( const std::filesystem::path& blendPath )
        {
            const std::string stem = blendPath.stem().string();
            return Common::Constants::Path::COOKED_PATH / "BlendConvert" / stem / ( stem + ".fbx" );
        }

        // Run Blender once to export <blend> -> <fbx>. Cached: if the FBX is already newer than the .blend we
        // reuse it (the expensive Blender launch is skipped). Returns the FBX path, or empty on failure.
        static std::filesystem::path ConvertToFbx( const std::filesystem::path& blendPath )
        {
            namespace fs = std::filesystem;
            std::error_code ec;

            const fs::path fbx = IntermediateFbx( blendPath );

            // Up-to-date conversion already on disk?
            if ( fs::exists( fbx, ec ) )
            {
                const auto fbxT   = fs::last_write_time( fbx, ec );
                const auto blendT = fs::last_write_time( blendPath, ec );
                if ( !ec && fbxT >= blendT )
                    return fbx;
            }

            const fs::path blender = FindBlenderExe();
            if ( blender.empty() )
            {
                LOG_ERROR( "[Blend] Blender not found — cannot import '{}'. Install Blender or set the "
                           "BLENDER_PATH environment variable to blender.exe.",
                           blendPath.string() );
                return {};
            }

            fs::create_directories( fbx.parent_path(), ec );
            const fs::path script = WriteConvertScript();
            if ( script.empty() )
                return {};

            const fs::path log = fbx.parent_path() / "_blender_convert.log";

            // cmd /S /C "<...>" : with /S, cmd strips ONLY the outer quote pair and runs the rest verbatim, so
            // the inner per-argument quotes survive (paths may contain spaces). std::system wraps in cmd /c.
            auto q = []( const fs::path& p ) { return "\"" + p.string() + "\""; };
            const std::string command = "\"" + q( blender ) + " --background " + q( blendPath ) +
                                        " --factory-startup --python " + q( script ) + " -- " + q( fbx ) +
                                        " > " + q( log ) + " 2>&1\"";

            LOG_INFO( "[Blend] converting '{}' -> FBX via {}", blendPath.string(), blender.string() );
            const int rc = std::system( command.c_str() );

            if ( !fs::exists( fbx, ec ) )
            {
                LOG_ERROR( "[Blend] conversion failed (rc={}) for '{}'. See {}", rc, blendPath.string(),
                           log.string() );
                return {};
            }
            return fbx;
        }

        // Locate blender.exe. Priority: explicit BLENDER_PATH (file OR folder) -> newest version under the
        // default Blender Foundation installer folder -> common Steam library locations (Blender ships on Steam).
        static std::filesystem::path FindBlenderExe()
        {
            namespace fs = std::filesystem;
            std::error_code ec;

            if ( const char* env = std::getenv( "BLENDER_PATH" ) )
            {
                fs::path p( env );
                if ( fs::is_directory( p, ec ) ) // allow pointing at the install folder
                    p /= "blender.exe";
                if ( fs::exists( p, ec ) )
                    return p;
            }

            // C:\Program Files\Blender Foundation\Blender X.Y\blender.exe — choose the latest version folder.
            for ( const char* root : { "C:/Program Files/Blender Foundation",
                                       "C:/Program Files (x86)/Blender Foundation" } )
            {
                if ( !fs::exists( root, ec ) )
                    continue;
                fs::path best;
                for ( const auto& dir : fs::directory_iterator( root, ec ) )
                {
                    if ( !dir.is_directory() )
                        continue;
                    const fs::path exe = dir.path() / "blender.exe";
                    if ( fs::exists( exe, ec ) && exe.string() > best.string() )
                        best = exe; // lexicographic ~ version order ("Blender 4.2" > "Blender 3.6")
                }
                if ( !best.empty() )
                    return best;
            }

            // Steam install (e.g. D:\Steam\steamapps\common\Blender\blender.exe). Steam libraries can be on any
            // drive / under a few common folder names — probe the usual ones. (For anything exotic: BLENDER_PATH.)
            for ( char drive = 'C'; drive <= 'H'; ++drive )
            {
                for ( const char* sub : { "Steam/steamapps/common/Blender",
                                          "SteamLibrary/steamapps/common/Blender",
                                          "Program Files (x86)/Steam/steamapps/common/Blender",
                                          "Games/SteamLibrary/steamapps/common/Blender" } )
                {
                    const fs::path exe =
                         fs::path( std::string( 1, drive ) + ":/" ) / sub / "blender.exe";
                    if ( fs::exists( exe, ec ) )
                        return exe;
                }
            }
            return {};
        }

        // Write the export script once (idempotent). Exports meshes + armatures + animations, applies
        // modifiers, and COPIES textures next to the FBX so the AssimpImporter's filename fallback finds
        // them. The script and the write live in BlendConvertScript.hpp — this class drags Assimp in with
        // it, and that was what kept the write's refusal branch out of reach of every test binary.
        static std::filesystem::path WriteConvertScript()
        {
            return WriteBlendConvertScript( Common::Constants::Path::COOKED_PATH / "BlendConvert/_convert.py" );
        }
    };
} // namespace Desert::Editor
