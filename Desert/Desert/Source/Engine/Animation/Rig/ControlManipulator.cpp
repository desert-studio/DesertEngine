#include "ControlManipulator.hpp"

#include <algorithm>
#include <cmath>

namespace Desert::Animation
{
    namespace
    {
        /// Clip w at or below this is behind the eye. Matches the viewport overlay's own threshold, which
        /// is the point: two different epsilons would make a shape appear in one and not the other.
        constexpr float kClipEpsilon = 1e-4F;

        /// A matrix whose determinant is below this cannot be inverted usefully here. A control space
        /// that degenerate means a parent was authored with a zero scale, and the honest answer is a
        /// refusal rather than a drag that sends the control to infinity.
        constexpr float kSingularEpsilon = 1e-8F;

        [[nodiscard]] glm::vec2 PixelToNdc( const ManipulatorView& view, const glm::vec2& pixel )
        {
            const glm::vec2 relative = ( pixel - view.ViewportOrigin ) / view.ViewportSize;
            return { ( relative.x * 2.0F ) - 1.0F, 1.0F - ( relative.y * 2.0F ) };
        }

        /**
         * @brief Both ends of the pixel's line, ORDERED NEAR-END FIRST — without being told which is which.
         *
         * THIS ORDERING IS THE WHOLE REASON THIS FUNCTION EXISTS, and getting it wrong is not a cosmetic
         * error. The engine's projections are REVERSED-Z (`Engine/Core/Projection.hpp`), so NDC z of 0 is
         * the FAR plane — and the default far plane is 50 km. Unprojecting there and then intersecting a
         * drag plane 3 m in front of the camera subtracts two numbers of magnitude 5e6 to get one of
         * magnitude 3e2: measured, that lost about a pixel of drag accuracy, and the homogeneous w at
         * that distance (~2e-7) is small enough that a fixed epsilon rejects the unprojection outright.
         *
         * THE DISCRIMINATOR NEEDS NO CONVENTION. For any perspective projection the unprojected
         * homogeneous w is proportional to 1/view-depth, so the end with the LARGER |w| is the nearer
         * one. Under an orthographic projection both are exactly 1 and either end will do, which is also
         * the right answer. So this file still does not know, and does not need to know, which way z runs.
         */
        [[nodiscard]] bool UnprojectLine( const glm::mat4& inverseViewProjection, const glm::vec2& ndc,
                                          glm::vec3& nearEnd, glm::vec3& farEnd )
        {
            const glm::vec4 atZeroZ = inverseViewProjection * glm::vec4( ndc.x, ndc.y, 0.0F, 1.0F );
            const glm::vec4 atOneZ  = inverseViewProjection * glm::vec4( ndc.x, ndc.y, 1.0F, 1.0F );

            // A RELATIVE FLOOR, NOT A PIXEL ONE. The only thing that makes this division illegal is a w
            // of zero; a merely small w means "far away", which is a legitimate answer about a 50 km
            // plane and used to be refused as if the matrix were broken.
            if ( !( std::abs( atZeroZ.w ) > 0.0F ) || !( std::abs( atOneZ.w ) > 0.0F ) )
            {
                return false;
            }

            const glm::vec3 first  = glm::vec3( atZeroZ ) / atZeroZ.w;
            const glm::vec3 second = glm::vec3( atOneZ ) / atOneZ.w;
            if ( !std::isfinite( first.x ) || !std::isfinite( first.y ) || !std::isfinite( first.z ) ||
                 !std::isfinite( second.x ) || !std::isfinite( second.y ) || !std::isfinite( second.z ) )
            {
                return false;
            }

            if ( std::abs( atZeroZ.w ) >= std::abs( atOneZ.w ) )
            {
                nearEnd = first;
                farEnd  = second;
            }
            else
            {
                nearEnd = second;
                farEnd  = first;
            }
            return true;
        }

        /// The near end of the pixel's line. What every direction here is measured at, for the reason
        /// `UnprojectLine` carries.
        [[nodiscard]] bool UnprojectNear( const glm::mat4& inverseViewProjection, const glm::vec2& ndc,
                                          glm::vec3& out )
        {
            glm::vec3 discard( 0.0F );
            return UnprojectLine( inverseViewProjection, ndc, out, discard );
        }

