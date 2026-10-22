#pragma once

#include <Editor/Core/EditableProperty.hpp>
#include <Editor/Core/Selection/MeshBakeNormalTool.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>

#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Editor::Core
{
    /**
     * @brief THE MODELING PANEL'S DRAGGED VALUES, AS THE CONTROL CHANNEL'S `modeling` SUBJECT.
     *
     * A drag or slider in ModelingPanel has no name, so it is not a palette entry (ControlProtocol.hpp says
     * why: an entry per value is an argument list pretending to be a vocabulary). It is a row here instead,
     * and `set` writes the very ModelingState field the widget writes - one route into the value, two ways
     * to reach it. The panel's closed choices (axes, modes, on/off) are palette entries in
     * EditorLayer::BuildPaletteCommands; the census suite ModelingPaletteCensus holds both halves to the
     * panel's widget list.
     *
     * The clamps are the widget's own, so a value the panel cannot hold is refused rather than stored.
     */
    struct ModelingStateRow
    {
        // Views of literals, not pointers: the rows own nothing and are never re-seated.
        std::string_view Name;  ///< what `set` names
        std::string_view Label; ///< the widget it stands for
        std::string_view Group;
        int              Components; ///< 1, or 3 for a float3
        bool             Integer;
        float            Min;
        float            Max;
        float* ( *Float )( ModelingState& ); ///< the field, when it is a float (or a float3's first component)
        int* ( *Int )( ModelingState& );     ///< the field, when it is an int
        // A field that is neither a float nor an int - a closed choice (an enum, as its index) or a double - is
        // read and written through these instead, and Set refuses a value the widget's list does not hold.
        double ( *Get )( ModelingState& )                            = nullptr;
        Common::BoolResultStr ( *Set )( ModelingState&, double )     = nullptr;
    };

    // clang-format off
    inline constexpr std::array<ModelingStateRow, 90> kModelingStateRows = { {
         { "CubeGrid.GridFrameOrigin",  "Grid Frame Origin",  "CubeGrid", 3, false, -1.0e6f, 1.0e6f,
           []( ModelingState& s ) { return &s.GridOrigin.x; }, nullptr },
         { "CubeGrid.GridFrameOrientation", "Grid Frame Orientation", "CubeGrid", 3, false, -180.0f, 180.0f,
           []( ModelingState& s ) { return &s.GridRotation.x; }, nullptr },
         { "CubeGrid.CurrentBlockSize", "Current Block Size", "CubeGrid", 1, false, ModelingState::MinCellSize,
           ModelingState::MaxCellSize, []( ModelingState& s ) { return &s.CellSize; }, nullptr },
         { "CubeGrid.BlocksPerStep",    "Blocks / Step",      "CubeGrid", 1, true, 1.0f, 32.0f, nullptr,
           []( ModelingState& s ) { return &s.BlocksPerStep; } },
         { "CreateShape.Box.Width", "Width", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Box.Width; }, nullptr },
         { "CreateShape.Box.Depth", "Depth", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Box.Depth; }, nullptr },
         { "CreateShape.Box.Height", "Height", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Box.Height; }, nullptr },
         { "CreateShape.Box.WidthSubdivisions", "Width Subdivisions", "Create Shape", 1, true, 1.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Box.WidthSubdivisions; } },
         { "CreateShape.Box.DepthSubdivisions", "Depth Subdivisions", "Create Shape", 1, true, 1.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Box.DepthSubdivisions; } },
         { "CreateShape.Box.HeightSubdivisions", "Height Subdivisions", "Create Shape", 1, true, 1.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Box.HeightSubdivisions; } },
         { "CreateShape.Sphere.Radius", "Radius", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Sphere.Radius; }, nullptr },
         { "CreateShape.Sphere.Subdivisions", "Subdivisions", "Create Shape", 1, true, 1.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Sphere.Subdivisions; } },
         { "CreateShape.Sphere.HorizontalSlices", "Horizontal Slices", "Create Shape", 1, true, 4.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Sphere.HorizontalSlices; } },
         { "CreateShape.Sphere.VerticalSlices", "Vertical Slices", "Create Shape", 1, true, 4.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Sphere.VerticalSlices; } },
         { "CreateShape.Cylinder.Radius", "Radius", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Cylinder.Radius; }, nullptr },
         { "CreateShape.Cylinder.Height", "Height", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Cylinder.Height; }, nullptr },
         { "CreateShape.Cylinder.RadialSlices", "Radial Slices", "Create Shape", 1, true, 3.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Cylinder.RadialSlices; } },
         { "CreateShape.Cylinder.HeightSubdivisions", "Height Subdivisions", "Create Shape", 1, true, 1.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Cylinder.HeightSubdivisions; } },
         { "CreateShape.Cone.Radius", "Radius", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Cone.Radius; }, nullptr },
         { "CreateShape.Cone.Height", "Height", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Cone.Height; }, nullptr },
         { "CreateShape.Cone.RadialSlices", "Radial Slices", "Create Shape", 1, true, 3.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Cone.RadialSlices; } },
         { "CreateShape.Cone.HeightSubdivisions", "Height Subdivisions", "Create Shape", 1, true, 1.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Cone.HeightSubdivisions; } },
         { "CreateShape.Capsule.Radius", "Radius", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Capsule.Radius; }, nullptr },
         { "CreateShape.Capsule.CylinderLength", "Cylinder Length", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Capsule.CylinderLength; }, nullptr },
         { "CreateShape.Capsule.HemisphereSlices", "Hemisphere Slices", "Create Shape", 1, true, 2.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Capsule.HemisphereSlices; } },
         { "CreateShape.Capsule.CylinderSlices", "Cylinder Slices", "Create Shape", 1, true, 3.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Capsule.CylinderSlices; } },
         { "CreateShape.Capsule.CylinderSubdivisions", "Cylinder Subdivisions", "Create Shape", 1, true, 0.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Capsule.CylinderSubdivisions; } },
         { "CreateShape.Pyramid.Width", "Width", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Pyramid.Width; }, nullptr },
         { "CreateShape.Pyramid.Depth", "Depth", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Pyramid.Depth; }, nullptr },
         { "CreateShape.Pyramid.Height", "Height", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Pyramid.Height; }, nullptr },
         { "CreateShape.Stairs.Steps", "Num Steps", "Create Shape", 1, true, 2.0f, 1.0e6f, nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Stairs.Steps; } },
         { "CreateShape.Stairs.StepWidth", "Step Width", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Stairs.StepWidth; }, nullptr },
         { "CreateShape.Stairs.StepHeight", "Step Height", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Stairs.StepHeight; }, nullptr },
         { "CreateShape.Stairs.StepDepth", "Step Depth", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Stairs.StepDepth; }, nullptr },
         { "CreateShape.Stairs.InnerRadius", "Inner Radius", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Stairs.InnerRadius; }, nullptr },
         { "CreateShape.Stairs.CurveAngle", "Curve Angle", "Create Shape", 1, false, -360000.0f, 360000.0f,
           []( ModelingState& s ) { return &s.CreateShape.Stairs.CurveAngle; }, nullptr },
         { "CreateShape.Torus.MajorRadius", "Major Radius", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Torus.MajorRadius; }, nullptr },
         { "CreateShape.Torus.MinorRadius", "Minor Radius", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Torus.MinorRadius; }, nullptr },
         { "CreateShape.Torus.MajorSlices", "Major Slices", "Create Shape", 1, true, 3.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Torus.MajorSlices; } },
         { "CreateShape.Torus.MinorSlices", "Minor Slices", "Create Shape", 1, true, 3.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Torus.MinorSlices; } },
         { "CreateShape.Arrow.ShaftRadius", "Shaft Radius", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Arrow.ShaftRadius; }, nullptr },
         { "CreateShape.Arrow.ShaftHeight", "Shaft Height", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Arrow.ShaftHeight; }, nullptr },
         { "CreateShape.Arrow.HeadRadius", "Head Radius", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Arrow.HeadRadius; }, nullptr },
         { "CreateShape.Arrow.HeadHeight", "Head Height", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Arrow.HeadHeight; }, nullptr },
         { "CreateShape.Arrow.RadialSlices", "Radial Slices", "Create Shape", 1, true, 3.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Arrow.RadialSlices; } },
         { "CreateShape.Arrow.HeightSubdivisions", "Height Subdivisions", "Create Shape", 1, true, 1.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Arrow.HeightSubdivisions; } },
         { "CreateShape.Disc.Radius", "Radius", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Disc.Radius; }, nullptr },
         { "CreateShape.Disc.RadialSlices", "Radial Slices", "Create Shape", 1, true, 3.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Disc.RadialSlices; } },
         { "CreateShape.Disc.RadialSubdivisions", "Radial Subdivisions", "Create Shape", 1, true, 1.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Disc.RadialSubdivisions; } },
         { "CreateShape.Disc.HoleRadius", "Hole Radius", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Disc.HoleRadius; }, nullptr },
         { "CreateShape.Rectangle.Width", "Width", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Rectangle.Width; }, nullptr },
         { "CreateShape.Rectangle.Depth", "Depth", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Rectangle.Depth; }, nullptr },
         { "CreateShape.Rectangle.WidthSubdivisions", "Width Subdivisions", "Create Shape", 1, true, 1.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Rectangle.WidthSubdivisions; } },
         { "CreateShape.Rectangle.DepthSubdivisions", "Depth Subdivisions", "Create Shape", 1, true, 1.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Rectangle.DepthSubdivisions; } },
         { "CreateShape.Rectangle.CornerRadius", "Corner Radius", "Create Shape", 1, false, Geometry::Detail::kMinExtent, 1.0e6f,
           []( ModelingState& s ) { return &s.CreateShape.Rectangle.CornerRadius; }, nullptr },
         { "CreateShape.Rectangle.CornerSlices", "Corner Slices", "Create Shape", 1, true, 3.0f, static_cast<float>( Geometry::kMaxShapeCount ), nullptr,
           []( ModelingState& s ) { return &s.CreateShape.Rectangle.CornerSlices; } },
         { "Element.Distance",          "Distance",           "Select Elements", 1, false, -10000.0f, 10000.0f,
           []( ModelingState& s ) { return &s.ElementOpDistance; }, nullptr },
         { "Element.LoopPosition",      "Loop at",            "Select Elements", 1, false, 0.01f, 0.99f,
           []( ModelingState& s ) { return &s.ElementLoopPosition; }, nullptr },
         { "Element.WeldTolerance",     "Weld",               "Select Elements", 1, false, 0.0f, 10.0f,
           []( ModelingState& s ) { return &s.ElementWeldTolerance; }, nullptr },
         { "Element.SubdivideLevels",   "Levels",             "Select Elements", 1, true, 1.0f,
           static_cast<float>( Geometry::kMaxSubdivisionLevel ), nullptr,
           []( ModelingState& s ) { return &s.ElementSubdivide.Level; } },
         { "Element.SubdivideScheme",   "Scheme",             "Select Elements", 1, true,
           0.0f, 2.0f, nullptr, nullptr,
           []( ModelingState& s ) { return static_cast<double>( s.ElementSubdivide.Scheme ); },
           []( ModelingState& s, double v ) -> Common::BoolResultStr
           {
               s.ElementSubdivide.Scheme = static_cast<Geometry::SubdivisionScheme>( static_cast<int>( v ) );
               return Common::MakeSuccess( true );
           } },
         { "Element.SimplifyPercentage", "Percentage",        "Select Elements", 1, false, 1.0f, 100.0f,
           []( ModelingState& s ) { return &s.ElementSimplify.Percentage; }, nullptr },
         { "Element.SimplifyVertexCount", "Vertex Count",     "Select Elements", 1, true, 3.0f, 1.0e6f, nullptr,
           []( ModelingState& s ) { return &s.ElementSimplify.VertexCount; } },
         { "PlaneCut.Offset",           "Offset",             "Plane Cut", 1, false, -100000.0f, 100000.0f,
           []( ModelingState& s ) { return &s.ElementPlaneCutOffset; }, nullptr },
         { "XForm.PivotWorldPoint",     "World Point",        "XForm", 3, false, -1.0e6f, 1.0e6f,
           []( ModelingState& s ) { return &s.XformPivotWorldPoint.x; }, nullptr },
         { "Pattern.Count",             "Count",              "Pattern", 1, true, 1.0f,
           static_cast<float>( Geometry::kMaxPatternCopies ), nullptr,
           []( ModelingState& s ) { return &s.XformPattern.Count; } },
         { "Pattern.Radius",            "Radius",             "Pattern", 1, false, 0.0f, 1.0e6f,
           []( ModelingState& s ) { return &s.XformPattern.Radius; }, nullptr },
         { "Pattern.Spacing",           "Spacing",            "Pattern", 1, false, -1.0e6f, 1.0e6f,
           []( ModelingState& s ) { return &s.XformPattern.Spacing; }, nullptr },
         { "Pattern.CountB",            "Count B",            "Pattern", 1, true, 1.0f,
           static_cast<float>( Geometry::kMaxPatternCopies ), nullptr,
           []( ModelingState& s ) { return &s.XformPattern.CountB; } },
         { "Pattern.SpacingB",          "Spacing B",          "Pattern", 1, false, -1.0e6f, 1.0e6f,
           []( ModelingState& s ) { return &s.XformPattern.SpacingB; }, nullptr },
         { "Pattern.SweepDegrees",      "Sweep",              "Pattern", 1, false, -360.0f, 360.0f,
           []( ModelingState& s ) { return &s.XformPattern.SweepDegrees; }, nullptr },
         { "AutoUV.MaxNormalDeviation", "Max Normal Deviation", "AutoUV", 1, false, 1.0f, 180.0f,
           []( ModelingState& s ) { return &s.UVAuto.MaxNormalDeviationDeg; }, nullptr },
         { "AutoUV.PackingResolution",  "Packing Resolution", "AutoUV", 1, true, 16.0f, 4096.0f, nullptr,
           []( ModelingState& s ) { return &s.UVAuto.PackingResolution; } },
         { "UVProject.Dimensions",      "Dimensions",         "UV Project", 3, false, 0.001f, 1.0e6f,
           []( ModelingState& s ) { return &s.UVProject.Dimensions.x; }, nullptr },
         { "UVProject.CenterOffset",    "Center Offset",      "UV Project", 3, false, -1.0e6f, 1.0e6f,
           []( ModelingState& s ) { return &s.UVProject.CenterOffset.x; }, nullptr },
         { "UVProject.Rotation",        "Rotation",           "UV Project", 3, false, -360.0f, 360.0f,
           []( ModelingState& s ) { return &s.UVProject.RotationDeg.x; }, nullptr },
         { "UVProject.CylinderSplitAngle", "Cylinder Split Angle", "UV Project", 1, false, 0.0f, 90.0f,
           []( ModelingState& s ) { return &s.UVProject.CylinderSplitAngle; }, nullptr },
         { "UVProject.UVRotation",      "UV Rotation",        "UV Project", 1, false, -360.0f, 360.0f,
           []( ModelingState& s ) { return &s.UVProject.UVRotationDeg; }, nullptr },
         { "UVProject.UVScaleU",        "UV Scale U",         "UV Project", 1, false, -1000.0f, 1000.0f,
           []( ModelingState& s ) { return &s.UVProject.UVScale.x; }, nullptr },
         { "UVProject.UVScaleV",        "UV Scale V",         "UV Project", 1, false, -1000.0f, 1000.0f,
           []( ModelingState& s ) { return &s.UVProject.UVScale.y; }, nullptr },
         { "UVProject.UVTranslateU",    "UV Translate U",     "UV Project", 1, false, -1000.0f, 1000.0f,
           []( ModelingState& s ) { return &s.UVProject.UVTranslate.x; }, nullptr },
         { "UVProject.UVTranslateV",    "UV Translate V",     "UV Project", 1, false, -1000.0f, 1000.0f,
           []( ModelingState& s ) { return &s.UVProject.UVTranslate.y; }, nullptr },
         { "UVUnwrap.PackingResolution", "Packing Resolution", "UV Unwrap", 1, true, 16.0f, 4096.0f, nullptr,
           []( ModelingState& s ) { return &s.UVUnwrap.PackingResolution; } },
         { "UVLayout.TextureResolution", "Texture Resolution", "UV Layout", 1, true, 16.0f, 4096.0f, nullptr,
           []( ModelingState& s ) { return &s.UVLayout.TextureResolution; } },
         { "UVLayout.Scale",            "Scale",              "UV Layout", 1, false, 0.001f, 1000.0f,
           []( ModelingState& s ) { return &s.UVLayout.Scale; }, nullptr },
         { "UVLayout.TranslateU",       "Translate U",        "UV Layout", 1, false, -1000.0f, 1000.0f,
           []( ModelingState& s ) { return &s.UVLayout.Translation.x; }, nullptr },
         { "UVLayout.TranslateV",       "Translate V",        "UV Layout", 1, false, -1000.0f, 1000.0f,
           []( ModelingState& s ) { return &s.UVLayout.Translation.y; }, nullptr },
         // The Bake palette (ModelingPanel::DrawBakePalette): its settings are Core::BakeNormalMapSettings(),
         // shared with the Modeling / Bake Normal Map command, not a ModelingState field.
         { "Bake.Resolution",           "Resolution",         "Bake", 1, true, 256.0f, 4096.0f, nullptr, nullptr,
           []( ModelingState& ) { return static_cast<double>( BakeNormalMapSettings().Settings.Resolution ); },
           []( ModelingState&, double v ) -> Common::BoolResultStr
           {
               const int resolution = static_cast<int>( v );
               if ( resolution != 256 && resolution != 512 && resolution != 1024 && resolution != 2048 &&
                    resolution != 4096 )
                   return Common::MakeFormattedError<bool>(
                        "'Bake.Resolution' = {} is not on the panel's list (256, 512, 1024, 2048, 4096)", resolution );
               BakeNormalMapSettings().Settings.Resolution = resolution;
               return Common::MakeSuccess( true );
           } },
         { "Bake.ProjectionDistance",   "Projection Distance", "Bake", 1, false, 0.001f, 1.0e6f, nullptr, nullptr,
           []( ModelingState& ) { return BakeNormalMapSettings().Settings.ProjectionDistance; },
           []( ModelingState&, double v ) -> Common::BoolResultStr
           {
               BakeNormalMapSettings().Settings.ProjectionDistance = v;
               return Common::MakeSuccess( true );
           } },
         { "Bake.Gutter",               "Gutter",             "Bake", 1, true, 0.0f, 64.0f, nullptr,
           []( ModelingState& ) { return &BakeNormalMapSettings().Settings.GutterTexels; } },
    } };
    // clang-format on

    [[nodiscard]] inline std::vector<EditableProperty> DescribeModelingState( ModelingState& state )
    {
        std::vector<EditableProperty> census;
        for ( const ModelingStateRow& row : kModelingStateRows )
        {
            EditableProperty property;
            property.Name       = std::string( row.Name );
            property.Label      = std::string( row.Label );
            property.Group      = std::string( row.Group );
            property.Type       = row.Integer ? "int" : ( row.Components == 3 ? "float3" : "float" );
            property.Components = row.Components;
            property.Min        = row.Min;
            property.Max        = row.Max;
            if ( row.Get )
                property.Value[0] = static_cast<float>( row.Get( state ) );
            else if ( row.Integer )
                property.Value[0] = static_cast<float>( *row.Int( state ) );
            else
                for ( int i = 0; i < row.Components; ++i )
                    property.Value[static_cast<std::size_t>( i )] = row.Float( state )[i];
            census.push_back( property );
        }
        return census;
    }

    [[nodiscard]] inline Common::BoolResultStr
    SetModelingStateProperty( ModelingState& state, const std::string& property, const std::vector<float>& value )
    {
        for ( const ModelingStateRow& row : kModelingStateRows )
        {
            if ( property != row.Name )
                continue;
            if ( static_cast<int>( value.size() ) != row.Components )
                return Common::MakeFormattedError<bool>( "'{}' takes {} number(s) and was given {}", property,
                                                         row.Components, value.size() );
            for ( const float v : value )
                if ( !std::isfinite( v ) || v < row.Min || v > row.Max )
                    return Common::MakeFormattedError<bool>(
                         "'{}' = {} is outside the panel's range [{}, {}]; the widget cannot hold it either",
                         property, v, row.Min, row.Max );
            if ( row.Integer && value[0] != std::floor( value[0] ) )
                return Common::MakeFormattedError<bool>( "'{}' is a whole number and was given {}", property,
                                                         value[0] );
            if ( row.Set )
                return row.Set( state, static_cast<double>( value[0] ) );
            if ( row.Integer )
                *row.Int( state ) = static_cast<int>( value[0] );
            else
                for ( int i = 0; i < row.Components; ++i )
                    row.Float( state )[i] = value[static_cast<std::size_t>( i )];
            return Common::MakeSuccess( true );
        }

        std::string known;
        for ( const ModelingStateRow& row : kModelingStateRows )
            known += ( known.empty() ? "" : ", " ) + std::string( row.Name );
        return Common::MakeFormattedError<bool>( "'{}' is not a Modeling panel value. The subject offers: {}",
                                                 property, known );
    }
} // namespace Desert::Editor::Core
