// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/DynamicMeshAttributeSet.cpp:1-2478,
// adapted: UE Core via UECore.hpp, namespace Desert::Geometry; only the layers the header ports (UV,
// normal/tangent, colour, MaterialID, polygroup layers, generic attributes) -- the
// weight/label/skin/morph/bone/sculpt branches of every function, IsSameAs, Serialize (1580-2227) and the bone
// helpers (2408-2478) are not ported; SplitAllBowties runs its layers serially.
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/MapLookup.hpp"
#include <Common/Core/Core.hpp>

using namespace Desert::Geometry;

namespace
{
    // TIndirectArray::SetNum over the std::vector<std::unique_ptr> the port stores layers in.
    template <typename LayerType, typename MakeFn>
    void SetNumLayers( std::vector<std::unique_ptr<LayerType>>& Layers, int32_t Num, MakeFn&& Make )
    {
        if ( static_cast<int32_t>( Layers.size() ) == Num )
        {
            return;
        }
        if ( Num >= static_cast<int32_t>( Layers.size() ) )
        {
            for ( auto i = static_cast<int32_t>( Layers.size() ); i < Num; ++i )
            {
                Layers.push_back( Make() );
            }
        }
        else
        {
            Layers.resize( Num );
        }
        DESERT_VERIFY_WARN( static_cast<int32_t>( Layers.size() ) == Num );
    }
} // namespace

DynamicMeshAttributeSet::DynamicMeshAttributeSet( DynamicMesh3* Mesh ) : ParentMesh( Mesh )
{
    SetNumUVLayers( 1 );
    SetNumNormalLayers( 1 );
}

DynamicMeshAttributeSet::DynamicMeshAttributeSet( DynamicMesh3* Mesh, int32_t NumUVLayersIn,
                                                  int32_t NumNormalLayersIn )
     : ParentMesh( Mesh )
{
    SetNumUVLayers( NumUVLayersIn );
    SetNumNormalLayers( NumNormalLayersIn );
}

DynamicMeshAttributeSet::~DynamicMeshAttributeSet() = default;

void DynamicMeshAttributeSet::Copy( const DynamicMeshAttributeSet& Copy )
{
    SetNumUVLayers( Copy.NumUVLayers() );
    for ( int UVIdx = 0; UVIdx < NumUVLayers(); UVIdx++ )
    {
        UVLayers[UVIdx]->Copy( *Copy.UVLayers[UVIdx] );
    }
    SetNumNormalLayers( Copy.NumNormalLayers() );
    for ( int NormalLayerIndex = 0; NormalLayerIndex < NumNormalLayers(); NormalLayerIndex++ )
    {
        NormalLayers[NormalLayerIndex]->Copy( *Copy.NormalLayers[NormalLayerIndex] );
    }
    if ( Copy.ColorLayer )
    {
        EnablePrimaryColors();
        ColorLayer->Copy( *( Copy.ColorLayer ) );
    }
    else
    {
        DisablePrimaryColors();
    }
    if ( Copy.MaterialIDAttrib )
    {
        EnableMaterialID();
        MaterialIDAttrib->Copy( *( Copy.MaterialIDAttrib ) );
    }
    else
    {
        DisableMaterialID();
    }

    SetNumPolygroupLayers( Copy.NumPolygroupLayers() );
    for ( int GroupIdx = 0; GroupIdx < NumPolygroupLayers(); ++GroupIdx )
    {
        PolygroupLayers[GroupIdx]->Copy( *Copy.PolygroupLayers[GroupIdx] );
    }

    ResetRegisteredAttributes();
    GenericAttributes.clear();
    for ( const auto& AttribPair : Copy.GenericAttributes )
    {
        AttachAttribute( AttribPair.first, AttribPair.second->MakeCopy( ParentMesh ) );
    }
}

