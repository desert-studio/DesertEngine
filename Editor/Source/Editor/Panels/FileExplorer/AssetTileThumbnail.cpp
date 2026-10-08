#define IMGUI_DEFINE_MATH_OPERATORS

// UIHelper pulls engine headers that use std::max/std::min; keep the windows.h macros from clobbering them.
#define NOMINMAX

#include "AssetTileThumbnail.hpp"

#include <Editor/Panels/FileExplorer/AssetThumbnailPool.hpp>
#include <Editor/Panels/FileExplorer/AssetTooltipLayout.hpp>
#include <Editor/Panels/FileExplorer/DirectoryInformation.hpp>
#include <Editor/Panels/FileExplorer/FileTypeInfo.hpp>
#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailKey.hpp>
#include <Editor/Widgets/ThumbnailPose.hpp>
#include <Editor/Widgets/ThumbnailProducers.hpp>
#include <Editor/Widgets/ThumbnailService.hpp>
#include <Editor/Widgets/ThumbnailSubject.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>
#include "../../Core/EditorResources.hpp"
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    AssetTileThumbnail::AssetTileThumbnail( AssetThumbnailPool& pool, Assets::AssetManager* assetManager )
         : m_Pool( pool ), m_AssetManager( assetManager ), m_UIHelper( std::make_unique<UI::UIHelper>() )
    {
        m_UIHelper->Init();
    }

    AssetTileThumbnail::~AssetTileThumbnail() = default;

    ImTextureID AssetTileThumbnail::TextureOf( const std::string& png )
    {
        const auto img = m_Pool.Cache().Get( png );
        return img ? m_UIHelper->GetTextureID( img ) : nullptr;
    }

    void AssetTileThumbnail::ForgetTooltip( const DirectoryInformation* entry )
    {
        if ( m_TooltipEntry == entry )
            m_TooltipEntry = nullptr;
    }

    void AssetTileThumbnail::DrawDragPreview( const DirectoryInformation& entry )
    {
        const std::string& assetPath = entry.AssetPath;
        // Drag preview: the tile's thumbnail (texture/material/model, when cached) or the big
        // coloured type icon, with the filename beside it — mirrors what the user grabbed.
        std::shared_ptr<Graphic::Image2D> img;
        if ( entry.Type == FileType::Texture )
            img = m_Pool.Cache().Get( assetPath );
        else if ( entry.Type == FileType::Material || entry.Type == FileType::Cloud )
            img = m_Pool.Cache().Get( ThumbnailKey::DiskPath( assetPath ) );
        else if ( entry.Type == FileType::Model )
        {
            // THE COOKED KEY, not the source one. This branch used to share the material's line,
            // so it looked up the picture under `DiskPath(<source>.fbx)` — a name nothing has
            // written since M10 moved a mesh's thumbnail onto its cooked `.stmesh`. The ghost has
            // been silently falling back to the type icon for every mesh ever since, which is
            // exactly the kind of "it still works, just worse" a re-read site decays into.
            if ( const std::optional<AssetThumbnailPool::MeshPicture> picture =
                      m_Pool.MeshPictureFor( assetPath, entry.Type ) )
                img = m_Pool.Cache().Get( ThumbnailKey::DiskPath( picture->Cooked ) );
        }

        constexpr float previewSize = 48.0f;
        if ( img && m_UIHelper )
            m_UIHelper->Image( img, ImVec2( previewSize, previewSize ) );
        else
        {
            const char*  icon = entry.IsFile ? FileTypeInfoOf( entry.Type ).Icon : ICON_MDI_FOLDER;
            const ImVec4 col  = entry.IsFile ? entry.FileTypeColour : ImVec4( 0.95f, 0.82f, 0.42f, 1.0f );
            ImGui::PushFont( EditorResources::GetBigIconFont() );
            ImGui::TextColored( col, "%s", icon );
            ImGui::PopFont();
        }
        ImGui::SameLine();
        // Center the single-line filename against the preview block.
        ImGui::SetCursorPosY( ImGui::GetCursorPosY() +
                              std::max( 0.0f, ( previewSize - ImGui::GetTextLineHeight() ) * 0.5f ) );
        ImGui::TextUnformatted( std::filesystem::path( assetPath ).filename().string().c_str() );
    }

    bool AssetTileThumbnail::DrawThumbnail( DirectoryInformation* entry, const ImVec2& size )
    {
        using ThumbnailProducers::Producer;
        const std::optional<Producer> producer = ThumbnailProducers::ProducerOf( entry->Type );
        if ( !producer )
            return false; // a kind with no row: ThumbnailProducers' census names it
        switch ( *producer )
        {
            case Producer::Decoded:
                return DrawTextureThumbnail( entry, size );
            case Producer::RenderedMaterial:
                return DrawRenderedMaterialThumbnail( entry, size );
            case Producer::RenderedMesh:
                return DrawRenderedMeshThumbnail( entry, size );
            case Producer::RenderedPose:
                return DrawRenderedPoseThumbnail( entry, size, entry->AssetPath );
            case Producer::Painted:
                return DrawPaintedThumbnail( entry, size );
            case Producer::RenderedSky:
            {
                // The Details Skybox row asks the same request with the same key: one picture per skybox.
                const Assets::AssetHandle skybox = Runtime::SkyboxHandleAtPath( entry->AssetPath );
                if ( static_cast<uint64_t>( skybox ) == 0 )
                    return false;
                const std::string png = ThumbnailService::Get().RequestSkybox( skybox, entry->AssetPath );
                if ( ThumbnailService::JudgeSkyboxPicture( entry->AssetPath ) !=
                     ThumbnailFreshness::Verdict::Show )
                {
                    m_Pool.Cache().Invalidate( png );
                    return false;
                }
                if ( auto img = m_Pool.Cache().Get( png ) )
                {
                    m_UIHelper->ImageButton( "##thumb", img, size );
                    return true;
                }
                return false;
            }
            case Producer::NotYetProduced:
            case Producer::TypeIcon:
                return false;
        }
        return false;
    }

    bool AssetTileThumbnail::DrawTextureThumbnail( DirectoryInformation* entry, const ImVec2& size )
    {
        if ( !m_UIHelper )
            return false;

        // Decode the source image directly (cached), independent of the cook pipeline — so EVERY image
        // previews, not just already-cooked ones.
        auto img = m_Pool.Cache().Get( entry->AssetPath );
        if ( !img )
            return false;

        // ImageButton (not Image) so the thumbnail is a real interactive item and can be a drag source.
        m_UIHelper->ImageButton( "##thumb", img, size );
        return true;
    }

    bool AssetTileThumbnail::DrawRenderedMaterialThumbnail( DirectoryInformation* entry, const ImVec2& size )
    {
        if ( !m_UIHelper || m_AssetManager == nullptr )
            return false;

        // Cache PNG path: <versioned thumbnail dir>/<sanitized source path>.png (persists across restarts).
        const std::string& pngPath = m_Pool.ThumbnailPngFor( entry->AssetPath );

        // Through Editor/Widgets/ThumbnailFreshness.hpp, the same rule ThumbnailService::ShouldQueue applies:
        // Judge says whether a capture is owed, Choose says what to draw meanwhile. The PNG is drawn FIRST,
        // before the material is resolved or loaded — a card whose picture is on disk never waits for the
        // asset, and an outdated picture stays on screen until its replacement lands (ThumbnailCache::Get
        // re-decodes the rewritten file), instead of a flat albedo swatch for the whole queue.
        const ThumbnailFreshness::Observation seen = ThumbnailFreshness::Observe( pngPath, entry->AssetPath );
        const bool owed = ThumbnailFreshness::Judge( seen ) == ThumbnailFreshness::Verdict::Capture;
        bool       drew = false;
        if ( ThumbnailFreshness::Choose( seen ) == ThumbnailFreshness::Picture::CachedPng )
        {
            if ( auto img = m_Pool.Cache().Get( pngPath ) )
            {
                m_UIHelper->ImageButton( "##thumb", img, size );
                drew = true;
            }
        }
        if ( drew && !owed )
        {
            m_CaptureAsked.erase( entry->AssetPath ); // current: a later edit that makes it stale asks again
            return true;
        }
        if ( const auto asked = m_CaptureAsked.find( entry->AssetPath ); asked != m_CaptureAsked.end() )
        {
            if ( !drew )
                ImGui::ColorButton( "##matswatch", asked->second,
                                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop |
                                         ImGuiColorEditFlags_NoBorder,
                                    size );
            return true;
        }

        // Resolve material -> handle (load + register so the offscreen render can use it). Through
        // Editor/Widgets/ThumbnailSubject.hpp, which is the SAME resolution the background sweep uses —
        // the two used to be one copy each, and "which file is photographed" is exactly the question this
        // subsystem has already answered twice and differently once.
        //
        // PENDING IS A FRAME OR TWO OF THE PLACEHOLDER: the material is being read on a worker, and when
        // it lands the arrival delegate queues the capture exactly as the line below would have.
        const auto subject = ThumbnailSubject::ResolveMaterial(
             *m_AssetManager, entry->AssetPath,
             []( const std::string& assetPath, const Common::ResultStr<ThumbnailSubject::Material>& resolved )
             {
                 if ( resolved )
                     ThumbnailService::Get().RequestMaterial( resolved.GetValue(), assetPath );
                 else
                     ThumbnailService::Get().Refuse( assetPath, resolved.GetError() );
             } );
        // A REFUSAL IS SAID, not dropped (THM1n-10): M_CubemapCheck (Skybox domain) and M_CheckerFloor_Inst
        // kept a document icon with no request and no line in the log. The card still shows its icon; the
        // log names why, once per asset.
        if ( !subject )
        {
            ThumbnailService::Get().Refuse( entry->AssetPath, subject.GetError() );
            return drew;
        }
        const auto& material = subject.GetValue();
        if ( !material )
            return drew;

        auto a = m_AssetManager->FindByPath<Assets::SurfaceMaterialAsset>( entry->AssetPath );
        if ( !a )
            return drew;

        // Queue through the editor-wide service: it owns the one renderer, deduplicates against what other
        // panels already asked for, skips anything already on disk and never retries an asset that failed.
        ThumbnailService::Get().RequestMaterial( *material, entry->AssetPath );

        // No picture of this material exists yet: the albedo colour is the placeholder.
        const glm::vec3 albedo =
             glm::vec3( a->Data().GetParam( "AlbedoColor", glm::vec4( 0.8f, 0.8f, 0.8f, 1.0f ) ) );
        const ImVec4 swatch( albedo.r, albedo.g, albedo.b, 1.0f );
        m_CaptureAsked.emplace( entry->AssetPath, swatch );
        if ( drew )
            return true;
        ImGui::ColorButton(
             "##matswatch", swatch,
             ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop | ImGuiColorEditFlags_NoBorder, size );
        return true;
    }

    bool AssetTileThumbnail::DrawRenderedMeshThumbnail( DirectoryInformation* entry, const ImVec2& size )
    {
        if ( !m_UIHelper || m_AssetManager == nullptr )
            return false;
        if ( m_Pool.IsRefused( entry->AssetPath ) ) // failed to load before -> icon, no per-frame retry
            return false;

        // ONE PICTURE, ONE KEY, AND THE KEY IS THE FILE THAT IS ACTUALLY PHOTOGRAPHED.
        //
        // This grid used to file a mesh's thumbnail under its SOURCE (`assets:Meshes/x.fbx`) while the
        // Details 3D Model row files it under the COOKED form (`cooked:Meshes/x.stmesh`) — because a scene
        // holds the cooked handle and nothing else. Same mesh, same render, two cache files: a mesh both
        // browsed and placed in a scene was photographed TWICE, at 370 ms and ~200 KB a time, and neither
        // capture could ever satisfy the other panel.
        //
        // Cooked is the side that had to win, and not merely because the Details row cannot reach the
        // source. Freshness is a comparison against the recipe, and the recipe for this picture is the
        // .stmesh — StaticMeshAsset::Load reads cooked JSON and never opens the FBX. Judging against the
        // source asked whether a file the capture never reads had changed: re-cooking an unchanged FBX left
        // a stale picture called fresh, and touching an FBX without re-cooking threw away a picture that
        // still matched the geometry exactly.
        //
        // The mapping is a pure path computation (CookPaths::MeshAsset — an extension swap, no stat), so
        // hoisting it above the freshness check costs nothing; the `exists()` gate that decides "not cooked
        // -> icon" stays where it was, below, because that one IS a filesystem question.
        const std::optional<AssetThumbnailPool::MeshPicture> picture =
             m_Pool.MeshPictureFor( entry->AssetPath, entry->Type );
        if ( !picture )
            return false; // not imported, clips only, or refused and named: the type icon
        if ( picture->Pose )
            return DrawRenderedPoseThumbnail( entry, size,
                                              picture->Cooked ); // a skinned source: its import's pose
        const std::optional<std::string> source =
             m_Pool.MeshSourceFor( entry->AssetPath, entry->Type ); // a model, or a foliage type's mesh
        if ( !source )
            return false;
        const std::string& cookedStr = picture->Cooked;

        const std::string pngPath = ThumbnailKey::DiskPath( cookedStr );

        // The one verdict of a mesh picture (ThumbnailService::JudgeMeshPicture): the enqueue gate's key and hash.
        const bool haveFresh =
             ThumbnailService::JudgeMeshPicture( cookedStr ) == ThumbnailFreshness::Verdict::Show;
        if ( !haveFresh )
            m_Pool.Cache().Invalidate( pngPath );
        if ( haveFresh )
        {
            if ( auto img = m_Pool.Cache().Get( pngPath ) )
            {
                m_UIHelper->ImageButton( "##thumb", img, size );
                return true;
            }
        }

        // Cook lookup, build and the three refusals now live in Editor/Widgets/ThumbnailSubject.hpp, so
        // this tile and the background sweep resolve a mesh the same way. Every one of those refusals is
        // permanent for the session here — an uncooked source, a cooked file that will not build, a mesh
        // with no drawable submeshes — so the blacklist keeps the (logging) retry from happening once per
        // frame, exactly as it did when the code was in this function.
        const auto subject = ThumbnailSubject::ResolveMesh( *m_AssetManager, *source );
        if ( !subject )
        {
            // Once per asset (the blacklist stops the retry): a tile left on its type icon says why.
            LOG_WARN( "[Thumbnail] '{}': {}", entry->AssetPath, subject.GetError() );
            m_Pool.Refuse( entry->AssetPath );
            return false;
        }
        // Read in flight: the tile asks again next frame and meets it resident (never blacklisted).
        if ( subject.GetValue().Pending )
            return false;

        ThumbnailService::Get().RequestMesh( subject.GetValue().Handle, subject.GetValue().CookedPath,
                                             subject.GetValue().Material );

        // No swatch for meshes — fall back to the type icon until the PNG is ready.
        return false;
    }

    bool AssetTileThumbnail::DrawRenderedPoseThumbnail( DirectoryInformation* entry, const ImVec2& size,
                                                        const std::string& subject )
    {
        if ( !m_UIHelper || m_AssetManager == nullptr )
            return false;
        if ( m_Pool.IsRefused( entry->AssetPath ) ) // refused before -> icon, no per-frame retry
            return false;

        // The mesh tile's rule, with the .skmesh as its own cooked form: one key, one freshness source.
        const std::string& pngPath = m_Pool.ThumbnailPngFor( subject );
        const bool haveFresh = ThumbnailService::JudgeMeshPicture( subject ) == ThumbnailFreshness::Verdict::Show;
        if ( !haveFresh )
            m_Pool.Cache().Invalidate( pngPath );
        else if ( auto img = m_Pool.Cache().Get( pngPath ) )
        {
            m_UIHelper->ImageButton( "##thumb", img, size );
            return true;
        }

        const auto posed = ThumbnailPose::ResolvePoseSubject( *m_AssetManager, subject );
        if ( !posed )
        {
            LOG_WARN( "[Thumbnail] '{}': {}", entry->AssetPath, posed.GetError() );
            m_Pool.Refuse( entry->AssetPath );
            return false;
        }
        if ( posed.GetValue().Pending )
            return false; // read in flight: asked again next frame
        ThumbnailService::Get().RequestPose( posed.GetValue() );
        return false;
    }

    bool AssetTileThumbnail::DrawPaintedThumbnail( DirectoryInformation* entry, const ImVec2& size )
    {
        if ( !m_UIHelper )
            return false;

        // NO ASSET MANAGER IN THIS FUNCTION, and that is the shape of the whole cloud path rather than an
        // oversight: the picture is computed from the file's own bytes, so nothing has to be created,
        // loaded or registered before it can be drawn. It is also why this tile keeps working in a
        // project whose asset layer has not finished starting.
        const std::string pngPath = ThumbnailKey::DiskPath( entry->AssetPath );

        const bool haveFresh =
             ThumbnailFreshness::Judge( ThumbnailFreshness::Observe( pngPath, entry->AssetPath ) ) ==
             ThumbnailFreshness::Verdict::Show;
        if ( !haveFresh )
            m_Pool.Cache().Invalidate( pngPath );

        if ( haveFresh )
        {
            if ( auto img = m_Pool.Cache().Get( pngPath ) )
            {
                m_UIHelper->ImageButton( "##thumb", img, size );
                return true;
            }
        }

        ThumbnailService::Get().RequestPainted( entry->AssetPath );

        // The type icon until the PNG lands — no placeholder swatch, because unlike a material there is
        // no single colour that says anything true about a cloud volume.
        return false;
    }

    void AssetTileThumbnail::DrawTooltip( DirectoryInformation* entry )
    {
        namespace Layout = Desert::Editor::AssetTooltipLayout;

        // The delay is ours because this ImGui (1.89 WIP) predates ImGuiHoveredFlags_DelayNormal.
        const double now = ImGui::GetTime();
        if ( m_TooltipEntry != entry )
        {
            m_TooltipEntry      = entry;
            m_TooltipHoverStart = now;
        }
        if ( !Layout::ShouldShow( static_cast<float>( now - m_TooltipHoverStart ) ) )
            return;

        const std::filesystem::path path( entry->AssetPath );
        const std::string           name     = path.filename().string();
        const char*                 typeName = entry->IsFile ? FileTypeInfoOf( entry->Type ).Name : "Folder";
        std::string                 sizeText;
        if ( entry->IsFile )
        {
            char buffer[32];
            if ( entry->FileSize >= std::size_t{ 1024 } * 1024 )
                std::snprintf( buffer, sizeof( buffer ), "%.1f MB", entry->FileSize / ( 1024.0f * 1024.0f ) );
            else
                std::snprintf( buffer, sizeof( buffer ), "%.1f KB", entry->FileSize / 1024.0f );
            sizeText = buffer;
        }
        // Shown relative to the PROJECT's directory (FPaths::ProjectDir), never to the working directory the
        // editor happened to be started from; a file outside the project (engine content in a foreign
        // project) keeps its full path rather than a chain of "..".
        std::error_code   ec;
        const std::string shownPath =
             std::filesystem::relative( path, Common::Constants::Path::ProjectDir(), ec ).generic_string();
        const bool         outside  = shownPath.starts_with( ".." );
        const std::string& pathText = ec || shownPath.empty() || outside ? entry->AssetPath : shownPath;

        // Natural size of the content: a 96 px picture beside name / type+size / path, the text wrapped
        // to what is left of the width cap. Layout::Compute then caps and places it on screen.
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec2      thumbSize( 96.0f, 96.0f );
        const float       wrapWidth =
             Layout::kMaxWidth - thumbSize.x - style.ItemSpacing.x - 2.0f * style.WindowPadding.x;
        const float textH = ImGui::CalcTextSize( name.c_str(), nullptr, false, wrapWidth ).y +
                            ImGui::GetTextLineHeightWithSpacing() +
                            ImGui::CalcTextSize( pathText.c_str(), nullptr, false, wrapWidth ).y +
                            2.0f * style.ItemSpacing.y;
        const float wantedH = std::max( thumbSize.y, textH ) + 2.0f * style.WindowPadding.y;

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const Layout::Rect   placed   = Layout::Compute(
             ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y, Layout::kMaxWidth, wantedH,
             Layout::Rect{ viewport->Pos.x, viewport->Pos.y, viewport->Size.x, viewport->Size.y } );
        ImGui::SetNextWindowPos( ImVec2( placed.X, placed.Y ) );
        ImGui::SetNextWindowSize( ImVec2( placed.Width, placed.Height ) );
        ImGui::BeginTooltip();

        const bool drewThumb = DrawThumbnail( entry, thumbSize );
        if ( !drewThumb )
        {
            const char*  icon   = FileTypeInfoOf( entry->Type ).Icon;
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 sz     = ImGui::CalcTextSize( icon );
            ImGui::GetWindowDrawList()->AddRectFilled( origin,
                                                       ImVec2( origin.x + thumbSize.x, origin.y + thumbSize.y ),
                                                       IM_COL32( 31, 31, 36, 255 ), 4.0f );
            ImGui::GetWindowDrawList()->AddText(
                 ImVec2( origin.x + ( thumbSize.x - sz.x ) * 0.5f, origin.y + ( thumbSize.y - sz.y ) * 0.5f ),
                 ImGui::GetColorU32( entry->FileTypeColour ), icon );
            ImGui::Dummy( thumbSize );
        }

        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos( ImGui::GetCursorPosX() + wrapWidth );
        ImGui::TextUnformatted( name.c_str() );
        if ( sizeText.empty() )
            ImGui::TextDisabled( "%s", typeName );
        else
            ImGui::TextDisabled( "%s  |  %s", typeName, sizeText.c_str() );
        ImGui::TextDisabled( "%s", pathText.c_str() );
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();

        ImGui::EndTooltip();
    }
} // namespace Desert::Editor
