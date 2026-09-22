#pragma once

#include <Engine/Core/Formats/ShaderProgramMeta.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/ResultStr.hpp>

#include <optional>
#include <string>

namespace Desert::Assets
{
    class AssetManager;
    class SurfaceMaterialAsset;
}

namespace Desert::Editor::ThumbnailSubject
{
    /**
     * @file
     * @brief FROM A PATH IN THE CONTENT BROWSER TO SOMETHING THE RENDERER CAN PHOTOGRAPH.
     *
     * WHY THIS IS ITS OWN HEADER AND NOT TWO MORE SCREENS INSIDE THE PANEL. Turning a `.demat` on disk
     * into a handle the capture can use is not one call: the asset may not be in the manager, may be a
     * shell the preloader created and never parsed, and must be registered with the runtime material
     * service before anything can shade with it. Turning a browsed `.fbx` into a photographable mesh is
     * harder still — the file that is actually photographed is the COOKED `.stmesh`, the cook may not
     * exist, `Register` parses before it builds (Г15) and its result has to be READ, and a mesh whose
     * built geometry is empty must be refused rather than captured as a picture of empty sky.
     *
     * All of that lived once, inside `FileExplorerPanel`, when the browser tile was the only thing that
     * ever asked. It is not any more: the background sweep (Editor/Widgets/ThumbnailSweep.hpp) asks the
     * same question about assets no panel has walked past. Two copies of this resolution would be two
     * answers to "which file is photographed" and "when is a mesh not photographable" — and the FIRST of
     * those two questions has already been got wrong in this subsystem once, when the browser filed a
     * mesh's picture under its source while the Details row filed it under the cooked form and the same
     * mesh was captured twice under two names.
     *
     * WHAT IS DELIBERATELY *NOT* HERE: the `ThumbnailService::Request*` call itself. It stays at each
     * drawing site, because `Desert/Tests/Editor/ThumbnailRequesters` asserts that a function which draws
     * a cached thumbnail also queues one — and a census that accepted "it calls a helper that queues" is
     * a census that can be satisfied one level of indirection away from the thing it is watching.
     *
     * MAIN THREAD ONLY. Every function here touches the AssetManager and the runtime services.
     */

    /**
     * @brief WHAT THE CAPTURE DRAWS, chosen from the material's own shader DOMAIN.
     *
     * It is not a preference and it is not the extension's business: `.demat` is one file type and the
     * mesh path executes exactly one domain (`Core::Formats::kMeshPathDomain`). Handing it any other is
     * refused by name at `MeshRenderer::DrawGenericMeshes`, one frame after the queue has already
     * committed — so the question has to be asked HERE, where the answer is still actionable.
     */
    enum class Preview
    {
        /// The material on a sphere. Surface domain, the ordinary case.
        Sphere,

        /// The material on a camera-facing card. Surface domain and a CUTOUT: a foliage atlas wraps and
        /// garbles on a ball, which is a picture of the sphere rather than of the leaf.
        Card,

        /// The SKY this material authors, seen from the ground — Volume domain. A cloud material describes
        /// a medium, not a surface: its weather cells are kilometres across and its profile is base and
        /// top in kilometres, none of which means anything on a one-metre ball.
        SkyDome
    };

    /**
     * @brief The picture a DOMAIN gets, before any material has been looked at. Nullopt = no producer
     *        draws it, and the caller must refuse rather than pick something.
     *
     * PURE, AND THAT IS THE POINT OF IT BEING SEPARATE. Deciding this inside PreviewRouteFor would put
     * the routing behind a ShaderService, an AssetManager and a Vulkan device — reachable only by
     * launching the editor. Here it is three lines a suite can call, and
     * `Desert/Tests/Editor/ThumbnailFormats` asserts the RELATION that matters: this routing and the draw
     * paths' own predicates must agree about which domains can be photographed at all.
     *
     * @p cutout picks the card over the ball inside the mesh path.
     *
     * FULLY QUALIFIED, and it is not decoration: a `Desert::Editor::Core` namespace also exists
     * (ViewportMode, FoliagePaint), so an unqualified `Core::Formats` resolves THERE and fails to compile
     * in every translation unit that has seen it — which is most of the editor's panels, and NOT this
     * header's own .cpp, so the mistake builds until a panel is recompiled. The same trap
     * AssetThumbnailRenderer.hpp names over `::Desert::Core::Scene`.
     */
    [[nodiscard]] constexpr std::optional<Preview> PreviewForDomain( ::Desert::Core::Formats::ShaderDomain domain,
                                                                     bool                                  cutout )
    {
        // THE DRAW PATHS' OWN PREDICATES, never `IsUserAssignable()` — that is their union, and asking a
        // union is exactly the mistake ShaderProgramMeta.hpp warns about above it.
        if ( ::Desert::Core::Formats::DrawnByMeshPath( domain ) )
            return cutout ? Preview::Card : Preview::Sphere;
        if ( ::Desert::Core::Formats::DrawnByVolumePath( domain ) )
            return Preview::SkyDome;
        return std::nullopt;
    }

