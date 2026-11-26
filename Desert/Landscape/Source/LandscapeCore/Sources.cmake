# Written by scripts/Dev/sources_sync.py from git; never edited by hand. A file added or removed in this
# directory: run `python scripts/Dev/sources_sync.py` and commit this list with the change (land_dev.sh
# refuses a list that differs from the tree). Read by desert_sources() (cmake/DesertWorkspace.cmake).
set(DESERT_SOURCES
    "Private/LandscapeCollisionShape.cpp"
    "Private/LandscapeData.cpp"
    "Private/LandscapeEditLayerStack.cpp"
    "Private/LandscapeEditLayers.cpp"
    "Private/LandscapeLayout.cpp"
    "Private/LandscapeSplines.cpp"
    "Private/LandscapeTileFiles.cpp"
    "Private/LandscapeWorldPartition.cpp"
    "Public/LandscapeCollisionShape.hpp"
    "Public/LandscapeData.hpp"
    "Public/LandscapeEditLayers.hpp"
    "Public/LandscapeFrame.hpp"
    "Public/LandscapeLayerLimits.hpp"
    "Public/LandscapeLayout.hpp"
    "Public/LandscapeSplines.hpp"
    "Public/LandscapeSplinesJson.hpp"
    "Public/LandscapeTileFiles.hpp"
    "Public/LandscapeWorldPartition.hpp"
)
