#pragma once

#include <Engine/Assets/CloudProceduralVolume.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Graphic
{
    class Image3D;

    /**
     * @brief The camera-centred clipmap of the procedural modelling volume: ONE image, three levels of the
     *        same 256 x 32 x 256 grid (Assets::kCloudProceduralClipLevels), level k in rows [32k, 32k + 32).
     *
     * Level k covers Assets::CloudProceduralLevelSideKm around the camera minus the wind, with the same voxel
     * count per side, so level 0 is the finest. Every level is stored TOROIDALLY: world voxel i of a level
     * lives at texel i mod side (Assets::CloudProceduralLevelTexel), so a camera that moves one voxel moves
     * the window by one column and only the entering columns are baked — on a worker, with cancellation —
     * and written as sub-regions of the existing image (Image3D::WriteRegions). Nothing else on the device
     * changes, so nothing pops.
     *
     * THE TWO FINE LEVELS ARE THE WORLD FIELD (Assets::BakeCloudProceduralBox): a texel's bytes depend on
     * the world voxel it holds and nothing else. THE LAST LEVEL IS THE PERIODIC REGION BAKE
     * (Assets::BakeCloudProceduralVolumeCached at the level's own voxel-snapped corner, rotated into torus
     * order), and that is the far path: past its window the REPEAT sampler tiles it, and only a periodic
     * bake tiles without a seam. Inside the window it is the world field except within one lump's reach of
     * its edge, 24 km out, where the finer levels have long stopped being read. A camera move re-bakes it
     * whole, as the single region always was.
     *
     * A SHAPE (new parameters, types or files) is all three levels baked whole and written TOGETHER when the
     * last lands, so the sky never shows two shapes at once; until then the old shape stays on the device
     * and stops scrolling. Per view, owned by VolumetricCloudRenderer — see the note on its member.
     */
    class CloudProceduralClipmap
    {
    public:
        static constexpr uint32_t kLevels = Assets::kCloudProceduralClipLevels;

        /// What a finished shape cost, for the renderer's one log line.
        struct ShapeLanded
        {
            double      Ms              = 0.0;
            uint32_t    LevelsFromCache = 0;
            std::string CacheWriteError;
        };

        CloudProceduralClipmap() = default;
        ~CloudProceduralClipmap();
        CloudProceduralClipmap( const CloudProceduralClipmap& )            = delete;
        CloudProceduralClipmap& operator=( const CloudProceduralClipmap& ) = delete;

        /// Starts a new shape around @p cameraKm (camera minus the wind, km): cancels every bake in flight
        /// and bakes every level whole. The parameters must already have passed
        /// Assets::ValidateCloudProceduralParams.
        void BeginShape( const Assets::CloudProceduralFieldParams& params, const glm::vec2& cameraKm );

        /// Once per frame: collects finished bakes, writes them into the image, starts the bakes that move
        /// each level toward @p cameraKm. @p block waits for a shape when nothing is on the device yet.
        /// @return the shape that landed this call, nothing when none did, or an error naming the bake or
        ///         the device write that failed (the bake is dropped; the caller latches the parameters).
        Common::ResultStr<std::optional<ShapeLanded>> Update( const glm::vec2& cameraKm, bool block );

        /// Cancels and drops every bake in flight. The volume on the device stays.
        void CancelAll();

        bool IsValid() const
        {
            return m_Valid;
        }
        /// A shape (not a scroll) is being baked — what a panel reports as "rebuilding".
        bool IsShapeInFlight() const
        {
            return m_ShapeInFlight;
        }
        /// The fraction of the shape in flight, 0..1 (the slowest level's).
        float    ShapeProgress() const;
        uint32_t CancelledBakes() const
        {
            return m_Cancelled;
        }

        /// The parameters the volume on the device was baked from.
        const Assets::CloudProceduralFieldParams& DeviceParams() const
        {
            return m_DeviceParams;
        }
        const Assets::CloudProceduralFieldParams& ShapeParams() const
        {
            return m_ShapeParams;
        }
        const std::shared_ptr<Image3D>& GetImage() const
        {
            return m_Image;
        }

        /// THE CONSERVATIVE DENSITY of the same levels (CLOUD-MARCH-COST-1): side x kLevels x side RGBA8, row k
        /// = level k, texel = each species' maximum over the column's 32 rows, dilated toroidally by
        /// CloudColumnProbeDilationTexels of that level's voxel. Written in the same Write as the levels, from
        /// the same bytes, so it never describes a shape or a window the volume does not hold. Read by the
        /// march's coarse-tier probe (Common/CloudField.glslh CloudProceduralColumnsEmpty) and nothing else.
        const std::shared_ptr<Image3D>& GetColumnImage() const
        {
            return m_ColumnImage;
        }

        /// How many times the image's texels were written (a shape landing, a level scrolling). A consumer that
        /// caches what it marched through the image (the cloud shadow map, the sky-occlusion volume) keys on it.
        uint64_t WriteGeneration() const
        {
            return m_WriteGeneration;
        }

        /// CloudLevel[k] for the march (CloudParams.glslh): xy the level's minimum corner on the device,
        /// world km; z 1 / its side, 1/km; w its voxel, km. Texel coordinate of a world point p (km) on
        /// level k: p * z (REPEAT), equivalently (p - xy) * z + fract( xy * z ).
        std::array<glm::vec4, kLevels> LevelUniforms() const;

        /// The far path's statistics of the last level ON THE DEVICE (Assets::CloudProceduralFarStatisticsOf),
        /// taken from the same bytes the image holds whenever that level lands; zero before the first shape.
        const Assets::CloudProceduralFarStatistics& FarStatistics() const
        {
            return m_FarStatistics;
        }

        /// The last level's corner on the device, km — the periodic region the rest of the sky reads.
        glm::vec2 RegionOriginKm() const;

        /// The texel boxes a level whose corner moves from @p from to @p to (whole voxels) must re-bake: the
        /// entering columns, each axis split where it crosses the torus' edge. Empty when it did not move;
        /// one whole-level box when it moved a side or more. Pure, public for the suite.
        static std::vector<Assets::CloudProceduralVoxelBox> EnteringBoxes( const glm::ivec2& from,
                                                                           const glm::ivec2& to, uint32_t side );

    private:
        struct Signal
        {
            std::atomic<bool>  Cancelled{ false };
            std::atomic<float> Fraction{ 0.0f };
        };

        struct BakedBytes
        {
            std::vector<std::vector<unsigned char>> Boxes; // one per box, Assets::CloudProceduralBoxBytes
            bool                                    FromCache = false;
            std::string                             CacheWriteError;
        };

        struct Bake
        {
            uint32_t                                     Level = 0;
            glm::ivec2                                   Origin{ 0 };
            std::vector<Assets::CloudProceduralVoxelBox> Boxes;
            std::shared_ptr<Signal>                      Flag = std::make_shared<Signal>();
            std::future<Common::ResultStr<BakedBytes>>   Future;
        };

        /// The corner level @p level wants for @p cameraKm, whole voxels of that level.
        glm::ivec2 WantedOrigin( const Assets::CloudProceduralFieldParams& params, uint32_t level,
                                 const glm::vec2& cameraKm ) const;
        Bake StartBake( const Assets::CloudProceduralFieldParams& params, uint32_t level, const glm::ivec2& origin,
                        std::vector<Assets::CloudProceduralVoxelBox> boxes, bool cached ) const;
        /// Writes the landed boxes into the volume and, for every level they touch, that level's column-max
        /// row; @p params are the parameters the bytes were baked from (the dilation is in their voxels).
        Common::BoolResultStr Write( const Assets::CloudProceduralFieldParams&                     params,
                                     const std::vector<std::pair<const Bake*, const BakedBytes*>>& landed );
        Common::BoolResultStr EnsureImage( uint32_t side );

        std::shared_ptr<Image3D> m_Image;
        std::shared_ptr<Image3D> m_ColumnImage;
        uint64_t                 m_WriteGeneration = 0;

        /// Per level, side x side RGBA8 (x fastest, toroidal texel order): each species' maximum over the
        /// column's rows, UNDILATED — the host copy a slab's columns replace, since the dilation of a column
        /// reads its neighbours and the device image cannot be read back.
        std::array<std::vector<unsigned char>, kLevels> m_ColumnMax;

        Assets::CloudProceduralFieldParams   m_DeviceParams{};
        std::array<glm::ivec2, kLevels>      m_DeviceOrigin{};
        bool                                 m_Valid = false;
        Assets::CloudProceduralFarStatistics m_FarStatistics{};

        Assets::CloudProceduralFieldParams             m_ShapeParams{};
        bool                                           m_ShapeInFlight = false;
        std::array<std::optional<Bake>, kLevels>       m_ShapeBakes;
        std::array<glm::ivec2, kLevels>                m_ShapeOrigin{};
        std::array<std::optional<BakedBytes>, kLevels> m_ShapeLanded;
        std::chrono::steady_clock::time_point          m_ShapeStarted{};

        std::array<std::optional<Bake>, kLevels> m_Scroll;
        uint32_t                                 m_Cancelled = 0;
    };
} // namespace Desert::Graphic
