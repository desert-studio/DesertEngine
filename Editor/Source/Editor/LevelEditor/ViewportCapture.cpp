// VIEWPORT CAPTURE — see ViewportCapture.hpp. Moved out of EditorLayer.cpp unchanged (EDL-4).
#include "Editor/LevelEditor/ViewportCapture.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Editor/Core/Control/InputInjection.hpp>
#include <Editor/Core/Control/PointerDrag.hpp>
#include <Engine/Core/Glfw.hpp>
#include <Engine/Core/PlayerStart.hpp>
#include <Editor/Core/SaveShortcut.hpp>
#include <Editor/Core/ContentCreateCommands.hpp>
#include <Editor/Core/DetailsNavigation.hpp>
#include <Engine/Graphic/ViewBudgetGate.hpp>
#include <Engine/Graphic/Environment/EnvironmentBake.hpp>
#include <Engine/Assets/ContentWork.hpp>
#include <Engine/Assets/BootContent.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Content/AssetRedirector.hpp>
#include <Common/Content/ContentChunks.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Editor/Import/MeshDeriver.hpp>
#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Utilities/ContentScanLedger.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>
#include <Engine/Graphic/MemoryReadout.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/DrawCounters.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <functional>
#include <set>
#include <Editor/Widgets/ThumbnailService.hpp>
#include <Editor/Widgets/ThumbnailWarmup.hpp>
#include <Editor/Widgets/ToolbarLayout.hpp>
#include <Engine/Core/SceneAssetRoots.hpp>
#include <Common/Core/Core.hpp>
#include <Common/Core/CrashHandler.hpp>
#include <Common/Core/Profiler.hpp>
#include <Editor/Import/CookPaths.hpp>
#include <Editor/Import/MeshDnD.hpp>
#include <Editor/Import/ImportOptionsDialog.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
#include <Common/Core/JobSystem.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Localization/LocalizationService.hpp>
#include <Engine/ECS/EntityLock.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>
#include <Common/Core/Units.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/ProceduralCharacterFactory.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Rig/ControlHierarchy.hpp>
#include <Engine/Animation/Rig/ControlRigStage.hpp>
#include <Engine/Assets/AssetEviction.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Scripting/ScriptEngine.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Core/WorldStreamer.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include "Editor/Core/CommandLine.hpp"
#include "Editor/Core/Control/ControlChannelOptions.hpp"
#include "Editor/Core/Control/ControlDispatch.hpp" // resolving a request to a palette entry
#include "Editor/Core/AutosavePaths.hpp"
#include "Editor/Core/CrashRecovery.hpp"
#include <Engine/Graphic/DeviceLost.hpp>
#include "Editor/Core/LayoutManager.hpp"
#include "Editor/Core/PanelRequests.hpp"
#include "Editor/Core/SceneOpenRequest.hpp"
#include "Editor/Core/SceneSaveRules.hpp"
#include "Editor/Core/ShotOptions.hpp"
#include "Editor/Core/DemoMaterials.hpp"
#include "Editor/Core/MaterialAssetUtils.hpp"
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include "Editor/Core/EditorResources.hpp"
#include "Editor/Core/ThemeManager.hpp"
#include "Editor/Core/GizmoState.hpp"
#include "Editor/Core/NumberFormat.hpp"
#include "Editor/Core/MeshResolve.hpp"            // the toolbar/status triangle census
#include "Editor/Core/Selection/ViewportMode.hpp" // the editor-mode rail
#include <Engine/Geometry/MeshStats.hpp>
#include "Editor/Core/CommandHistory.hpp"
#include "Editor/Core/Commands/LandscapeLayerCommands.hpp"
#include "Editor/Core/Commands/SceneCommands.hpp"
#include "Editor/Core/EditorPreferences.hpp"
#include "Editor/Packaging/GamePackager.hpp"
#include "Editor/Core/ProjectContext.hpp"
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp> // reading the PRESENTED frame back (shot.window)
#include <Engine/Graphic/Image.hpp>                      // Image2D::ReadPixelsRGBA8 (debug frame dump)
#include <Engine/Core/Input.hpp>
#include <Common/Core/KeyCodes.hpp>
#include <Common/Core/Version.hpp>
#include <Common/Settings/MachineSettings.hpp>
#include <stb_image/stb_image_write.h>
#include "Editor/Core/ImGuiUtilities.hpp"
#include <ImGui/imgui_internal.h>
#include <array>
#include <format>
#include <ImGuizmo.h>
#include "Editor/Import/ImportManager.hpp"
#include "Editor/Splash/SplashControls.hpp"
#include "Editor/Splash/SplashImage.hpp"
#include "Editor/Widgets/WindowButtonStyle.hpp"
#include "Editor/Builtin/BuiltinMeshRegistry.hpp"
#include "Editor/Panels/SceneHierarchy/SceneHierarchyPanel.hpp"
#include <Editor/Core/SkeletonAssignPalette.hpp>
#include <Editor/Panels/AnimationEditor/SkeletonReferenceSlots.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Serialization/AnimationClipWrite.hpp>
#include "Editor/Panels/SceneProperties/ScenePropertiesPanel.hpp"
#include "Editor/Panels/Debug/ShaderLibraryPanel.hpp"
#include "Editor/Panels/Debug/UIDebuggerPanel.hpp"
#include "Editor/Panels/FileExplorer/FileExplorerPanel.hpp"
#include "Editor/Widgets/ThumbnailEdit.hpp"
#include "Editor/Panels/ViewportPanel/ViewportPanel.hpp"
#include "Editor/Panels/ViewportPanel/Tools/ActiveToolBar.hpp"
#include "Editor/Panels/Scalability/ScalabilityPanel.hpp"
#include "Editor/Panels/WorldSettings/WorldSettingsPanel.hpp"
#include "Editor/Panels/WorldPartition/WorldPartitionPanel.hpp"
#include <Engine/Core/Serialize/WorldPartitionConversion.hpp>
#include "Editor/Panels/Landscape/LandscapePanel.hpp"
#include "Editor/Panels/Landscape/LandscapeCommands.hpp"
#include "Editor/LevelEditor/ViewportCommands.hpp"
#include "Editor/Panels/Clouds/CloudCommands.hpp"
#include "Editor/Panels/Localization/LanguageCommands.hpp"
#include "Editor/Panels/Build/BuildCommands.hpp"
#include "Editor/Panels/UI/UICommands.hpp"
#include "Editor/Core/DebugCommands.hpp"
#include "Editor/Panels/Modeling/ModelingCommands.hpp"
#include "Editor/Panels/Foliage/FoliageCommands.hpp"
#include "Editor/Core/Rigging/HumanoidCommands.hpp"
#include "Editor/Panels/Modeling/ModelingPanel.hpp"
#include "Editor/Panels/Logs/LogsPanel.hpp"
#include "Editor/Panels/Collections/CollectionsPanel.hpp"
#include "Editor/Panels/NodeGraph/NodeGraphPanel.hpp"
#include "Editor/Panels/NodeGraph/ShaderGraphDocumentOpen.hpp"
#include "Editor/Panels/MaterialEditor/MaterialEditorPanel.hpp"
#include "Editor/Panels/MaterialEditor/MaterialDocumentOpen.hpp"
#include "Editor/Panels/SceneProperties/ComponentWidgets/MaterialsPanelComponent.hpp"
#include "Editor/Core/AssetOpen.hpp"
#include "Editor/Panels/Animation/AnimGraphPanel.hpp"
#include "Editor/Panels/Photogrammetry/PhotogrammetryPanel.hpp"
#include "Editor/Panels/Particles/ParticleEditorPanel.hpp"
#include "Editor/Panels/UI/UIEditorPanel.hpp"
#include "Editor/Panels/AssetReferences/AssetReferencesPanel.hpp"
#include "Editor/Panels/LuaConsole/LuaConsolePanel.hpp"
#include "Editor/Panels/Sequencer/SequencerPanel.hpp"
#include <Engine/Animation/Timeline/Hosts.hpp>
#include "Editor/Panels/Build/BuildSettingsPanel.hpp"
#include "Editor/Panels/Build/ContentChunksPanel.hpp"
#include "Editor/Packaging/ProjectChunkScheme.hpp"
#include "Editor/Panels/History/HistoryPanel.hpp"
#include "Editor/Panels/Localization/LocalizationPanel.hpp"
#include "Editor/Panels/Validation/SceneValidationPanel.hpp"
#include "Editor/Panels/Clouds/CloudModellingVolumePanel.hpp"
#include "Editor/Panels/Clouds/CloudDocumentOpen.hpp"
#include "Editor/Panels/Clouds/CloudLayoutPanel.hpp"
#include "Editor/Panels/Clouds/CloudNoiseVolumePanel.hpp"
#include "Editor/Panels/SkyboxViewer/SkyboxViewerDocument.hpp"
#include "Editor/Panels/AnimationEditor/AnimationEditorDocument.hpp"
#include <Common/Content/ContentKinds.hpp>
#include "Editor/Panels/StaticMeshViewer/StaticMeshViewerDocument.hpp"
#include "Editor/Panels/TextureViewer/TextureViewerDocument.hpp"
#include "Editor/Panels/Clouds/CloudTypePanel.hpp"
#include "Editor/Panels/Clouds/CloudsPanel.hpp"
#include "Editor/Panels/Animation/ControlRigPanel.hpp"
#include "Editor/Core/Selection/AuthoringContext.hpp"
#include "Editor/Core/ToastManager.hpp"
#include "Editor/Core/OpenableAssets.hpp"
#include "Editor/Core/ViewportCameraProperties.hpp"
#include "Editor/Core/SubjectEditorRegistry.hpp"
#include "Editor/Core/ControlNudgeRequest.hpp"
#include "Editor/Core/Commands/PoseEditTransaction.hpp"
#include <Engine/Animation/Rig/ControlManipulator.hpp>
#include "Editor/Core/SubjectOpenRequest.hpp"
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <Engine/Core/SceneRenderCollectors.hpp>
#include <Engine/ECS/System/MeshECSSystem.hpp>
#include <Engine/ECS/System/TextECSSystem.hpp>
#include <Engine/ECS/System/SkyboxECSSystem.hpp>
#include <Engine/ECS/System/HeightFogECSSystem.hpp>
#include <Engine/ECS/System/VolumetricCloudECSSystem.hpp>
#include <Engine/ECS/System/TimeOfDayECSSystem.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Editor/Core/DocumentPlacement.hpp>
#include <Editor/Core/Rigging/RigBuilder.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/Selection/ModelingStateProperties.hpp>
#include <Editor/Core/Selection/SelectionTransformProperties.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Engine/ECS/System/PointLightSystem.hpp>
#include <Engine/ECS/System/SpotLightSystem.hpp>
#include <Engine/ECS/System/AnimationECSSystem.hpp>
#include <Engine/ECS/System/AttachmentSystem.hpp>
#include <Engine/ECS/System/PhysicsECSSystem.hpp>
#include <Engine/ECS/System/LevelSequenceSystem.hpp>
#include <Engine/ECS/System/LocomotionSystem.hpp>
#include <Engine/ECS/System/ScriptSystem.hpp>
#include <Engine/ECS/System/AudioECSSystem.hpp>
#include <algorithm> // std::sort / std::transform (scene list)
#include <span>      // the View menu's groups, declared as data rather than as control flow
#include <cctype>    // std::tolower (scene filter)
#include <chrono>    // per-stage startup timing (see the staged boot in OnUpdate)

