#include "SkyboxComponent.hpp"
#include <Editor/Widgets/AssetFieldOpen.hpp>
#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>

#include <ImGui/imgui.h>

#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>
#include <Editor/Panels/PropertyEditor/PropertyEditorBuilder.hpp>
#include <Editor/Widgets/PreviewViewport.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>
#include <Engine/Graphic/Materials/Skybox/MaterialSkybox.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
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

                  auto bindSkybox = [&]( const Assets::AssetHandle& handle )
                  {
                      if ( handle == skybox.SkyboxHandle )
                          return;
                      auto& svc = *Runtime::ResourceRegistry::GetSkyboxService();
                      if ( !svc.Get( handle ) )
                      {
                          if ( auto a = assetManager->FindByHandle<Assets::SkyboxAsset>( handle ) )
                          {
                              Graphic::Renderer::GetInstance().WaitDeviceIdle();
                              svc.Register( a );
                          }
                      }
                      skybox.SkyboxHandle = handle;
                  };

                  const auto  current = assetManager->FindByHandle<Assets::SkyboxAsset>( skybox.SkyboxHandle );
                  std::string currentName =
                       current ? Common::Utils::FileSystem::GetFileName( current->GetMetadata().Filepath )
                               : "None";

                  // ── THE PREVIEW ON A BALL, beside the picker ───────────────────────────────────────
                  //
                  // 96 px rather than the material slot's 64: a cubemap is READ for where things are in
                  // it (is the sun behind me now?), and that is the question the rotation slider below
                  // exists to answer, so the picture has to be big enough to answer it.
                  constexpr float kPreview = 96.0f;
                  const bool      drewLive = ctx.Preview && ctx.Preview->HasContent() &&
                                        ctx.Preview->GetFill() == PreviewViewport::Fill::Cubemap &&
                                        ctx.DrawPreview( ImVec2( kPreview, kPreview ),
                                                         static_cast<uint64_t>( skybox.SkyboxHandle ) );
                  // The live ball is Static (DrawPreview): it keeps one angle and its double-click opens the
                  // skybox, with its own "Double-click to open" tooltip.
                  if ( !drewLive )
                  {
                      // THREE STATES, NOT TWO. "no asset", "the panel was lent no renderer" and "the
                      // cubes are not baked yet" are different facts, and a single grey box for all
                      // three is how a person concludes the feature is broken when it is merely busy.
                      const ImVec2 at = ImGui::GetCursorScreenPos();
                      ImGui::Dummy( ImVec2( kPreview, kPreview ) );
                      const ImVec2 br( at.x + kPreview, at.y + kPreview );
                      ImDrawList*  dl = ImGui::GetWindowDrawList();
                      dl->AddRectFilled( at, br, IM_COL32( 15, 15, 15, 255 ), 2.0f );
                      const char*  icon = ICON_MDI_IMAGE_FILTER_HDR;
                      const ImVec2 ts   = ImGui::CalcTextSize( icon );
                      dl->AddText( ImVec2( at.x + ( kPreview - ts.x ) * 0.5f, at.y + ( kPreview - ts.y ) * 0.5f ),
                                   ImGui::GetColorU32( ImGuiCol_TextDisabled ), icon );
                      dl->AddRect( at, br, ImGui::GetColorU32( ImGuiCol_Border ), 2.0f );
                      Utils::ImGuiUtilities::Tooltip( !current ? "No HDR skybox assigned"
                                                               : "Preview starting — the cubemap is baking" );
                  }

                  ImGui::SameLine();
                  ImGui::BeginGroup();

                  // --- HDR skybox picker (dropdown of loaded SkyboxAssets) ---
                  ImGui::TextUnformatted( "Skybox (HDR)" );
                  if ( ImGui::Button( currentName.c_str(), ImVec2( ImGui::GetContentRegionAvail().x, 0 ) ) )
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
                              if ( auto a = assetManager->FindByPath<Assets::SkyboxAsset>( path ) )
                                  bindSkybox( a->GetMetadata().Handle );
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
                      for ( const auto& [handle, rowKey, rowPath, rowGuid] : skyboxes )
                      {
                          const std::string name = Common::Utils::FileSystem::GetFileName( rowPath );
                          if ( filter.PassFilter( name.c_str() ) &&
                               ImGui::Selectable( name.c_str(), handle == skybox.SkyboxHandle ) )
                              bindSkybox( handle );
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
