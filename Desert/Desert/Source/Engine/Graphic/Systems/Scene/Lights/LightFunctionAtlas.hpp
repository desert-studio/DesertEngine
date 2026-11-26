#pragma once

#include <Engine/Assets/Common.hpp>
#include <Engine/Assets/FrameRetireQueue.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/LightingLayout.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/Scene/Lights/LightFunctionProxy.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Desert::Graphic
{
    class Image2D;
    class PipelineCache;
} // namespace Desert::Graphic

namespace Desert::Graphic::System
{
    // A tile's side in texels: Mesh/LightFunctionAtlas.glslh LIGHT_FUNCTION_TILE_TEXELS (the reader's constant).
    inline constexpr uint32_t kLightFunctionTileTexels = 128u;

    // The pass's own input, the one spelling Declare binds and every light function program is validated against.
    struct LightFunctionShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, LightFunctionRowsBuffer );
        static constexpr std::array kAll{ RDG::Erase( LightFunctionRowsBuffer ) };
    };

    /**
     * @brief "Lights: Light Function Atlas" (LIGHT-FUNCTIONS; UE FLightFunctionAtlas, UE 5.3+).
     *
     * Per frame SceneRenderer::AddPointLight / AddSpotLight hand each light with a light function material to Add,
     * which gives the light its tile (the light's index in this frame's list; -1 for none). PrepareFrame grows the
     * atlas to the next power-of-two square of tiles, resolves each tile's program (a `Domain LightFunction`
     * shader; a material still loading or refused draws nothing and its tile stays cleared to 1, the identity)
     * and bakes the rows. The pass draws one quad per tile, consecutive tiles of one program as one instanced
     * draw, into the atlas, a colour attachment with a bindless slot (Core::Formats::GraphDeclaredBindless): every
     * lit pass samples it through the payload's LightFunctionAtlasTexture / LightFunctionTile
     * (Mesh/LightFunctionAtlas.glslh). Each reader declares the atlas in its own pass (ViewLightBuffers::
     * LightFunctionAtlas -> BindlessRead, UE: the atlas in each consumer's pass parameters); AddPasses adds
     * only the writer.
     *
     * Not a RenderSystem. Every container is reserved once and cleared per frame; programs are keyed by the
     * material handle. A frame with no light function adds no node.
     */
    class LightFunctionAtlas final
    {
    public:
        LightFunctionAtlas();

        // The light's tile in this frame's atlas, or -1 for a null material.
        [[nodiscard]] int32_t Add( const LightFunctionProxy&                proxy,
                                   const ShaderProtocols::SpotLightPayload& light );
        [[nodiscard]] int32_t Add( const LightFunctionProxy&                 proxy,
                                   const ShaderProtocols::PointLightPayload& light );
        void                  ClearQueue();

        // Grows the atlas, resolves the programs, bakes the rows with @p timeSeconds (the scene clock). Returns
        // the atlas's bindless slot for the payloads (0 when no tile is drawn this frame: the readers then answer
        // 1).
        [[nodiscard]] int32_t PrepareFrame( float timeSeconds, PipelineCache& pipelines );
        [[nodiscard]] bool    HasDraws() const
        {
            return !m_Draws.empty();
        }
        [[nodiscard]] const std::shared_ptr<Image2D>& GetAtlas() const
        {
            return m_Atlas;
        }

        // The writer pass, onto @p atlas (the atlas imported into @p graph).
        void AddPasses( RDG::Builder& graph, RDG::TextureRef atlas ) const;

    private:
        struct Program
        {
            std::shared_ptr<GraphicsPipeline>   Pipeline;
            std::unique_ptr<DataDrivenMaterial> Material;
            mutable ShaderBindingLayoutCache    Layout;
            std::shared_ptr<Shader>             BuiltShader; // kept when refused, so a hot reload retries it
            uint64_t                            Generation = 0u;
            bool                                Refused    = false;
            mutable uint32_t                    Block      = 0u; // this frame's binding block (Declare order)
        };
        struct Tile
        {
            Assets::AssetHandle Material;
            LightFunctionRowGpu Row; // TileUV and time filled by PrepareFrame
        };
        struct FrameTile
        {
            const Program*      Program;
            LightFunctionRowGpu Row;
        };
        struct Draw
        {
            const Program* Program;
            uint32_t       FirstRow;
            uint32_t       Count;
        };
        struct RetiredProgram
        {
            std::shared_ptr<GraphicsPipeline>   Pipeline;
            std::unique_ptr<DataDrivenMaterial> Material;
        };

        int32_t               Push( const LightFunctionProxy& proxy, const LightFunctionRowGpu& row );
        Common::BoolResultStr EnsureAtlas( uint32_t tilesPerSide );
        const Program*        ProgramFor( const Assets::AssetHandle& material, PipelineCache& pipelines );
        void FollowShaderReload( Program& program, const Assets::AssetHandle& material, PipelineCache& pipelines );
        const Program*        RetryRefused( Program& program, const Assets::AssetHandle& material,
                                            PipelineCache& pipelines );
        Common::BoolResultStr Build( Program& program, const Assets::AssetHandle& material,
                                     PipelineCache& pipelines ) const;

        [[nodiscard]] RDG::BufferRef        UploadRows( RDG::Builder& graph ) const;
        void                                Declare( RDG::PassBuilder& pass, RDG::BufferRef rows ) const;
        [[nodiscard]] Common::BoolResultStr Render( const RDG::PassContext& context ) const;

        std::vector<Tile>                        m_Tiles;    // this frame's, by tile index
        std::vector<FrameTile>                   m_Frame;    // the drawable ones, grouped by program
        std::vector<LightFunctionRowGpu>         m_Rows;     // m_Frame's rows in draw order
        std::vector<Draw>                        m_Draws;    // runs of m_Rows
        std::vector<const Program*>              m_Programs; // the distinct programs of m_Draws, block order
        std::unordered_map<uint64_t, Program>    m_ProgramsByMaterial;
        Assets::FrameRetireQueue<RetiredProgram> m_Retired;
        std::shared_ptr<Image2D>                 m_Atlas; // grows only
        uint32_t                                 m_TilesPerSide = 0u;
        Assets::FrameRetireQueue<std::shared_ptr<Image2D>> m_RetiredAtlases; // replaced by growth
        bool                                               m_AtlasRefusedLogged = false;
    };
} // namespace Desert::Graphic::System
