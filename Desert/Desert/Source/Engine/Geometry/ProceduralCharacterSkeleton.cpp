#include "ProceduralCharacterSkeleton.hpp"

#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <array>

namespace Desert::Geometry
{
    namespace
    {
        constexpr std::array<HumanoidSegment, 12> kSegments = { {
             { "Hips", "Spine", 0.15f, 0.15f, "Hips" },   // lower torso
             { "Spine", "Chest", 0.15f, 0.18f, "Chest" }, // chest (widening)
             { "Chest", "Neck", 0.14f, 0.055f, "Chest" }, // upper chest -> neck
             { "Neck", "Head", 0.05f, 0.05f, "Neck" },    // neck

             { "Shoulder.L", "Elbow.L", 0.058f, 0.05f, "Shoulder.L" }, // upper arm L
             { "Elbow.L", "Hand.L", 0.048f, 0.04f, "Elbow.L" },        // forearm L
             { "Shoulder.R", "Elbow.R", 0.058f, 0.05f, "Shoulder.R" }, // upper arm R
             { "Elbow.R", "Hand.R", 0.048f, 0.04f, "Elbow.R" },        // forearm R

             { "Hip.L", "Knee.L", 0.09f, 0.07f, "Hip.L" },    // thigh L
             { "Knee.L", "Foot.L", 0.065f, 0.05f, "Knee.L" }, // shin L
             { "Hip.R", "Knee.R", 0.09f, 0.07f, "Hip.R" },    // thigh R
             { "Knee.R", "Foot.R", 0.065f, 0.05f, "Knee.R" }, // shin R
        } };

        // Spheres round off the joints (shoulders/elbows/knees/head/hips) so segments blend, not butt-join.
        constexpr std::array<HumanoidSphere, 10> kSpheres = { {
             { "Hips", 0.155f },
             { "Chest", 0.175f },
             { "Head", 0.13f },
             { "Neck", 0.055f },
             { "Shoulder.L", 0.062f },
             { "Elbow.L", 0.05f },
             { "Knee.L", 0.067f },
             { "Shoulder.R", 0.062f },
             { "Elbow.R", 0.05f },
             { "Knee.R", 0.067f },
        } };

        constexpr std::array<const char*, 2> kFeet = { "Foot.L", "Foot.R" };
    } // namespace

    std::filesystem::path HumanoidSkeletonFile()
    {
        // Engine content: found in every project through the engine mount (Content::ScanRootsOf).
        return Common::Constants::Path::ENGINE_CONTENT_PATH / "Meshes/Skinned/Humanoid.skeleton";
    }

    std::unique_ptr<Animation::Skeleton> LoadHumanoidSkeleton()
    {
        auto read = Assets::Serialization::ReadSkeletonFile( HumanoidSkeletonFile() );
        if ( !read )
        {
            LOG_ERROR(
                 "[Geometry] the built-in humanoid's skeleton does not read: {}; the procedural humanoid has "
                 "no rig.",
                 read.GetError() );
            return nullptr;
        }
        auto data = read.ExtractValue();
        return std::make_unique<Animation::Skeleton>( std::move( data.Bones ) );
    }

    std::span<const HumanoidSegment> HumanoidSegments()
    {
        return kSegments;
    }

    std::span<const HumanoidSphere> HumanoidSpheres()
    {
        return kSpheres;
    }

    std::span<const char* const> HumanoidFeet()
    {
        return kFeet;
    }
} // namespace Desert::Geometry
