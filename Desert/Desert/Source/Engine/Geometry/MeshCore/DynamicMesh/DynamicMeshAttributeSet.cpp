// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/DynamicMeshAttributeSet.cpp:1-2478,
// adapted: UE Core as std/glm, namespace Desert::Geometry; only the layers the header ports (UV,
// normal/tangent, colour, MaterialID, polygroup layers, generic attributes) -- the
// weight/label/skin/morph/bone/sculpt branches of every function, IsSameAs, Serialize (1580-2227) and the bone
// helpers (2408-2478) are not ported; SplitAllBowties runs its layers serially.
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/MapLookup.hpp"
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

DynamicMeshAttributeSet::DynamicMeshAttributeSet( DynamicMesh3* Mesh ) : m_ParentMesh( Mesh )
{
    SetNumUVLayers( 1 );
    SetNumNormalLayers( 1 );
}

DynamicMeshAttributeSet::DynamicMeshAttributeSet( DynamicMesh3* Mesh, int32_t NumUVLayersIn,
                                                  int32_t NumNormalLayersIn )
     : m_ParentMesh( Mesh )
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
        m_UVLayers[UVIdx]->Copy( *Copy.m_UVLayers[UVIdx] );
    }
    SetNumNormalLayers( Copy.NumNormalLayers() );
    for ( int NormalLayerIndex = 0; NormalLayerIndex < NumNormalLayers(); NormalLayerIndex++ )
    {
        m_NormalLayers[NormalLayerIndex]->Copy( *Copy.m_NormalLayers[NormalLayerIndex] );
    }
    if ( Copy.m_ColorLayer )
    {
        EnablePrimaryColors();
        m_ColorLayer->Copy( *( Copy.m_ColorLayer ) );
    }
    else
    {
        DisablePrimaryColors();
    }
    if ( Copy.m_MaterialIDAttrib )
    {
        EnableMaterialID();
        m_MaterialIDAttrib->Copy( *( Copy.m_MaterialIDAttrib ) );
    }
    else
    {
        DisableMaterialID();
    }

    SetNumPolygroupLayers( Copy.NumPolygroupLayers() );
    for ( int GroupIdx = 0; GroupIdx < NumPolygroupLayers(); ++GroupIdx )
    {
        m_PolygroupLayers[GroupIdx]->Copy( *Copy.m_PolygroupLayers[GroupIdx] );
    }

    ResetRegisteredAttributes();
    m_GenericAttributes.clear();
    for ( const auto& AttribPair : Copy.m_GenericAttributes )
    {
        AttachAttribute( AttribPair.first, AttribPair.second->MakeCopy( m_ParentMesh ) );
    }
}

