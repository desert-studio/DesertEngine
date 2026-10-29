# cmake -D SOURCE=<file> -D DEST=<dir> -D MISSING=<message> -P CopyIfExists.cmake
# A post-build copy of an optional vendor binary: copied when fetched, reported (not fatal) when not, which is
# what premake's `if exist ... {COPYFILE}` / `if not exist ... echo` pair did.
if(EXISTS "${SOURCE}")
    file(COPY "${SOURCE}" DESTINATION "${DEST}")
else()
    message("${MISSING}")
endif()
