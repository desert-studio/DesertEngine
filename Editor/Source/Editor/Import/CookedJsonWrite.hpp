#pragma once

#include <Engine/Assets/ContentRegistry.hpp>

#include <Common/Core/Core.hpp> // BOOLSUCCESS
#include <Common/Core/ResultStr.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <filesystem>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>

namespace Desert::Editor
{
    // ONE COPY OF THIS FUNCTION (Д35). It stood BYTE FOR BYTE IDENTICAL in ImportManager.cpp and in
    // TextureImporter.cpp, both spelled `Desert::Editor::WriteJsonToFile` — two definitions of one
    // external-linkage template, and one hole present in both. That duplication is why the hole is
    // worth naming rather than only patching: a defect fixed in one copy comes back through the other,
    // and neither copy's reader can tell there is another.
    //
    // WHAT THE HOLE WAS. Both copies opened a std::ofstream, threw on a failed OPEN, then did
    // `out << json` and returned void. Nothing was checked after the insertion, so whether the bytes
    // reached the disk depended on whether the payload happened to exceed the filebuf — and the
    // importer carried on cooking as though the `.stmesh` / `.tex` metadata existed. It goes through
    // the write primitive now, which closes before it decides.
    //
    // THE THROW IS GONE WITH IT. `throw std::runtime_error` from here ran on a JobSystem worker
    // (ImportAllFromDirectory cooks in parallel) with nothing catching it, so a read-only cooked
    // directory terminated the editor. The refusal is a value now, like everywhere else in this tree.
    //
    // The filename is sanitised because a source name may legally contain characters Windows refuses
    // in a path. NOTE, and it is not this function's to fix: the freshness check in
    // ImportManager::Import asks about the UNSANITISED path, so a source whose name needs sanitising
    // re-cooks on every scan.
    // THE BYTES HALF, SPLIT OUT WHEN `.stmesh`/`.skmesh` STOPPED BEING TEXT (B11). Everything this
    // header's comment describes — the sanitised filename, the checked atomic write, the registry row —
    // is about a cooked FILE and not about JSON, and a cooked mesh is now a binary container. Splitting
    // is what keeps the hole described above closed in ONE place for both payload kinds rather than
    // reopening it in a second copy, which is the mistake the Д35 note is about.
    [[nodiscard]] inline Common::BoolResultStr WriteCookedBytes( const std::string&           bytes,
                                                                 const std::filesystem::path& path )
    {
        static const std::regex illegal( R"([<>:"/\\|?*])" );

        std::error_code ec;
        std::filesystem::create_directories( path.parent_path(), ec );

        const std::filesystem::path fixedPath =
             path.parent_path() / std::regex_replace( path.filename().string(), illegal, "_" );

        if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( fixedPath, bytes );
             !written )
        {
            return Common::MakeFormattedError<bool>( "cooked metadata '{}' ({} bytes) was not written: {}",
                                                     fixedPath.string(), bytes.size(), written.GetError() );
        }

        // THE COOKED FILE ENTERS THE CONTENT REGISTRY THE MOMENT IT EXISTS, and this is the only place
        // a `.stmesh`, `.skmesh`, `.skeleton`, `.anim` or `.tex` ever comes into being. Since T2.4 the
        // boot no longer walks the cooked tree, so a file written here and not entered here would be a
        // file the next session does not have — which is precisely the "on disk no longer means
        // shipped" hazard GAP_ANALYSIS T2.7 names. Recording it at the write, rather than by a scan
        // somebody has to remember to run, is the same argument `AssetHandle::FromCookedPath` makes for
        // recording its own inverse where the number is derived.
        //
        // AFTER the write and only on success: a row naming a file that was not written would send the
        // loader to a path it cannot read, once per boot, for ever.
        Assets::ContentRegistry::NoteFile( fixedPath );

        return BOOLSUCCESS;
    }

    template <typename T>
    [[nodiscard]] Common::BoolResultStr WriteCookedJson( const T& data, const std::filesystem::path& path )
    {
        return WriteCookedBytes( rfl::json::write( data ), path );
    }
} // namespace Desert::Editor
