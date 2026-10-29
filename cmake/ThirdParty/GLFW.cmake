# BuildScripts/ThirdParty/GLFW.lua: GLFW as a C static library, the platform backend chosen per OS.
set(_root "${DESERT_ROOT}/ThirdParty/GLFW")
set(_src
    include/GLFW/glfw3.h include/GLFW/glfw3native.h  # premake also listed src/glfw_config.h, which does not exist (premake drops a missing file silently)
    src/context.c src/init.c src/input.c src/monitor.c src/null_init.c src/null_joystick.c
    src/null_monitor.c src/null_window.c src/platform.c src/vulkan.c src/window.c)
if(DESERT_IS_WINDOWS)
    list(APPEND _src src/win32_init.c src/win32_joystick.c src/win32_module.c src/win32_monitor.c src/win32_time.c
                     src/win32_thread.c src/win32_window.c src/wgl_context.c src/egl_context.c src/osmesa_context.c)
    set(_defs _GLFW_WIN32 _CRT_SECURE_NO_WARNINGS)
elseif(DESERT_IS_MACOS)
    list(APPEND _src src/cocoa_init.m src/cocoa_joystick.m src/cocoa_monitor.m src/cocoa_window.m src/cocoa_time.c
                     src/nsgl_context.m src/posix_module.c src/posix_thread.c src/egl_context.c src/osmesa_context.c)
    set(_defs _GLFW_COCOA)
else()
    list(APPEND _src src/x11_init.c src/x11_monitor.c src/x11_window.c src/xkb_unicode.c src/posix_module.c
                     src/posix_time.c src/posix_thread.c src/posix_poll.c src/glx_context.c src/egl_context.c
                     src/osmesa_context.c src/linux_joystick.c)
    set(_defs _GLFW_X11)
endif()
list(TRANSFORM _src PREPEND "${_root}/")
if(DESERT_IS_MACOS)
    enable_language(OBJC)
endif()
desert_add_library(GLFW FOLDER ThirdParty SOURCES ${_src})
target_compile_definitions(GLFW PRIVATE ${_defs})
if(DESERT_IS_WINDOWS)
    target_link_libraries(GLFW PRIVATE Dwmapi)
else()
    set_target_properties(GLFW PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