bool DynamicMeshAttributeSet::IsCompact() const
{
    for ( int UVIdx = 0; UVIdx < NumUVLayers(); UVIdx++ )
    {
        if ( !UVLayers[UVIdx]->IsCompact() )
        {
            return false;
        }
    }
    for ( int NormalLayerIndex = 0; NormalLayerIndex < NumNormalLayers(); NormalLayerIndex++ )
    {
        if ( !NormalLayers[NormalLayerIndex]->IsCompact() )
        {
            return false;
        }
    }
    if ( HasPrimaryColors() )
    {
        if ( !ColorLayer->IsCompact() )
        {
            return false;
        }
    }
    // material ID and generic per-triangle attributes are compact if the parent mesh is compact
    return true;
}

void DynamicMeshAttributeSet::CompactCopy( const DynamicMeshCompactMaps&  CompactMaps,
                                           const DynamicMeshAttributeSet& Copy )
{
    SetNumUVLayers( Copy.NumUVLayers() );
    for ( int UVIdx = 0; UVIdx < NumUVLayers(); UVIdx++ )
    {
        UVLayers[UVIdx]->CompactCopy( CompactMaps, *Copy.UVLayers[UVIdx] );
    }
    SetNumNormalLayers( Copy.NumNormalLayers() );
    for ( int NormalLayerIndex = 0; NormalLayerIndex < NumNormalLayers(); NormalLayerIndex++ )
    {
        NormalLayers[NormalLayerIndex]->CompactCopy( CompactMaps, *Copy.NormalLayers[NormalLayerIndex] );
    }
    if ( Copy.ColorLayer )
    {
        EnablePrimaryColors();
        ColorLayer->CompactCopy( CompactMaps, *( Copy.ColorLayer ) );
    }
    else
    {
        DisablePrimaryColors();
    }
    if ( Copy.MaterialIDAttrib )
    {
        EnableMaterialID();
        MaterialIDAttrib->CompactCopy( CompactMaps, *( Copy.MaterialIDAttrib ) );
    }
    else
    {
        DisableMaterialID();
    }

    SetNumPolygroupLayers( Copy.NumPolygroupLayers() );
    for ( int GroupIdx = 0; GroupIdx < NumPolygroupLayers(); ++GroupIdx )
    {
        PolygroupLayers[GroupIdx]->CompactCopy( CompactMaps, *Copy.PolygroupLayers[GroupIdx] );
    }

    ResetRegisteredAttributes();
    GenericAttributes.clear();
    for ( const auto& AttribPair : Copy.GenericAttributes )
    {
        AttachAttribute( AttribPair.first, AttribPair.second->MakeCompactCopy( CompactMaps, ParentMesh ) );
    }
}

void DynamicMeshAttributeSet::Append( const DynamicMeshAttributeSet&  ToAppend,
                                      const DynamicMesh3::AppendInfo& AppendInfo )
{
    auto AppendHelper = [&AppendInfo]<typename T>( T& Target, const T* ToAppendLayer )
    {
        if ( ToAppendLayer )
        {
            Target.Append( *ToAppendLayer, AppendInfo );
        }
        else
        {
            Target.AppendDefaulted( AppendInfo );
        }
    };

    for ( int32_t Idx = 0; Idx < NumUVLayers(); ++Idx )
    {
        AppendHelper( *UVLayers[Idx], ToAppend.GetUVLayer( Idx ) );
    }
    for ( int32_t Idx = 0; Idx < NumNormalLayers(); ++Idx )
    {
        AppendHelper( *NormalLayers[Idx], ToAppend.GetNormalLayer( Idx ) );
    }
    if ( ColorLayer )
    {
        AppendHelper( *ColorLayer, ToAppend.ColorLayer.get() );
    }
    if ( MaterialIDAttrib )
    {
        AppendHelper( *MaterialIDAttrib, ToAppend.MaterialIDAttrib.get() );
    }
    for ( int Idx = 0; Idx < NumPolygroupLayers(); ++Idx )
    {
        AppendHelper( *PolygroupLayers[Idx],
                      Idx < ToAppend.NumPolygroupLayers() ? ToAppend.GetPolygroupLayer( Idx ) : nullptr );
    }
    for ( const auto& AttribPair : GenericAttributes )
    {
        const std::unique_ptr<FDynamicMeshAttributeBase>* AppendAttr =
             FindValue( ToAppend.GenericAttributes, AttribPair.first );
        FDynamicMeshAttributeBase& Target = *AttribPair.second;
        if ( AppendAttr && *AppendAttr )
        {
            Target.Append( **AppendAttr, AppendInfo );
        }
        else
        {
            Target.AppendDefaulted( AppendInfo );
        }
    }
}

