#include "WindowsFileSystem.hpp"

#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <commdlg.h>
#include <shlobj.h>

namespace Common::Utils
{
    std::filesystem::path WindowsFileSystem::OpenFileDialog( const char* filter )
    {
        OPENFILENAMEA ofn;
        CHAR          szFile[260] = { 0 };

        ZeroMemory( &ofn, sizeof( OPENFILENAME ) );
        ofn.lStructSize  = sizeof( OPENFILENAME );
        ofn.lpstrFile    = szFile;
        ofn.nMaxFile     = sizeof( szFile );
        ofn.lpstrFilter  = filter;
        ofn.nFilterIndex = 1;
        ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if ( GetOpenFileNameA( &ofn ) == TRUE )
        {
            std::string fp = ofn.lpstrFile;
            std::replace( fp.begin(), fp.end(), '\\', '/' );
            return std::filesystem::path( fp );
        }

        return std::filesystem::path();
    }

    std::filesystem::path WindowsFileSystem::OpenFolderDialog( const char* initialFolder )
    {
        (void)initialFolder;

        BROWSEINFOA bi                  = { 0 };
        CHAR        szDisplay[MAX_PATH] = { 0 };
        bi.pszDisplayName               = szDisplay;
        bi.lpszTitle                    = "Select Folder";
        bi.ulFlags                      = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;

        LPITEMIDLIST pidl = SHBrowseForFolderA( &bi );
        if ( pidl != nullptr )
        {
            CHAR szPath[MAX_PATH] = { 0 };
            if ( SHGetPathFromIDListA( pidl, szPath ) )
            {
                CoTaskMemFree( pidl );
                std::string fp = szPath;
                std::replace( fp.begin(), fp.end(), '\\', '/' );
                return std::filesystem::path( fp );
            }
            CoTaskMemFree( pidl );
        }

        return std::filesystem::path();
    }

    std::filesystem::path WindowsFileSystem::SaveFileDialog( const char* filter )
    {
        OPENFILENAMEA ofn;
        CHAR          szFile[260] = { 0 };

        ZeroMemory( &ofn, sizeof( OPENFILENAME ) );
        ofn.lStructSize  = sizeof( OPENFILENAME );
        ofn.lpstrFile    = szFile;
        ofn.nMaxFile     = sizeof( szFile );
        ofn.lpstrFilter  = filter;
        ofn.nFilterIndex = 1;
        ofn.Flags        = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

        if ( GetSaveFileNameA( &ofn ) == TRUE )
        {
            std::string fp = ofn.lpstrFile;
            std::replace( fp.begin(), fp.end(), '\\', '/' );
            return std::filesystem::path( fp );
        }

        return std::filesystem::path();
    }
} // namespace Common::Utils