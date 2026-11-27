# Desert: the engine module libraries, the three modules with their own projects, and the tests.
# Common first: it is the donor of the PCH group "Common" (the group's first declared member holds the PCH).
add_subdirectory(Common)
add_subdirectory(Desert)
add_subdirectory(UI)
add_subdirectory(Render2DCore)
add_subdirectory(Net)
if(DESERT_WITH_TESTS)
    add_subdirectory(Tests)
endif()