        /**
         * @brief Screen right, screen up and the axis out of the screen, in world, at one pixel.
         *
         * BUILT FROM PIXEL OFFSETS AT ONE FIXED DEPTH, which is what keeps this file free of the depth
         * convention: the DIFFERENCE between two points unprojected at the same z is the same direction
         * whichever end of the range that z is. Nothing here needs the camera position either.
         */
        [[nodiscard]] bool ScreenBasis( const ManipulatorView& view, const glm::mat4& inverseViewProjection,
                                        const glm::vec2& pixel, glm::vec3& right, glm::vec3& up,
                                        glm::vec3& toward )
        {
            glm::vec3 centre( 0.0F );
            glm::vec3 oneRight( 0.0F );
            glm::vec3 oneDown( 0.0F );
            if ( !UnprojectNear( inverseViewProjection, PixelToNdc( view, pixel ), centre ) )
            {
                return false;
            }
            if ( !UnprojectNear( inverseViewProjection, PixelToNdc( view, pixel + glm::vec2( 1.0F, 0.0F ) ),
                                 oneRight ) )
            {
                return false;
            }
            if ( !UnprojectNear( inverseViewProjection, PixelToNdc( view, pixel + glm::vec2( 0.0F, 1.0F ) ),
                                 oneDown ) )
            {
                return false;
            }

            const glm::vec3 acrossX = oneRight - centre;
            const glm::vec3 downY   = oneDown - centre;
            if ( glm::dot( acrossX, acrossX ) < 1e-20F || glm::dot( downY, downY ) < 1e-20F )
            {
                return false;
            }

            right = glm::normalize( acrossX );

            // PIXEL Y GROWS DOWNWARD, so screen-up is the other way. Orthogonalised against `right`
            // rather than assumed perpendicular: a projection carrying shear would otherwise hand back a
            // basis that is not a rotation, and the arcball built on it would not preserve angles.
            const glm::vec3 upRaw   = -downY;
            const glm::vec3 upOrtho = upRaw - ( right * glm::dot( upRaw, right ) );
            if ( glm::dot( upOrtho, upOrtho ) < 1e-20F )
            {
                return false;
            }
            up = glm::normalize( upOrtho );

            const glm::vec3 outward = glm::cross( right, up );
            if ( glm::dot( outward, outward ) < 1e-20F )
            {
                return false;
            }
            toward = glm::normalize( outward );
            return true;
        }

        /**
         * @brief The line through a pixel, from its NEAR end outwards.
         *
         * The origin is the near end and the direction points away from the eye — not because this file
         * knows the depth convention, but because `UnprojectLine` orders the two ends by their own
         * homogeneous w. Measuring from the near end is what keeps a plane intersection three metres away
         * accurate: from the far end it is a difference of two 50 km numbers (see `UnprojectLine`).
         */
        [[nodiscard]] bool LineThroughPixel( const ManipulatorView& view, const glm::mat4& inverseViewProjection,
                                             const glm::vec2& pixel, glm::vec3& origin, glm::vec3& direction )
        {
            glm::vec3 nearEnd( 0.0F );
            glm::vec3 farEnd( 0.0F );
            if ( !UnprojectLine( inverseViewProjection, PixelToNdc( view, pixel ), nearEnd, farEnd ) )
            {
                return false;
            }
            const glm::vec3 along = farEnd - nearEnd;
            if ( glm::dot( along, along ) < 1e-20F )
            {
                return false;
            }
            origin    = nearEnd;
            direction = glm::normalize( along );
            return true;
        }

        [[nodiscard]] bool IntersectPlane( const glm::vec3& origin, const glm::vec3& direction,
                                           const glm::vec3& planePoint, const glm::vec3& planeNormal,
                                           glm::vec3& hit )
        {
            const float denominator = glm::dot( direction, planeNormal );
            if ( std::abs( denominator ) < 1e-6F )
            {
                return false;
            }
            const float distance = glm::dot( planePoint - origin, planeNormal ) / denominator;
            hit                  = origin + ( direction * distance );
            return std::isfinite( hit.x ) && std::isfinite( hit.y ) && std::isfinite( hit.z );
        }