void DynamicMeshAttributeSet::AppendDefaulted( const DynamicMesh3::AppendInfo& AppendInfo )
{
    for ( int32_t Idx = 0; Idx < NumUVLayers(); ++Idx )
    {
        UVLayers[Idx]->AppendDefaulted( AppendInfo );
    }
    for ( int32_t Idx = 0; Idx < NumNormalLayers(); ++Idx )
    {
        NormalLayers[Idx]->AppendDefaulted( AppendInfo );
    }
    if ( ColorLayer )
    {
        ColorLayer->AppendDefaulted( AppendInfo );
    }
    if ( MaterialIDAttrib )
    {
        MaterialIDAttrib->AppendDefaulted( AppendInfo );
    }
    for ( int Idx = 0; Idx < NumPolygroupLayers(); ++Idx )
    {
        PolygroupLayers[Idx]->AppendDefaulted( AppendInfo );
    }
    for ( const auto& AttribPair : GenericAttributes )
    {
        AttribPair.second->AppendDefaulted( AppendInfo );
    }
}

void DynamicMeshAttributeSet::CompactInPlace( const DynamicMeshCompactMaps& CompactMaps )
{
    for ( int UVIdx = 0; UVIdx < NumUVLayers(); UVIdx++ )
    {
        UVLayers[UVIdx]->CompactInPlace( CompactMaps );
    }
    for ( int NormalLayerIndex = 0; NormalLayerIndex < NumNormalLayers(); NormalLayerIndex++ )
    {
        NormalLayers[NormalLayerIndex]->CompactInPlace( CompactMaps );
    }
    if ( ColorLayer )
    {
        ColorLayer->CompactInPlace( CompactMaps );
    }
    if ( MaterialIDAttrib )
    {
        MaterialIDAttrib->CompactInPlace( CompactMaps );
    }
    for ( int GroupIdx = 0; GroupIdx < NumPolygroupLayers(); ++GroupIdx )
    {
        PolygroupLayers[GroupIdx]->CompactInPlace( CompactMaps );
    }
    for ( FDynamicMeshAttributeBase* RegAttrib : RegisteredAttributes )
    {
        RegAttrib->CompactInPlace( CompactMaps );
    }
}

void DynamicMeshAttributeSet::SplitAllBowties( bool bParallel )
{
    for ( int UVIdx = 0; UVIdx < NumUVLayers(); UVIdx++ )
    {
        UVLayers[UVIdx]->SplitBowties( bParallel );
    }
    for ( int NormalLayerIndex = 0; NormalLayerIndex < NumNormalLayers(); NormalLayerIndex++ )
    {
        NormalLayers[NormalLayerIndex]->SplitBowties( bParallel );
    }
    if ( ColorLayer )
    {
        ColorLayer->SplitBowties( bParallel );
    }
}