bool DynamicMeshAttributeSet::IsCompact() const
{
    for ( int UVIdx = 0; UVIdx < NumUVLayers(); UVIdx++ )
    {
        if ( !m_UVLayers[UVIdx]->IsCompact() )
        {
            return false;
        }
    }
    for ( int NormalLayerIndex = 0; NormalLayerIndex < NumNormalLayers(); NormalLayerIndex++ )
    {
        if ( !m_NormalLayers[NormalLayerIndex]->IsCompact() )
        {
            return false;
        }
    }
    if ( HasPrimaryColors() )
    {
        if ( !m_ColorLayer->IsCompact() )
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
        m_UVLayers[UVIdx]->CompactCopy( CompactMaps, *Copy.m_UVLayers[UVIdx] );
    }
    SetNumNormalLayers( Copy.NumNormalLayers() );
    for ( int NormalLayerIndex = 0; NormalLayerIndex < NumNormalLayers(); NormalLayerIndex++ )
    {
        m_NormalLayers[NormalLayerIndex]->CompactCopy( CompactMaps, *Copy.m_NormalLayers[NormalLayerIndex] );
    }
    if ( Copy.m_ColorLayer )
    {
        EnablePrimaryColors();
        m_ColorLayer->CompactCopy( CompactMaps, *( Copy.m_ColorLayer ) );
    }
    else
    {
        DisablePrimaryColors();
    }
    if ( Copy.m_MaterialIDAttrib )
    {
        EnableMaterialID();
        m_MaterialIDAttrib->CompactCopy( CompactMaps, *( Copy.m_MaterialIDAttrib ) );
    }
    else
    {
        DisableMaterialID();
    }

    SetNumPolygroupLayers( Copy.NumPolygroupLayers() );
    for ( int GroupIdx = 0; GroupIdx < NumPolygroupLayers(); ++GroupIdx )
    {
        m_PolygroupLayers[GroupIdx]->CompactCopy( CompactMaps, *Copy.m_PolygroupLayers[GroupIdx] );
    }

    ResetRegisteredAttributes();
    m_GenericAttributes.clear();
    for ( const auto& AttribPair : Copy.m_GenericAttributes )
    {
        AttachAttribute( AttribPair.first, AttribPair.second->MakeCompactCopy( CompactMaps, m_ParentMesh ) );
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
        AppendHelper( *m_UVLayers[Idx], ToAppend.GetUVLayer( Idx ) );
    }
    for ( int32_t Idx = 0; Idx < NumNormalLayers(); ++Idx )
    {
        AppendHelper( *m_NormalLayers[Idx], ToAppend.GetNormalLayer( Idx ) );
    }
    if ( m_ColorLayer )
    {
        AppendHelper( *m_ColorLayer, ToAppend.m_ColorLayer.get() );
    }
    if ( m_MaterialIDAttrib )
    {
        AppendHelper( *m_MaterialIDAttrib, ToAppend.m_MaterialIDAttrib.get() );
    }
    for ( int Idx = 0; Idx < NumPolygroupLayers(); ++Idx )
    {
        AppendHelper( *m_PolygroupLayers[Idx],
                      Idx < ToAppend.NumPolygroupLayers() ? ToAppend.GetPolygroupLayer( Idx ) : nullptr );
    }
    for ( const auto& AttribPair : m_GenericAttributes )
    {
        const std::unique_ptr<DynamicMeshAttributeBase>* AppendAttr =
             FindValue( ToAppend.m_GenericAttributes, AttribPair.first );
        DynamicMeshAttributeBase& Target = *AttribPair.second;
        if ( ( AppendAttr != nullptr ) && *AppendAttr )
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
        m_UVLayers[Idx]->AppendDefaulted( AppendInfo );
    }
    for ( int32_t Idx = 0; Idx < NumNormalLayers(); ++Idx )
    {
        m_NormalLayers[Idx]->AppendDefaulted( AppendInfo );
    }
    if ( m_ColorLayer )
    {
        m_ColorLayer->AppendDefaulted( AppendInfo );
    }
    if ( m_MaterialIDAttrib )
    {
        m_MaterialIDAttrib->AppendDefaulted( AppendInfo );
    }
    for ( int Idx = 0; Idx < NumPolygroupLayers(); ++Idx )
    {
        m_PolygroupLayers[Idx]->AppendDefaulted( AppendInfo );
    }
    for ( const auto& AttribPair : m_GenericAttributes )
    {
        AttribPair.second->AppendDefaulted( AppendInfo );
    }
}

void DynamicMeshAttributeSet::CompactInPlace( const DynamicMeshCompactMaps& CompactMaps )
{
    for ( int UVIdx = 0; UVIdx < NumUVLayers(); UVIdx++ )
    {
        m_UVLayers[UVIdx]->CompactInPlace( CompactMaps );
    }
    for ( int NormalLayerIndex = 0; NormalLayerIndex < NumNormalLayers(); NormalLayerIndex++ )
    {
        m_NormalLayers[NormalLayerIndex]->CompactInPlace( CompactMaps );
    }
    if ( m_ColorLayer )
    {
        m_ColorLayer->CompactInPlace( CompactMaps );
    }
    if ( m_MaterialIDAttrib )
    {
        m_MaterialIDAttrib->CompactInPlace( CompactMaps );
    }
    for ( int GroupIdx = 0; GroupIdx < NumPolygroupLayers(); ++GroupIdx )
    {
        m_PolygroupLayers[GroupIdx]->CompactInPlace( CompactMaps );
    }
    for ( DynamicMeshAttributeBase* RegAttrib : m_RegisteredAttributes )
    {
        RegAttrib->CompactInPlace( CompactMaps );
    }
}

