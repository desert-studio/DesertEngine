# Written by scripts/Dev/sources_sync.py from git; never edited by hand. A file added or removed in this
# directory: run `python scripts/Dev/sources_sync.py` and commit this list with the change (land_dev.sh
# refuses a list that differs from the tree). Read by desert_sources() (cmake/DesertWorkspace.cmake).
set(DESERT_SOURCES
    "Components.gen.cpp.tpl"
    "ComponentsModule.gen.cpp.tpl"
    "Event.tpl"
    "Field.tpl"
    "Function.tpl"
    "Metadata.tpl"
    "ReflectedComponentBlocks.gen.hpp.tpl"
    "Reflection.gen.cpp.tpl"
    "ReflectionModule.gen.cpp.tpl"
    "ReplicatedFields.gen.cpp.tpl"
    "ShaderPushConstants.gen.hpp.tpl"
    "Subsystems.gen.cpp.tpl"
)
