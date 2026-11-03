# Desert: the engine module libraries, the three modules with their own projects, and the tests.
add_subdirectory(Desert)
add_subdirectory(UI)
add_subdirectory(Render2DCore)
add_subdirectory(Common)
if(DESERT_WITH_TESTS)
    add_subdirectory(Tests)
endif()
