#include <Common/Core/DestructorGuard.hpp>
#include "PhotogrammetryPanel.hpp"

#include <Editor/Core/EditorPreferences.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Import/MeshDnD.hpp>
#include <Editor/Widgets/FaceTracker.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Common/Utilities/CameraCapture.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/System/MeshECSSystem.hpp>
#include <Engine/ECS/System/SkyboxECSSystem.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

#if defined( __APPLE__ ) || defined( __unix__ )
#define DE_POSIX_PROC 1
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        constexpr ImVec4 kColHeader    = { 0.13f, 0.14f, 0.17f, 1.0f };
        constexpr ImVec4 kColSection   = { 0.09f, 0.10f, 0.12f, 1.0f };
        constexpr ImVec4 kColLogBg     = { 0.07f, 0.07f, 0.08f, 1.0f };
        constexpr ImVec4 kColAccent    = { 0.20f, 0.44f, 0.72f, 1.0f };
        constexpr ImVec4 kColAccentHov = { 0.26f, 0.52f, 0.82f, 1.0f };
        constexpr ImVec4 kColDanger    = { 0.62f, 0.22f, 0.22f, 1.0f };
        constexpr ImVec4 kColRecord    = { 0.78f, 0.20f, 0.20f, 1.0f };
        constexpr ImVec4 kColOk        = { 0.55f, 0.85f, 0.55f, 1.0f };
        constexpr ImVec4 kColWarn      = { 0.95f, 0.75f, 0.35f, 1.0f };
        constexpr ImVec4 kColErr       = { 0.95f, 0.45f, 0.45f, 1.0f };

        constexpr std::size_t kMaxLogLines  = 600;
        constexpr double      kTexInterval  = 0.12; // ~8 fps camera-texture rebuild (bounds descriptor churn)
        constexpr double      kSaveInterval = 0.35; // ~3 fps frame saving while recording

        struct CmdPreset
        {
            const char* name;
            const char* cmd;
        };

        constexpr CmdPreset kReconstructPresets[] = {
             { "Meshroom (AliceVision)", "meshroom_batch --input {input} --output {outdir}" },
             { "COLMAP (automatic)",
               "colmap automatic_reconstructor --workspace_path {outdir} --image_path {input}" },
             { "RealityCapture (CLI)",
               "RealityCapture -addFolder {input} -align -setReconstructionRegionAuto -calculateModel "
               "-exportModel {output} -quit" },
        };

        std::string Substitute( std::string cmd, const std::string& input, const std::string& output )
        {
            const std::string outdir  = std::filesystem::path( output ).parent_path().string();
            const auto        replace = [&]( const char* token, const std::string& value )
            {
                for ( size_t p = cmd.find( token ); p != std::string::npos; p = cmd.find( token, p ) )
                    cmd.replace( p, std::strlen( token ), value );
            };
            replace( "{input}", input );
            replace( "{output}", output );
            replace( "{outdir}", outdir );
            return cmd;
        }

        // First whitespace-delimited token of a command (the binary name), for preflight + messages.
        std::string FirstToken( const std::string& cmd )
        {
            const size_t s = cmd.find_first_not_of( " \t" );
            if ( s == std::string::npos )
                return {};
            const size_t e = cmd.find_first_of( " \t", s );
            return cmd.substr( s, e == std::string::npos ? std::string::npos : e - s );
        }

        // True if the command's binary resolves on PATH (POSIX `command -v`). Non-POSIX: assume present.
        bool CommandExists( const std::string& cmd )
        {
            const std::string tok = FirstToken( cmd );
            if ( tok.empty() )
                return false;
#if defined( DE_POSIX_PROC )
            const std::string probe = "command -v \"" + tok + "\" >/dev/null 2>&1";
            return std::system( probe.c_str() ) == 0;
#else
            return true;
#endif
        }

        bool CommandField( const char* id, std::string& value )
        {
            char buf[2048];
            std::snprintf( buf, sizeof( buf ), "%s", value.c_str() );
            const ImVec2 sz( -1.0f, ImGui::GetTextLineHeight() * 2.2f );
            const bool   changed = ImGui::InputTextMultiline( id, buf, sizeof( buf ), sz );
            if ( changed )
                value = buf;
            return changed;
        }

        bool PresetCombo( const char* id, std::string& value )
        {
            const int   count   = static_cast<int>( std::size( kReconstructPresets ) );
            const char* current = "Custom";
            for ( int i = 0; i < count; ++i )
            {
                if ( value == kReconstructPresets[i].cmd )
                {
                    current = kReconstructPresets[i].name;
                    break;
                }
            }

            bool changed = false;
            ImGui::SetNextItemWidth( -1.0f );
            if ( ImGui::BeginCombo( id, current ) )
            {
                for ( int i = 0; i < count; ++i )
                {
                    const bool sel = ( value == kReconstructPresets[i].cmd );
                    if ( ImGui::Selectable( kReconstructPresets[i].name, sel ) )
                    {
                        value   = kReconstructPresets[i].cmd;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            return changed;
        }

        bool PathPicker( const char* id, std::string& value, bool folder )
        {
            constexpr float btnW = 92.0f;
            ImGui::PushID( id );

            char buf[1024];
            std::snprintf( buf, sizeof( buf ), "%s", value.c_str() );
            ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x - btnW - 8.0f );
            bool changed = ImGui::InputText( "##field", buf, sizeof( buf ) );
            if ( changed )
                value = buf;

            ImGui::SameLine();
            if ( ImGui::Button( ICON_MDI_FOLDER_OPEN " Browse", ImVec2( btnW, 0.0f ) ) )
            {
                std::filesystem::path picked;
                if ( folder )
                    picked = Common::Utils::FileSystem::OpenFolderDialog();
                else
                    picked = Common::Utils::FileSystem::SaveFileDialog(
                         "Meshes\0*.obj;*.glb;*.gltf;*.fbx;*.ply\0All\0*.*\0" );
                if ( !picked.empty() )
                {
                    value   = picked.string();
                    changed = true;
                }
            }

            ImGui::PopID();
            return changed;
        }

        // Placeholder face-landmark guide over the camera rect (real dlib tracking is wired separately).
        // A bright green ring + feature dots + frame + caption so it's unmistakably visible; centered as a
        // guide (it does NOT follow the face until the tracker feeds real points).
        void DrawLandmarks( const ImVec2& mn, const ImVec2& mx )
        {
            ImDrawList*  dl   = ImGui::GetWindowDrawList();
            const ImVec2 c    = ImVec2( ( mn.x + mx.x ) * 0.5f, ( mn.y + mx.y ) * 0.5f );
            const float  w    = mx.x - mn.x;
            const float  h    = mx.y - mn.y;
            const float  rw   = w * 0.22f;
            const float  rh   = h * 0.30f;
            const float  rad  = std::max( 3.0f, std::min( w, h ) * 0.008f );
            const ImU32  col  = IM_COL32( 70, 245, 90, 255 );
            const ImU32  ring = IM_COL32( 70, 245, 90, 110 );
            const auto   dot  = [&]( float nx, float ny )
            { dl->AddCircleFilled( ImVec2( c.x + nx, c.y + ny ), rad, col ); };

            // Jaw / face outline ring (thin connecting line + dots).
            ImVec2 prev;
            for ( int i = 0; i <= 28; ++i )
            {
                const float  a = ( 3.14159265f * 2.0f ) * ( static_cast<float>( i ) / 28.0f );
                const ImVec2 p( c.x + std::cos( a ) * rw, c.y + std::sin( a ) * rh );
                if ( i > 0 )
                    dl->AddLine( prev, p, ring, 1.5f );
                if ( i < 28 )
                    dl->AddCircleFilled( p, rad, col );
                prev = p;
            }
            // Eyes.
            for ( int i = 0; i < 8; ++i )
            {
                const float a  = ( 3.14159265f * 2.0f ) * ( static_cast<float>( i ) / 8.0f );
                const float ex = std::cos( a ) * rw * 0.16f;
                const float ey = std::sin( a ) * rh * 0.10f;
                dot( -rw * 0.42f + ex, -rh * 0.22f + ey );
                dot( rw * 0.42f + ex, -rh * 0.22f + ey );
            }
            // Brows.
            for ( int i = -2; i <= 2; ++i )
            {
                dot( -rw * 0.42f + rw * 0.12f * static_cast<float>( i ), -rh * 0.44f );
                dot( rw * 0.42f + rw * 0.12f * static_cast<float>( i ), -rh * 0.44f );
            }
            // Nose + mouth.
            for ( int i = 0; i < 4; ++i )
                dot( 0.0f, -rh * 0.04f + rh * 0.06f * static_cast<float>( i ) );
            for ( int i = -4; i <= 4; ++i )
                dot( rw * 0.20f * static_cast<float>( i ) / 4.0f, rh * 0.44f );

            // Frame + caption so the overlay reads clearly against any video.
            dl->AddRect( mn, mx, IM_COL32( 70, 245, 90, 70 ) );
            dl->AddText( ImVec2( mn.x + 6.0f, mn.y + 4.0f ), IM_COL32( 150, 245, 150, 235 ),
                         "landmarks (placeholder)" );
        }
    } // namespace

    PhotogrammetryPanel::PhotogrammetryPanel( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                              Assets::AssetManager*                         assets )
         : IPanel( "Model from Photos", /*showPanel=*/false ), m_Scene( scene ), m_Assets( assets )
    {
        m_UIHelper = std::make_unique<UI::UIHelper>();
        m_UIHelper->Init();
        m_Camera      = std::make_unique<Common::Utils::CameraCapture>();
        m_FaceTracker = std::make_unique<FaceTracker>();

        auto& prefs = EditorPreferences::Get();
        if ( !prefs.PhotogrammetryFaceModel.empty() && m_FaceTracker->LoadModel( prefs.PhotogrammetryFaceModel ) )
            m_LoadedFaceModel = prefs.PhotogrammetryFaceModel;
    }

    PhotogrammetryPanel::~PhotogrammetryPanel()
    try
    {
        StopCamera();
        CancelJob();
        if ( m_Worker.joinable() )
            m_Worker.join();
        ReleasePreview();
    }
    DESERT_DESTRUCTOR_GUARD( "~PhotogrammetryPanel" )

    // ---------------------------------------------------------------------------------------------------
    // Camera
    // ---------------------------------------------------------------------------------------------------

    void PhotogrammetryPanel::StartCamera()
    {
        if ( m_CameraOn )
            return;
        if ( m_Camera && m_Camera->Start() )
        {
            m_CameraOn    = true;
            m_Status      = "Camera started. Point it at the subject and press Record.";
            m_StatusError = false;
        }
        else
        {
            m_Status      = "Could not open the camera (no device / permission denied).";
            m_StatusError = true;
        }
    }

    void PhotogrammetryPanel::StopCamera()
    {
        m_Recording = false;
        if ( m_Camera )
            m_Camera->Stop();
        m_CameraOn = false;
    }

    void PhotogrammetryPanel::UpdateCamera()
    {
        if ( !m_CameraOn || !m_Camera )
            return;

        const double now = ImGui::GetTime();

        // Rebuild the display texture at a throttled rate (each rebuild registers a new ImGui descriptor).
        if ( now - m_LastTexTime >= kTexInterval )
        {
            int w = 0, h = 0;
            if ( m_Camera->GetLatestFrame( m_FrameBuf, w, h ) && w > 0 && h > 0 )
            {
                m_LastTexTime = now;
                m_CamW        = w;
                m_CamH        = h;

                std::vector<uint8_t>                data = m_FrameBuf; // copy: spec takes ownership
                Core::Formats::Image2DSpecification spec = {
                     .Tag        = "CameraFeed",
                     .Width      = static_cast<uint32_t>( w ),
                     .Height     = static_cast<uint32_t>( h ),
                     .Format     = Core::Formats::ImageFormat::RGBA8F,
                     .Mips       = 1u,
                     .Data       = std::move( data ),
                     .Usage      = Core::Formats::Image2DUsage::Image2D,
                     .Properties = Core::Formats::Sample,
                };
                m_CameraImage = Graphic::Image2D::Create( spec );

                // Real face-landmark tracking (dlib) on the same throttled cadence, if a model is loaded.
                if ( m_FaceTracker && m_FaceTracker->Ready() )
                    m_Landmarks = m_FaceTracker->Detect( m_FrameBuf.data(), w, h );

                // Save the frame to the photos folder while recording (also throttled).
                if ( m_Recording && now - m_LastSaveTime >= kSaveInterval )
                {
                    m_LastSaveTime = now;
                    auto& prefs    = EditorPreferences::Get();
                    if ( prefs.PhotogrammetryPhotosDir.empty() )
                    {
                        prefs.PhotogrammetryPhotosDir = "Cooked/Photogrammetry/frames";
                        EditorPreferences::Save();
                    }
                    std::error_code ec;
                    std::filesystem::create_directories( prefs.PhotogrammetryPhotosDir, ec );
                    char name[64];
                    std::snprintf( name, sizeof( name ), "frame_%05d.png", m_RecordedFrames );
                    const std::string path =
                         ( std::filesystem::path( prefs.PhotogrammetryPhotosDir ) / name ).string();
                    stbi_flip_vertically_on_write( 0 );
                    if ( stbi_write_png( path.c_str(), w, h, 4, m_FrameBuf.data(), w * 4 ) )
                        ++m_RecordedFrames;
                }
            }
        }
    }

    // ---------------------------------------------------------------------------------------------------
    // Reconstruction pipeline
    // ---------------------------------------------------------------------------------------------------

    void PhotogrammetryPanel::PushLog( const std::string& line )
    {
        std::lock_guard<std::mutex> lk( m_LogMutex );
        if ( m_Log.size() >= kMaxLogLines )
            m_Log.erase( m_Log.begin(), m_Log.begin() + ( m_Log.size() - kMaxLogLines + 1 ) );
        m_Log.push_back( line );
    }

    void PhotogrammetryPanel::RunCommand( const std::string& cmd, Job job )
    {
        LOG_INFO( "[Photogrammetry] running: {}", cmd );
        m_Job         = job;
        m_StatusError = false;
        m_Running     = true;
        m_Done        = false;
        {
            std::lock_guard<std::mutex> lk( m_LogMutex );
            m_Log.clear();
            m_Log.push_back( "$ " + cmd );
        }

        if ( m_Worker.joinable() )
            m_Worker.join();

        m_Worker = std::thread(
             [this, cmd]()
             {
#if defined( DE_POSIX_PROC )
                 int fds[2];
                 if ( pipe( fds ) != 0 )
                 {
                     m_ExitCode = -1;
                     m_Running  = false;
                     m_Done     = true;
                     return;
                 }

                 const pid_t pid = fork();
                 if ( pid == 0 )
                 {
                     setpgid( 0, 0 );
                     dup2( fds[1], STDOUT_FILENO );
                     dup2( fds[1], STDERR_FILENO );
                     close( fds[0] );
                     close( fds[1] );
                     execl( "/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>( nullptr ) );
                     _exit( 127 );
                 }

                 close( fds[1] );
                 m_ChildPid = pid;

                 std::string acc;
                 char        buf[512];
                 ssize_t     n;
                 while ( ( n = read( fds[0], buf, sizeof( buf ) ) ) > 0 )
                 {
                     acc.append( buf, static_cast<size_t>( n ) );
                     size_t nl;
                     while ( ( nl = acc.find( '\n' ) ) != std::string::npos )
                     {
                         PushLog( acc.substr( 0, nl ) );
                         acc.erase( 0, nl + 1 );
                     }
                 }
                 if ( !acc.empty() )
                     PushLog( acc );
                 close( fds[0] );

                 int status = 0;
                 waitpid( pid, &status, 0 );
                 m_ChildPid = -1;
                 if ( WIFEXITED( status ) )
                     m_ExitCode = WEXITSTATUS( status );
                 else if ( WIFSIGNALED( status ) )
                     m_ExitCode = 130;
                 else
                     m_ExitCode = -1;
#else
                 const int rc = std::system( cmd.c_str() );
                 m_ExitCode   = rc;
#endif
                 m_Running = false;
                 m_Done    = true;
             } );
    }

    void PhotogrammetryPanel::CancelJob()
    {
#if defined( DE_POSIX_PROC )
        const long pid = m_ChildPid.load();
        if ( pid > 0 )
        {
            kill( static_cast<pid_t>( -pid ), SIGTERM );
            PushLog( "[cancelled by user]" );
        }
#endif
    }

    void PhotogrammetryPanel::StartReconstruction()
    {
        auto& prefs = EditorPreferences::Get();
        if ( prefs.PhotogrammetryPhotosDir.empty() )
        {
            m_Status      = "No frames captured yet — start the camera and Record first.";
            m_StatusError = true;
            return;
        }

        // Preflight: the tool has to exist on PATH, otherwise the shell returns 127 ("command not found").
        if ( !CommandExists( prefs.PhotogrammetryCommand ) )
        {
            const std::string tok = FirstToken( prefs.PhotogrammetryCommand );
            m_Status              = "Reconstruction tool '";
            m_Status += tok;
            m_Status += "' is not installed / not on PATH. Install a photogrammetry tool (Meshroom, COLMAP, "
                        "RealityCapture) and pick it in Reconstruction settings. Nothing was run.";
            m_StatusError = true;
            PushLog( "[preflight] '" + tok + "' not found — is it installed and on PATH?" );
            return;
        }

        m_OutputCaptured = prefs.PhotogrammetryOutputMesh;
        std::error_code ec;
        std::filesystem::create_directories( std::filesystem::path( m_OutputCaptured ).parent_path(), ec );

        const std::string cmd =
             Substitute( prefs.PhotogrammetryCommand, prefs.PhotogrammetryPhotosDir, m_OutputCaptured );

        m_Status = "Running the external tool… (this can take minutes)";
        RunCommand( cmd, Job::Reconstruct );
    }

    void PhotogrammetryPanel::ImportResult()
    {
        std::error_code ec;
        if ( m_ExitCode != 0 )
        {
            if ( m_ExitCode == 127 )
                m_Status = "External tool not found (exit 127). Install a photogrammetry tool (Meshroom / "
                           "COLMAP) and set its command in Reconstruction settings.";
            else if ( m_ExitCode == 130 )
                m_Status = "Reconstruction was cancelled.";
            else
            {
                m_Status = "The external tool exited with code ";
                m_Status += std::to_string( m_ExitCode.load() );
                m_Status += ". See the log below.";
            }
            m_StatusError = true;
            return;
        }
        if ( m_OutputCaptured.empty() || !std::filesystem::exists( m_OutputCaptured, ec ) )
        {
            m_Status      = "Tool finished but no mesh at '" + m_OutputCaptured + "'. Check {output} vs the tool.";
            m_StatusError = true;
            return;
        }
        if ( !m_Assets || !m_Scene )
        {
            m_Status      = "No scene / asset manager to import into.";
            m_StatusError = true;
            return;
        }

        auto&      mgr      = const_cast<Assets::AssetManager&>( *m_Assets );
        const auto resolved = MeshDnD::ResolveOrImportMesh( mgr, m_OutputCaptured );
        if ( resolved.Handle.IsNull() )
        {
            m_Status      = "Reconstruction succeeded but the mesh failed to import.";
            m_StatusError = true;
            return;
        }

        auto& e = m_Scene->CreateNewEntity( "Reconstructed Model" );
        if ( resolved.Skinned )
        {
            e.AddComponent<ECS::SkinnedMeshComponent>();
            e.GetComponent<ECS::SkinnedMeshComponent>().MeshHandle = resolved.Handle;
            e.AddComponent<ECS::AnimationComponent>();
        }
        else
        {
            e.AddComponent<ECS::StaticMeshComponent>();
            e.GetComponent<ECS::StaticMeshComponent>().MeshHandle = resolved.Handle;
        }

        if ( !resolved.Skinned )
            SetPreviewMesh( resolved.Handle ); // show it spinning in the left viewport
        m_Status      = "Imported the reconstructed mesh and spawned it into the scene.";
        m_StatusError = false;
    }

    void PhotogrammetryPanel::Reimport()
    {
        if ( m_OutputCaptured.empty() )
            m_OutputCaptured = EditorPreferences::Get().PhotogrammetryOutputMesh;
        m_ExitCode = 0;
        ImportResult();
    }

    void PhotogrammetryPanel::LoadMeshFile()
    {
        const auto picked =
             Common::Utils::FileSystem::OpenFileDialog( "Meshes\0*.obj;*.glb;*.gltf;*.fbx;*.ply\0All\0*.*\0" );
        if ( picked.empty() )
            return;
        m_OutputCaptured = picked.string();
        m_ExitCode       = 0;
        ImportResult();
    }

    void PhotogrammetryPanel::OpenOutputFolder()
    {
        std::string out = m_OutputCaptured;
        if ( out.empty() )
            out = EditorPreferences::Get().PhotogrammetryOutputMesh;
        std::error_code   ec;
        const std::string dir = std::filesystem::absolute( out, ec ).parent_path().string();
        if ( dir.empty() )
            return;
#if defined( __APPLE__ )
        std::system( ( "open \"" + dir + "\"" ).c_str() );
#elif defined( __unix__ )
        std::system( ( "xdg-open \"" + dir + "\" &" ).c_str() );
#elif defined( _WIN32 )
        std::system( ( "explorer \"" + dir + "\"" ).c_str() );
#endif
    }

    // ---------------------------------------------------------------------------------------------------
    // Live mesh preview (own offscreen scene)
    // ---------------------------------------------------------------------------------------------------

    void PhotogrammetryPanel::EnsurePreview()
    {
        if ( m_PreviewInit )
            return;

        // NO CASCADES: this preview shows a reconstructed mesh under a fill light and has never drawn a
        // shadow. Said at CONSTRUCTION because that is when the cascade framebuffers are budgeted — the
        // `EnableShadows = false` that used to stand below ran after Scene::Init() had already allocated
        // them, so the flag read as the saving and 320 MiB was spent anyway. See Graphic::ShadowQuality.
        m_PreviewRenderer = std::make_unique<Graphic::SceneRenderer>( Graphic::kNoShadowQuality );
        m_PreviewScene    = std::make_shared<::Desert::Core::Scene>( "ReconPreview", m_PreviewRenderer.get() );
        // `m_PreviewInit` stays false so the next call retries; with the result dropped the flag was set
        // regardless and every frame afterwards recorded into a scene that had never initialised.
        if ( const auto inited = m_PreviewScene->Init(); !inited.IsSuccess() )
        {
            LOG_ERROR( "[Photogrammetry] preview scene failed to initialise: {}", inited.GetError() );
            m_PreviewScene.reset();
            m_PreviewRenderer.reset();
            return;
        }

        // No grid line here: overlays live on the RENDERER (Graphic::DebugViewState) and default to off,
        // and only the main editor loop pushes the user's flags into one.
        auto& settings       = m_PreviewScene->GetSettings();
        settings.EnableBloom = false;

        // `settings.AA = FXAA` used to stand here and restated the default. The mode is machine quality
        // now (К3) and this preview renderer is never pushed to, so it keeps the schema defaults.

        m_PreviewRenderer->SetOutlineSettings( glm::vec3( 0.0f ), 0.0f, 0.0f, false );

        auto cam                                                   = m_PreviewScene->CreateNewEntity( "PrevCam" );
        cam.AddComponent<ECS::CameraComponent>().Data.IsMainCamera = true;

        auto  light           = m_PreviewScene->CreateNewEntity( "PrevLight" );
        auto& lightC          = light.AddComponent<ECS::DirectionLightComponent>();
        lightC.Data.Intensity = 3.5f;
        lightC.Data.Color     = { 1.0f, 0.97f, 0.92f };
        light.GetComponent<ECS::TransformComponent>().Translation = { 2.0f, -6.0f, 5.0f };

        m_PreviewTarget = m_PreviewScene->CreateNewEntity( "PrevTarget" );
        m_PreviewTarget.AddComponent<ECS::StaticMeshComponent>();

        m_PreviewScene->AddSystem<ECS::MeshECSSystem>();
        m_PreviewScene->AddSystem<ECS::SkyboxECSSystem>();

        auto  skyEnt             = m_PreviewScene->CreateNewEntity( "PrevSky" );
        auto& skyC               = skyEnt.AddComponent<ECS::SkyAtmosphereComponent>();
        skyC.Data.ZenithColor    = { 0.26f, 0.46f, 0.78f };
        skyC.Data.HorizonColor   = { 0.62f, 0.73f, 0.87f };
        skyC.Data.GroundColor    = { 0.45f, 0.56f, 0.72f };
        skyC.Data.SunColor       = { 1.00f, 0.95f, 0.85f };
        skyC.Data.SkyBrightness  = 1.15f;
        skyC.Data.HorizonFalloff = 0.5f;
        skyC.Data.SunGlow        = 0.8f;
        skyC.Data.StarIntensity  = 0.0f;
        skyC.Data.SunIntensity   = 16.0f;
        skyC.RequestBake         = true;

        m_PreviewInit = true;

        // A rebuild after ReleasePreview starts with an empty target, so the mesh the user was looking at
        // has to be put back. Without this, closing and reopening the panel showed a blank pane with a
        // reconstructed model still selected — which reads as the reconstruction having been lost.
        if ( static_cast<uint64_t>( m_PreviewMesh ) != 0 )
            ApplyPreviewMesh();
    }

    void PhotogrammetryPanel::ReleasePreview()
    {
        if ( !m_PreviewInit )
            return;

        // The last frame this recorded into may still be executing against the preview's pipelines,
        // framebuffers and descriptor pools. Idle, then release the scene before the renderer that owns its
        // passes — whose destructor is what hands the renderer slot back
        // (Engine/Core/RendererSlotPool.hpp). Same order as ~PreviewViewport, which is the pattern this
        // editor's preview surfaces are destroyed by.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();
        m_PreviewTarget = ECS::Entity();
        m_PreviewScene.reset();
        m_PreviewRenderer.reset();
        m_PreviewInit = false;
        m_PreviewW    = 0;
        m_PreviewH    = 0;
    }

    void PhotogrammetryPanel::OnPreUpdate()
    {
        // Hidden or closed: give the slot back. This panel is opened rarely and left closed for whole
        // sessions, so a slot it never returned was a permanent tax on every other preview surface.
        if ( !m_SowPanel )
            ReleasePreview();
    }

    void PhotogrammetryPanel::SetPreviewMesh( const Assets::AssetHandle& mesh )
    {
        m_PreviewMesh = mesh;
        EnsurePreview();
        ApplyPreviewMesh();
    }

    void PhotogrammetryPanel::ApplyPreviewMesh()
    {
        auto& smc = m_PreviewTarget.GetComponent<ECS::StaticMeshComponent>();
        smc.RuntimeMesh.reset();
        smc.Primitive.reset();
        smc.RuntimeMaterialInstances.clear();
        smc.MaterialSlots.clear();
        smc.MeshHandle = m_PreviewMesh;

        // Center + scale the mesh to fill the fixed preview camera (which sits at ~distance 8.66).
        glm::vec3 center( 0.0f );
        float     extent = 1.0f;
        if ( auto* m = Runtime::ResourceRegistry::GetMeshService()->Get( m_PreviewMesh ) )
        {
            glm::vec3 mn( 1e9f ), mx( -1e9f );
            for ( const auto& sm : m->GetSubmeshes() )
            {
                const glm::vec3 lo = sm.BoundingBox.Min, hi = sm.BoundingBox.Max;
                for ( int corner = 0; corner < 8; ++corner )
                {
                    const glm::vec3 p( ( corner & 1 ) ? hi.x : lo.x, ( corner & 2 ) ? hi.y : lo.y,
                                       ( corner & 4 ) ? hi.z : lo.z );
                    const glm::vec3 w = glm::vec3( sm.Transform * glm::vec4( p, 1.0f ) );
                    mn                = glm::min( mn, w );
                    mx                = glm::max( mx, w );
                }
            }
            if ( mx.x >= mn.x )
            {
                center               = ( mn + mx ) * 0.5f;
                const glm::vec3 size = mx - mn;
                extent               = std::max( size.x, std::max( size.y, size.z ) );
            }
        }

        constexpr float kFitSpan = 4.0f;
        const float     scale    = extent > 1e-4f ? kFitSpan / extent : kFitSpan;
        auto&           tc       = m_PreviewTarget.GetComponent<ECS::TransformComponent>();
        tc.Scale                 = glm::vec3( scale );
        tc.Translation           = -center * scale;
    }

    void PhotogrammetryPanel::RenderPreview( uint32_t w, uint32_t h )
    {
        if ( !m_PreviewInit || w == 0 || h == 0 )
            return;

        if ( w != m_PreviewW || h != m_PreviewH )
        {
            m_PreviewScene->Resize( w, h );
            m_PreviewW = w;
            m_PreviewH = h;
        }

        // Slow orbit by spinning the model (the preview camera is fixed).
        m_Spin += 0.01f;
        m_PreviewTarget.GetComponent<ECS::TransformComponent>().Rotation = glm::vec3( 0.0f, m_Spin, 0.0f );

        // The scene brackets its own renderer now, so a frame that refuses cannot leave the editor's
        // command buffer holding half a pass — it is reported here and the preview skips a frame.
        if ( const auto frame = m_PreviewScene->OnUpdate( Common::Timestep( 0.016f ) ); !frame.IsSuccess() )
            LOG_ERROR( "[Photogrammetry] preview frame skipped: {}", frame.GetError() );
    }

    // ---------------------------------------------------------------------------------------------------
    // UI
    // ---------------------------------------------------------------------------------------------------

    void PhotogrammetryPanel::DrawToolbar( bool running )
    {
        auto& prefs = EditorPreferences::Get();

        ImGui::PushStyleColor( ImGuiCol_ChildBg, kColHeader );
        ImGui::BeginChild( "##pgToolbar", ImVec2( 0.0f, 44.0f ), false,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse );
        ImGui::SetCursorPos( ImVec2( 8.0f, 8.0f ) );

        // Start / Stop camera.
        if ( !m_CameraOn )
        {
            ImGui::PushStyleColor( ImGuiCol_Button, kColAccent );
            ImGui::PushStyleColor( ImGuiCol_ButtonHovered, kColAccentHov );
            if ( ImGui::Button( ICON_MDI_VIDEO "  Start camera", ImVec2( 150.0f, 28.0f ) ) )
                StartCamera();
            ImGui::PopStyleColor( 2 );
        }
        else
        {
            ImGui::PushStyleColor( ImGuiCol_Button, kColDanger );
            if ( ImGui::Button( ICON_MDI_VIDEO_OFF "  Stop camera", ImVec2( 150.0f, 28.0f ) ) )
                StopCamera();
            ImGui::PopStyleColor();
        }
        ImGui::SameLine( 0.0f, 6.0f );

        // Record toggle.
        ImGui::BeginDisabled( !m_CameraOn );
        ImGui::PushStyleColor( ImGuiCol_Button, m_Recording ? kColRecord : ImVec4( 0.22f, 0.22f, 0.24f, 1.0f ) );
        if ( ImGui::Button( m_Recording ? ICON_MDI_RECORD "  Recording…" : ICON_MDI_RECORD "  Record",
                            ImVec2( 150.0f, 28.0f ) ) )
        {
            m_Recording = !m_Recording;
            if ( m_Recording )
            {
                m_RecordedFrames = 0;
                m_Status         = "Recording frames — slowly move around the subject.";
                m_StatusError    = false;
            }
        }
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::SameLine( 0.0f, 6.0f );

        // Reconstruct.
        std::error_code ec;
        bool            hasFrames = !prefs.PhotogrammetryPhotosDir.empty();
        if ( hasFrames )
            hasFrames = std::filesystem::is_directory( prefs.PhotogrammetryPhotosDir, ec );
        ImGui::BeginDisabled( running || !hasFrames );
        ImGui::PushStyleColor( ImGuiCol_Button, kColAccent );
        ImGui::PushStyleColor( ImGuiCol_ButtonHovered, kColAccentHov );
        if ( ImGui::Button( ICON_MDI_CUBE_SCAN "  Reconstruct", ImVec2( 150.0f, 28.0f ) ) )
            StartReconstruction();
        ImGui::PopStyleColor( 2 );
        ImGui::EndDisabled();
        ImGui::SameLine( 0.0f, 6.0f );

        // Cancel.
        ImGui::BeginDisabled( !running );
        ImGui::PushStyleColor( ImGuiCol_Button, kColDanger );
        if ( ImGui::Button( ICON_MDI_STOP "  Cancel", ImVec2( 110.0f, 28.0f ) ) )
            CancelJob();
        ImGui::PopStyleColor();
        ImGui::EndDisabled();

        // Right-aligned: Import result + Open output.
        const float rightW = 130.0f + 150.0f + 12.0f;
        ImGui::SameLine();
        ImGui::SetCursorPosX( ImGui::GetWindowContentRegionMax().x - rightW );
        std::string outPath = m_OutputCaptured;
        if ( outPath.empty() )
            outPath = prefs.PhotogrammetryOutputMesh;
        const bool hasOutput = std::filesystem::exists( outPath, ec );
        ImGui::BeginDisabled( running || !hasOutput );
        if ( ImGui::Button( ICON_MDI_IMPORT "  Import result", ImVec2( 130.0f, 28.0f ) ) )
            Reimport();
        ImGui::EndDisabled();
        ImGui::SameLine( 0.0f, 6.0f );
        if ( ImGui::Button( ICON_MDI_FOLDER_OPEN "  Open output", ImVec2( 150.0f, 28.0f ) ) )
            OpenOutputFolder();

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void PhotogrammetryPanel::DrawPreviewPane( const ImVec2& size )
    {
        ImGui::PushStyleColor( ImGuiCol_ChildBg, kColSection );
        ImGui::BeginChild( "##pgPreview", size, true );
        ImGui::PopStyleColor();

        ImGui::TextColored( ImVec4( 0.70f, 0.78f, 0.90f, 1.0f ), ICON_MDI_CUBE_OUTLINE " Reconstructed model" );
        ImGui::Separator();

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        if ( static_cast<uint64_t>( m_PreviewMesh ) == 0 )
        {
            ImGui::Dummy( ImVec2( 0.0f, avail.y * 0.40f ) );
            const char* msg = "Reconstruct to see the model here.";
            ImGui::SetCursorPosX( ( ImGui::GetWindowWidth() - ImGui::CalcTextSize( msg ).x ) * 0.5f );
            ImGui::TextDisabled( "%s", msg );
            ImGui::Spacing();
            const float btnW = 150.0f;
            ImGui::SetCursorPosX( ( ImGui::GetWindowWidth() - btnW ) * 0.5f );
            if ( ImGui::Button( ICON_MDI_FILE_IMPORT "  Load mesh…", ImVec2( btnW, 0.0f ) ) )
                LoadMeshFile(); // preview an existing mesh (test the viewport without a photogrammetry tool)
        }
        else if ( avail.x > 4.0f && avail.y > 4.0f )
        {
            EnsurePreview();
            RenderPreview( static_cast<uint32_t>( avail.x ), static_cast<uint32_t>( avail.y ) );
            if ( auto img = m_PreviewScene ? m_PreviewScene->GetFinalImage() : nullptr )
                m_UIHelper->Image( img, avail );
        }

        ImGui::EndChild();
    }

    void PhotogrammetryPanel::DrawCameraPane( const ImVec2& size )
    {
        ImGui::PushStyleColor( ImGuiCol_ChildBg, kColSection );
        ImGui::BeginChild( "##pgCamera", size, true );
        ImGui::PopStyleColor();

        ImGui::TextColored( ImVec4( 0.70f, 0.78f, 0.90f, 1.0f ), ICON_MDI_CAMERA " Camera" );
        ImGui::SameLine();
        if ( m_CameraOn )
            ImGui::TextColored( kColOk, ICON_MDI_CIRCLE " live" );
        else
            ImGui::TextDisabled( ICON_MDI_CIRCLE_OUTLINE " off" );
        if ( m_Recording )
        {
            ImGui::SameLine();
            ImGui::TextColored( kColRecord, ICON_MDI_RECORD " %d frames", m_RecordedFrames );
        }
        ImGui::Separator();

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        if ( m_CameraImage && m_CamW > 0 && m_CamH > 0 )
        {
            // Fit the frame into the pane preserving aspect.
            const float aspect = static_cast<float>( m_CamW ) / static_cast<float>( m_CamH );
            float       dw     = avail.x;
            float       dh     = dw / aspect;
            if ( dh > avail.y )
            {
                dh = avail.y;
                dw = dh * aspect;
            }
            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + ( avail.x - dw ) * 0.5f );
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            m_UIHelper->Image( m_CameraImage, ImVec2( dw, dh ) );

            if ( !m_Landmarks.empty() && m_CamW > 0 && m_CamH > 0 )
            {
                // Real tracked points: map image-pixel coords into the displayed rect.
                ImDrawList* dl  = ImGui::GetWindowDrawList();
                const float sx  = dw / static_cast<float>( m_CamW );
                const float sy  = dh / static_cast<float>( m_CamH );
                const ImU32 col = IM_COL32( 70, 245, 90, 255 );
                for ( const auto& p : m_Landmarks )
                    dl->AddCircleFilled( ImVec2( cursor.x + p.x * sx, cursor.y + p.y * sy ), 2.6f, col );
                dl->AddText( ImVec2( cursor.x + 6.0f, cursor.y + 4.0f ), IM_COL32( 150, 245, 150, 235 ),
                             "landmarks (dlib)" );
            }
            else
            {
                DrawLandmarks( cursor, ImVec2( cursor.x + dw, cursor.y + dh ) );
            }
        }
        else
        {
            ImGui::Dummy( ImVec2( 0.0f, avail.y * 0.45f ) );
            const char* msg = m_CameraOn ? "Waiting for the camera…" : "Press ‘Start camera’.";
            ImGui::SetCursorPosX( ( ImGui::GetWindowWidth() - ImGui::CalcTextSize( msg ).x ) * 0.5f );
            ImGui::TextDisabled( "%s", msg );
        }

        ImGui::EndChild();
    }

    void PhotogrammetryPanel::DrawBottom( bool running )
    {
        auto& prefs = EditorPreferences::Get();
        bool  dirty = false;

        if ( Utils::ImGuiUtilities::SectionHeader( ICON_MDI_TUNE "  Reconstruction settings", false ) )
        {
            ImGui::TextDisabled( "{input}=frames folder  {output}=mesh file  {outdir}=its folder" );
            dirty |= PresetCombo( "##recPreset", prefs.PhotogrammetryCommand );
            dirty |= CommandField( "##recCmd", prefs.PhotogrammetryCommand );
            ImGui::TextDisabled( "Frames folder" );
            dirty |= PathPicker( "frames", prefs.PhotogrammetryPhotosDir, /*folder=*/true );
            ImGui::TextDisabled( "Output mesh" );
            dirty |= PathPicker( "outmesh", prefs.PhotogrammetryOutputMesh, /*folder=*/false );
        }

        if ( Utils::ImGuiUtilities::SectionHeader( ICON_MDI_FACE_RECOGNITION "  Face tracking (landmarks)",
                                                   false ) )
        {
            if ( !FaceTracker::Compiled() )
            {
                ImGui::TextColored( kColWarn,
                                    ICON_MDI_ALERT " dlib not built in — the overlay is a placeholder." );
                ImGui::TextDisabled( "Clone https://github.com/davisking/dlib into ThirdParty/dlib and rebuild "
                                     "(premake auto-enables it)." );
            }
            else
            {
                ImGui::TextDisabled( "dlib 68-point model (shape_predictor_68_face_landmarks.dat)." );
                if ( PathPicker( "facemodel", prefs.PhotogrammetryFaceModel, /*folder=*/false ) )
                {
                    dirty = true;
                    if ( m_FaceTracker && m_FaceTracker->LoadModel( prefs.PhotogrammetryFaceModel ) )
                        m_LoadedFaceModel = prefs.PhotogrammetryFaceModel;
                    else
                        m_LoadedFaceModel.clear();
                }
                if ( m_FaceTracker && m_FaceTracker->Ready() )
                    ImGui::TextColored( kColOk, ICON_MDI_CHECK_CIRCLE " Tracking model loaded." );
                else
                    ImGui::TextColored( kColWarn, ICON_MDI_ALERT " No model loaded — pick the .dat file above." );
            }
        }

        if ( dirty )
            EditorPreferences::Save();

        // Log.
        ImGui::TextColored( ImVec4( 0.70f, 0.78f, 0.90f, 1.0f ), ICON_MDI_CONSOLE " Output log" );
        ImGui::SameLine();
        if ( running )
            ImGui::TextColored( kColWarn, ICON_MDI_PROGRESS_CLOCK " running…" );
        ImGui::SameLine();
        if ( ImGui::SmallButton( ICON_MDI_DELETE " Clear" ) )
        {
            std::lock_guard<std::mutex> lk( m_LogMutex );
            m_Log.clear();
        }
        ImGui::SameLine();
        ImGui::Checkbox( "Auto-scroll", &m_LogAutoScroll );

        ImGui::PushStyleColor( ImGuiCol_ChildBg, kColLogBg );
        ImGui::BeginChild( "##pgLog", ImVec2( 0.0f, 110.0f ), true, ImGuiWindowFlags_HorizontalScrollbar );
        {
            std::lock_guard<std::mutex> lk( m_LogMutex );
            for ( const auto& line : m_Log )
            {
                const bool isCmd = ( !line.empty() && line[0] == '$' );
                const bool isErr =
                     ( line.find( "error" ) != std::string::npos || line.find( "Error" ) != std::string::npos ||
                       line.find( "ERROR" ) != std::string::npos );
                if ( isCmd )
                    ImGui::TextColored( ImVec4( 0.55f, 0.80f, 1.0f, 1.0f ), "%s", line.c_str() );
                else if ( isErr )
                    ImGui::TextColored( kColErr, "%s", line.c_str() );
                else
                    ImGui::TextUnformatted( line.c_str() );
            }
        }
        if ( m_LogAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f )
            ImGui::SetScrollHereY( 1.0f );
        ImGui::EndChild();
        ImGui::PopStyleColor();

        if ( !m_Status.empty() )
        {
            const ImVec4 col = m_StatusError ? kColErr : kColOk;
            ImGui::PushTextWrapPos( 0.0f );
            ImGui::TextColored( col, "%s", m_Status.c_str() );
            ImGui::PopTextWrapPos();
        }
    }

    void PhotogrammetryPanel::OnUIRender()
    {
        // Header.
        ImGui::TextColored( ImVec4( 0.55f, 0.80f, 1.0f, 1.0f ), ICON_MDI_CAMERA_OUTLINE );
        ImGui::SameLine( 0.0f, 6.0f );
        ImGui::TextUnformatted( "Model from Camera" );
        ImGui::SameLine( 0.0f, 6.0f );
        ImGui::TextColored( kColWarn, "(WIP)" );
        ImGui::SameLine();
        ImGui::TextDisabled( ICON_MDI_HELP_CIRCLE_OUTLINE );
        if ( ImGui::IsItemHovered() )
        {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos( 420.0f );
            ImGui::TextUnformatted(
                 "Capture-to-mesh from the webcam. Start the camera, press Record and slowly move around the "
                 "subject to collect frames, then Reconstruct — an EXTERNAL tool (Meshroom / COLMAP / ...) "
                 "turns the frames into a mesh, which is imported and shown spinning on the left.\n\n"
                 "Landmarks on the camera view are a placeholder; real face tracking is a follow-up external "
                 "tool." );
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }

        // Poll the reconstruction worker.
        if ( m_Done.exchange( false ) )
        {
            if ( m_Worker.joinable() )
                m_Worker.join();
            if ( m_Job == Job::Reconstruct )
                ImportResult();
            m_Job = Job::None;
        }

        UpdateCamera();

        const bool running = m_Running.load();

        DrawToolbar( running );
        ImGui::Spacing();

        // Split view: reconstructed model (left) | live camera (right).
        const float bottom = 110.0f + ImGui::GetTextLineHeightWithSpacing() * 4.0f;
        const float paneH  = ImGui::GetContentRegionAvail().y - bottom;
        const float totalW = ImGui::GetContentRegionAvail().x;
        const float halfW  = ( totalW - 8.0f ) * 0.5f;

        if ( paneH > 40.0f )
        {
            DrawPreviewPane( ImVec2( halfW, paneH ) );
            ImGui::SameLine( 0.0f, 8.0f );
            DrawCameraPane( ImVec2( totalW - halfW - 8.0f, paneH ) );
        }

        ImGui::Spacing();
        DrawBottom( running );
    }
} // namespace Desert::Editor
