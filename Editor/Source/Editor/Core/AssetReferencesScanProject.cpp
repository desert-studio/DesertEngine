#include "AssetReferences.hpp"

#include <Common/Core/Constants.hpp>

#include <Engine/Project/ProjectContext.hpp>

namespace Desert::Editor
{
    // THE ONLY PART OF THE SCAN THAT KNOWS ABOUT AN OPEN PROJECT, and it is a file of its own so that
    // the rest can be linked by something that has no engine at all. Tools/AssetClosure computes what
    // a build must ship for one scene to open, and it must do that from a .deproj on disk without
    // booting Vulkan; the moment this ProjectContext include sat in the same translation unit as the
    // token rule, the tool's only options were to link the whole engine or to re-implement the rule.
    void BuildProjectAssetReferenceIndex( AssetReferenceIndex& index )
    {
        index.Clear();
        if ( !::Desert::Project::ProjectContext::HasProject() )
            return;

        BuildAssetReferenceIndex( index, Common::Constants::Path::ASSETS_PATH,
                                  ::Desert::Project::ProjectContext::Directory() );
    }
} // namespace Desert::Editor
