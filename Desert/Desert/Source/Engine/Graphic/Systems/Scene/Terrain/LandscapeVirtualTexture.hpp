#pragma once

#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <array>
#include <climits>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace Desert::Graphic
{
    struct ViewFrame;
    class FrameTextures;
    class ComputePipeline;
    namespace RDG
    {
        class Builder;
    }
} // namespace Desert::Graphic

namespace Desert::Graphic::System
{
    // Textures.LandscapeVirtualTexture* (Scalability.hpp; UE r.VT.* under sg.TextureQuality), handed by
    // SceneRenderer::OnUpdate through TerrainRenderer::SetVirtualTextureQuality.
    struct LandscapeVirtualTextureQuality
    {
        uint32_t SizeLog2        = 16; // mip0 side of the virtual square, log2 texels (12..17)
        uint32_t PoolPages       = 32; // the atlases' side in pages (8..64)
        uint32_t UpdatesPerFrame = 64; // pages rendered per frame (8..256)

        bool operator==( const LandscapeVirtualTextureQuality& ) const = default;
    };

    // One queued tile as the page render reads it: its rectangle on the ground (cm) and its hit-group row
    // (RayTracing::LandscapeRayHitRow, the per-frame view fields zeroed), and the key whose change re-renders the
    // resident pages over the rectangle (a paint stroke: Weights.Revision; a material or layer edit: the row).
    struct LandscapeVirtualTextureTile
    {
        glm::vec2                       Origin{ 0.0f }; // x, z (cm) of the tile's corner
        float                           Side = 0.0f;    // cm
        Core::Formats::MaterialParamRow Row;
        uint64_t                        Key = 0u;
    };

    // What the G-buffer binds (TerrainGBuffer.shader, Common/LandscapeVirtualTexture.glslh). Valid only in the
    // graph AddPasses declared them in. A disabled view binds a zero header (the shader then blends per pixel).
    struct LandscapeVirtualTextureBindings
    {
        RDG::BufferRef  PageTable;
        RDG::TextureRef Color;
        RDG::TextureRef Normal;
        RDG::TextureRef Occlusion;
        RDG::BufferRef  Feedback;
    };

    // THE LANDSCAPE'S RUNTIME VIRTUAL TEXTURE (LANDSCAPE-RVT; UE Runtime Virtual Texture for landscape): the
    // layer blend (Common/LandscapeSurface.glslh EvaluateLandscapeLayers: albedo, tangent-space normal,
    // roughness, occlusion - a function of the ground's xz alone, so sculpting does not invalidate a page)
    // rendered once into 128-texel pages (+4 border) of a virtual square over the queued tiles, kept in three
    // persistent atlases and sampled by the G-buffer through one page table. The G-buffer marks the pages it wants
    // in a feedback bit field, read back frames later; the CPU keeps the mirror, the LRU and the invalidation.
    // Owned by TerrainRenderer, one per view.
    class LandscapeVirtualTexture
    {
    public:
        static constexpr uint32_t kPageTexels  = 128u;
        static constexpr uint32_t kPageBorder  = 4u;
        static constexpr uint32_t kPageStride  = kPageTexels + 2u * kPageBorder; // 136
        static constexpr uint32_t kHeaderWords = 32u;
        static constexpr uint32_t kMaxMips     = 16u;

        LandscapeVirtualTexture();
        ~LandscapeVirtualTexture();
        LandscapeVirtualTexture( const LandscapeVirtualTexture& )            = delete;
        LandscapeVirtualTexture& operator=( const LandscapeVirtualTexture& ) = delete;

        // A change drops every page and the atlases (they are remade at the new size next frame).
        void SetQuality( const LandscapeVirtualTextureQuality& quality ) noexcept;

        // Before the G-buffer pass is declared: reads back the feedback of the frame whose slot comes round,
        // places the virtual square over @p tiles, invalidates, requests, evicts, uploads the changed page table
        // words and renders up to UpdatesPerFrame pages ("Landscape: Virtual Texture Pages"). @p enabled false
        // (forward path, scene capture, no tile) declares the disabled bindings. On an error the bindings are the
        // disabled ones and the G-buffer blends per pixel.
        [[nodiscard]] Common::BoolResultStr AddPasses( RDG::Builder& graph, FrameTextures& textures,
                                                       const ViewFrame&                             frame,
                                                       std::span<const LandscapeVirtualTextureTile> tiles,
                                                       bool                                         enabled );

        [[nodiscard]] const LandscapeVirtualTextureBindings& GetBindings() const noexcept
        {
            return m_Bindings;
        }

    private:
        struct Slot
        {
            int64_t  Virtual  = -1; // the virtual page (mip's first word + y x side + x), -1 = free
            uint64_t LastUsed = 0u;
            bool     Dirty    = false;
        };
        struct FeedbackSlot
        {
            RDG::ExternalBuffer Buffer;
            uint64_t            Generation = 0u;
            uint32_t            Words      = 0u;
        };

    void                                DeclareDisabled( RDG::Builder& graph, FrameTextures& textures );
    void                                ResetLayout( glm::vec2 origin, float extent ) noexcept;
    [[nodiscard]] uint32_t              MipOf( uint32_t entry ) const noexcept;
    [[nodiscard]] glm::vec4             PageRect( uint32_t entry ) const noexcept; // x0, z0, x1, z1 (cm)
    void                                ReadFeedback( uint64_t frameIndex );
    void                                InvalidateRect( glm::vec4 rect ) noexcept;
    void                                SetEntry( uint32_t entry, uint32_t value ) noexcept;
    [[nodiscard]] Common::BoolResultStr EnsurePipeline();

    LandscapeVirtualTextureQuality  m_Quality;
    LandscapeVirtualTextureBindings m_Bindings;

    // The layout of the virtual square. Generation counts layouts: a feedback slot of an older one is dropped.
    bool                           m_LayoutValid = false;
    uint64_t                       m_Generation  = 0u;
    glm::vec2                      m_Origin{ 0.0f };
    float                          m_Extent      = 0.0f;
    uint32_t                       m_Mips        = 0u;
    uint32_t                       m_Mip0Pages   = 0u;
    uint32_t                       m_PoolPages   = 0u;
    int32_t                        m_DistanceKey = INT32_MIN;
    uint32_t                       m_TopEntry    = 0u;
    std::array<uint32_t, kMaxMips> m_MipFirst{};
    std::vector<uint32_t>          m_Entries; // the CPU mirror of the page table's entries (slot + 1, 0 = none)
    std::vector<uint32_t>          m_ChangedEntries;
    bool                           m_UploadAll = true;
    std::vector<Slot>              m_Slots;
    std::vector<uint32_t>          m_Requests;         // virtual pages asked for by the feedback, not resident
    std::unordered_map<uint64_t, uint64_t> m_TileKeys; // last frame's tiles (TileId) and their keys

    // Persistent resources (UE: the RVT's physical space and page table, pooled across frames).
    bool                      m_ResourcesValid = false;
    RDG::ExternalBuffer       m_PageTable;
    RDG::ExternalTexture      m_Color;
    RDG::ExternalTexture      m_Normal;
    RDG::ExternalTexture      m_Occlusion;
    std::vector<FeedbackSlot> m_Feedback;

    std::shared_ptr<ComputePipeline> m_PagePipeline;
    ShaderBindingLayoutCache         m_PageLayout;
};
} // namespace Desert::Graphic::System
