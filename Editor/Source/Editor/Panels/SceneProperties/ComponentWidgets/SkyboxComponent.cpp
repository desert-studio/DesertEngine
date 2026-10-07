#include <Editor/Core/DetailsNavigation.hpp>
#include "SkyboxComponent.hpp"
#include <Editor/Widgets/AssetFieldOpen.hpp>
#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailService.hpp>

#include <ImGui/imgui.h>

#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>
#include <Editor/Panels/PropertyEditor/PropertyEditorBuilder.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>
#include <Engine/Graphic/Materials/Skybox/MaterialSkybox.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
#include <Engine/Core/Scene.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <glm/gtc/type_ptr.hpp>
#include <Editor/Core/AssetPickerRows.hpp>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    // The Skybox component is the HDR-cubemap background and the environment that lights the scene from
    // it — the procedural atmosphere (palette, sun, stars, the IBL bake) lives on SkyAtmosphereComponent
    // and is drawn by its own widget.
    //
    // WHAT THE OWNER ASKED FOR AND WHY IT IS SHAPED LIKE A MESH'S MATERIAL SECTION. The report was that
    // "an HDR skybox has no material with parameters and no preview on a sphere, unlike a mesh". The
    // second half is literal and is answered literally: the Details preview viewport — the same one the
    // Static Mesh row borrows — now accepts a CUBEMAP, drawn as a ball by the very pass the
    // Material Editor's cubemap pane uses. The first half is answered by giving the sky the three knobs
    // it was missing, laid out as the property rows a material's are.
    //
    // WHAT IT IS NOT: a `.demat`. An `.hdr` has no shader, no domain and no parameter schema, so wrapping
    // it in a material asset would be inventing a second identity for a file that already has one — and
    // the one thing a material would have bought (a place to keep values) is exactly what the three
    // PROPERTY fields on the component are. UE reaches the same picture from the other side, with a
    // TextureCube asset and a SkyLight component; our SkyboxAsset already IS "a file that yields
    // radiance, irradiance and prefiltered cubes", so the asset half of that is work we would be doing
    // twice.
    //
    // The ONE hand-drawn control is the HDR SkyboxAsset picker (a dropdown of loaded skyboxes +
    // drag-drop), because the generic reflected asset slot is texture-oriented and doesn't resolve
    // SkyboxAssets — so SkyboxHandle is marked Hidden and drawn here.
    DESERT_REGISTER_CUSTOM_COMPONENT(
         ECS::SkyboxComponent, "Skybox", false,
         (
              []( ECS::Entity& entity, ::Desert::Core::Scene* /*scene*/, const ComponentEditContext& ctx )
              {
                  auto* assetManager = ctx.AssetMgr();
                  if ( !assetManager )
                      return;
                  auto& skybox = entity.GetComponent<ECS::SkyboxComponent>();

                  // The row is required (created from the content registry and requested) before the handle is
                  // bound; a handle the registry has no skybox under is refused with its number in the log.
                  auto bindSkybox = [&]( const Assets::AssetHandle& handle )
                  {
                      if ( handle == skybox.SkyboxHandle )
                          return;
                      if ( Runtime::RequireSkybox( handle ) )
                          skybox.SkyboxHandle = handle;
                  };

                  // Named from the registry row, not a loaded shell: the name is known before anything is read.
                  const auto current = Assets::ContentRegistry::RowOf(
                       Common::Content::ContentKind::Skybox, static_cast<uint64_t>( skybox.SkyboxHandle ) );
                  std::string currentName =
                       current ? ::Desert::Editor::PickerDisplayName( *current ) : std::string( "None" );

                  // ── THE SKY'S TILE, beside the picker ─────────────────────────────────────────────
                  //
                  // NO LIVE BALL (THM-FIXF). Details holds no render view — UE's Details slots are thumbnails
                  // from the shared pool (THM-FIXG: ThumbnailService::RequestSkybox, the same request and key the
                  // Content Browser tile uses), and the live ball lives in the skybox's own window
                  // (double-click opens it). The type icon shows until the picture is on disk and current.
                  constexpr float kTile = 96.0f;
                  {
                      const ImVec2 at = ImGui::GetCursorScreenPos();
                      ImGui::Dummy( ImVec2( kTile, kTile ) );
                      if ( current && ImGui::IsItemHovered() &&
                           ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
                          Core::AssetFieldRequests::Request( skybox.SkyboxHandle, Core::AssetFieldAction::Open );
                      const ImVec2 br( at.x + kTile, at.y + kTile );
                      ImDrawList*  dl = ImGui::GetWindowDrawList();
                      dl->AddRectFilled( at, br, IM_COL32( 15, 15, 15, 255 ), 2.0f );
                      static ThumbnailCache             s_Thumbnails;
                      std::shared_ptr<Graphic::Image2D> thumb;
                      if ( current )
                      {
                          const std::string source = current->Path.generic_string();
                          const std::string png =
                               ThumbnailService::Get().RequestSkybox( skybox.SkyboxHandle, source );
                          if ( ThumbnailService::JudgeSkyboxPicture( source ) ==
                               ThumbnailFreshness::Verdict::Show )
                              thumb = s_Thumbnails.Get( png );
                          else
                              s_Thumbnails.Invalidate( png );
                      }
                      const void* tex = ( thumb && ctx.UIHelper ) ? ctx.UIHelper->GetTextureID( thumb ) : nullptr;
                      if ( tex )
                          dl->AddImageRounded( reinterpret_cast<ImTextureID>( const_cast<void*>( tex ) ),
                                               ImVec2( at.x + 1.0f, at.y + 1.0f ),
                                               ImVec2( br.x - 1.0f, br.y - 1.0f ), ImVec2( 0, 0 ), ImVec2( 1, 1 ),
                                               IM_COL32_WHITE, 2.0f );
                      else
                      {
                          const char*  icon = ICON_MDI_IMAGE_FILTER_HDR;
                          const ImVec2 ts   = ImGui::CalcTextSize( icon );
                          dl->AddText( ImVec2( at.x + ( kTile - ts.x ) * 0.5f, at.y + ( kTile - ts.y ) * 0.5f ),
                                       ImGui::GetColorU32( ImGuiCol_TextDisabled ), icon );
                      }
                      dl->AddRect( at, br, ImGui::GetColorU32( ImGuiCol_Border ), 2.0f );
                      Utils::ImGuiUtilities::Tooltip( !current ? "No HDR skybox assigned"
                                                               : "Double-click to open the skybox in its viewer" );
                  }

                  ImGui::SameLine();
                  ImGui::BeginGroup();

                  // --- HDR skybox picker (dropdown of loaded SkyboxAssets) ---
                  ImGui::TextUnformatted( "Skybox (HDR)" );
                  const bool clicked =
                       ImGui::Button( currentName.c_str(), ImVec2( ImGui::GetContentRegionAvail().x, 0 ) );
                  if ( TakeDetailsPickerRequest( "Skybox" ) || clicked )
                      ImGui::OpenPopup( "skybox_selector" );

                  // --- drag-drop a skybox/texture file from the File Explorer ---
                  if ( ImGui::BeginDragDropTarget() )
                  {
                      const char* types[] = { ::Desert::Editor::DragPayloads::SkyboxAsset,
                                              ::Desert::Editor::DragPayloads::TextureAsset, "AssetFile" };
                      for ( const char* t : types )
                      {
                          if ( const ImGuiPayload* p = ImGui::AcceptDragDropPayload( t ) )
                          {
                              const std::string path( static_cast<const char*>( p->Data ) );
                              if ( const auto handle = Runtime::SkyboxHandleAtPath( path ); handle != 0 )
                                  bindSkybox( handle );
                              break;
                          }
                      }
                      ImGui::EndDragDropTarget();
                  }

                  if ( ImGui::BeginPopup( "skybox_selector" ) )
                  {
                      static ImGuiTextFilter filter;
                      filter.Draw( "##Search", 200 );
                      ImGui::Separator();
                      auto skyboxes = Assets::ContentRegistry::Rows( Common::Content::ContentKind::Skybox );
                      for ( const auto& row : skyboxes )
                      {
                          const std::string name = ::Desert::Editor::PickerDisplayName( row );
                          if ( filter.PassFilter( name.c_str() ) &&
                               ImGui::Selectable( name.c_str(), row.Handle == skybox.SkyboxHandle ) )
                              bindSkybox( row.Handle );
                      }
                      if ( skyboxes.empty() )
                          ImGui::TextDisabled( "No skybox assets available" );
                      ImGui::EndPopup();
                  }

                  ImGui::EndGroup();
                  DrawAssetFieldOpen( static_cast<uint64_t>( skybox.SkyboxHandle ) );

                  // ── THE PARAMETERS ────────────────────────────────────────────────────────────────
                  //
                  // All three are applied where the environment cubes are SAMPLED — the backdrop, the ambient
                  // and the reflections alike — so a drag is a uniform write per frame, never a rebake. See
                  // Engine/Graphic/Environment/SkyLook.hpp.
                  Utils::ImGuiUtilities::ResetPropertyRows();

                  Utils::ImGuiUtilities::BeginPropertyRow( "Intensity" );
                  ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x );
                  ImGui::SliderFloat( "##skyintensity", &skybox.Intensity, 0.0f, 10.0f );
                  Utils::ImGuiUtilities::EndPropertyRow();

                  Utils::ImGuiUtilities::BeginPropertyRow( "Rotation" );
                  ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x );
                  ImGui::SliderFloat( "##skyrotation", &skybox.Rotation, 0.0f, 360.0f, "%.1f deg" );
                  Utils::ImGuiUtilities::EndPropertyRow();

                  Utils::ImGuiUtilities::BeginPropertyRow( "Tint" );
                  ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x );
                  ImGui::ColorEdit3( "##skytint", glm::value_ptr( skybox.Tint ),
                                     ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float );
                  Utils::ImGuiUtilities::EndPropertyRow();
              } ) )

} // namespace Desert::Editor
