#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Desert::Editor
{
    struct AssetViewState;
    struct DirectoryInformation;
    class ContentBrowserHistory;

    /// THE STRIP ABOVE THE ASSET VIEW (UE: SContentBrowser's toolbar + SNavigationBar): view settings, the
    /// name search, sort and type filter (all edits of AssetViewState), Back/Forward/Up, the favourites menu,
    /// Import, and the breadcrumb path of the open folder. Every action is the panel's, through the delegates.
    class ContentBrowserToolbar
    {
    public:
        struct Delegates
        {
            std::function<void( DirectoryInformation* )> OnFolderSelected;    // Up, a breadcrumb
            std::function<void( const std::string& )>    OnFavouriteSelected; // a pinned folder
            std::function<void()>                        OnBack;
            std::function<void()>                        OnForward;
            std::function<void()>                        OnRefresh;
            std::function<void()>                        OnNewFolder;
            std::function<void()>                        OnImport;
        };

        explicit ContentBrowserToolbar( Delegates delegates );

        /// The toolbar's child window, two frame rows tall.
        void Draw( AssetViewState& state, const ContentBrowserHistory& history, DirectoryInformation* current,
                   DirectoryInformation* root );

        /// The open folder changed or was re-listed: the breadcrumbs are rebuilt on the next Draw.
        void InvalidateBreadcrumbs()
        {
            m_BreadcrumbsStale = true;
        }

    private:
        void DrawBreadcrumbs( DirectoryInformation* current, DirectoryInformation* root );

        Delegates                          m_On;
        std::vector<DirectoryInformation*> m_Breadcrumbs; // root .. open folder
        bool                               m_BreadcrumbsStale = true;
    };
} // namespace Desert::Editor