void DynamicMeshAttributeSet::EnableMatchingAttributes( const DynamicMeshAttributeSet& ToMatch,
                                                        bool bClearExisting, bool bDiscardExtraAttributes )
{
    const bool bUseToMatch = bClearExisting || bDiscardExtraAttributes;

    int32_t const ExistingUVLayers = NumUVLayers();
    int32_t const RequiredUVLayers =
         bUseToMatch ? ToMatch.NumUVLayers() : std::max( ExistingUVLayers, ToMatch.NumUVLayers() );
    SetNumUVLayers( RequiredUVLayers );
    for ( int32_t k = bClearExisting ? 0 : ExistingUVLayers; k < NumUVLayers(); k++ )
    {
        UVLayers[k]->ClearElements();
    }

    int32_t const ExistingNormalLayers = NumNormalLayers();
    int32_t const RequiredNormalLayers =
         bUseToMatch ? ToMatch.NumNormalLayers() : std::max( ExistingNormalLayers, ToMatch.NumNormalLayers() );
    SetNumNormalLayers( RequiredNormalLayers );
    for ( int32_t k = bClearExisting ? 0 : ExistingNormalLayers; k < NumNormalLayers(); k++ )
    {
        NormalLayers[k]->ClearElements();
    }

    bool bWantColorLayer =
         bUseToMatch ? ToMatch.HasPrimaryColors() : ( ToMatch.HasPrimaryColors() || this->HasPrimaryColors() );
    if ( bClearExisting || bWantColorLayer == false )
    {
        DisablePrimaryColors();
    }
    if ( bWantColorLayer )
    {
        EnablePrimaryColors();
    }

    bool bWantMaterialID =
         bUseToMatch ? ToMatch.HasMaterialID() : ( ToMatch.HasMaterialID() || this->HasMaterialID() );
    if ( bClearExisting || bWantMaterialID == false )
    {
        DisableMaterialID();
    }
    if ( bWantMaterialID )
    {
        EnableMaterialID();
    }

    int32_t const ExistingPolygroupLayers = NumPolygroupLayers();
    int32_t const RequiredPolygroupLayers =
         bUseToMatch ? ToMatch.NumPolygroupLayers()
                     : std::max( ExistingPolygroupLayers, ToMatch.NumPolygroupLayers() );
    SetNumPolygroupLayers( RequiredPolygroupLayers );
    for ( int32_t k = bClearExisting ? 0 : ExistingPolygroupLayers; k < NumPolygroupLayers(); k++ )
    {
        PolygroupLayers[k]->Initialize( static_cast<int32_t>( 0 ) );
        if ( k < ToMatch.NumPolygroupLayers() && PolygroupLayers[k]->GetName().empty() )
        {
            PolygroupLayers[k]->SetName( ToMatch.GetPolygroupLayer( k )->GetName() );
        }
    }

    if ( bClearExisting )
    {
        ResetRegisteredAttributes();
        GenericAttributes.clear();
    }
    else if ( bDiscardExtraAttributes )
    {
        std::vector<std::string> ToRemove;
        for ( const auto& AttribPair : GenericAttributes )
        {
            if ( !ToMatch.GenericAttributes.contains( AttribPair.first ) )
            {
                ToRemove.push_back( AttribPair.first );
            }
        }
        for ( const std::string& Name : ToRemove )
        {
            RemoveAttribute( Name );
        }
    }
    for ( const auto& AttribPair : ToMatch.GenericAttributes )
    {
        if ( !GenericAttributes.contains( AttribPair.first ) )
        {
            AttachAttribute( AttribPair.first, AttribPair.second->MakeNew( ParentMesh ) );
        }
    }
}

void DynamicMeshAttributeSet::Reparent( DynamicMesh3* NewParent )
{
    ParentMesh = NewParent;

    for ( int UVIdx = 0; UVIdx < NumUVLayers(); UVIdx++ )
    {
        UVLayers[UVIdx]->Reparent( NewParent );
    }
    for ( int NormalLayerIndex = 0; NormalLayerIndex < NumNormalLayers(); NormalLayerIndex++ )
    {
        NormalLayers[NormalLayerIndex]->Reparent( NewParent );
    }
    if ( ColorLayer )
    {
        ColorLayer->Reparent( NewParent );
    }
    if ( MaterialIDAttrib )
    {
        MaterialIDAttrib->Reparent( NewParent );
    }
    for ( int GroupIdx = 0; GroupIdx < NumPolygroupLayers(); ++GroupIdx )
    {
        PolygroupLayers[GroupIdx]->Reparent( NewParent );
    }
    for ( const auto& AttribPair : GenericAttributes )
    {
        AttribPair.second->Reparent( NewParent );
    }
}