        /// Shoemake's trackball point, with the outside of the circle mapped onto the silhouette so a
        /// pointer dragged past the rim keeps turning the control instead of sticking.
        [[nodiscard]] glm::vec3 ArcballVector( const glm::vec2& pixel, const glm::vec2& centre, float radius,
                                               const glm::vec3& right, const glm::vec3& up,
                                               const glm::vec3& toward )
        {
            glm::vec2 onDisc = ( pixel - centre ) / radius;
            onDisc.y         = -onDisc.y; // pixel y grows downward; the trackball's y is screen-up

            const float squared = ( onDisc.x * onDisc.x ) + ( onDisc.y * onDisc.y );
            if ( squared > 1.0F )
            {
                const glm::vec2 rim = onDisc / std::sqrt( squared );
                return glm::normalize( ( right * rim.x ) + ( up * rim.y ) );
            }
            return glm::normalize( ( right * onDisc.x ) + ( up * onDisc.y ) +
                                   ( toward * std::sqrt( 1.0F - squared ) ) );
        }

        [[nodiscard]] float DistanceToSegment( const glm::vec2& point, const glm::vec2& a, const glm::vec2& b )
        {
            const glm::vec2 along         = b - a;
            const float     lengthSquared = glm::dot( along, along );
            if ( lengthSquared < 1e-12F )
            {
                return glm::length( point - a );
            }
            const float parameter = std::clamp( glm::dot( point - a, along ) / lengthSquared, 0.0F, 1.0F );
            return glm::length( point - ( a + ( along * parameter ) ) );
        }

        /**
         * @brief The control's parent space times its offset, recovered from what the hierarchy just said.
         *
         * `global = parent * offset * pose`, so `parent * offset = global * inverse(pose)`. Recovered
         * rather than asked for because T5.1 keeps `ParentSpace` private and should — but note WHICH
         * global this is: `GetGlobalTransform` is called here, on this frame, and that call is what
         * resolves the control's ancestors. This is the same discipline T5.1's `SetGlobalTransform` needed
         * from the other side, where a parent nobody had read still held last frame's cache.
         */
        [[nodiscard]] Common::ResultStr<glm::mat4> ParentTimesOffset( ControlHierarchy& hierarchy,
                                                                      uint32_t          control )
        {
            const glm::mat4 pose = hierarchy.Get( control ).Pose.ToMatrix();
            if ( std::abs( glm::determinant( pose ) ) < kSingularEpsilon )
            {
                return Common::MakeFormattedError<glm::mat4>(
                     "control '{}': its pose has a zero scale, so the space it sits in cannot be recovered "
                     "from its own transform",
                     hierarchy.Get( control ).Name );
            }

            const glm::mat4 space = hierarchy.GetGlobalTransform( control ) * glm::inverse( pose );
            if ( std::abs( glm::determinant( space ) ) < kSingularEpsilon )
            {
                return Common::MakeFormattedError<glm::mat4>(
                     "control '{}': its parent space is degenerate (determinant {}), so a pointer movement "
                     "cannot be converted into it",
                     hierarchy.Get( control ).Name, glm::determinant( space ) );
            }
            return Common::MakeSuccess( glm::mat4( space ) );
        }
    } // namespace

    ProjectedPoint ProjectToViewport( const ManipulatorView& view, const glm::vec3& world )
    {
        ProjectedPoint projected;

        const glm::vec4 clip = view.ViewProjection * glm::vec4( world, 1.0F );
        if ( clip.w <= kClipEpsilon )
        {
            // BEHIND THE EYE, AND THE ANSWER IS "NO", not a mirrored pixel. Dividing by a non-positive w
            // anyway is what produced the ghost light bulb when the camera turned 180 degrees and the
            // radius circles streaking across the whole viewport.
            return projected;
        }

        const glm::vec3 ndc = glm::vec3( clip ) / clip.w;
        projected.Pixel.x   = view.ViewportOrigin.x + ( ( ( ndc.x * 0.5F ) + 0.5F ) * view.ViewportSize.x );
        projected.Pixel.y =
             view.ViewportOrigin.y + ( ( 1.0F - ( ( ndc.y * 0.5F ) + 0.5F ) ) * view.ViewportSize.y );
        projected.Depth   = ndc.z;
        projected.InFront = true;
        return projected;
    }

