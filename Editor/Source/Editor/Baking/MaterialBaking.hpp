#pragma once

// MATERIAL BAKING (UE 5.5 IMaterialBakingModule::BakeMaterials / FMaterialBakingModule, FMeshMaterialRenderItem,
// FFlattenMaterial): one material slot of one mesh rendered in that mesh's UV space into BaseColor, Normal
// (tangent), ORM, Emissive and OpacityMask images of a requested size, the islands' borders dilated so no sampler
// reads the clear colour across a seam (UE FMaterialBakingHelpers::PerformUVBorderSmear). The output is CPU
// images: the HLOD bake and a future Merge Actors build their flattened material from them, and the palette's
// "Bake Material to Textures" writes them as texture assets to look at.
//
// HOW THE MATERIAL IS EVALUATED. As UE's FMeshMaterialRenderItem does, the slot's triangles become a UV-SPACE
// PROXY: each vertex placed at its texture coordinate on a plane facing an orthographic camera that frames the
// unit UV square exactly, and carrying the source vertex's normal, tangent frame and UVs unchanged. The proxy is
// drawn by the engine's own deferred G-buffer cell of the slot's material - any template, any graph (procedural
// masks, instance parameters) - so the bake reads the material as the renderer evaluates it, never a CPU guess of
// "the texture as it is". The G-buffer holds the surface before any light: albedo (BaseColor), world normal +
// roughness, emissive + material AO. The world normal is brought back into the source mesh's tangent frame,
// rasterised in the same UV space; the opacity mask is the coverage the masked cell's clip left inside the
// islands.
//
// The capture is recorded into the editor's frames, as LandscapeHLODBake's is: Tick() once per editor frame until
// it answers true.

#include <Engine/Geometry/RenderMeshData.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Graphic
{
    class SceneRenderer;
    class ImageReadback;
} // namespace Desert::Graphic

namespace Desert::Core
{
    class Scene;
    class GameplayCamera;
} // namespace Desert::Core

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor::Baking
{
    // What to bake: the source mesh in its render form, which of its material slots, the material that slot draws
    // with, and the square size of every output image.
    struct MaterialBakeRequest
    {
        Geometry::RenderMeshData Mesh;     // the source mesh (ToRenderMesh of the component's mesh), object space
        int                      Slot = 0; // the material id of the submeshes to bake
        Common::AssetHandle      Material; // the slot's material asset
        uint32_t                 Size = 1024;
        std::string              Name; // the stem of written assets (the entity's name)
    };

    // UE FFlattenMaterial: one image per property, row 0 = texture coordinate v = 0 (the top,
    // DesertMeshTextureUV's convention), RGBA8 each, Size x Size.
    struct FlattenedMaterial
    {
        uint32_t             Size = 0;
        std::vector<uint8_t> BaseColor;   // sRGB
        std::vector<uint8_t> Normal;      // tangent space, xyz * 0.5 + 0.5 (flat = 128,128,255)
        std::vector<uint8_t> ORM;         // R = AO, G = roughness, B = metallic (the engine's PackedMasks)
        std::vector<uint8_t> Emissive;    // sRGB of emissive / EmissiveScale
        std::vector<uint8_t> OpacityMask; // R = G = B = mask
        // UE FFlattenMaterial::EmissiveScale: the HDR emissive's largest channel; the image holds emissive /
        // scale.
        float EmissiveScale = 0.0f;
        // Texels inside the slot's UV islands, and of those the ones the material's mask kept.
        std::size_t CoveredTexels = 0;
        std::size_t OpaqueTexels  = 0;
    };

    class MaterialBake
    {
    public:
        explicit MaterialBake( MaterialBakeRequest request );
        ~MaterialBake();

        MaterialBake( const MaterialBake& )            = delete;
        MaterialBake& operator=( const MaterialBake& ) = delete;

        // One editor frame of the bake. True once the result is ready or the bake was refused (Error()).
        [[nodiscard]] bool Tick();

        [[nodiscard]] const std::optional<FlattenedMaterial>& Result() const
        {
            return m_Result;
        }
        [[nodiscard]] const std::optional<std::string>& Error() const
        {
            return m_Error;
        }
        [[nodiscard]] const MaterialBakeRequest& Request() const
        {
            return m_Request;
        }

    private:
        enum class Phase
        {
            Build,    // the renderer is built a stage per frame
            Frame,    // the proxy is framed; warm-up frames are recorded
            Readback, // the albedo, G-buffer B and emissive copies are in flight
            Done,
        };

        [[nodiscard]] Common::BoolResultStr StartScene();
        [[nodiscard]] Common::BoolResultStr Flatten( const std::vector<uint8_t>& albedo,
                                                     const std::vector<uint8_t>& gbufferB,
                                                     const std::vector<uint8_t>& emissive );
        void                                Fail( std::string error );

        MaterialBakeRequest m_Request;

        std::unique_ptr<Graphic::SceneRenderer>         m_Renderer;
        std::shared_ptr<::Desert::Core::Scene>          m_Scene;
        std::shared_ptr<::Desert::Core::GameplayCamera> m_Camera;
        std::shared_ptr<Graphic::ImageReadback>         m_Albedo; // G-buffer attachment 0, RGBA8 (Albedo.rgb)
        std::shared_ptr<Graphic::ImageReadback> m_GBufferB; // attachment 1, RGBA16F (Normal.xyz, Roughness.a)
        std::shared_ptr<Graphic::ImageReadback> m_Emissive; // attachment 3, RGBA16F (Emissive.rgb, AO.a)
        Phase                                   m_Phase      = Phase::Build;
        int                                     m_WarmFrames = 0;
        std::optional<FlattenedMaterial>        m_Result;
        std::optional<std::string>              m_Error;
    };

    // UE FMeshMaterialRenderItem::PopulateWithMeshData: the slot's triangles of @p mesh as a UV-space proxy - each
    // vertex at (u, 0, v) x @p extentCm on the X/Z plane, its normal, tangent frame and UVs the source's, each
    // triangle wound to face +Y (the bake camera) so a mirrored island is not culled. Refused when the slot has no
    // triangle.
    [[nodiscard]] Common::ResultStr<Geometry::RenderMeshData>
    MakeUVSpaceProxy( const Geometry::RenderMeshData& mesh, int slot, float extentCm );

    // UE FMaterialBakingHelpers::PerformUVBorderSmear: every texel outside @p covered takes the mean of its
    // covered neighbours, ring by ring, until every texel is filled (or @p rings rings were grown). RGBA8, Size x
    // Size.
    void DilateIslands( std::vector<uint8_t>& rgba, std::vector<uint8_t> covered, uint32_t size, uint32_t rings );

    // THE PALETTE'S "Bake Material to Textures" on the selected entity's slot @p slot: starts the bake (one at a
    // time). Refused: no single selected entity with a static mesh, no readable mesh, a slot without a material or
    // without triangles, a bake already running.
    [[nodiscard]] Common::BoolResultStr StartBakeOfSelection( ::Desert::Core::Scene& scene, int slot,
                                                              uint32_t size );
    // One editor frame of the running palette bake; on completion its images are written under
    // Assets/Textures/Baked as `T_<entity>_<slot>_{BaseColor,Normal,ORM,Emissive,OpacityMask}` texture assets.
    void TickBakeOfSelection( Assets::AssetManager& assets );
    // Whether a palette bake is running (DesertCtl waits on it through the editor's quiescence).
    [[nodiscard]] bool IsBakeOfSelectionRunning();
} // namespace Desert::Editor::Baking
