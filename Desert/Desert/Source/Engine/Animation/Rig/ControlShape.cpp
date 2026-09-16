#include "ControlShape.hpp"

#include <array>
#include <cmath>
#include <numbers>
#include <utility>

namespace Desert::Animation
{
    namespace
    {
        /// How many segments a control circle is drawn with. Thirty-two is where the flat spots stop being
        /// visible at the pixel radius a viewport control is actually drawn at; sixty-four costs twice the
        /// segments for a difference nobody can see.
        constexpr int kCircleSegments = 32;

        [[nodiscard]] bool Finite( const glm::vec3& v )
        {
            return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
        }

        [[nodiscard]] bool Finite( const glm::mat4& m )
        {
            for ( int column = 0; column < 4; ++column )
            {
                for ( int row = 0; row < 4; ++row )
                {
                    if ( !std::isfinite( m[column][row] ) )
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        /**
         * @brief A unit circle spanned by two orthogonal axes.
         *
         * TAKES THE TWO AXES rather than being written out three times: three hand-authored point lists
         * are three places for the segment count to drift apart, and the first symptom is a rig where one
         * control looks smoother than another for no reason anybody can name.
         */
        [[nodiscard]] ControlShapePolyline UnitCircle( const glm::vec3& axisU, const glm::vec3& axisV )
        {
            ControlShapePolyline run;
            run.Points.reserve( kCircleSegments );
            for ( int i = 0; i < kCircleSegments; ++i )
            {
                const float angle = ( 2.0F * std::numbers::pi_v<float> * static_cast<float>( i ) ) /
                                    static_cast<float>( kCircleSegments );
                run.Points.push_back( ( axisU * std::cos( angle ) ) + ( axisV * std::sin( angle ) ) );
            }
            run.Closed = true;
            return run;
        }

        /// The twelve edges of a cube of half-extent one, as two closed rings plus the four uprights.
        /// Four runs rather than twelve segments: the two rings and the uprights are what the eye reads.
        [[nodiscard]] std::vector<ControlShapePolyline> UnitCube()
        {
            std::vector<ControlShapePolyline> runs;

            ControlShapePolyline bottom;
            bottom.Points = {
                 { -1.0F, -1.0F, -1.0F }, { 1.0F, -1.0F, -1.0F }, { 1.0F, -1.0F, 1.0F }, { -1.0F, -1.0F, 1.0F } };
            bottom.Closed = true;
            runs.push_back( bottom );

            ControlShapePolyline top;
            top.Points = {
                 { -1.0F, 1.0F, -1.0F }, { 1.0F, 1.0F, -1.0F }, { 1.0F, 1.0F, 1.0F }, { -1.0F, 1.0F, 1.0F } };
            top.Closed = true;
            runs.push_back( top );

            for ( int corner = 0; corner < 4; ++corner )
            {
                const float          x = ( corner == 0 || corner == 3 ) ? -1.0F : 1.0F;
                const float          z = ( corner < 2 ) ? -1.0F : 1.0F;
                ControlShapePolyline upright;
                upright.Points = { { x, -1.0F, z }, { x, 1.0F, z } };
                upright.Closed = false;
                runs.push_back( upright );
            }
            return runs;
        }

        /// A unit octahedron: three closed squares, one per axis plane. Twelve edges, three runs.
        [[nodiscard]] std::vector<ControlShapePolyline> UnitOctahedron()
        {
            ControlShapePolyline xy;
            xy.Points = {
                 { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F }, { -1.0F, 0.0F, 0.0F }, { 0.0F, -1.0F, 0.0F } };
            xy.Closed = true;

            ControlShapePolyline xz;
            xz.Points = {
                 { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { -1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, -1.0F } };
            xz.Closed = true;

            ControlShapePolyline yz;
            yz.Points = {
                 { 0.0F, 1.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 0.0F, -1.0F, 0.0F }, { 0.0F, 0.0F, -1.0F } };
            yz.Closed = true;

            return { xy, xz, yz };
        }
    } // namespace

    Common::BoolResultStr ControlShapeLibrary::Add( std::string name, ControlShape shape )
    {
        if ( name.empty() )
        {
            return Common::MakeError<bool>( "a shape with no name cannot be referenced by a control" );
        }
        if ( Find( name ) != nullptr )
        {
            return Common::MakeFormattedError<bool>( "shape '{}' is already in this library", name );
        }
        if ( shape.Polylines.empty() )
        {
            return Common::MakeFormattedError<bool>( "shape '{}' has no line runs — a control drawn with it "
                                                     "would be invisible and unhittable",
                                                     name );
        }
        if ( !Finite( shape.Transform ) )
        {
            return Common::MakeFormattedError<bool>( "shape '{}': a non-finite library transform", name );
        }
        for ( size_t run = 0; run < shape.Polylines.size(); ++run )
        {
            const ControlShapePolyline& polyline = shape.Polylines[run];
            if ( polyline.Points.size() < 2 )
            {
                return Common::MakeFormattedError<bool>( "shape '{}': run {} has {} point(s); a run needs two",
                                                         name, run, polyline.Points.size() );
            }
            for ( size_t point = 0; point < polyline.Points.size(); ++point )
            {
                if ( !Finite( polyline.Points[point] ) )
                {
                    return Common::MakeFormattedError<bool>( "shape '{}': run {} point {} is not finite", name,
                                                             run, point );
                }
            }
        }

        m_Entries.push_back( Entry{ std::move( name ), std::move( shape ) } );
        return Common::MakeSuccess( true );
    }

    const ControlShape* ControlShapeLibrary::Find( const std::string& name ) const
    {
        for ( const Entry& entry : m_Entries )
        {
            if ( entry.Name == name )
            {
                return &entry.Shape;
            }
        }
        return nullptr;
    }

    std::vector<std::string> ControlShapeLibrary::Names() const
    {
        std::vector<std::string> names;
        names.reserve( m_Entries.size() );
        for ( const Entry& entry : m_Entries )
        {
            names.push_back( entry.Name );
        }
        return names;
    }

    Common::ResultStr<ControlShapeLibrary> ControlShapeLibrary::BuiltIn()
    {
        const glm::vec3 x( 1.0F, 0.0F, 0.0F );
        const glm::vec3 y( 0.0F, 1.0F, 0.0F );
        const glm::vec3 z( 0.0F, 0.0F, 1.0F );

        ControlShape circleXY;
        circleXY.Polylines = { UnitCircle( x, y ) };

        // ONE GEOMETRY, THREE ORIENTATIONS, AND THE ENTRY TRANSFORM IS WHERE THE ORIENTATION LIVES. This
        // is the only user of `ControlShape::Transform` in the built-in table, and it is why the field is
        // not decoration: without it the other two planes would be two more point lists to keep in step.
        ControlShape circleXZ;
        circleXZ.Polylines = circleXY.Polylines;
        circleXZ.Transform =
             glm::mat4( glm::vec4( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec4( 0.0F, 0.0F, 1.0F, 0.0F ),
                        glm::vec4( 0.0F, -1.0F, 0.0F, 0.0F ), glm::vec4( 0.0F, 0.0F, 0.0F, 1.0F ) );

        ControlShape circleYZ;
        circleYZ.Polylines = circleXY.Polylines;
        circleYZ.Transform = glm::mat4( glm::vec4( 0.0F, 1.0F, 0.0F, 0.0F ), glm::vec4( 0.0F, 0.0F, 1.0F, 0.0F ),
                                        glm::vec4( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec4( 0.0F, 0.0F, 0.0F, 1.0F ) );

        // A sphere is three runs at once, so its orientation cannot come from the entry transform (an
        // entry has one). Baked here, which is the honest place for a shape that is genuinely three runs.
        ControlShape sphere;
        sphere.Polylines = { UnitCircle( x, y ), UnitCircle( x, z ), UnitCircle( y, z ) };

        ControlShape box;
        box.Polylines = UnitCube();

        ControlShape diamond;
        diamond.Polylines = UnitOctahedron();

        ControlShapeLibrary library;
        // `std::array` AND NOT A C ARRAY: the contract's rule, and the reason is that a table which can
        // reach zero rows has no legal C spelling — clang takes a zero-length array as a GNU extension
        // and MSVC rejects it outright (C2466). This one cannot reach zero today, and the type should not
        // be the thing that decides that.
        const std::array<std::pair<const char*, const ControlShape*>, 6> table = { { { "CircleXY", &circleXY },
                                                                                     { "CircleXZ", &circleXZ },
                                                                                     { "CircleYZ", &circleYZ },
                                                                                     { "Sphere", &sphere },
                                                                                     { "Box", &box },
                                                                                     { "Diamond", &diamond } } };
        for ( const auto& entry : table )
        {
            auto added = library.Add( entry.first, *entry.second );
            if ( !added.IsSuccess() )
            {
                return Common::MakeFormattedError<ControlShapeLibrary>( "the built-in control shapes are not a "
                                                                        "valid library: {}",
                                                                        added.GetError() );
            }
        }
        return Common::MakeSuccess( std::move( library ) );
    }
} // namespace Desert::Animation