void DynamicMeshAttributeSet::SetNumUVLayers( int Num )
{
    SetNumLayers( UVLayers, Num,
                  [this]()
                  {
                      auto NewUVLayer = std::make_unique<FDynamicMeshUVOverlay>( ParentMesh );
                      NewUVLayer->InitializeTriangles( ParentMesh->MaxTriangleID() );
                      return NewUVLayer;
                  } );
}

void DynamicMeshAttributeSet::EnableTangents()
{
    SetNumNormalLayers( 3 );
}

void DynamicMeshAttributeSet::DisableTangents()
{
    SetNumNormalLayers( 1 );
}

void DynamicMeshAttributeSet::SetNumNormalLayers( int Num )
{
    SetNumLayers( NormalLayers, Num,
                  [this]()
                  {
                      auto NewNormalLayer = std::make_unique<FDynamicMeshNormalOverlay>( ParentMesh );
                      NewNormalLayer->InitializeTriangles( ParentMesh->MaxTriangleID() );
                      return NewNormalLayer;
                  } );
}

void DynamicMeshAttributeSet::EnablePrimaryColors()
{
    if ( HasPrimaryColors() == false )
    {
        ColorLayer = std::make_unique<FDynamicMeshColorOverlay>( ParentMesh );
        ColorLayer->InitializeTriangles( ParentMesh->MaxTriangleID() );
    }
}

void DynamicMeshAttributeSet::DisablePrimaryColors()
{
    ColorLayer.reset();
}

int32_t DynamicMeshAttributeSet::NumPolygroupLayers() const
{
    return static_cast<int32_t>( PolygroupLayers.size() );
}

void DynamicMeshAttributeSet::SetNumPolygroupLayers( int32_t Num )
{
    SetNumLayers( PolygroupLayers, Num,
                  [this]() { return std::make_unique<DynamicMeshPolygroupAttribute>( ParentMesh ); } );
}

DynamicMeshPolygroupAttribute* DynamicMeshAttributeSet::GetPolygroupLayer( int Index )
{
    return PolygroupLayers[Index].get();
}

const DynamicMeshPolygroupAttribute* DynamicMeshAttributeSet::GetPolygroupLayer( int Index ) const
{
    return PolygroupLayers[Index].get();
}

void DynamicMeshAttributeSet::EnableMaterialID()
{
    if ( HasMaterialID() == false )
    {
        MaterialIDAttrib = std::make_unique<DynamicMeshMaterialAttribute>( ParentMesh );
        MaterialIDAttrib->Initialize( static_cast<int32_t>( 0 ) );
    }
}

void DynamicMeshAttributeSet::DisableMaterialID()
{
    MaterialIDAttrib.reset();
}

bool DynamicMeshAttributeSet::IsSeamEdge( int eid ) const
{
    for ( const auto& UVLayer : UVLayers )
    {
        if ( UVLayer->IsSeamEdge( eid ) )
        {
            return true;
        }
    }
    for ( const auto& NormalLayer : NormalLayers )
    {
        if ( NormalLayer->IsSeamEdge( eid ) )
        {
            return true;
        }
    }
    if ( ColorLayer && ColorLayer->IsSeamEdge( eid ) )
    {
        return true;
    }
    return false;
}

bool DynamicMeshAttributeSet::IsSeamEndEdge( int eid ) const
{
    for ( const auto& UVLayer : UVLayers )
    {
        if ( UVLayer->IsSeamEndEdge( eid ) )
        {
            return true;
        }
    }
    for ( const auto& NormalLayer : NormalLayers )
    {
        if ( NormalLayer->IsSeamEndEdge( eid ) )
        {
            return true;
        }
    }
    if ( ColorLayer && ColorLayer->IsSeamEndEdge( eid ) )
    {
        return true;
    }
    return false;
}