    void BuildFrame( ControlHierarchy& hierarchy, const ControlShapeLibrary& library, const ManipulatorView& view,
                     ManipulatorFrame& out )
    {
        out.Shapes.clear();
        out.UnknownShapes.clear();

        for ( uint32_t control = 0; control < static_cast<uint32_t>( hierarchy.Size() ); ++control )
        {
            const ControlElement& element = hierarchy.Get( control );
            if ( element.ShapeName.empty() )
            {
                continue; // a control the rigger chose not to draw. Distinct from one whose name is wrong.
            }

            const ControlShape* shape = library.Find( element.ShapeName );
            if ( shape == nullptr )
            {
                out.UnknownShapes.push_back( element.ShapeName );
                continue;
            }

            ControlShapeDraw draw;
            draw.Control = control;
            draw.World   = hierarchy.GetGlobalTransform( control );

            // THE PLACEMENT, AND IT IS THE WHOLE CLAIM OF THIS TIER: report 01 §(a)6 composes a shape as
            // the library's transform under the control's global. There is no third term, because a
            // per-control shape transform is state T5.1 deliberately does not carry.
            const glm::mat4 placement = draw.World * shape->Transform;

            for ( const ControlShapePolyline& run : shape->Polylines )
            {
                const auto base = static_cast<uint32_t>( draw.WorldPoints.size() );
                for ( const glm::vec3& point : run.Points )
                {
                    draw.WorldPoints.emplace_back( placement * glm::vec4( point, 1.0F ) );
                }
                const auto count = static_cast<uint32_t>( run.Points.size() );
                for ( uint32_t i = 0; ( i + 1 ) < count; ++i )
                {
                    draw.Segments.emplace_back( base + i, base + i + 1 );
                }
                if ( run.Closed && count > 2 )
                {
                    draw.Segments.emplace_back( base + count - 1, base );
                }
            }

            draw.Origin = ProjectToViewport( view, glm::vec3( draw.World[3] ) );

            for ( const glm::uvec2& segment : draw.Segments )
            {
                glm::vec4 clipA = view.ViewProjection * glm::vec4( draw.WorldPoints[segment.x], 1.0F );
                glm::vec4 clipB = view.ViewProjection * glm::vec4( draw.WorldPoints[segment.y], 1.0F );
                if ( clipA.w <= kClipEpsilon && clipB.w <= kClipEpsilon )
                {
                    continue; // wholly behind the eye: it contributes nothing, not a wrapped line
                }
                // THE CROSSING IS CUT IN CLIP SPACE, where the near plane is a plane. A segment with one
                // end behind the camera, projected end-to-end, streaks across the whole viewport — the
                // defect the bone overlay's DrawWorldLine carries the same arithmetic for.
                if ( clipA.w <= kClipEpsilon )
                {
                    clipA = clipA + ( ( ( kClipEpsilon - clipA.w ) / ( clipB.w - clipA.w ) ) * ( clipB - clipA ) );
                }
                else if ( clipB.w <= kClipEpsilon )
                {
                    clipB = clipB + ( ( ( kClipEpsilon - clipB.w ) / ( clipA.w - clipB.w ) ) * ( clipA - clipB ) );
                }

                const glm::vec3 ndcA = glm::vec3( clipA ) / clipA.w;
                const glm::vec3 ndcB = glm::vec3( clipB ) / clipB.w;

                ManipulatorSegment drawn;
                drawn.A.x = view.ViewportOrigin.x + ( ( ( ndcA.x * 0.5F ) + 0.5F ) * view.ViewportSize.x );
                drawn.A.y =
                     view.ViewportOrigin.y + ( ( 1.0F - ( ( ndcA.y * 0.5F ) + 0.5F ) ) * view.ViewportSize.y );
                drawn.B.x = view.ViewportOrigin.x + ( ( ( ndcB.x * 0.5F ) + 0.5F ) * view.ViewportSize.x );
                drawn.B.y =
                     view.ViewportOrigin.y + ( ( 1.0F - ( ( ndcB.y * 0.5F ) + 0.5F ) ) * view.ViewportSize.y );
                draw.Screen.push_back( drawn );

                if ( draw.Origin.InFront )
                {
                    draw.ScreenRadius = std::max( { draw.ScreenRadius, glm::length( drawn.A - draw.Origin.Pixel ),
                                                    glm::length( drawn.B - draw.Origin.Pixel ) } );
                }
            }

            out.Shapes.push_back( std::move( draw ) );
        }
    }