namespace Desert::Editor
{
    // The one readback. Every picture the editor writes out of the viewport comes through here, so a
    // capture cannot quietly differ from a dump in flip, format or the device-idle wait that makes the
    // readback legal at all.
    Common::BoolResultStr ViewportCapture::ReadViewportRGBA8( std::vector<uint8_t>& outPixels, uint32_t& outWidth,
                                                          uint32_t& outHeight )
    {
        if ( !m_Workspace.ActiveScene() )
            return Common::MakeError<bool>( "no scene to capture" );

        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        auto img = m_Workspace.ActiveScene()->GetFinalImage();
        if ( !img )
            return Common::MakeError<bool>( "scene has no final image" );

        // Г13: ReadPixelsRGBA8 answers instead of returning an empty vector for every kind of failure.
        // The size check below is kept and now means only what it says — this arm carries the reason.
        auto read = img->ReadPixelsRGBA8();
        if ( !read.IsSuccess() )
            return Common::MakeFormattedError<bool>( "readback refused: {}", read.GetError() );

        outPixels = read.ExtractValue();
        outWidth  = img->GetWidth();
        outHeight = img->GetHeight();
        if ( outPixels.size() != static_cast<size_t>( outWidth ) * outHeight * 4 )
            return Common::MakeFormattedError<bool>( "readback is {} bytes, expected {}x{}x4 = {}",
                                                     outPixels.size(), outWidth, outHeight,
                                                     static_cast<size_t>( outWidth ) * outHeight * 4 );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ViewportCapture::WriteProjectThumbnail()
    {
        // The picture belongs to a project, so with no project there is nowhere for it to go. Not an
        // error: the editor can be running a scene that was opened without one.
        const std::string projectDirectory = Desert::Project::ProjectContext::Directory();
        if ( projectDirectory.empty() )
            return BOOLSUCCESS;

        std::vector<uint8_t> px;
        uint32_t             w = 0;
        uint32_t             h = 0;
        if ( const auto read = ReadViewportRGBA8( px, w, h ); !read.IsSuccess() )
            return read;
        if ( w == 0 || h == 0 )
            return Common::MakeError<bool>( "the viewport has no size" );

        // 16:9 window out of the middle of whatever the viewport is, then a box downsample to the
        // fixed output size. Cropping rather than squashing, because a squashed frame is a picture
        // of the wrong world; centre rather than top, because the interesting part of a viewport is
        // where the camera is pointed.
        constexpr uint32_t kOutW = 512;
        constexpr uint32_t kOutH = 288; // 16:9 — the aspect the launcher's grid is built out of

        uint32_t cropW = w;
        uint32_t cropH = ( w * kOutH ) / kOutW;
        if ( cropH > h )
        {
            cropH = h;
            cropW = ( h * kOutW ) / kOutH;
        }
        const uint32_t cropX = ( w - cropW ) / 2;
        const uint32_t cropY = ( h - cropH ) / 2;

        std::vector<uint8_t> out( static_cast<size_t>( kOutW ) * kOutH * 4 );
        for ( uint32_t y = 0; y < kOutH; ++y )
        {
            // Source rows this output row averages over. Integer bounds on both ends so no source
            // pixel is counted twice and none is skipped.
            const uint32_t sy0 = cropY + ( y * cropH ) / kOutH;
            const uint32_t sy1 = std::max( sy0 + 1u, cropY + ( ( y + 1 ) * cropH ) / kOutH );
            for ( uint32_t x = 0; x < kOutW; ++x )
            {
                const uint32_t sx0 = cropX + ( x * cropW ) / kOutW;
                const uint32_t sx1 = std::max( sx0 + 1u, cropX + ( ( x + 1 ) * cropW ) / kOutW );

                uint32_t accum[4] = { 0, 0, 0, 0 };
                uint32_t samples  = 0;
                for ( uint32_t sy = sy0; sy < sy1 && sy < h; ++sy )
                    for ( uint32_t sx = sx0; sx < sx1 && sx < w; ++sx )
                    {
                        const size_t at = ( static_cast<size_t>( sy ) * w + sx ) * 4;
                        for ( int c = 0; c < 4; ++c )
                            accum[c] += px[at + static_cast<size_t>( c )];
                        ++samples;
                    }
                const size_t dst = ( static_cast<size_t>( y ) * kOutW + x ) * 4;
                for ( int c = 0; c < 4; ++c )
                    out[dst + static_cast<size_t>( c )] =
                         static_cast<uint8_t>( samples ? accum[c] / samples : 0u );
            }
        }

        // The name is a CONVENTION shared with the launcher, which looks for exactly this file
        // beside the .deproj. Leading dot so it does not show up as project content.
        const std::string file = ( std::filesystem::path( projectDirectory ) / ".thumbnail.png" ).string();
        stbi_flip_vertically_on_write( 0 );
        if ( stbi_write_png( file.c_str(), static_cast<int>( kOutW ), static_cast<int>( kOutH ), 4, out.data(),
                             static_cast<int>( kOutW ) * 4 ) == 0 )
            return Common::MakeFormattedError<bool>( "could not write {}", file );
        LOG_INFO( "[Project] thumbnail -> {} ({}x{})", file, kOutW, kOutH );
        return BOOLSUCCESS;
    }

    // Read the resolved viewport back off the GPU and write it as a PNG. The one place that does this: the
    // still capture, every frame of a `--shot-sequence`, and the F9 dump all go through here, so a capture
    // cannot quietly differ from a dump in flip, format or the device-idle wait that makes the readback
    // legal at all.
    bool ViewportCapture::WriteViewportPng( const std::string& path )
    {
        if ( !m_Workspace.ActiveScene() )
        {
            LOG_ERROR( "[Shot] no scene to capture ('{}')", path );
            return false;
        }

        // The directory of a sequence is named on the command line and usually does not exist yet. Create
        // it rather than letting stb fail on a path that is only missing a folder.
        const std::filesystem::path file = std::filesystem::path( path );
        if ( file.has_parent_path() && !file.parent_path().empty() )
        {
            std::error_code ec;
            std::filesystem::create_directories( file.parent_path(), ec );
            if ( ec && !std::filesystem::exists( file.parent_path() ) )
            {
                LOG_ERROR( "[Shot] could not create '{}': {}", file.parent_path().string(), ec.message() );
                return false;
            }
        }

        std::vector<uint8_t> px;
        uint32_t             w = 0;
        uint32_t             h = 0;
        if ( const auto read = ReadViewportRGBA8( px, w, h ); !read.IsSuccess() )
        {
            LOG_ERROR( "[Shot] {} ('{}')", read.GetError(), path );
            return false;
        }

        stbi_flip_vertically_on_write( 0 );
        const bool written = stbi_write_png( path.c_str(), w, h, 4, px.data(), w * 4 ) != 0;
        LOG_INFO( "[Shot] {} -> {} ({}x{})", written ? "wrote" : "FAILED to write", path, w, h );
        return written;
    }

    // The record half of a window capture, on the frame the control channel's gate named (see
    // EditorLayer::RecordWindowCaptureIfDue): a swapchain image may only be touched between acquire and present.
    void ViewportCapture::RecordWindowCapture()
    {
        auto swapChain = std::dynamic_pointer_cast<Graphic::API::Vulkan::VulkanSwapChain>(
             EngineContext::GetInstance().GetWindow()->GetWindowSwapChain() );
        if ( !swapChain )
        {
            m_CaptureError = "there is no Vulkan swapchain to capture the presented frame from.";
            return;
        }

        if ( const auto recorded = swapChain->RecordFrameCapture(); !recorded )
        {
            m_CaptureError = recorded.GetError();
            return;
        }
        m_CaptureError.clear();
    }

    bool ViewportCapture::WriteWindowPng( const std::string& path, std::string& outError )
    {
        // THE WHOLE EDITOR, INTERFACE INCLUDED — which is what WriteViewportPng next door cannot do and
        // never could. That one reads the scene's own final image; ImGui is recorded into the SWAPCHAIN
        // render pass, so no capture this engine took before this existed held a single pixel of a panel,
        // a menu or a dialog. It is why proving anything about the interface meant photographing the
        // window from outside the process, by PID.
        //
        // This is the second half of the capture: the copy was recorded into this frame's command buffer
        // by RecordWindowCaptureIfDue, and the bytes are collected here, once the present that carried it
        // has gone out.
        if ( !m_CaptureError.empty() )
        {
            outError = m_CaptureError;
            m_CaptureError.clear();
            return false;
        }

        auto swapChain = std::dynamic_pointer_cast<Graphic::API::Vulkan::VulkanSwapChain>(
             EngineContext::GetInstance().GetWindow()->GetWindowSwapChain() );
        if ( !swapChain )
        {
            outError = "there is no Vulkan swapchain to collect the captured frame from.";
            return false;
        }

        const std::filesystem::path file = std::filesystem::path( path );
        if ( file.has_parent_path() && !file.parent_path().empty() )
        {
            std::error_code ec;
            std::filesystem::create_directories( file.parent_path(), ec );
            if ( ec && !std::filesystem::exists( file.parent_path() ) )
            {
                outError = "could not create '" + file.parent_path().string() + "': " + ec.message();
                return false;
            }
        }

        // The copy was submitted with this frame; waiting is what makes the staging buffer readable.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        uint32_t   width  = 0;
        uint32_t   height = 0;
        const auto pixels = swapChain->TakeCapturedFrameRGBA8( width, height );
        if ( !pixels )
        {
            outError = pixels.GetError();
            return false;
        }

        stbi_flip_vertically_on_write( 0 );
        const bool written = stbi_write_png( path.c_str(), static_cast<int>( width ), static_cast<int>( height ),
                                             4, pixels.GetValue().data(), static_cast<int>( width ) * 4 ) != 0;
        if ( !written )
        {
            outError = "stb_image_write refused to write '" + path + "'.";
            return false;
        }

        LOG_INFO( "[Control] wrote the presented frame (editor and interface) -> {} ({}x{})", path, width,
                  height );
        return true;
    }
} // namespace Desert::Editor