bool DynamicMeshAttributeSet::IsSeamEdge( int EdgeID, bool& bIsUVSeamOut, bool& bIsNormalSeamOut,
                                          bool& bIsColorSeamOut, bool& bIsTangentSeamOut ) const
{
    bIsUVSeamOut = false;
    for ( const auto& UVLayer : UVLayers )
    {
        bIsUVSeamOut = bIsUVSeamOut || UVLayer->IsSeamEdge( EdgeID );
    }
    bIsNormalSeamOut  = false;
    bIsTangentSeamOut = false;
    for ( int32_t Idx = 0; Idx < NumNormalLayers(); ++Idx )
    {
        const bool bSeam = NormalLayers[Idx]->IsSeamEdge( EdgeID );
        // normal layer 0 is the normals; layers 1 and 2 are the tangent frame
        ( Idx == 0 ? bIsNormalSeamOut : bIsTangentSeamOut ) =
             ( Idx == 0 ? bIsNormalSeamOut : bIsTangentSeamOut ) || bSeam;
    }
    bIsColorSeamOut = ColorLayer && ColorLayer->IsSeamEdge( EdgeID );
    return bIsUVSeamOut || bIsNormalSeamOut || bIsColorSeamOut || bIsTangentSeamOut;
}

bool DynamicMeshAttributeSet::IsSeamVertex( int VID, bool bBoundaryIsSeam ) const
{
    for ( const auto& UVLayer : UVLayers )
    {
        if ( UVLayer->IsSeamVertex( VID, bBoundaryIsSeam ) )
        {
            return true;
        }
    }
    for ( const auto& NormalLayer : NormalLayers )
    {
        if ( NormalLayer->IsSeamVertex( VID, bBoundaryIsSeam ) )
        {
            return true;
        }
    }
    if ( ColorLayer && ColorLayer->IsSeamVertex( VID, bBoundaryIsSeam ) )
    {
        return true;
    }
    return false;
}

bool DynamicMeshAttributeSet::IsSeamIntersectionVertex( int32_t VertexID ) const
{
    for ( const auto& UVLayer : UVLayers )
    {
        if ( UVLayer->IsSeamIntersectionVertex( VertexID ) )
        {
            return true;
        }
    }
    for ( const auto& NormalLayer : NormalLayers )
    {
        if ( NormalLayer->IsSeamIntersectionVertex( VertexID ) )
        {
            return true;
        }
    }
    if ( ColorLayer && ColorLayer->IsSeamIntersectionVertex( VertexID ) )
    {
        return true;
    }
    return false;
}

bool DynamicMeshAttributeSet::IsMaterialBoundaryEdge( int EdgeID ) const
{
    if ( MaterialIDAttrib == nullptr )
    {
        return false;
    }
    assert( ParentMesh->IsEdge( EdgeID ) );
    if ( ParentMesh->IsEdge( EdgeID ) && !ParentMesh->IsBoundaryEdge( EdgeID ) )
    {
        const Index2i  EdgeTris = ParentMesh->GetEdgeT( EdgeID );
        const int      MatA     = MaterialIDAttrib->GetValue( EdgeTris.A );
        const int      MatB     = MaterialIDAttrib->GetValue( EdgeTris.B );
        return MatA != MatB;
    }
    return false;
}

void DynamicMeshAttributeSet::OnNewVertex( int VertexID, bool bInserted )
{
    FDynamicMeshAttributeSetBase::OnNewVertex( VertexID, bInserted );
}

void DynamicMeshAttributeSet::OnRemoveVertex( int VertexID )
{
    FDynamicMeshAttributeSetBase::OnRemoveVertex( VertexID );
}