    ManipulatorHit HitTest( const ManipulatorFrame& frame, const glm::vec2& pointer, float radiusPx )
    {
        ManipulatorHit hit;
        float          best = radiusPx;

        for ( const ControlShapeDraw& shape : frame.Shapes )
        {
            for ( const ManipulatorSegment& segment : shape.Screen )
            {
                const float distance = DistanceToSegment( pointer, segment.A, segment.B );
                if ( distance < best )
                {
                    best           = distance;
                    hit.Control    = shape.Control;
                    hit.DistancePx = distance;
                }
            }
        }
        return hit;
    }

    Common::BoolResultStr ControlDrag::Begin( ControlHierarchy& hierarchy, uint32_t control, ManipulatorMode mode,
                                              const ManipulatorView& view, const glm::vec2& pointer,
                                              float arcballRadiusPx )
    {
        m_Active = false;

        if ( control >= static_cast<uint32_t>( hierarchy.Size() ) )
        {
            return Common::MakeFormattedError<bool>( "no control {} to grab in a rig of {}", control,
                                                     hierarchy.Size() );
        }
        if ( !( view.ViewportSize.x > 0.0F ) || !( view.ViewportSize.y > 0.0F ) )
        {
            return Common::MakeFormattedError<bool>( "a viewport of {} x {} has no pixels to drag in",
                                                     view.ViewportSize.x, view.ViewportSize.y );
        }

        const glm::mat4      global = hierarchy.GetGlobalTransform( control );
        const ProjectedPoint origin = ProjectToViewport( view, glm::vec3( global[3] ) );
        if ( !origin.InFront )
        {
            return Common::MakeFormattedError<bool>(
                 "control '{}' is behind the camera; a drag has no screen position to measure from",
                 hierarchy.Get( control ).Name );
        }

        const glm::mat4 inverseViewProjection = glm::inverse( view.ViewProjection );

        glm::vec3 right( 0.0F );
        glm::vec3 up( 0.0F );
        glm::vec3 toward( 0.0F );
        if ( !ScreenBasis( view, inverseViewProjection, origin.Pixel, right, up, toward ) )
        {
            return Common::MakeError<bool>( "this view-projection cannot be inverted into a screen basis" );
        }

        glm::vec3 lineOrigin( 0.0F );
        glm::vec3 lineDirection( 0.0F );
        if ( !LineThroughPixel( view, inverseViewProjection, pointer, lineOrigin, lineDirection ) )
        {
            return Common::MakeFormattedError<bool>( "the pointer at ({}, {}) does not unproject to a line",
                                                     pointer.x, pointer.y );
        }

        m_PlanePoint  = glm::vec3( global[3] );
        m_PlaneNormal = toward;
        if ( !IntersectPlane( lineOrigin, lineDirection, m_PlanePoint, m_PlaneNormal, m_HitAtGrab ) )
        {
            return Common::MakeError<bool>( "the pointer's line is parallel to the drag plane" );
        }

        m_ArcballCenter = origin.Pixel;
        m_ArcballRadius = std::max( arcballRadiusPx, 1.0F );
        m_ArcAtGrab     = ArcballVector( pointer, m_ArcballCenter, m_ArcballRadius, right, up, toward );

        // LAST, AND IT IS THE LOCAL SIDE. Everything above is screen geometry; this is the only thing
        // remembered about the rig, and there is deliberately no global among it.
        m_PoseAtGrab = hierarchy.Get( control ).Pose;
        m_Control    = control;
        m_Mode       = mode;
        m_Active     = true;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr ControlDrag::Update( ControlHierarchy& hierarchy, const ManipulatorView& view,
                                               const glm::vec2& pointer )
    {
        if ( !m_Active )
        {
            return Common::MakeError<bool>( "no drag is in progress" );
        }
        if ( m_Control >= static_cast<uint32_t>( hierarchy.Size() ) )
        {
            return Common::MakeFormattedError<bool>( "the dragged control {} is not in this rig of {}", m_Control,
                                                     hierarchy.Size() );
        }

        auto space = ParentTimesOffset( hierarchy, m_Control );
        if ( !space.IsSuccess() )
        {
            return Common::MakeError<bool>( space.GetError() );
        }

        const glm::mat4 inverseViewProjection = glm::inverse( view.ViewProjection );
        BoneTransform   pose                  = m_PoseAtGrab;

        if ( m_Mode == ManipulatorMode::Translate )
        {
            glm::vec3 lineOrigin( 0.0F );
            glm::vec3 lineDirection( 0.0F );
            if ( !LineThroughPixel( view, inverseViewProjection, pointer, lineOrigin, lineDirection ) )
            {
                return Common::MakeFormattedError<bool>( "the pointer at ({}, {}) does not unproject to a line",
                                                         pointer.x, pointer.y );
            }
            glm::vec3 hit( 0.0F );
            if ( !IntersectPlane( lineOrigin, lineDirection, m_PlanePoint, m_PlaneNormal, hit ) )
            {
                return Common::MakeError<bool>( "the pointer's line is parallel to the drag plane" );
            }

            // THE POINTER DELTA, CONVERTED INTO THE SPACE AS IT IS NOW. Note the w of 0: this is a
            // direction, so the parent's translation must not be added to it — a w of 1 here would make
            // every drag also teleport the control by the parent's own position.
            const glm::vec3 worldDelta = hit - m_HitAtGrab;
            const glm::vec3 localDelta =
                 glm::vec3( glm::inverse( space.GetValue() ) * glm::vec4( worldDelta, 0.0F ) );
            pose.Translation = m_PoseAtGrab.Translation + localDelta;
        }
        else
        {
            glm::vec3 right( 0.0F );
            glm::vec3 up( 0.0F );
            glm::vec3 toward( 0.0F );
            if ( !ScreenBasis( view, inverseViewProjection, m_ArcballCenter, right, up, toward ) )
            {
                return Common::MakeError<bool>( "this view-projection cannot be inverted into a screen basis" );
            }

            const glm::vec3 arc = ArcballVector( pointer, m_ArcballCenter, m_ArcballRadius, right, up, toward );
            const glm::quat worldDelta = glm::rotation( m_ArcAtGrab, arc );

            const auto decomposed = BoneTransform::FromMatrix( space.GetValue() );
            if ( !decomposed.IsSuccess() )
            {
                return Common::MakeFormattedError<bool>(
                     "control '{}': the space it sits in has no rotation to turn a screen arc into ({})",
                     hierarchy.Get( m_Control ).Name, decomposed.GetError() );
            }

            // TURNED IN THE WORLD, WRITTEN IN THE PARENT'S FRAME. The control's world rotation is to
            // become `worldDelta * world`, and `world = spaceRotation * pose`, so the pose that produces
            // it is `inverse(spaceRotation) * worldDelta * spaceRotation * pose`. Quaternions throughout:
            // report 01 §(c)4 spends nine call layers on Euler flipping, and the only reason it has to is
            // that UE's control values are Euler. Ours are not, so that entire problem is absent.
            const glm::quat spaceRotation = decomposed.GetValue().Rotation;
            pose.Rotation = glm::normalize( glm::inverse( spaceRotation ) * worldDelta * spaceRotation *
                                            m_PoseAtGrab.Rotation );
        }

        return hierarchy.SetPose( m_Control, pose );
    }

    void ControlDrag::End()
    {
        m_Active  = false;
        m_Control = ControlHierarchy::INVALID;
    }
} // namespace Desert::Animation
