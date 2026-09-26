-- imgui-node-editor (thedmd, v0.9.3) static library — the node-graph canvas the editor's
-- Node Graph / future shader-graph tools render with. Depends only on ImGui core.

local root  = _MAIN_SCRIPT_DIR .. "/ThirdParty/imgui-node-editor"
local imgui = _MAIN_SCRIPT_DIR .. "/ThirdParty/ImGui"

project "ImGuiNodeEditor"
	kind "StaticLib"
	language "C++"
	location ( root )

	files
	{
		root .. "/crude_json.cpp",
		root .. "/crude_json.h",
		root .. "/imgui_bezier_math.h",
		root .. "/imgui_canvas.cpp",
		root .. "/imgui_canvas.h",
		root .. "/imgui_extra_math.h",
		root .. "/imgui_node_editor.cpp",
		root .. "/imgui_node_editor.h",
		root .. "/imgui_node_editor_api.cpp",
		root .. "/imgui_node_editor_internal.h",
	}

	includedirs
	{
		imgui,
	}

	links { "ImGui" }

	filter "system:windows"
		systemversion "latest"
		cppdialect "C++17"
		-- MUST match the rest of the workspace (/MD). Unlike ImGui — which gets away with
		-- staticruntime "On" because it never touches the C++ stdlib — crude_json.cpp uses
		-- <sstream>/<locale>, so a static CRT here drags libcpmtd.lib into the Editor link and
		-- collides with msvcprtd.lib (LNK2038 RuntimeLibrary mismatch + a wall of LNK2005).
		staticruntime "off"

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
