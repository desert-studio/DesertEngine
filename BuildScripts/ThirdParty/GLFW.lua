-- GLFW static library. Project definition lives here (parent repo) instead of
-- inside the ThirdParty/GLFW submodule so platform support can be versioned
-- with the engine. Generated project files still land in the submodule dir to
-- keep the previous on-disk layout.

local root = _MAIN_SCRIPT_DIR .. "/ThirdParty/GLFW"

project "GLFW"
	kind "StaticLib"
	language "C"
	staticruntime "off"
	location ( root )

	files
	{
		root .. "/include/GLFW/glfw3.h",
		root .. "/include/GLFW/glfw3native.h",
		root .. "/src/glfw_config.h",
		root .. "/src/context.c",
		root .. "/src/init.c",
		root .. "/src/input.c",
		root .. "/src/monitor.c",

		root .. "/src/null_init.c",
		root .. "/src/null_joystick.c",
		root .. "/src/null_monitor.c",
		root .. "/src/null_window.c",

		root .. "/src/platform.c",
		root .. "/src/vulkan.c",
		root .. "/src/window.c",
	}

	filter "system:linux"
		pic "On"

		systemversion "latest"

		files
		{
			root .. "/src/x11_init.c",
			root .. "/src/x11_monitor.c",
			root .. "/src/x11_window.c",
			root .. "/src/xkb_unicode.c",
			root .. "/src/posix_module.c",
			root .. "/src/posix_time.c",
			root .. "/src/posix_thread.c",
			root .. "/src/posix_poll.c",
			root .. "/src/glx_context.c",
			root .. "/src/egl_context.c",
			root .. "/src/osmesa_context.c",
			root .. "/src/linux_joystick.c"
		}

		defines
		{
			"_GLFW_X11"
		}

	filter "system:macosx"
		files
		{
			root .. "/src/cocoa_init.m",
			root .. "/src/cocoa_joystick.m",
			root .. "/src/cocoa_monitor.m",
			root .. "/src/cocoa_window.m",
			root .. "/src/cocoa_time.c",
			root .. "/src/nsgl_context.m",
			root .. "/src/posix_module.c",
			root .. "/src/posix_thread.c",
			root .. "/src/egl_context.c",
			root .. "/src/osmesa_context.c",
		}

		defines
		{
			"_GLFW_COCOA"
		}

	filter "system:windows"
		systemversion "latest"

		files
		{
			root .. "/src/win32_init.c",
			root .. "/src/win32_joystick.c",
			root .. "/src/win32_module.c",
			root .. "/src/win32_monitor.c",
			root .. "/src/win32_time.c",
			root .. "/src/win32_thread.c",
			root .. "/src/win32_window.c",
			root .. "/src/wgl_context.c",
			root .. "/src/egl_context.c",
			root .. "/src/osmesa_context.c"
		}

		defines
		{
			"_GLFW_WIN32",
			"_CRT_SECURE_NO_WARNINGS"
		}

		links
		{
			"Dwmapi.lib"
		}

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"

	filter {}
