# cmake -D SOURCE=<file> -D DEST=<dir> -D MISSING=<message> -P CopyIfExists.cmake
# A post-build copy of an optional vendor binary: copied when fetched, reported (not fatal) when not, so an unfetched
# SDK binary never fails a build that does not need it.
if(EXISTS "${SOURCE}")
    file(COPY "${SOURCE}" DESTINATION "${DEST}")
else()
    message("${MISSING}")
endif()
