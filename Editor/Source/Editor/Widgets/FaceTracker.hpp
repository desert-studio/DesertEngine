#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Desert::Editor
{
    // Optional face-landmark tracker backed by dlib — compiled in only when ThirdParty/dlib is present and
    // the build defines DESERT_WITH_DLIB (see cmake/ThirdParty/Dlib.cmake). Feed it RGBA frames; it
    // returns up to 68 landmark points (image-pixel coordinates) for the most prominent face. Without dlib,
    // or without a loaded model, it is a no-op that reports Ready()==false so callers draw a placeholder.
    class FaceTracker
    {
    public:
        FaceTracker();
        ~FaceTracker();

        FaceTracker( const FaceTracker& )            = delete;
        FaceTracker& operator=( const FaceTracker& ) = delete;

        static bool Compiled(); // true when built against dlib

        // Loads the 68-point shape predictor (shape_predictor_68_face_landmarks.dat). Returns false when dlib
        // isn't compiled in or the file can't be read/parsed.
        bool LoadModel( const std::string& datPath );
        bool Ready() const; // Compiled() && a model is loaded

        // Detects landmarks on an RGBA8 frame; returns image-pixel points (empty when unavailable / no face).
        std::vector<glm::vec2> Detect( const uint8_t* rgba, int width, int height );

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
} // namespace Desert::Editor