void DynamicMeshAttributeSet::OnNewTriangle( int TriangleID, bool bInserted )
{
    FDynamicMeshAttributeSetBase::OnNewTriangle( TriangleID, bInserted );

    for ( const auto& UVLayer : UVLayers )
    {
        UVLayer->InitializeNewTriangle( TriangleID );
    }
    for ( const auto& NormalLayer : NormalLayers )
    {
        NormalLayer->InitializeNewTriangle( TriangleID );
    }
    if ( ColorLayer )
    {
        ColorLayer->InitializeNewTriangle( TriangleID );
    }
    if ( MaterialIDAttrib )
    {
        int NewValue = 0;
        MaterialIDAttrib->SetNewValue( TriangleID, &NewValue );
    }
    for ( const auto& PolygroupLayer : PolygroupLayers )
    {
        int32_t const NewGroup = 0;
        PolygroupLayer->SetNewValue( TriangleID, &NewGroup );
    }
}

void DynamicMeshAttributeSet::OnRemoveTriangle( int TriangleID )
{
    FDynamicMeshAttributeSetBase::OnRemoveTriangle( TriangleID );

    for ( const auto& UVLayer : UVLayers )
    {
        UVLayer->OnRemoveTriangle( TriangleID );
    }
    for ( const auto& NormalLayer : NormalLayers )
    {
        NormalLayer->OnRemoveTriangle( TriangleID );
    }
    if ( ColorLayer )
    {
        ColorLayer->OnRemoveTriangle( TriangleID );
    }
    // material ID and polygroup attributes do not need to be updated when a triangle is removed
}

void DynamicMeshAttributeSet::OnReverseTriOrientation( int TriangleID )
{
    FDynamicMeshAttributeSetBase::OnReverseTriOrientation( TriangleID );

    for ( const auto& UVLayer : UVLayers )
    {
        UVLayer->OnReverseTriOrientation( TriangleID );
    }
    for ( const auto& NormalLayer : NormalLayers )
    {
        NormalLayer->OnReverseTriOrientation( TriangleID );
    }
    if ( ColorLayer )
    {
        ColorLayer->OnReverseTriOrientation( TriangleID );
    }
}

// The eight topology handlers below share one shape in UE: the registered (generic) attributes first, then every
// overlay, then the per-triangle attributes.
#define DESERT_ATTRIBUTE_SET_FORWARD( Handler, ... )                                                              \
    FDynamicMeshAttributeSetBase::Handler( __VA_ARGS__ );                                                         \
    for ( const auto& UVLayer : UVLayers )                                                                        \
    {                                                                                                             \
        UVLayer->Handler( __VA_ARGS__ );                                                                          \
    }                                                                                                             \
    for ( const auto& NormalLayer : NormalLayers )                                                                \
    {                                                                                                             \
        NormalLayer->Handler( __VA_ARGS__ );                                                                      \
    }                                                                                                             \
    if ( ColorLayer )                                                                                             \
    {                                                                                                             \
        ColorLayer->Handler( __VA_ARGS__ );                                                                       \
    }                                                                                                             \
    if ( MaterialIDAttrib )                                                                                       \
    {                                                                                                             \
        MaterialIDAttrib->Handler( __VA_ARGS__ );                                                                 \
    }                                                                                                             \
    for ( const auto& PolygroupLayer : PolygroupLayers )                                                          \
    {                                                                                                             \
        PolygroupLayer->Handler( __VA_ARGS__ );                                                                   \
    }

void DynamicMeshAttributeSet::OnSplitEdge( const DynamicMeshInfo::EdgeSplitInfo& SplitInfo )
{
    DESERT_ATTRIBUTE_SET_FORWARD( OnSplitEdge, SplitInfo )
}

void DynamicMeshAttributeSet::OnFlipEdge( const DynamicMeshInfo::EdgeFlipInfo& FlipInfo )
{
    DESERT_ATTRIBUTE_SET_FORWARD( OnFlipEdge, FlipInfo )
}