    /// A material ready to be captured.
    struct Material
    {
        Common::AssetHandle Handle{ static_cast<uint64_t>( 0 ) };

        Preview How = Preview::Sphere;
    };

    /**
     * @brief The ONE place that decides how a material is photographed, from a LOADED material asset.
     *
     * Separate from ResolveMaterial because two panels already hold the asset and only wanted this
     * answer — and each had written its own half of it (`AlphaCutoff > 0` copied twice, the domain asked
     * nowhere). A rule with three copies is a rule that will have three behaviours.
     *
     * REFUSES, WITH THE DOMAIN NAMED, for a domain no producer draws: Skybox, Terrain, PostProcess and
     * Unspecified all reach the mesh path today, get refused there, and leave a photograph of an empty
     * sphere on disk that the freshness rule then calls a current picture for ever.
     *
     * @p asset MUST be loaded. An unparsed shell states no ShaderName, and MaterialData::EffectiveShaderName
     * then answers "StaticMeshPBR" — a real name, a Surface domain and a completely wrong answer. That is
     * exactly how the Volume-domain refusal reached the log: the sweep asked a shell.
     */
    [[nodiscard]] Common::ResultStr<Preview> PreviewRouteFor( const Assets::SurfaceMaterialAsset& asset );

    /// A mesh ready to be captured.
    struct Mesh
    {
        Common::AssetHandle Handle{ static_cast<uint64_t>( 0 ) };

        /// THE FILE THAT IS PHOTOGRAPHED, and therefore the key the picture is filed under: the cooked
        /// `.stmesh`, never the browsed source. Both halves of that follow from the same fact — a
        /// `StaticMeshAsset` loads cooked JSON and never opens the FBX, so the cook is the recipe for
        /// this picture and the only file whose modification time means anything about it.
        std::string CookedPath;

        /// The sidecar material to apply to every submesh, or a zero handle when the mesh has none. It
        /// is resolved from the SOURCE, because a sidecar `.demat` is what an artist leaves beside the
        /// `.fbx` — a different question from which file gets photographed.
        Common::AssetHandle Material{ static_cast<uint64_t>( 0 ) };
    };

    /**
     * @brief Load, create-if-missing and register the material at @p assetPath.
     *
     * REFUSES WITH THE REASON rather than returning an invalid handle. "This file is not a material the
     * manager will accept" and "this material is ready" have to be distinguishable by the caller, or the
     * browser draws a swatch for ever and the sweep re-queues the same doomed asset every scan.
     */
    [[nodiscard]] Common::ResultStr<Material> ResolveMaterial( Assets::AssetManager& manager,
                                                               const std::string&    assetPath );

    /**
     * @brief Map a browsed mesh source to its cooked form, build it, and refuse if there is nothing to
     *        photograph.
     *
     * The refusals are three and they are different facts, so each names itself: the source has never
     * been cooked (the browser shows an icon and that is correct); the cooked asset could not be
     * registered (Г15's `Register` parses AND builds, and its result is read here rather than dropped);
     * the built mesh has no submeshes, which is what a skinned mesh's empty static buffer looks like and
     * would otherwise be captured as a photograph of nothing at all.
     */
    [[nodiscard]] Common::ResultStr<Mesh> ResolveMesh( Assets::AssetManager& manager,
                                                       const std::string&    sourcePath );
} // namespace Desert::Editor::ThumbnailSubject
