-- Dear ImGui static library (core only; the GLFW/Vulkan backends are compiled
-- into the EDITOR via Editor/Source/Editor/ImGuiIntegration/ImGuiBuild.cpp -- they need the Vulkan and
-- GLFW headers, which this core library is deliberately independent of).
--
-- Only the Editor links this project. The engine does not: Desert/Tests/Engine/ImGuiBoundary is the
-- census that keeps it that way.

local root = _MAIN_SCRIPT_DIR .. "/ThirdParty/ImGui"

project "ImGui"
	kind "StaticLib"
	language "C++"
	location ( root )

	files
	{
		root .. "/imconfig.h",
		root .. "/imgui.h",
		root .. "/imgui.cpp",
		root .. "/imgui_draw.cpp",
		root .. "/imgui_internal.h",
		root .. "/imgui_tables.cpp",
		root .. "/imgui_widgets.cpp",
		root .. "/imstb_rectpack.h",
		root .. "/imstb_textedit.h",
		root .. "/imstb_truetype.h",
		root .. "/imgui_demo.cpp"
	}

	filter "system:windows"
		systemversion "latest"
		cppdialect "C++17"
		-- staticruntime: the workspace's decision on Windows (BuildScripts/PlatformWindows.lua).

	filter "system:linux"
		pic "On"
		systemversion "latest"
		cppdialect "C++17"
		staticruntime "On"

	filter "system:macosx"
		pic "On"
		cppdialect "C++17"
		staticruntime "On"

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"

	filter {}