void DynamicMeshAttributeSet::OnCollapseEdge( const DynamicMeshInfo::EdgeCollapseInfo& CollapseInfo )
{
    DESERT_ATTRIBUTE_SET_FORWARD( OnCollapseEdge, CollapseInfo )
}

void DynamicMeshAttributeSet::OnPokeTriangle( const DynamicMeshInfo::PokeTriangleInfo& PokeInfo )
{
    DESERT_ATTRIBUTE_SET_FORWARD( OnPokeTriangle, PokeInfo )
}

void DynamicMeshAttributeSet::OnMergeEdges( const DynamicMeshInfo::MergeEdgesInfo& MergeInfo )
{
    DESERT_ATTRIBUTE_SET_FORWARD( OnMergeEdges, MergeInfo )
}

void DynamicMeshAttributeSet::OnMergeVertices( const DynamicMeshInfo::MergeVerticesInfo& MergeInfo )
{
    DESERT_ATTRIBUTE_SET_FORWARD( OnMergeVertices, MergeInfo )
}

void DynamicMeshAttributeSet::OnSplitVertex( const DynamicMeshInfo::VertexSplitInfo& SplitInfo,
                                             const std::span<const int>&             TrianglesToUpdate ){
     DESERT_ATTRIBUTE_SET_FORWARD( OnSplitVertex, SplitInfo, TrianglesToUpdate ) }

#undef DESERT_ATTRIBUTE_SET_FORWARD

size_t DynamicMeshAttributeSet::GetByteCount() const
{
    size_t ByteCount = 0;
    for ( const auto& UVLayer : UVLayers )
    {
        ByteCount += UVLayer->GetByteCount();
    }
    for ( const auto& NormalLayer : NormalLayers )
    {
        ByteCount += NormalLayer->GetByteCount();
    }
    if ( ColorLayer )
    {
        ByteCount += ColorLayer->GetByteCount();
    }
    if ( MaterialIDAttrib )
    {
        ByteCount += MaterialIDAttrib->GetByteCount();
    }
    for ( const auto& PolygroupLayer : PolygroupLayers )
    {
        ByteCount += PolygroupLayer->GetByteCount();
    }
    for ( const auto& AttribPair : GenericAttributes )
    {
        ByteCount += AttribPair.second->GetByteCount();
    }
    return ByteCount;
}

bool DynamicMeshAttributeSet::CheckValidity( bool bAllowNonmanifold, ValidityCheckFailMode FailMode ) const
{
    bool bValid = FDynamicMeshAttributeSetBase::CheckValidity( bAllowNonmanifold, FailMode );
    for ( int UVLayerIndex = 0; UVLayerIndex < NumUVLayers(); UVLayerIndex++ )
    {
        bValid = GetUVLayer( UVLayerIndex )->CheckValidity( bAllowNonmanifold, FailMode ) && bValid;
    }
    // UE checks PrimaryNormals() only; every normal layer (tangent frame included) is checked here
    for ( int NormalLayerIndex = 0; NormalLayerIndex < NumNormalLayers(); NormalLayerIndex++ )
    {
        bValid = GetNormalLayer( NormalLayerIndex )->CheckValidity( bAllowNonmanifold, FailMode ) && bValid;
    }
    if ( ColorLayer )
    {
        bValid = ColorLayer->CheckValidity( bAllowNonmanifold, FailMode ) && bValid;
    }
    if ( MaterialIDAttrib )
    {
        bValid = MaterialIDAttrib->CheckValidity( bAllowNonmanifold, FailMode ) && bValid;
    }
    for ( int PolygroupLayerIndex = 0; PolygroupLayerIndex < NumPolygroupLayers(); PolygroupLayerIndex++ )
    {
        bValid = GetPolygroupLayer( PolygroupLayerIndex )->CheckValidity( bAllowNonmanifold, FailMode ) && bValid;
    }
    return bValid;
}