void DynamicMeshAttributeSet::SplitAllBowties( bool bParallel )
{
    for ( int UVIdx = 0; UVIdx < NumUVLayers(); UVIdx++ )
    {
        m_UVLayers[UVIdx]->SplitBowties( bParallel );
    }
    for ( int NormalLayerIndex = 0; NormalLayerIndex < NumNormalLayers(); NormalLayerIndex++ )
    {
        m_NormalLayers[NormalLayerIndex]->SplitBowties( bParallel );
    }
    if ( m_ColorLayer )
    {
        m_ColorLayer->SplitBowties( bParallel );
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
        m_UVLayers[k]->ClearElements();
    }

    int32_t const ExistingNormalLayers = NumNormalLayers();
    int32_t const RequiredNormalLayers =
         bUseToMatch ? ToMatch.NumNormalLayers() : std::max( ExistingNormalLayers, ToMatch.NumNormalLayers() );
    SetNumNormalLayers( RequiredNormalLayers );
    for ( int32_t k = bClearExisting ? 0 : ExistingNormalLayers; k < NumNormalLayers(); k++ )
    {
        m_NormalLayers[k]->ClearElements();
    }

    const bool bWantColorLayer =
         bUseToMatch ? ToMatch.HasPrimaryColors() : ( ToMatch.HasPrimaryColors() || this->HasPrimaryColors() );
    if ( bClearExisting || !bWantColorLayer )
    {
        DisablePrimaryColors();
    }
    if ( bWantColorLayer )
    {
        EnablePrimaryColors();
    }

    const bool bWantMaterialID =
         bUseToMatch ? ToMatch.HasMaterialID() : ( ToMatch.HasMaterialID() || this->HasMaterialID() );
    if ( bClearExisting || !bWantMaterialID )
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
        m_PolygroupLayers[k]->Initialize( static_cast<int32_t>( 0 ) );
        if ( k < ToMatch.NumPolygroupLayers() && m_PolygroupLayers[k]->GetName().empty() )
        {
            m_PolygroupLayers[k]->SetName( ToMatch.GetPolygroupLayer( k )->GetName() );
        }
    }

    if ( bClearExisting )
    {
        ResetRegisteredAttributes();
        m_GenericAttributes.clear();
    }
    else if ( bDiscardExtraAttributes )
    {
        std::vector<std::string> ToRemove;
        for ( const auto& AttribPair : m_GenericAttributes )
        {
            if ( !ToMatch.m_GenericAttributes.contains( AttribPair.first ) )
            {
                ToRemove.push_back( AttribPair.first );
            }
        }
        for ( const std::string& Name : ToRemove )
        {
            RemoveAttribute( Name );
        }
    }
    for ( const auto& AttribPair : ToMatch.m_GenericAttributes )
    {
        if ( !m_GenericAttributes.contains( AttribPair.first ) )
        {
            AttachAttribute( AttribPair.first, AttribPair.second->MakeNew( m_ParentMesh ) );
        }
    }
}

void DynamicMeshAttributeSet::Reparent( DynamicMesh3* NewParent )
{
    m_ParentMesh = NewParent;

    for ( int UVIdx = 0; UVIdx < NumUVLayers(); UVIdx++ )
    {
        m_UVLayers[UVIdx]->Reparent( NewParent );
    }
    for ( int NormalLayerIndex = 0; NormalLayerIndex < NumNormalLayers(); NormalLayerIndex++ )
    {
        m_NormalLayers[NormalLayerIndex]->Reparent( NewParent );
    }
    if ( m_ColorLayer )
    {
        m_ColorLayer->Reparent( NewParent );
    }
    if ( m_MaterialIDAttrib )
    {
        m_MaterialIDAttrib->Reparent( NewParent );
    }
    for ( int GroupIdx = 0; GroupIdx < NumPolygroupLayers(); ++GroupIdx )
    {
        m_PolygroupLayers[GroupIdx]->Reparent( NewParent );
    }
    for ( const auto& AttribPair : m_GenericAttributes )
    {
        AttribPair.second->Reparent( NewParent );
    }
}

