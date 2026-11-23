#include "MeshSlotThumbnail.hpp"

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Editor/Widgets/ThumbnailService.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>

#include <ImGui/imgui.h>

#include <memory>
#include <string>

namespace Desert::Editor
{
    void DrawMeshSlotThumbnail( Assets::AssetManager* manager, UI::UIHelper* uiHelper, Assets::AssetHandle mesh,
                                Assets::AssetHandle fallbackMaterial, float size )
    {
        // THE REQUEST COMES FIRST, BEFORE THE LIVE PREVIEW IS EVEN CONSIDERED, and the ordering is the
        // whole lesson of this function.
        //
        // The obvious arrangement — show the live preview, and ask for a cached picture only on the frames
        // there is no live one — asks for the fallback at exactly the moment it cannot be produced. What
        // takes the live preview away is a shortage of renderer slots, and a capture needs a renderer slot
        // too; the service refuses to take the last one (Engine/Core/ViewBudget.hpp), and
        // rightly, so the row would sit on "queued" for as long as the shortage lasted. Measured, not
        // reasoned: with the request placed after the branch, a selected mesh produced no capture at all
        // and the log showed the queue draining a material nobody had asked this row for.
        //
        // So the cache is warmed WHILE there is room to warm it. It costs one capture per mesh asset, ever
        // — 360-385 ms over seven runs on this machine, then a PNG that survives restarts — and the service
        // drops the request outright when the picture on disk is still fresh, which after the first time
        // it is.
        //
        // Asked through the service, never by reading `ThumbnailCache::DiskPath` and hoping. That hope was
        // the defect: the file existed only if the asset browser had happened to walk past this asset, and
        // if it had not, the row showed a grey cube glyph for the life of the project with nothing anywhere
        // saying why — the very wart ThumbnailService's header declares removed for the material slot, and
        // did not keep for this one. Desert/Tests/Editor/ThumbnailRequesters is the census that keeps every
        // showing slot a requesting slot.
        //
        // THE MESH first: this row is the mesh slot, and showing a material sphere where the model belongs
        // answers a question nobody asked. The material is the last resort, for an entity whose mesh slot
        // is empty (a primitive).
        static ThumbnailCache s_Thumbnails;

        std::shared_ptr<Graphic::Image2D> thumb;
        std::string                       png;
        std::string                       source;
        bool                              materialPicture = false;
        if ( manager )
        {
            if ( mesh )
            {
                if ( auto asset = manager->FindByHandle<Assets::MeshAsset>( mesh ) )
                {
                    source = asset->GetMetadata().Filepath.generic_string();
                    // NO material override, and that is a decision rather than an omission. The cache is
                    // keyed on the ASSET (Editor/Widgets/ThumbnailKey.hpp), so what it holds has to be a
                    // picture of the asset: handing over THIS entity's slot materials would put two
                    // entities that share one mesh in a fight over one file, and the second one selected
                    // would be shown the first one's paint with nothing able to tell them apart. The
                    // per-asset picture is what Details shows, as UE's does (THM-FIXF: no live view here).
                    png = ThumbnailService::Get().RequestMesh( mesh, source );
                }
            }
            if ( png.empty() && fallbackMaterial )
            {
                if ( auto mat = manager->FindByHandle<Assets::SurfaceMaterialAsset>( fallbackMaterial ) )
                {
                    source = mat->GetMetadata().Filepath.generic_string();
                    // WHICH PICTURE, from the one place that decides — this file used to hold its own
                    // copy of the cutout rule and to ask nothing at all about the domain. A refusal leaves
                    // `png` empty, which this row already reads as "no rendered thumbnail".
                    png             = ThumbnailService::Get().RequestLoadedMaterial( *manager, mat, source );
                    materialPicture = true;
                }
            }

            if ( !png.empty() )
            {
                // The service's one verdict for this picture — the key and hash its enqueue gate used above,
                // not a second judgement of our own. When it says Capture the request above has already queued
                // the replacement, so the decoded copy is dropped here — otherwise this cache would keep
                // handing back the OLD render after the new one lands.
                const ThumbnailFreshness::Verdict verdict = materialPicture
                                                                 ? ThumbnailService::JudgeMaterialPicture( source )
                                                                 : ThumbnailService::JudgeMeshPicture( source );
                if ( verdict == ThumbnailFreshness::Verdict::Show )
                    thumb = s_Thumbnails.Get( png );
                else
                    s_Thumbnails.Invalidate( png );
            }
        }

        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 br( at.x + size, at.y + size );
        ImDrawList*  dl = ImGui::GetWindowDrawList();

        ImGui::Dummy( ImVec2( size, size ) );
        dl->AddRectFilled( at, br, IM_COL32( 15, 15, 15, 255 ), 2.0f );

        if ( thumb && uiHelper )
        {
            if ( const void* tex = uiHelper->GetTextureID( thumb ) )
                dl->AddImageRounded( reinterpret_cast<ImTextureID>( const_cast<void*>( tex ) ),
                                     ImVec2( at.x + 1.0f, at.y + 1.0f ), ImVec2( br.x - 1.0f, br.y - 1.0f ),
                                     ImVec2( 0, 0 ), ImVec2( 1, 1 ), IM_COL32_WHITE, 2.0f );
        }
        else
        {
            const char*  icon = ICON_MDI_CUBE_OUTLINE;
            const ImVec2 ts   = ImGui::CalcTextSize( icon );
            dl->AddText( ImVec2( at.x + ( size - ts.x ) * 0.5f, at.y + ( size - ts.y ) * 0.5f ),
                         ImGui::GetColorU32( ImGuiCol_TextDisabled ), icon );
        }
        dl->AddRect( at, br, ImGui::GetColorU32( ImGuiCol_Border ), 2.0f );
        // Not "rendered by the asset browser" any more, and the old wording was the defect written down:
        // this row now asks for its own picture, so the browser is not what it is waiting for.
        // Three states, not two: "here it is", "it is coming" and "there is nothing to draw one of" are
        // different answers, and collapsing the last two would let an empty slot look like a slow one.
        Utils::ImGuiUtilities::Tooltip( thumb         ? "Cached preview of this asset"
                                        : png.empty() ? "Nothing in this slot to preview"
                                                      : "Preview queued — it will appear in a moment" );

        ImGui::SameLine();
    }
} // namespace Desert::Editor
