#pragma once

// THE DEFORMATION FIELD AROUND A VIEW (DEFORM-FIELD; Notion Вики DEFORM-ARCH «Выбранная архитектура»). The render
// half of surface deformation: a two-level toroidal clipmap of what the world's contacts (::Desert::World::DeformationWorld)
// did to the ground near the camera - RoTR/HZD's player-centred deformation map, filled the way UE bins decals
// into a virtual texture's pages.
//
//   Level 0: 2048^2 texels of 2 cm (41 m across). Level 1: 1024^2 of 8 cm (82 m across).
//   Height (RGBA16F): sink cm (<= 0, min), rim cm (>= 0, saturating add), compaction (max), flatten (max).
//   Detail (RGBA8):   flatten direction X, Z (0.5-biased), wetness (max), touched (1 where any contact wrote).
//
// THE WINDOW follows the UNJITTERED view position (ViewFrame::CameraPosition), snapped to whole texels of each
// level, so a still camera never shifts the field and a moving one moves it by whole cells: a texel holds world
// cell c at c mod N, and a move clears only the texels whose cell left the window (one dispatch per level).
//
// THE CONTACTS are read from the world's log by sequence (this renderer's own m_Applied), so two views of one
// world each keep their own field. The new contacts are cut into BATCHES - runs that name the same stamp asset -
// and each batch is binned on the CPU into 64^2-texel tiles per level; ONE dispatch per batch per level, one
// thread per texel looping its tile's contacts in submission order (no atomics; the Windows lead's choice (b)). A
// stamp's shape is its asset's graph compiled into DeformStampApply (ShaderService::StampSourceOf -> an
// AcquireVariant per stamp).
//
// ZERO COST WHEN NOTHING DEFORMS: a world that never submitted a contact registers no pass and creates no image
// (the Windows lead's condition) - the images are made on the first contact, or when the debug view asks to see
// them.
//
// What reads the field is not here: p2 shows it in the "Deformation Field" view mode; terrain displacement and
// grass flattening (DEFORM-GROUND / DEFORM-GRASS) read FrameField from SceneRenderer before the base pass.

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Desert::World
{
    class DeformationWorld;
}

namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert::Graphic
{
    class Image;
    class Image2D;
    class Shader;
    class ComputePipeline;
    class GraphicsPipeline;
} // namespace Desert::Graphic

namespace Desert::Graphic::Deformation
{
    inline constexpr int kDeformationLevels = 2;

    // DeformStampApply's parameters: the one spelling the passes bind and Initialize checks.
    struct DeformStampApplyShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, DeformHeightTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, DeformDetailTexture );
        static constexpr std::array kAll{ RDG::Erase( DeformHeightTexture ), RDG::Erase( DeformDetailTexture ) };
    };

    // DeformationFieldView's parameters.
    struct DeformationFieldViewShaderParameters
    {
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, DeformHeight0Texture );
        DESERT_SHADER_PARAMETER( Texture, DeformHeight1Texture );
        DESERT_SHADER_PARAMETER( Texture, SceneDepthTexture );
        DESERT_SHADER_PARAMETER( Texture, SceneShadingWordTexture );
        static constexpr std::array kAll{ RDG::Erase( PointClampSampler ), RDG::Erase( DeformHeight0Texture ),
                                          RDG::Erase( DeformHeight1Texture ), RDG::Erase( SceneDepthTexture ),
                                          RDG::Erase( SceneShadingWordTexture ) };
    };

    // The pure parameters of a level: its texel (cm) and resolution (a power of two, a multiple of the 64-texel
    // tile).
    struct DeformationLevel
    {
        float Texel      = 0.0f;
        int   Resolution = 0;
    };
    [[nodiscard]] DeformationLevel DeformationLevelOf( int level );

    // The window of a level around @p position: its minimum cell, texel-snapped (the cell holding the position
    // sits at N/2).
    [[nodiscard]] glm::ivec2 DeformationWindowMin( const glm::vec3& position, int level );

    // What this graph's readers bind: each level's images (imported into the graph) and window.
    struct DeformationFieldFrame
    {
        std::array<RDG::TextureRef, kDeformationLevels> Height{};
        std::array<RDG::TextureRef, kDeformationLevels> Detail{};
        std::array<glm::ivec2, kDeformationLevels>      WindowMin{};
    };

    class DeformationFieldRenderer
    {
    public:
        using ImportImage = std::function<RDG::TextureRef( const std::shared_ptr<Image>&, std::string_view )>;

        DeformationFieldRenderer();
        ~DeformationFieldRenderer();
        DeformationFieldRenderer( const DeformationFieldRenderer& )            = delete;
        DeformationFieldRenderer& operator=( const DeformationFieldRenderer& ) = delete;

        // Moves the windows to @p viewPosition and applies @p world's contacts this renderer has not applied yet.
        // Nothing (no pass, no image, an empty optional) while the world has never deformed and @p wanted is
        // false;
        // @p wanted (a reader this frame, e.g. the debug view) brings the field up empty.
        [[nodiscard]] Common::ResultStr<std::optional<DeformationFieldFrame>>
        AddUpdatePasses( RDG::Builder& graph, const ImportImage& import, const ::Desert::World::DeformationWorld& world,
                         const glm::vec3& viewPosition, bool wanted );

        // "Debug: Deformation Field" over @p target: the field at each pixel's surface (@p depth, the scene
        // depth), drawn only where @p shadingWord (the G-buffer's) marks the landscape.
        [[nodiscard]] Common::BoolResultStr AddDebugViewPass( RDG::Builder&                graph,
                                                              const DeformationFieldFrame& field,
                                                              RDG::TextureRef depth, RDG::TextureRef shadingWord,
                                                              RDG::TextureRef target, const RDG::LoadOp& load,
                                                              const glm::mat4& inverseViewProjection );

    private:
        // One stamp asset's program: DeformStampApply compiled against the asset's Stamp body.
        struct StampProgram
        {
            uint64_t                         SourceHash = 0;
            std::shared_ptr<Shader>          Shader;
            std::shared_ptr<ComputePipeline> Pipeline;
            ShaderBindingLayoutCache         Layout;
        };

        Common::BoolResultStr            Initialize();
        Common::ResultStr<StampProgram*> ProgramOf( const std::string& stamp );
        void                             BindBuffers( ComputePipeline& pipeline ) const;

        std::shared_ptr<Shader>             m_ClearShader;
        std::shared_ptr<ComputePipeline>    m_ClearPipeline;
        ShaderBindingLayoutCache            m_ClearLayout;
        std::map<std::string, StampProgram> m_Stamps;

        std::array<std::shared_ptr<Image2D>, kDeformationLevels> m_Height{};
        std::array<std::shared_ptr<Image2D>, kDeformationLevels> m_Detail{};
        // The window each level's images hold; empty until the level was cleared once.
        std::array<std::optional<glm::ivec2>, kDeformationLevels> m_Window{};

        std::shared_ptr<ShaderResources::StorageBuffer> m_StampBuffer;
        std::shared_ptr<ShaderResources::StorageBuffer> m_TileBuffer;
        std::shared_ptr<ShaderResources::StorageBuffer> m_TileStampBuffer;

        uint64_t m_Applied = 0; // the next contact sequence this field has not applied

        std::shared_ptr<Shader>                                               m_ViewShader;
        ShaderBindingLayoutCache                                              m_ViewLayout;
        std::map<std::pair<int, uint32_t>, std::shared_ptr<GraphicsPipeline>> m_ViewPipelines;
    };
} // namespace Desert::Graphic::Deformation