void DynamicMeshAttributeSet::SetNumUVLayers( int Num )
{
    SetNumLayers( m_UVLayers, Num,
                  [this]()
                  {
                      auto NewUVLayer = std::make_unique<DynamicMeshUVOverlay>( m_ParentMesh );
                      NewUVLayer->InitializeTriangles( m_ParentMesh->MaxTriangleID() );
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
    SetNumLayers( m_NormalLayers, Num,
                  [this]()
                  {
                      auto NewNormalLayer = std::make_unique<DynamicMeshNormalOverlay>( m_ParentMesh );
                      NewNormalLayer->InitializeTriangles( m_ParentMesh->MaxTriangleID() );
                      return NewNormalLayer;
                  } );
}

void DynamicMeshAttributeSet::EnablePrimaryColors()
{
    if ( !HasPrimaryColors() )
    {
        m_ColorLayer = std::make_unique<DynamicMeshColorOverlay>( m_ParentMesh );
        m_ColorLayer->InitializeTriangles( m_ParentMesh->MaxTriangleID() );
    }
}

void DynamicMeshAttributeSet::DisablePrimaryColors()
{
    m_ColorLayer.reset();
}

int32_t DynamicMeshAttributeSet::NumPolygroupLayers() const
{
    return static_cast<int32_t>( m_PolygroupLayers.size() );
}

void DynamicMeshAttributeSet::SetNumPolygroupLayers( int32_t Num )
{
    SetNumLayers( m_PolygroupLayers, Num,
                  [this]() { return std::make_unique<DynamicMeshPolygroupAttribute>( m_ParentMesh ); } );
}

DynamicMeshPolygroupAttribute* DynamicMeshAttributeSet::GetPolygroupLayer( int Index )
{
    return m_PolygroupLayers[Index].get();
}

const DynamicMeshPolygroupAttribute* DynamicMeshAttributeSet::GetPolygroupLayer( int Index ) const
{
    return m_PolygroupLayers[Index].get();
}

void DynamicMeshAttributeSet::EnableMaterialID()
{
    if ( !HasMaterialID() )
    {
        m_MaterialIDAttrib = std::make_unique<DynamicMeshMaterialAttribute>( m_ParentMesh );
        m_MaterialIDAttrib->Initialize( static_cast<int32_t>( 0 ) );
    }
}

void DynamicMeshAttributeSet::DisableMaterialID()
{
    m_MaterialIDAttrib.reset();
}

bool DynamicMeshAttributeSet::IsSeamEdge( int eid ) const
{
    for ( const auto& UVLayer : m_UVLayers )
    {
        if ( UVLayer->IsSeamEdge( eid ) )
        {
            return true;
        }
    }
    for ( const auto& NormalLayer : m_NormalLayers )
    {
        if ( NormalLayer->IsSeamEdge( eid ) )
        {
            return true;
        }
    }
    return m_ColorLayer && m_ColorLayer->IsSeamEdge( eid );
}

bool DynamicMeshAttributeSet::IsSeamEndEdge( int eid ) const
{
    for ( const auto& UVLayer : m_UVLayers )
    {
        if ( UVLayer->IsSeamEndEdge( eid ) )
        {
            return true;
        }
    }
    for ( const auto& NormalLayer : m_NormalLayers )
    {
        if ( NormalLayer->IsSeamEndEdge( eid ) )
        {
            return true;
        }
    }
    return m_ColorLayer && m_ColorLayer->IsSeamEndEdge( eid );
}

bool DynamicMeshAttributeSet::IsSeamEdge( int EdgeID, bool& bIsUVSeamOut, bool& bIsNormalSeamOut,
                                          bool& bIsColorSeamOut, bool& bIsTangentSeamOut ) const
{
    bIsUVSeamOut = false;
    for ( const auto& UVLayer : m_UVLayers )
    {
        bIsUVSeamOut = bIsUVSeamOut || UVLayer->IsSeamEdge( EdgeID );
    }
    bIsNormalSeamOut  = false;
    bIsTangentSeamOut = false;
    for ( int32_t Idx = 0; Idx < NumNormalLayers(); ++Idx )
    {
        const bool bSeam = m_NormalLayers[Idx]->IsSeamEdge( EdgeID );
        // normal layer 0 is the normals; layers 1 and 2 are the tangent frame
        ( Idx == 0 ? bIsNormalSeamOut : bIsTangentSeamOut ) =
             ( Idx == 0 ? bIsNormalSeamOut : bIsTangentSeamOut ) || bSeam;
    }
    bIsColorSeamOut = m_ColorLayer && m_ColorLayer->IsSeamEdge( EdgeID );
    return bIsUVSeamOut || bIsNormalSeamOut || bIsColorSeamOut || bIsTangentSeamOut;
}

bool DynamicMeshAttributeSet::IsSeamVertex( int VID, bool bBoundaryIsSeam ) const
{
    for ( const auto& UVLayer : m_UVLayers )
    {
        if ( UVLayer->IsSeamVertex( VID, bBoundaryIsSeam ) )
        {
            return true;
        }
    }
    for ( const auto& NormalLayer : m_NormalLayers )
    {
        if ( NormalLayer->IsSeamVertex( VID, bBoundaryIsSeam ) )
        {
            return true;
        }
    }
    return m_ColorLayer && m_ColorLayer->IsSeamVertex( VID, bBoundaryIsSeam );
}

bool DynamicMeshAttributeSet::IsSeamIntersectionVertex( int32_t VertexID ) const
{
    for ( const auto& UVLayer : m_UVLayers )
    {
        if ( UVLayer->IsSeamIntersectionVertex( VertexID ) )
        {
            return true;
        }
    }
    for ( const auto& NormalLayer : m_NormalLayers )
    {
        if ( NormalLayer->IsSeamIntersectionVertex( VertexID ) )
        {
            return true;
        }
    }
    return m_ColorLayer && m_ColorLayer->IsSeamIntersectionVertex( VertexID );
}

bool DynamicMeshAttributeSet::IsMaterialBoundaryEdge( int EdgeID ) const
{
    if ( m_MaterialIDAttrib == nullptr )
    {
        return false;
    }
    assert( m_ParentMesh->IsEdge( EdgeID ) );
    if ( m_ParentMesh->IsEdge( EdgeID ) && !m_ParentMesh->IsBoundaryEdge( EdgeID ) )
    {
        const Index2i EdgeTris = m_ParentMesh->GetEdgeT( EdgeID );
        const int     MatA     = m_MaterialIDAttrib->GetValue( EdgeTris.A );
        const int     MatB     = m_MaterialIDAttrib->GetValue( EdgeTris.B );
        return MatA != MatB;
    }
    return false;
}

void DynamicMeshAttributeSet::OnNewVertex( int VertexID, bool bInserted )
{
    DynamicMeshAttributeSetBase::OnNewVertex( VertexID, bInserted );
}

void DynamicMeshAttributeSet::OnRemoveVertex( int VertexID )
{
    DynamicMeshAttributeSetBase::OnRemoveVertex( VertexID );
}

void DynamicMeshAttributeSet::OnNewTriangle( int TriangleID, bool bInserted )
{
    DynamicMeshAttributeSetBase::OnNewTriangle( TriangleID, bInserted );

    for ( const auto& UVLayer : m_UVLayers )
    {
        UVLayer->InitializeNewTriangle( TriangleID );
    }
    for ( const auto& NormalLayer : m_NormalLayers )
    {
        NormalLayer->InitializeNewTriangle( TriangleID );
    }
    if ( m_ColorLayer )
    {
        m_ColorLayer->InitializeNewTriangle( TriangleID );
    }
    if ( m_MaterialIDAttrib )
    {
        const int NewValue = 0;
        m_MaterialIDAttrib->SetNewValue( TriangleID, &NewValue );
    }
    for ( const auto& PolygroupLayer : m_PolygroupLayers )
    {
        int32_t const NewGroup = 0;
        PolygroupLayer->SetNewValue( TriangleID, &NewGroup );
    }
}

void DynamicMeshAttributeSet::OnRemoveTriangle( int TriangleID )
{
    DynamicMeshAttributeSetBase::OnRemoveTriangle( TriangleID );

    for ( const auto& UVLayer : m_UVLayers )
    {
        UVLayer->OnRemoveTriangle( TriangleID );
    }
    for ( const auto& NormalLayer : m_NormalLayers )
    {
        NormalLayer->OnRemoveTriangle( TriangleID );
    }
    if ( m_ColorLayer )
    {
        m_ColorLayer->OnRemoveTriangle( TriangleID );
    }
    // material ID and polygroup attributes do not need to be updated when a triangle is removed
}

void DynamicMeshAttributeSet::OnReverseTriOrientation( int TriangleID )
{
    DynamicMeshAttributeSetBase::OnReverseTriOrientation( TriangleID );

    for ( const auto& UVLayer : m_UVLayers )
    {
        UVLayer->OnReverseTriOrientation( TriangleID );
    }
    for ( const auto& NormalLayer : m_NormalLayers )
    {
        NormalLayer->OnReverseTriOrientation( TriangleID );
    }
    if ( m_ColorLayer )
    {
        m_ColorLayer->OnReverseTriOrientation( TriangleID );
    }
}

// The eight topology handlers below share one shape in UE: the registered (generic) attributes first, then every
// overlay, then the per-triangle attributes.
#define DESERT_ATTRIBUTE_SET_FORWARD( Handler, ... )                                                              \
    DynamicMeshAttributeSetBase::Handler( __VA_ARGS__ );                                                          \
    for ( const auto& UVLayer : m_UVLayers )                                                                      \
    {                                                                                                             \
        UVLayer->Handler( __VA_ARGS__ );                                                                          \
    }                                                                                                             \
    for ( const auto& NormalLayer : m_NormalLayers )                                                              \
    {                                                                                                             \
        NormalLayer->Handler( __VA_ARGS__ );                                                                      \
    }                                                                                                             \
    if ( m_ColorLayer )                                                                                           \
    {                                                                                                             \
        m_ColorLayer->Handler( __VA_ARGS__ );                                                                     \
    }                                                                                                             \
    if ( m_MaterialIDAttrib )                                                                                     \
    {                                                                                                             \
        m_MaterialIDAttrib->Handler( __VA_ARGS__ );                                                               \
    }                                                                                                             \
    for ( const auto& PolygroupLayer : m_PolygroupLayers )                                                        \
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
    for ( const auto& UVLayer : m_UVLayers )
    {
        ByteCount += UVLayer->GetByteCount();
    }
    for ( const auto& NormalLayer : m_NormalLayers )
    {
        ByteCount += NormalLayer->GetByteCount();
    }
    if ( m_ColorLayer )
    {
        ByteCount += m_ColorLayer->GetByteCount();
    }
    if ( m_MaterialIDAttrib )
    {
        ByteCount += m_MaterialIDAttrib->GetByteCount();
    }
    for ( const auto& PolygroupLayer : m_PolygroupLayers )
    {
        ByteCount += PolygroupLayer->GetByteCount();
    }
    for ( const auto& AttribPair : m_GenericAttributes )
    {
        ByteCount += AttribPair.second->GetByteCount();
    }
    return ByteCount;
}

bool DynamicMeshAttributeSet::CheckValidity( bool bAllowNonmanifold, ValidityCheckFailMode FailMode ) const
{
    bool bValid = DynamicMeshAttributeSetBase::CheckValidity( bAllowNonmanifold, FailMode );
    for ( int UVLayerIndex = 0; UVLayerIndex < NumUVLayers(); UVLayerIndex++ )
    {
        bValid = GetUVLayer( UVLayerIndex )->CheckValidity( bAllowNonmanifold, FailMode ) && bValid;
    }
    // UE checks PrimaryNormals() only; every normal layer (tangent frame included) is checked here
    for ( int NormalLayerIndex = 0; NormalLayerIndex < NumNormalLayers(); NormalLayerIndex++ )
    {
        bValid = GetNormalLayer( NormalLayerIndex )->CheckValidity( bAllowNonmanifold, FailMode ) && bValid;
    }
    if ( m_ColorLayer )
    {
        bValid = m_ColorLayer->CheckValidity( bAllowNonmanifold, FailMode ) && bValid;
    }
    if ( m_MaterialIDAttrib )
    {
        bValid = m_MaterialIDAttrib->CheckValidity( bAllowNonmanifold, FailMode ) && bValid;
    }
    for ( int PolygroupLayerIndex = 0; PolygroupLayerIndex < NumPolygroupLayers(); PolygroupLayerIndex++ )
    {
        bValid = GetPolygroupLayer( PolygroupLayerIndex )->CheckValidity( bAllowNonmanifold, FailMode ) && bValid;
    }
    return bValid;
}
