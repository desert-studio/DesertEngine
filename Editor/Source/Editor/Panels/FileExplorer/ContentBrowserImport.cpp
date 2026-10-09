#define NOMINMAX // engine headers below use std::min/max; keep the windows.h macros out

#include "ContentBrowserImport.hpp"

#include <Editor/Import/TextureDnD.hpp>
#include <Editor/Panels/FileExplorer/DirectoryInformation.hpp>
#include <Editor/Panels/FileExplorer/FileType.hpp>
#include <Editor/Platform/DesktopPlatform.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <system_error>

// Port pattern: UE Editor/ContentBrowser/Private/DragDropHandler.cpp (an external drop is an import into the
// target folder) and the toolbar's Import (a picker feeding the same import).
namespace Desert::Editor::ContentBrowserImport
{
    bool ImportFile( Assets::AssetManager* assetManager, const std::filesystem::path& source,
                     const DirectoryInformation* folder )
    {
        std::error_code ec;
        if ( !assetManager || source.empty() || !std::filesystem::exists( source, ec ) ||
             std::filesystem::is_directory( source, ec ) )
            return false;

        // Copy into the open folder (assets must live under Resources/ so the cook paths stay project-relative).
        const std::filesystem::path texDir = Common::Constants::Path::TEXTUREDIR_PATH;
        std::filesystem::path destDir = folder != nullptr ? std::filesystem::path( folder->AssetPath ) : texDir;
        if ( !std::filesystem::is_directory( destDir ) )
            destDir = texDir;
        std::filesystem::create_directories( destDir, ec );

        const std::filesystem::path dest = destDir / source.filename();
        std::filesystem::copy_file( source, dest, std::filesystem::copy_options::overwrite_existing, ec );
        if ( ec )
        {
            LOG_ERROR( "Import: failed to copy '{}' -> '{}': {}", source.string(), dest.string(), ec.message() );
            return false;
        }

        // Textures cook + register immediately (instantly usable / draggable). Other files just appear in the
        // browser (meshes cook on next launch / Rebuild Cooked).
        std::string ext = dest.extension().string();
        if ( !ext.empty() && ext[0] == '.' )
            ext = ext.substr( 1 );
        std::transform( ext.begin(), ext.end(), ext.begin(),
                        []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
        const FileType type = FileTypeOf( ext );
        if ( type == FileType::Texture || type == FileType::Cubemap )
            TextureDnD::ResolveOrImport( *assetManager, dest.generic_string() );
        return true;
    }

    bool ImportTextureFromDialog( Assets::AssetManager* assetManager, const DirectoryInformation* folder )
    {
        const auto picked =
             DesktopPlatform::OpenFileDialog( "Images\0*.png;*.tga;*.jpg;*.jpeg;*.bmp;*.hdr\0All\0*.*\0" );
        return !picked.empty() && ImportFile( assetManager, picked, folder );
    }
} // namespace Desert::Editor::ContentBrowserImport
