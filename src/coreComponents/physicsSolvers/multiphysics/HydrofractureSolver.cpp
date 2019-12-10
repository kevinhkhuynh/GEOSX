/*
 * ------------------------------------------------------------------------------------------------------------
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (c) 2018-2019 Lawrence Livermore National Security LLC
 * Copyright (c) 2018-2019 The Board of Trustees of the Leland Stanford Junior University
 * Copyright (c) 2018-2019 Total, S.A
 * Copyright (c) 2019-     GEOSX Contributors
 * All right reserved
 *
 * See top level LICENSE, COPYRIGHT, CONTRIBUTORS, NOTICE, and ACKNOWLEDGEMENTS files for details.
 * ------------------------------------------------------------------------------------------------------------
 */

/**
 * @file HydrofractureSolver.cpp
 *
 */


#include "HydrofractureSolver.hpp"

#include "common/TimingMacros.hpp"
#include "constitutive/ConstitutiveManager.hpp"
#include "constitutive/contact/ContactRelationBase.hpp"
#include "constitutive/fluid/SingleFluidBase.hpp"
#include "finiteElement/Kinematics.h"
#include "finiteVolume/FiniteVolumeManager.hpp"
#include "finiteVolume/FluxApproximationBase.hpp"
#include "managers/DomainPartition.hpp"
#include "managers/FieldSpecification/FieldSpecificationManager.hpp"
#include "managers/NumericalMethodsManager.hpp"
#include "mesh/FaceElementRegion.hpp"
#include "mesh/MeshForLoopInterface.hpp"
#include "meshUtilities/ComputationalGeometry.hpp"
#include "physicsSolvers/fluidFlow/FlowSolverBase.hpp"
#include "physicsSolvers/solidMechanics/SolidMechanicsLagrangianFEM.hpp"
#include "rajaInterface/GEOS_RAJA_Interface.hpp"
#include "linearAlgebra/utilities/LAIHelperFunctions.hpp"


namespace geosx
{

using namespace dataRepository;
using namespace constitutive;

HydrofractureSolver::HydrofractureSolver( const std::string& name,
                                      Group * const parent ):
  SolverBase(name,parent),
  m_solidSolverName(),
  m_flowSolverName(),
  m_couplingTypeOptionString("FixedStress"),
  m_couplingTypeOption(),
  m_solidSolver(nullptr),
  m_flowSolver(nullptr),
  m_maxNumResolves(10)
{
  registerWrapper(viewKeyStruct::solidSolverNameString, &m_solidSolverName, 0)->
    setInputFlag(InputFlags::REQUIRED)->
    setDescription("Name of the solid mechanics solver to use in the poroelastic solver");

  registerWrapper(viewKeyStruct::fluidSolverNameString, &m_flowSolverName, 0)->
    setInputFlag(InputFlags::REQUIRED)->
    setDescription("Name of the fluid mechanics solver to use in the poroelastic solver");

  registerWrapper(viewKeyStruct::couplingTypeOptionStringString, &m_couplingTypeOptionString, 0)->
    setInputFlag(InputFlags::REQUIRED)->
    setDescription("Coupling option: (FixedStress, TightlyCoupled)");

  registerWrapper(viewKeyStruct::contactRelationNameString, &m_contactRelationName, 0)->
    setInputFlag(InputFlags::REQUIRED)->
    setDescription("Name of contact relation to enforce constraints on fracture boundary.");

  registerWrapper(viewKeyStruct::maxNumResolvesString, &m_maxNumResolves, 0)->
    setApplyDefaultValue(10)->
    setInputFlag(InputFlags::OPTIONAL)->
    setDescription("Value to indicate how many resolves may be executed to perform surface generation after the execution of flow and mechanics solver. ");
}

void HydrofractureSolver::RegisterDataOnMesh( dataRepository::Group * const GEOSX_UNUSED_ARG( MeshBodies ) )
{

}

void HydrofractureSolver::ImplicitStepSetup( real64 const & time_n,
                                             real64 const & dt,
                                             DomainPartition * const domain,
                                             DofManager & GEOSX_UNUSED_ARG( dofManager ),
                                             ParallelMatrix & GEOSX_UNUSED_ARG( matrix ),
                                             ParallelVector & GEOSX_UNUSED_ARG( rhs ),
                                             ParallelVector & GEOSX_UNUSED_ARG( solution ) )
{
  m_solidSolver = this->getParent()->GetGroup<SolidMechanicsLagrangianFEM>(m_solidSolverName);
  m_flowSolver = this->getParent()->GetGroup<FlowSolverBase>(m_flowSolverName);

  m_solidSolver->ImplicitStepSetup( time_n, dt, domain,
                                    m_solidSolver->getDofManager(),
                                    m_solidSolver->getSystemMatrix(),
                                    m_solidSolver->getSystemRhs(),
                                    m_solidSolver->getSystemSolution() );

  m_flowSolver->ImplicitStepSetup( time_n, dt, domain,
                                   m_flowSolver->getDofManager(),
                                   m_flowSolver->getSystemMatrix(),
                                   m_flowSolver->getSystemRhs(),
                                   m_flowSolver->getSystemSolution() );
}

void HydrofractureSolver::ImplicitStepComplete( real64 const& time_n,
                                                real64 const& dt,
                                                DomainPartition * const domain)
{
  m_flowSolver->ImplicitStepComplete( time_n, dt, domain );
  m_solidSolver->ImplicitStepComplete( time_n, dt, domain );
}

void HydrofractureSolver::PostProcessInput()
{
  string ctOption = this->getReference<string>(viewKeyStruct::couplingTypeOptionStringString);

  if( ctOption == "FixedStress" )
  {
    this->m_couplingTypeOption = couplingTypeOption::FixedStress;
  }
  else if( ctOption == "TightlyCoupled" )
  {
    this->m_couplingTypeOption = couplingTypeOption::TightlyCoupled;
  }
  else
  {
    GEOSX_ERROR("invalid coupling type option");
  }
}

void HydrofractureSolver::InitializePostInitialConditions_PreSubGroups(Group * const GEOSX_UNUSED_ARG( problemManager ) )
{

}

HydrofractureSolver::~HydrofractureSolver()
{
  // TODO Auto-generated destructor stub
}

void HydrofractureSolver::ResetStateToBeginningOfStep( DomainPartition * const domain )
{
  m_flowSolver->ResetStateToBeginningOfStep(domain);
  m_solidSolver->ResetStateToBeginningOfStep(domain);
}

real64 HydrofractureSolver::SolverStep( real64 const & time_n,
                                        real64 const & dt,
                                        int const cycleNumber,
                                        DomainPartition * const domain )
{
  real64 dtReturn = dt;

  SolverBase * const surfaceGenerator =  this->getParent()->GetGroup<SolverBase>("SurfaceGen");

  if( m_couplingTypeOption == couplingTypeOption::FixedStress )
  {
    dtReturn = SplitOperatorStep( time_n, dt, cycleNumber, domain->group_cast<DomainPartition*>() );
  }
  else if( m_couplingTypeOption == couplingTypeOption::TightlyCoupled )
  {

    ImplicitStepSetup( time_n,
                       dt,
                       domain,
                       m_dofManager,
                       m_matrix,
                       m_rhs,
                       m_solution );

    int const maxIter = m_maxNumResolves + 1;
    for( int solveIter=0 ; solveIter<maxIter ; ++solveIter )
    {
      int locallyFractured = 0;
      int globallyFractured = 0;

      SetupSystem( domain,
                   m_dofManager,
                   m_matrix,
                   m_rhs,
                   m_solution  );

      if( solveIter>0 )
      {
        m_solidSolver->ResetStressToBeginningOfStep( domain );
      }

      // currently the only method is implicit time integration
      dtReturn = this->NonlinearImplicitStep( time_n,
                                              dt,
                                              cycleNumber,
                                              domain,
                                              m_dofManager,
                                              m_matrix,
                                              m_rhs,
                                              m_solution );

      m_solidSolver->updateStress( domain );

      if( surfaceGenerator!=nullptr )
      {
        if( surfaceGenerator->SolverStep( time_n, dt, cycleNumber, domain ) > 0 )
        {
          locallyFractured = 1;
        }
        MpiWrapper::allReduce( &locallyFractured,
                               &globallyFractured,
                               1,
                               MPI_MAX,
                               MPI_COMM_GEOSX );
      }
      if( globallyFractured == 0 )
      {
        break;
      }
      else
      {
        if( getLogLevel() >= 1 )
        {
          GEOSX_LOG_RANK_0("++ Fracture propagation. Re-entering Newton Solve.");
        }
      }
    }

    // final step for completion of timestep. typically secondary variable updates and cleanup.
    ImplicitStepComplete( time_n, dtReturn, domain );
  }
  return dtReturn;
}

void HydrofractureSolver::UpdateDeformationForCoupling( DomainPartition * const domain )
{
  MeshLevel * const meshLevel = domain->getMeshBody(0)->getMeshLevel(0);
  ElementRegionManager * const elemManager = meshLevel->getElemManager();
  NodeManager * const nodeManager = meshLevel->getNodeManager();
  FaceManager * const faceManager = meshLevel->getFaceManager();

  arrayView1d<R1Tensor> const & u = nodeManager->getReference< array1d<R1Tensor> >( keys::TotalDisplacement );
  arrayView1d<R1Tensor const> const & faceNormal = faceManager->faceNormal();
  // arrayView1d<real64 const> const & faceArea = faceManager->faceArea();
  ArrayOfArraysView< localIndex const > const & faceToNodeMap = faceManager->nodeList();

  ConstitutiveManager const * const
  constitutiveManager = domain->GetGroup<ConstitutiveManager>(keys::ConstitutiveManager);

  ContactRelationBase const * const
  contactRelation = constitutiveManager->GetGroup<ContactRelationBase>(m_contactRelationName);

  elemManager->forElementRegions<FaceElementRegion>([&]( FaceElementRegion * const faceElemRegion )
  {
    faceElemRegion->forElementSubRegions<FaceElementSubRegion>([&]( FaceElementSubRegion * const subRegion )
    {
      arrayView1d<real64> const & aperture = subRegion->getElementAperture();
      arrayView1d<real64> const & volume = subRegion->getElementVolume();
      arrayView1d<real64> const & deltaVolume = subRegion->getReference<array1d<real64> >(FlowSolverBase::viewKeyStruct::deltaVolumeString);
      arrayView1d<real64 const> const & area = subRegion->getElementArea();
      arrayView2d< localIndex const > const & elemsToFaces = subRegion->faceList();

      for( localIndex kfe=0 ; kfe<subRegion->size() ; ++kfe )
      {
        localIndex const kf0 = elemsToFaces[kfe][0];
        localIndex const kf1 = elemsToFaces[kfe][1];
        localIndex const numNodesPerFace = faceToNodeMap.sizeOfArray(kf0);
        R1Tensor temp;
        for( localIndex a=0 ; a<numNodesPerFace ; ++a )
        {
          temp += u[faceToNodeMap(kf0, a)];
          temp -= u[faceToNodeMap(kf1, a)];
        }
        //area[kfe] = faceArea[kf0];

        // TODO this needs a proper contact based strategy for aperture
        aperture[kfe] = -Dot(temp,faceNormal[kf0]) / numNodesPerFace;
        aperture[kfe] = contactRelation->effectiveAperture( aperture[kfe] );

        deltaVolume[kfe] = aperture[kfe] * area[kfe] - volume[kfe];
      }

    });
  });
}

real64 HydrofractureSolver::SplitOperatorStep( real64 const & GEOSX_UNUSED_ARG( time_n ),
                                               real64 const & dt,
                                               integer const GEOSX_UNUSED_ARG( cycleNumber ),
                                               DomainPartition * const GEOSX_UNUSED_ARG( domain ) )
{
  real64 dtReturn = dt;
//  real64 dtReturnTemporary = dtReturn;
//
//  m_flowSolver->ImplicitStepSetup( time_n, dt, domain, getLinearSystemRepository() );
//  m_solidSolver->ImplicitStepSetup( time_n, dt, domain, getLinearSystemRepository() );
//  this->ImplicitStepSetup( time_n, dt, domain, getLinearSystemRepository() );
//
//
//
//  fluidSolver.ImplicitStepSetup( time_n, dt, domain,
//                                 fluidSolver.getDofManager(),
//                                 fluidSolver.getSystemMatrix(),
//                                 fluidSolver.getSystemRhs(),
//                                 fluidSolver.getSystemSolution() );
//
//  solidSolver.ImplicitStepSetup( time_n, dt, domain,
//                                 solidSolver.getDofManager(),
//                                 solidSolver.getSystemMatrix(),
//                                 solidSolver.getSystemRhs(),
//                                 solidSolver.getSystemSolution() );
//
//  this->UpdateDeformationForCoupling(domain);
//
//  int iter = 0;
//  while (iter < solverParams->maxIterNewton() )
//  {
//    if (iter == 0)
//    {
//      // reset the states of all slave solvers if any of them has been reset
//      m_flowSolver->ResetStateToBeginningOfStep( domain );
//      m_solidSolver->ResetStateToBeginningOfStep( domain );
//      ResetStateToBeginningOfStep( domain );
//    }
//    LOG_LEVEL_RANK_0( 1, "\tIteration: " << iter+1  << ", FlowSolver: " );
//
//    // call assemble to fill the matrix and the rhs
//    m_flowSolver->AssembleSystem( domain, getLinearSystemRepository(), time_n+dt, dt );
//
//    // apply boundary conditions to system
//    m_flowSolver->ApplyBoundaryConditions( domain, getLinearSystemRepository(), time_n, dt );
//
//    // call the default linear solver on the system
//    m_flowSolver->SolveSystem( getLinearSystemRepository(),
//                 getSystemSolverParameters() );
//
//    // apply the system solution to the fields/variables
//    m_flowSolver->ApplySystemSolution( getLinearSystemRepository(), 1.0, domain );
//
//    if (dtReturnTemporary < dtReturn)
//    {
//      iter = 0;
//      dtReturn = dtReturnTemporary;
//      continue;
//    }
//
////    if (m_fluidSolver->getSystemSolverParameters()->numNewtonIterations() == 0 && iter > 0 && getLogLevel() >= 1)
////    {
////      GEOSX_LOG_RANK_0( "***** The iterative coupling has converged in " << iter  << " iterations! *****\n" );
////      break;
////    }
//
//    if (getLogLevel() >= 1)
//    {
//      GEOSX_LOG_RANK_0( "\tIteration: " << iter+1  << ", MechanicsSolver: " );
//    }
//
//    // call assemble to fill the matrix and the rhs
//    m_solidSolver->AssembleSystem( domain, getLinearSystemRepository(), time_n+dt, dt );
//
//
//    ApplyFractureFluidCoupling( domain, *getLinearSystemRepository() );
//
//    // apply boundary conditions to system
//    m_solidSolver->ApplyBoundaryConditions( domain, getLinearSystemRepository(), time_n, dt );
//
//    // call the default linear solver on the system
//    m_solidSolver->SolveSystem( getLinearSystemRepository(),
//                 getSystemSolverParameters() );
//
//    // apply the system solution to the fields/variables
//    m_solidSolver->ApplySystemSolution( getLinearSystemRepository(), 1.0, domain );
//
//    if( m_flowSolver->CalculateResidualNorm( getLinearSystemRepository(), domain ) < solverParams->newtonTol() &&
//        m_solidSolver->CalculateResidualNorm( getLinearSystemRepository(), domain ) < solverParams->newtonTol() )
//    {
//      GEOSX_LOG_RANK_0( "***** The iterative coupling has converged in " << iter  << " iterations! *****\n" );
//      break;
//    }
//
//    if (dtReturnTemporary < dtReturn)
//    {
//      iter = 0;
//      dtReturn = dtReturnTemporary;
//      continue;
//    }
////    if (m_solidSolver->getSystemSolverParameters()->numNewtonIterations() > 0)
//    {
//      this->UpdateDeformationForCoupling(domain);
////      m_fluidSolver->UpdateState(domain);
//    }
//    ++iter;
//  }
//
//  this->ImplicitStepComplete( time_n, dt, domain );

  return dtReturn;
}

real64 HydrofractureSolver::ExplicitStep( real64 const& time_n,
                                          real64 const& dt,
                                          const int cycleNumber,
                                          DomainPartition * const domain )
{
  GEOSX_MARK_FUNCTION;
  m_solidSolver->ExplicitStep( time_n, dt, cycleNumber, domain );
  m_flowSolver->SolverStep( time_n, dt, cycleNumber, domain );

  return dt;
}


void HydrofractureSolver::SetupDofs( DomainPartition const * const domain,
                                     DofManager & dofManager ) const
{
  GEOSX_MARK_FUNCTION;
  m_solidSolver->SetupDofs( domain, dofManager );
  m_flowSolver->SetupDofs( domain, dofManager );

  dofManager.addCoupling( keys::TotalDisplacement,
                          FlowSolverBase::viewKeyStruct::pressureString,
                          DofManager::Connectivity::Elem );
}

void HydrofractureSolver::SetupSystem( DomainPartition * const domain,
                                       DofManager & GEOSX_UNUSED_ARG( dofManager ),
                                       ParallelMatrix & GEOSX_UNUSED_ARG( matrix ),
                                       ParallelVector & GEOSX_UNUSED_ARG( rhs ),
                                       ParallelVector & GEOSX_UNUSED_ARG( solution ) )
{
  GEOSX_MARK_FUNCTION;
  m_flowSolver->ResetViews( domain );

  m_solidSolver->SetupSystem( domain,
                           m_solidSolver->getDofManager(),
                           m_solidSolver->getSystemMatrix(),
                           m_solidSolver->getSystemRhs(),
                           m_solidSolver->getSystemSolution() );

  m_flowSolver->SetupSystem( domain,
                           m_flowSolver->getDofManager(),
                           m_flowSolver->getSystemMatrix(),
                           m_flowSolver->getSystemRhs(),
                           m_flowSolver->getSystemSolution() );

  // TODO: once we move to a monolithic matrix, we can just use SolverBase implementation

//  dofManager.setSparsityPattern( m_matrix01,
//                                 keys::TotalDisplacement,
//                                 FlowSolverBase::viewKeyStruct::pressureString );
//
//  dofManager.setSparsityPattern( m_matrix10,
//                                 FlowSolverBase::viewKeyStruct::pressureString,
//                                 keys::TotalDisplacement );

  m_matrix01.createWithLocalSize( m_solidSolver->getSystemMatrix().localRows(),
                                  m_flowSolver->getSystemMatrix().localCols(),
                                  9,
                                  MPI_COMM_GEOSX);
  m_matrix10.createWithLocalSize( m_flowSolver->getSystemMatrix().localCols(),
                                  m_solidSolver->getSystemMatrix().localRows(),
                                  24,
                                  MPI_COMM_GEOSX);

  MeshLevel * const mesh = domain->getMeshBodies()->GetGroup<MeshBody>(0)->getMeshLevel(0);
  NodeManager * const nodeManager = mesh->getNodeManager();
  ElementRegionManager * const elemManager = mesh->getElemManager();

  std::unique_ptr<CRSMatrix<real64,localIndex,localIndex> > &
  derivativeFluxResidual_dAperture = m_flowSolver->getRefDerivativeFluxResidual_dAperture();
  {

    localIndex numRows = 0;
    localIndex numCols = 0;
    string_array const & flowRegions = m_flowSolver->getTargetRegions();
    elemManager->forElementSubRegions( flowRegions, [&]( ElementSubRegionBase const * const elementSubRegion )
    {
      numRows += elementSubRegion->size();
      numCols += elementSubRegion->size();
    });

    derivativeFluxResidual_dAperture = std::make_unique<CRSMatrix<real64,localIndex,localIndex>>( numRows, numCols );

    derivativeFluxResidual_dAperture->reserveNonZeros( m_flowSolver->getSystemMatrix().localNonzeros() );
    localIndex maxRowSize = -1;
    for( localIndex row=0 ; row<m_flowSolver->getSystemMatrix().localRows() ; ++row )
    {
      localIndex const rowSize = m_flowSolver->getSystemMatrix().getLocalRowGlobalLength( row );
      maxRowSize = maxRowSize > rowSize ? maxRowSize : rowSize;

      derivativeFluxResidual_dAperture->reserveNonZeros( row,
                                                         rowSize );
    }
    for( localIndex row=m_flowSolver->getSystemMatrix().localRows() ; row<numRows ; ++row )
    {
      derivativeFluxResidual_dAperture->reserveNonZeros( row,
                                                         maxRowSize );
    }


  }
//  CRSMatrixView<real64,localIndex,localIndex const> const &
//  derivativeFluxResidual_dAperture = m_flowSolver->getDerivativeFluxResidual_dAperture();

  string const presDofKey = m_flowSolver->getDofManager().getKey( FlowSolverBase::viewKeyStruct::pressureString );
  string const dispDofKey = m_solidSolver->getDofManager().getKey( keys::TotalDisplacement );

  arrayView1d<globalIndex> const &
  dispDofNumber =  nodeManager->getReference<globalIndex_array>( dispDofKey );

  elemManager->forElementSubRegions<FaceElementSubRegion>([&]( FaceElementSubRegion const * const elementSubRegion )
  {
    localIndex const numElems = elementSubRegion->size();
    array1d<array1d<localIndex > > const & elemsToNodes = elementSubRegion->nodeList();
    arrayView1d<globalIndex> const &
    faceElementDofNumber = elementSubRegion->getReference< array1d<globalIndex> >( presDofKey );

    for( localIndex k=0 ; k<numElems ; ++k )
    {
      globalIndex const activeFlowDOF = faceElementDofNumber[k];
      localIndex const numNodesPerElement = elemsToNodes[k].size();
      array1d<globalIndex> activeDisplacementDOF(3 * numNodesPerElement);
      array1d<real64> values( 3*numNodesPerElement );
      values = 1;

      for( localIndex a=0 ; a<numNodesPerElement ; ++a )
      {
        for( int d=0 ; d<3 ; ++d )
        {
          activeDisplacementDOF[a * 3 + d] = dispDofNumber[elemsToNodes[k][a]] + d;
        }
      }

      m_matrix01.insert( activeDisplacementDOF.data(),
                         &activeFlowDOF,
                         values.data(),
                         activeDisplacementDOF.size(),
                         1 );

      m_matrix10.insert( &activeFlowDOF,
                         activeDisplacementDOF.data(),
                         values.data(),
                         1,
                         activeDisplacementDOF.size() );
    }
  });

  NumericalMethodsManager const * numericalMethodManager =
    domain->getParent()->GetGroup<NumericalMethodsManager>( keys::numericalMethodsManager );

  FiniteVolumeManager const * fvManager =
    numericalMethodManager->GetGroup<FiniteVolumeManager>( keys::finiteVolumeManager );

  FluxApproximationBase const * fluxApprox = fvManager->getFluxApproximation( m_flowSolver->getDiscretization() );


  fluxApprox->forStencils<FaceElementStencil>( [&]( FaceElementStencil const & stencil )
  {
//    forall_in_range<serialPolicy>( 0, stencil.size(), GEOSX_LAMBDA ( localIndex iconn )
    for( localIndex iconn=0 ; iconn<stencil.size() ; ++iconn)
    {
      localIndex const numFluxElems = stencil.stencilSize(iconn);
      typename FaceElementStencil::IndexContainerViewConstType const & seri = stencil.getElementRegionIndices();
      typename FaceElementStencil::IndexContainerViewConstType const & sesri = stencil.getElementSubRegionIndices();
      typename FaceElementStencil::IndexContainerViewConstType const & sei = stencil.getElementIndices();

      FaceElementSubRegion const * const
      elementSubRegion = elemManager->GetRegion(seri[iconn][0])->GetSubRegion<FaceElementSubRegion>(sesri[iconn][0]);

//      GEOS_LOG_RANK("connector, numLocal, numGhost: "<<iconn<<", "<<elementSubRegion->size()-elementSubRegion->GetNumberOfGhosts()<<", "<<elementSubRegion->GetNumberOfGhosts());
//      GEOS_LOG_RANK("connector, numRows, numCols: "<<iconn<<", "<<derivativeFluxResidual_dAperture->numRows()<<", "<<derivativeFluxResidual_dAperture->numColumns());

      array1d<array1d<localIndex > > const & elemsToNodes = elementSubRegion->nodeList();

//      arrayView1d<integer const> const & ghostRank = elementSubRegion->GhostRank();

      arrayView1d<globalIndex> const &
      faceElementDofNumber = elementSubRegion->getReference< array1d<globalIndex> >( presDofKey );
      for( localIndex k0=0 ; k0<numFluxElems ; ++k0 )
      {
        globalIndex const activeFlowDOF = faceElementDofNumber[sei[iconn][k0]];

        for( localIndex k1=0 ; k1<numFluxElems ; ++k1 )
        {
//          GEOS_LOG_RANK("ei0, ei1, nonZeroCapacitys: "<<sei[iconn][k0]<<", "<<sei[iconn][k1]<<", "<<derivativeFluxResidual_dAperture->nonZeroCapacity(sei[iconn][k0]));
          derivativeFluxResidual_dAperture->insertNonZero( sei[iconn][k0],sei[iconn][k1], 0.0 );

          localIndex const numNodesPerElement = elemsToNodes[sei[iconn][k1]].size();
          array1d<globalIndex> activeDisplacementDOF(3 * numNodesPerElement);
          array1d<real64> values( 3*numNodesPerElement );
          values = 1;

          for( localIndex a=0 ; a<numNodesPerElement ; ++a )
          {
            for( int d=0 ; d<3 ; ++d )
            {
              activeDisplacementDOF[a * 3 + d] = dispDofNumber[elemsToNodes[sei[iconn][k1]][a]] + d;
            }
          }

          m_matrix10.insert( &activeFlowDOF,
                             activeDisplacementDOF.data(),
                             values.data(),
                             1,
                             activeDisplacementDOF.size() );
        }
      }
    }//);
  });

  m_matrix01.close();
  m_matrix10.close();
}

void HydrofractureSolver::AssembleSystem( real64 const time,
                                          real64 const dt,
                                          DomainPartition * const domain,
                                          DofManager const & GEOSX_UNUSED_ARG( dofManager ),
                                          ParallelMatrix & GEOSX_UNUSED_ARG( matrix ),
                                          ParallelVector & GEOSX_UNUSED_ARG( rhs ) )
{
  GEOSX_MARK_FUNCTION;
  m_solidSolver->AssembleSystem( time,
                                 dt,
                                 domain,
                                 m_solidSolver->getDofManager(),
                                 m_solidSolver->getSystemMatrix(),
                                 m_solidSolver->getSystemRhs() );

  m_flowSolver->AssembleSystem( time,
                                dt,
                                domain,
                                m_flowSolver->getDofManager(),
                                m_flowSolver->getSystemMatrix(),
                                m_flowSolver->getSystemRhs() );



  AssembleForceResidualDerivativeWrtPressure( domain, &m_matrix01, &(m_solidSolver->getSystemRhs()) );
  AssembleFluidMassResidualDerivativeWrtDisplacement( domain, &m_matrix10, &(m_flowSolver->getSystemRhs()) );
}

void HydrofractureSolver::ApplyBoundaryConditions( real64 const time,
                                                   real64 const dt,
                                                   DomainPartition * const domain,
                                                   DofManager const & GEOSX_UNUSED_ARG( dofManager ),
                                                   ParallelMatrix & GEOSX_UNUSED_ARG( matrix ),
                                                   ParallelVector & GEOSX_UNUSED_ARG( rhs ) )
{
  GEOSX_MARK_FUNCTION;
  m_solidSolver->ApplyBoundaryConditions( time,
                                          dt,
                                          domain,
                                          m_solidSolver->getDofManager(),
                                          m_solidSolver->getSystemMatrix(),
                                          m_solidSolver->getSystemRhs() );

  MeshLevel * const mesh = domain->getMeshBodies()->GetGroup<MeshBody>(0)->getMeshLevel(0);

  FieldSpecificationManager const & fsManager = FieldSpecificationManager::get();
  string const dispDofKey = m_solidSolver->getDofManager().getKey( keys::TotalDisplacement );
  NodeManager const * const nodeManager = mesh->getNodeManager();
  arrayView1d<globalIndex const> const & dispDofNumber = nodeManager->getReference<globalIndex_array>( dispDofKey );
  arrayView1d<integer const> const & nodeGhostRank = nodeManager->GhostRank();

  fsManager.Apply( time + dt,
                   domain,
                   "nodeManager",
                   keys::TotalDisplacement,
                   [&]( FieldSpecificationBase const * const bc,
                        string const &,
                        set<localIndex> const & targetSet,
                        Group * const ,
                        string const )
  {
    set<localIndex> localSet;
    for( auto const & a : targetSet )
    {
      if( nodeGhostRank[a]<0 )
      {
        localSet.insert(a);
      }
    }
    bc->ZeroSystemRowsForBoundaryCondition<LAInterface>( localSet,
                                                         dispDofNumber,
                                                         m_matrix01 );
  } );


  m_flowSolver->ApplyBoundaryConditions( time,
                                         dt,
                                         domain,
                                         m_flowSolver->getDofManager(),
                                         m_flowSolver->getSystemMatrix(),
                                         m_flowSolver->getSystemRhs() );

  string const presDofKey = m_flowSolver->getDofManager().getKey( FlowSolverBase::viewKeyStruct::pressureString );

  fsManager.Apply( time + dt,
                    domain,
                    "ElementRegions",
                    FlowSolverBase::viewKeyStruct::pressureString,
                    [&]( FieldSpecificationBase const * const fs,
                         string const &,
                         set<localIndex> const & lset,
                         Group * subRegion,
                         string const & ) -> void
  {
    arrayView1d<globalIndex const> const &
    dofNumber = subRegion->getReference< array1d<globalIndex> >( presDofKey );
    arrayView1d<integer const> const & ghostRank = subRegion->group_cast<ObjectManagerBase*>()->GhostRank();

    set<localIndex> localSet;
    for( auto const & a : lset )
    {
      if( ghostRank[a]<0 )
      {
        localSet.insert(a);
      }
    }

    fs->ZeroSystemRowsForBoundaryCondition<LAInterface>( localSet,
                                                         dofNumber,
                                                         m_matrix10 );
  });

  // debugging info.  can be trimmed once everything is working.
  if( getLogLevel()>=10 )
  {
    // Before outputting anything generate permuation matrix and permute.
    ElementRegionManager * const elemManager = mesh->getElemManager();

    LAIHelperFunctions::CreatePermutationMatrix(nodeManager,
                                                m_solidSolver->getSystemMatrix().globalRows(),
                                                m_solidSolver->getSystemMatrix().globalCols(),
                                                3,
                                                m_solidSolver->getDofManager().getKey( keys::TotalDisplacement ),
                                                m_permutationMatrix0);

    LAIHelperFunctions::CreatePermutationMatrix(elemManager,
                                                m_flowSolver->getSystemMatrix().globalRows(),
                                                m_flowSolver->getSystemMatrix().globalCols(),
                                                1,
                                                m_flowSolver->getDofManager().getKey( FlowSolverBase::viewKeyStruct::pressureString ),
                                                m_permutationMatrix1);

    GEOSX_LOG_RANK_0("***********************************************************");
    GEOSX_LOG_RANK_0("matrix00");
    GEOSX_LOG_RANK_0("***********************************************************");
//    LAIHelperFunctions::PrintPermutedMatrix(m_solidSolver->getSystemMatrix(), m_permutationMatrix0, std::cout);
    m_solidSolver->getSystemMatrix().print(std::cout);
    MpiWrapper::Barrier();

    GEOSX_LOG_RANK_0("***********************************************************");
    GEOSX_LOG_RANK_0("matrix01");
    GEOSX_LOG_RANK_0("***********************************************************");
//    LAIHelperFunctions::PrintPermutedMatrix(m_matrix01, m_permutationMatrix0, m_permutationMatrix1, std::cout);
    m_matrix01.print(std::cout);
    MpiWrapper::Barrier();

    GEOSX_LOG_RANK_0("***********************************************************");
    GEOSX_LOG_RANK_0("matrix10");
    GEOSX_LOG_RANK_0("***********************************************************");
//    LAIHelperFunctions::PrintPermutedMatrix(m_matrix10, m_permutationMatrix1, m_permutationMatrix0, std::cout);
    m_matrix10.print(std::cout);
    MpiWrapper::Barrier();

    GEOSX_LOG_RANK_0("***********************************************************");
    GEOSX_LOG_RANK_0("matrix11");
    GEOSX_LOG_RANK_0("***********************************************************");
//    LAIHelperFunctions::PrintPermutedMatrix(m_flowSolver->getSystemMatrix(), m_permutationMatrix1, std::cout);
    m_flowSolver->getSystemMatrix().print(std::cout);
    MpiWrapper::Barrier();

    GEOSX_LOG_RANK_0("***********************************************************");
    GEOSX_LOG_RANK_0("residual0");
    GEOSX_LOG_RANK_0("***********************************************************");
//    LAIHelperFunctions::PrintPermutedVector(m_solidSolver->getSystemRhs(), m_permutationMatrix0, std::cout);
    m_solidSolver->getSystemRhs().print(std::cout);
    MpiWrapper::Barrier();

    GEOSX_LOG_RANK_0("***********************************************************");
    GEOSX_LOG_RANK_0("residual1");
    GEOSX_LOG_RANK_0("***********************************************************");
//    LAIHelperFunctions::PrintPermutedVector(m_flowSolver->getSystemRhs(), m_permutationMatrix1, std::cout);
    m_flowSolver->getSystemRhs().print(std::cout);
    MpiWrapper::Barrier();
  }

  if( getLogLevel() >= 10 )
  {
    SystemSolverParameters * const solverParams = getSystemSolverParameters();
    integer newtonIter = solverParams->numNewtonIterations();

    {
      string filename = "matrix00_" + std::to_string( time ) + "_" + std::to_string( newtonIter ) + ".mtx";
      m_solidSolver->getSystemMatrix().write( filename, true );
      GEOSX_LOG_RANK_0( "matrix00: written to " << filename );
    }
    {
      string filename = "matrix01_" + std::to_string( time ) + "_" + std::to_string( newtonIter ) + ".mtx";
      m_matrix01.write( filename, true );
      GEOSX_LOG_RANK_0( "matrix01: written to " << filename );
    }
    {
      string filename = "matrix10_" + std::to_string( time ) + "_" + std::to_string( newtonIter ) + ".mtx";
      m_matrix10.write( filename, true );
      GEOSX_LOG_RANK_0( "matrix10: written to " << filename );
    }
    {
      string filename = "matrix11_" + std::to_string( time ) + "_" + std::to_string( newtonIter ) + ".mtx";
      m_flowSolver->getSystemMatrix().write( filename, true );
      GEOSX_LOG_RANK_0( "matrix11: written to " << filename );
    }
    {
      string filename = "residual0_" + std::to_string( time ) + "_" + std::to_string( newtonIter ) + ".mtx";
      m_solidSolver->getSystemRhs().write( filename, true );
      GEOSX_LOG_RANK_0( "residual0: written to " << filename );
    }
    {
      string filename = "residual1_" + std::to_string( time ) + "_" + std::to_string( newtonIter ) + ".mtx";
      m_flowSolver->getSystemRhs().write( filename, true );
      GEOSX_LOG_RANK_0( "residual1: written to " << filename );
    }
  }
}

real64
HydrofractureSolver::
CalculateResidualNorm( DomainPartition const * const domain,
                       DofManager const & GEOSX_UNUSED_ARG( dofManager ),
                       ParallelVector const & GEOSX_UNUSED_ARG( rhs ) )
{
  GEOSX_MARK_FUNCTION;

  /*
  real64 const fluidResidual = m_flowSolver->getSystemRhs().norm2();
  real64 const solidResidual = m_solidSolver->getSystemRhs().norm2();
  */

  real64 const fluidResidual = m_flowSolver->CalculateResidualNorm( domain,
                                                                    m_flowSolver->getDofManager(),
                                                                    m_flowSolver->getSystemRhs() );

  real64 const solidResidual = m_solidSolver->CalculateResidualNorm( domain,
                                                                     m_solidSolver->getDofManager(),
                                                                     m_solidSolver->getSystemRhs() );

  GEOSX_LOG_RANK_0("residuals for fluid, solid: "<<fluidResidual<<", "<<solidResidual);

  return fluidResidual + solidResidual;
}



void
HydrofractureSolver::
AssembleForceResidualDerivativeWrtPressure( DomainPartition * const domain,
                                            ParallelMatrix * const matrix01,
                                            ParallelVector * const rhs0 )
{
  GEOSX_MARK_FUNCTION;
  MeshLevel * const mesh = domain->getMeshBodies()->GetGroup<MeshBody>(0)->getMeshLevel(0);

  FaceManager const * const faceManager = mesh->getFaceManager();
  NodeManager * const nodeManager = mesh->getNodeManager();
  ElementRegionManager * const elemManager = mesh->getElemManager();

  arrayView1d<R1Tensor const> const & faceNormal = faceManager->faceNormal();
  ArrayOfArraysView< localIndex const > const & faceToNodeMap = faceManager->nodeList();

  arrayView1d<R1Tensor> const &
  fext = nodeManager->getReference< array1d<R1Tensor> >( SolidMechanicsLagrangianFEM::viewKeyStruct::forceExternal );
  fext = {0,0,0};

  string const presDofKey = m_flowSolver->getDofManager().getKey( FlowSolverBase::viewKeyStruct::pressureString );
  string const dispDofKey = m_solidSolver->getDofManager().getKey( keys::TotalDisplacement );

  arrayView1d<globalIndex> const &
  dispDofNumber =  nodeManager->getReference<globalIndex_array>( dispDofKey );


  matrix01->open();
  matrix01->zero();
  rhs0->open();

  elemManager->forElementSubRegions<FaceElementSubRegion>([&]( FaceElementSubRegion * const subRegion )->void
  {

    arrayView1d<globalIndex> const &
    faceElementDofNumber = subRegion->getReference< array1d<globalIndex> >( presDofKey );

    if( subRegion->hasWrapper( "pressure" ) )
    {
      arrayView1d<real64 const> const & fluidPressure = subRegion->getReference<array1d<real64> >("pressure");
      arrayView1d<real64 const> const & deltaFluidPressure = subRegion->getReference<array1d<real64> >("deltaPressure");
      arrayView1d<integer const> const & ghostRank = subRegion->GhostRank();
      arrayView1d<real64> const & area = subRegion->getElementArea();
      arrayView2d< localIndex const > const & elemsToFaces = subRegion->faceList();

      forall_in_range<serialPolicy>( 0,
                                   subRegion->size(),
                                   GEOSX_LAMBDA ( localIndex const kfe )
      {
        R1Tensor Nbar = faceNormal[elemsToFaces[kfe][0]];
        Nbar -= faceNormal[elemsToFaces[kfe][1]];
        Nbar.Normalize();

        localIndex const kf0 = elemsToFaces[kfe][0];
        localIndex const numNodesPerFace = faceToNodeMap.sizeOfArray(kf0);

        globalIndex rowDOF[24];
        real64 nodeRHS[24];
        stackArray2d<real64, 12*12> dRdP(numNodesPerFace*3, 1);
        globalIndex colDOF = faceElementDofNumber[kfe];

        real64 const Ja = area[kfe] / numNodesPerFace;

        //          std::cout<<"fluidPressure["<<kfe<<"] = "<<fluidPressure[kfe]+deltaFluidPressure[kfe]<<std::endl;
        real64 nodalForceMag = ( fluidPressure[kfe]+deltaFluidPressure[kfe] ) * Ja;
        R1Tensor nodalForce(Nbar);
        nodalForce *= nodalForceMag;

        //          std::cout << "    rank " << MpiWrapper::Comm_rank(MPI_COMM_GEOSX) << ", faceElement " << kfe << std::endl;
        //          std::cout << "    fluid pressure " << fluidPressure[kfe]+deltaFluidPressure[kfe] << std::endl;
        //          std::cout << "    nodalForce " << nodalForce << std::endl;
        for( localIndex kf=0 ; kf<2 ; ++kf )
        {
          localIndex const faceIndex = elemsToFaces[kfe][kf];


          for( localIndex a=0 ; a<numNodesPerFace ; ++a )
          {

            for( int i=0 ; i<3 ; ++i )
            {
              rowDOF[3*a+i] = dispDofNumber[faceToNodeMap(faceIndex, a)] + i;
              nodeRHS[3*a+i] = - nodalForce[i] * pow(-1,kf);
              fext[faceToNodeMap(faceIndex, a)][i] += - nodalForce[i] * pow(-1,kf);

              dRdP(3*a+i,0) = - Ja * Nbar[i] * pow(-1,kf);
              // this is for debugging
              //                if (dispDofNumber[faceToNodeMap(faceIndex, a)] == 0 || dispDofNumber[faceToNodeMap(faceIndex, a)] == 6 || dispDofNumber[faceToNodeMap(faceIndex, a)] == 12 || dispDofNumber[faceToNodeMap(faceIndex, a)] == 18)
              //                  std::cout << "rank " << MpiWrapper::Comm_rank(MPI_COMM_GEOSX) << "DOF index " << dispDofNumber[faceToNodeMap(faceIndex, a)] + i << " contribution " << nodeRHS[3*a+i] << std::endl;

            }
          }
          if( ghostRank[kfe] < 0 )
          {

            rhs0->add( rowDOF,
                       nodeRHS,
                       numNodesPerFace*3 );


            matrix01->add( rowDOF,
                           &colDOF,
                           dRdP.data(),
                           numNodesPerFace * 3,
                           1 );
          }
        }
      });
    }
  });

  rhs0->close();
  matrix01->close();
  rhs0->close();
}

void
HydrofractureSolver::
AssembleFluidMassResidualDerivativeWrtDisplacement( DomainPartition const * const domain,
                                                    ParallelMatrix * const matrix10,
                                                    ParallelVector * const GEOSX_UNUSED_ARG( rhs0 ) )
{
  GEOSX_MARK_FUNCTION;

  MeshLevel const * const mesh = domain->getMeshBodies()->GetGroup<MeshBody>(0)->getMeshLevel(0);
  ElementRegionManager const * const elemManager = mesh->getElemManager();
  FaceManager const * const faceManager = mesh->getFaceManager();
  NodeManager const * const nodeManager = mesh->getNodeManager();
  ConstitutiveManager const * const constitutiveManager = domain->getConstitutiveManager();

  string const constitutiveName = constitutiveManager->GetGroup(m_flowSolver->fluidIndex())->getName();
  string const presDofKey = m_flowSolver->getDofManager().getKey( FlowSolverBase::viewKeyStruct::pressureString );
  string const dispDofKey = m_solidSolver->getDofManager().getKey( keys::TotalDisplacement );

  CRSMatrixView<real64 const,localIndex const,localIndex const> const &
  dFluxResidual_dAperture = m_flowSolver->getDerivativeFluxResidual_dAperture();

  ContactRelationBase const * const
  contactRelation = constitutiveManager->GetGroup<ContactRelationBase>( m_contactRelationName );

  matrix10->open();
  matrix10->zero();

  elemManager->forElementSubRegionsComplete<FaceElementSubRegion>( this->m_targetRegions,
                                                                   [&] ( localIndex GEOSX_UNUSED_ARG( er ),
                                                                         localIndex GEOSX_UNUSED_ARG( esr ),
                                                                         ElementRegionBase const * const GEOSX_UNUSED_ARG( region ),
                                                                         FaceElementSubRegion const * const subRegion )
  {


    dataRepository::Group const * const constitutiveGroup = subRegion->GetConstitutiveModels();
    dataRepository::Group const * const constitutiveRelation = constitutiveGroup->GetGroup(constitutiveName);

    arrayView1d<integer const>     const & elemGhostRank = subRegion->GhostRank();
    arrayView1d<globalIndex const> const & presDofNumber = subRegion->getReference<array1d<globalIndex>>( presDofKey );
    arrayView1d<globalIndex const> const & dispDofNumber = nodeManager->getReference<array1d<globalIndex>>( dispDofKey );

    arrayView2d<real64 const> const &
    dens = constitutiveRelation->getReference<array2d<real64>>(SingleFluidBase::viewKeyStruct::densityString);

    arrayView1d<real64 const> const & aperture  = subRegion->getElementAperture();
    arrayView1d<real64 const> const & area      = subRegion->getElementArea();

    arrayView2d<localIndex const> const & elemsToFaces = subRegion->faceList();
    ArrayOfArraysView< localIndex const > const & faceToNodeMap = faceManager->nodeList();

    arrayView1d<R1Tensor const> const & faceNormal = faceManager->faceNormal();


    forall_in_range<serialPolicy>( 0, subRegion->size(), GEOSX_LAMBDA ( localIndex ei )
    {
      //if (elemGhostRank[ei] < 0)
      {
        globalIndex const elemDOF = presDofNumber[ei];
        localIndex const numNodesPerFace = faceToNodeMap.sizeOfArray(elemsToFaces[ei][0]);
        real64 const dAccumulationResidualdAperture = dens[ei][0] * area[ei];


        globalIndex nodeDOF[8*3];

        R1Tensor Nbar = faceNormal[elemsToFaces[ei][0]];
        Nbar -= faceNormal[elemsToFaces[ei][1]];
        Nbar.Normalize();

        stackArray1d<real64, 24> dRdU(2*numNodesPerFace*3);

        // Accumulation derivative
        if (elemGhostRank[ei] < 0)
        {
          //GEOS_LOG_RANK( "dAccumulationResidualdAperture("<<ei<<") = "<<dAccumulationResidualdAperture );
          for( localIndex kf=0 ; kf<2 ; ++kf )
          {
            for( localIndex a=0 ; a<numNodesPerFace ; ++a )
            {
              for( int i=0 ; i<3 ; ++i )
              {
                nodeDOF[ kf*3*numNodesPerFace + 3*a+i] = dispDofNumber[faceToNodeMap(elemsToFaces[ei][kf],a)] +i;
                real64 const dGap_dU = - pow(-1,kf) * Nbar[i] / numNodesPerFace;
                real64 const dAper_dU = contactRelation->dEffectiveAperture_dAperture( aperture[ei] ) * dGap_dU;
                dRdU(kf*3*numNodesPerFace + 3*a+i) = dAccumulationResidualdAperture * dAper_dU;
              }
            }
          }
          matrix10->add( elemDOF,
                         nodeDOF,
                         dRdU.data(),
                         2*numNodesPerFace*3 );
        }

        // flux derivative
        localIndex const numColumns = dFluxResidual_dAperture.numNonZeros(ei);
        arraySlice1d<localIndex const> const & columns = dFluxResidual_dAperture.getColumns( ei );
        arraySlice1d<real64 const> const & values = dFluxResidual_dAperture.getEntries( ei );

        for( localIndex kfe2=0 ; kfe2<numColumns ; ++kfe2 )
        {
          real64 dRdAper = values[kfe2];
          localIndex const ei2 = columns[kfe2];
//          GEOS_LOG_RANK( "dRdAper("<<ei<<", "<<ei2<<") = "<<dRdAper );

          for( localIndex kf=0 ; kf<2 ; ++kf )
          {
            for( localIndex a=0 ; a<numNodesPerFace ; ++a )
            {
              for( int i=0 ; i<3 ; ++i )
              {
                nodeDOF[ kf*3*numNodesPerFace + 3*a+i] = dispDofNumber[faceToNodeMap(elemsToFaces[ei2][kf],a)] +i;
                real64 const dGap_dU = - pow(-1,kf) * Nbar[i] / numNodesPerFace;
                real64 const dAper_dU = contactRelation->dEffectiveAperture_dAperture( aperture[ei2] ) * dGap_dU;
                dRdU(kf*3*numNodesPerFace + 3*a+i) = dRdAper * dAper_dU;
              }
            }
          }
          matrix10->add( elemDOF,
                         nodeDOF,
                         dRdU.data(),
                         2*numNodesPerFace*3 );

        }
      }
    });
  });

  matrix10->close();
}

void
HydrofractureSolver::
ApplySystemSolution( DofManager const & GEOSX_UNUSED_ARG( dofManager ),
                     ParallelVector const & GEOSX_UNUSED_ARG( solution ),
                     real64 const scalingFactor,
                     DomainPartition * const domain )
{
  GEOSX_MARK_FUNCTION;
  m_solidSolver->ApplySystemSolution( m_solidSolver->getDofManager(),
                                      m_solidSolver->getSystemSolution(),
                                      scalingFactor,
                                      domain );
  m_flowSolver->ApplySystemSolution( m_flowSolver->getDofManager(),
                                     m_flowSolver->getSystemSolution(),
                                     -scalingFactor,
                                     domain );

  this->UpdateDeformationForCoupling(domain);
}

}

#include "EpetraExt_MatrixMatrix.h"
#include "Thyra_OperatorVectorClientSupport.hpp"
#include "Thyra_AztecOOLinearOpWithSolveFactory.hpp"
#include "Thyra_AztecOOLinearOpWithSolve.hpp"
#include "Thyra_EpetraThyraWrappers.hpp"
#include "Thyra_EpetraLinearOp.hpp"
#include "Thyra_EpetraLinearOpBase.hpp"
#include "Thyra_LinearOpBase.hpp"
#include "Thyra_LinearOpWithSolveBase.hpp"
#include "Thyra_LinearOpWithSolveFactoryHelpers.hpp"
#include "Thyra_DefaultBlockedLinearOp.hpp"
#include "Thyra_DefaultIdentityLinearOp.hpp"
#include "Thyra_DefaultZeroLinearOp.hpp"
#include "Thyra_DefaultLinearOpSource.hpp"
#include "Thyra_DefaultPreconditioner.hpp"
#include "Thyra_EpetraThyraWrappers.hpp"
#include "Thyra_PreconditionerFactoryHelpers.hpp"
#include "Thyra_VectorStdOps.hpp"
#include "Thyra_PreconditionerFactoryHelpers.hpp"
#include "Thyra_DefaultInverseLinearOp.hpp"
#include "Thyra_PreconditionerFactoryBase.hpp"
#include "Thyra_get_Epetra_Operator.hpp"
#include "Thyra_MLPreconditionerFactory.hpp"
#include "Teuchos_ParameterList.hpp"
#include "Teuchos_RCP.hpp"
#include "Teuchos_Time.hpp"
#include "Stratimikos_DefaultLinearSolverBuilder.hpp"

namespace geosx
{
  
void HydrofractureSolver::SolveSystem( DofManager const & GEOSX_UNUSED_ARG( dofManager ),
                                       ParallelMatrix & ,
                                       ParallelVector & ,
                                       ParallelVector &  )
{
  GEOSX_MARK_FUNCTION;

  /*
  globalIndex numU = m_solidSolver->getSystemRhs().globalSize();
  globalIndex numP = m_flowSolver->getSystemRhs().globalSize();
  GEOSX_LOG_RANK_0("size = " << numU << " + " << numP);
  */

  SystemSolverParameters * const params = &m_systemSolverParameters;
  integer newtonIter = params->numNewtonIterations();

  using namespace Teuchos;
  using namespace Thyra;

  Teuchos::Time clock("solveClock");  

  GEOSX_MARK_BEGIN(Setup);
  Epetra_FECrsMatrix * p_matrix[2][2];
  Epetra_FEVector * p_rhs[2];
  Epetra_FEVector * p_solution[2];

  p_rhs[0] = m_solidSolver->getSystemRhs().unwrappedPointer();
  p_rhs[1] = m_flowSolver->getSystemRhs().unwrappedPointer();

  p_solution[0] = m_solidSolver->getSystemSolution().unwrappedPointer();
  p_solution[1] = m_flowSolver->getSystemSolution().unwrappedPointer();

  p_matrix[0][0] = m_solidSolver->getSystemMatrix().unwrappedPointer();
  p_matrix[0][1] = m_matrix01.unwrappedPointer();
  p_matrix[1][0] = m_matrix10.unwrappedPointer();
  p_matrix[1][1] = m_flowSolver->getSystemMatrix().unwrappedPointer();

  // scale and symmetrize

  m_densityScaling = 1e-3;
  m_pressureScaling = 1e9;

  p_matrix[0][1]->Scale(m_pressureScaling);
  p_matrix[1][0]->Scale(m_pressureScaling*m_densityScaling);
  p_matrix[1][1]->Scale(m_pressureScaling*m_pressureScaling*m_densityScaling);
  p_rhs[1]->Scale(m_pressureScaling*m_densityScaling);

    // SCHEME CHOICES
    //
    // there are several flags to control solver behavior.
    // these should be compared in a scaling study.
    //
    // -- whether to use a block diagonal or a 
    //    block triangular preconditioner.
    // -- whether to use BiCGstab or GMRES for the
    //    krylov solver.  GMRES is generally more robust,
    //    BiCGstab sometimes shows better parallel performance.
    //    false is probably better.

  const bool use_diagonal_prec = true;
  const bool use_bicgstab      = params->m_useBicgstab;

    // set initial guess to zero

  p_solution[0]->PutScalar(0.0);
  p_solution[1]->PutScalar(0.0);

    // create separate displacement component matrix

  clock.start(true);
  if(newtonIter==0)
  {
    m_blockDiagUU.reset(new ParallelMatrix());
    LAIHelperFunctions::SeparateComponentFilter(m_solidSolver->getSystemMatrix(),*m_blockDiagUU,3);
  }

    // create schur complement approximation matrix

  Epetra_CrsMatrix* schurApproxPP = NULL; // confirm we delete this at end of function!
  {
    Epetra_Vector diag(p_matrix[0][0]->RowMap());
    Epetra_Vector diagInv(p_matrix[0][0]->RowMap());
 
    p_matrix[0][0]->ExtractDiagonalCopy(diag); 
    diagInv.Reciprocal(diag);
 
    Epetra_FECrsMatrix DB(*p_matrix[0][1]);
    DB.LeftScale(diagInv);
    DB.FillComplete();

    Epetra_FECrsMatrix BtDB(Epetra_DataAccess::Copy,p_matrix[1][1]->RowMap(),1); 
    EpetraExt::MatrixMatrix::Multiply(*p_matrix[1][0],false,DB,false,BtDB);
    EpetraExt::MatrixMatrix::Add(BtDB,false,-1.0,*p_matrix[1][1],false,1.0,schurApproxPP);

    schurApproxPP->FillComplete();
  }
  double auxTime = clock.stop();
  GEOSX_MARK_END(Setup);

    // we want to use thyra to wrap epetra operators and vectors
    // for individual blocks.  this is an ugly conversion, but
    // it is basically just window dressing.
    //
    // note the use of Teuchos::RCP reference counted pointers.
    // The general syntax is usually one of:
    //
    //   RCP<T> Tptr = rcp(new T)
    //   RCP<T> Tptr = nonMemberConstructor();
    //   RCP<T> Tptr (t_ptr,false)
    //
    // where "false" implies the RCP does not own the object and
    // should not attempt to delete it when finished.

  GEOSX_MARK_BEGIN(THYRA_SETUP);

  RCP<const Thyra::LinearOpBase<double> >  matrix_block[2][2];
  RCP<Thyra::MultiVectorBase<double> >     lhs_block[2];
  RCP<Thyra::MultiVectorBase<double> >     rhs_block[2];

  for(unsigned i=0; i<2; ++i)
  for(unsigned j=0; j<2; ++j)
  {
    RCP<Epetra_Operator> mmm (&*p_matrix[i][j],false);
    matrix_block[i][j] = Thyra::epetraLinearOp(mmm);
  }

  RCP<Epetra_Operator> bbb(m_blockDiagUU->unwrappedPointer(),false);
  RCP<Epetra_Operator> ppp(schurApproxPP,false);

  RCP<const Thyra::LinearOpBase<double> >  blockDiagOp = Thyra::epetraLinearOp(bbb);
  RCP<const Thyra::LinearOpBase<double> >  schurOp = Thyra::epetraLinearOp(ppp);

  for(unsigned i=0; i<2; ++i)
  {
    RCP<Epetra_MultiVector> lll (&*p_solution[i],false);
    RCP<Epetra_MultiVector> rrr (&*p_rhs[i],false);

    lhs_block[i] = Thyra::create_MultiVector(lll,matrix_block[i][i]->domain());
    rhs_block[i] = Thyra::create_MultiVector(rrr,matrix_block[i][i]->range());
  }

    // now use thyra to create an operator representing
    // the full block 2x2 system

  RCP<const Thyra::LinearOpBase<double> > matrix = Thyra::block2x2(matrix_block[0][0],
                                                                   matrix_block[0][1],
                                                                   matrix_block[1][0],
                                                                   matrix_block[1][1]);

    // creating a representation of the blocked
    // rhs and lhs is a little uglier. 

  RCP<Thyra::ProductMultiVectorBase<double> > rhs;
  {
    Teuchos::Array<RCP<Thyra::MultiVectorBase<double> > > mva;
    Teuchos::Array<RCP<const Thyra::VectorSpaceBase<double> > > mvs;

    for(unsigned i=0; i<2; ++i)
    {
      mva.push_back(rhs_block[i]);
      mvs.push_back(rhs_block[i]->range());
    }

    RCP<const Thyra::DefaultProductVectorSpace<double> > vs = Thyra::productVectorSpace<double>(mvs);

    rhs = Thyra::defaultProductMultiVector<double>(vs,mva);
  }

  RCP<Thyra::ProductMultiVectorBase<double> > lhs;

  {
    Teuchos::Array<RCP<Thyra::MultiVectorBase<double> > > mva;
    Teuchos::Array<RCP<const Thyra::VectorSpaceBase<double> > > mvs;

    for(unsigned i=0; i<2; ++i)
    {
      mva.push_back(lhs_block[i]);
      mvs.push_back(lhs_block[i]->range());
    }

    RCP<const Thyra::DefaultProductVectorSpace<double> > vs = Thyra::productVectorSpace<double>(mvs);

    lhs = Thyra::defaultProductMultiVector<double>(vs,mva);
  }

  GEOSX_MARK_END(THYRA_SETUP);

    // for the preconditioner, we need two approximate inverses,
    // we store both "sub operators" in a 1x2 array:

  RCP<const Thyra::LinearOpBase<double> > sub_op[2];

  clock.start(true);
  GEOSX_MARK_BEGIN(PRECONDITIONER);

  for(unsigned i=0; i<2; ++i) // loop over diagonal blocks
  {
    RCP<Teuchos::ParameterList> list = rcp(new Teuchos::ParameterList("precond_list"),true);

    if(params->m_useMLPrecond)
    {
      list->set("Preconditioner Type","ML");
      list->sublist("Preconditioner Types").sublist("ML").set("Base Method Defaults","SA");
      list->sublist("Preconditioner Types").sublist("ML").sublist("ML Settings").set("PDE equations",(i==0?3:1));
      list->sublist("Preconditioner Types").sublist("ML").sublist("ML Settings").set("ML output", 0);
      list->sublist("Preconditioner Types").sublist("ML").sublist("ML Settings").set("aggregation: type","Uncoupled");
      list->sublist("Preconditioner Types").sublist("ML").sublist("ML Settings").set("aggregation: threshold",1e-3);

      if(i==0) // smoother for mechanics block
      {
        list->sublist("Preconditioner Types").sublist("ML").sublist("ML Settings").set("smoother: type","Chebyshev");
        list->sublist("Preconditioner Types").sublist("ML").sublist("ML Settings").set("smoother: sweeps",3);
        list->sublist("Preconditioner Types").sublist("ML").sublist("ML Settings").set("coarse: type","Chebyshev");
        list->sublist("Preconditioner Types").sublist("ML").sublist("ML Settings").set("coarse: sweeps",3);
      }
      else // smoother for flow block
      {
        list->sublist("Preconditioner Types").sublist("ML").sublist("ML Settings").set("smoother: type","Chebyshev");
        list->sublist("Preconditioner Types").sublist("ML").sublist("ML Settings").set("smoother: sweeps",3);
      }

    }
    else // use ILU for both blocks
    {
      list->set("Preconditioner Type","Ifpack");
      list->sublist("Preconditioner Types").sublist("Ifpack").set("Prec Type","ILU");
    }

    Stratimikos::DefaultLinearSolverBuilder builder;
    builder.setParameterList(list);

    RCP<const Thyra::PreconditionerFactoryBase<double> > strategy = createPreconditioningStrategy(builder);
    RCP<Thyra::PreconditionerBase<double> > tmp;

    if(i==0)
      tmp = prec(*strategy,blockDiagOp);
    else
      tmp = prec(*strategy,schurOp);
      //tmp = prec(*strategy,matrix_block[i][i]);

    sub_op[i] = tmp->getUnspecifiedPrecOp();
  }
 

    // create zero operators for off diagonal blocks

  RCP<const Thyra::LinearOpBase<double> > zero_01
    = rcp(new Thyra::DefaultZeroLinearOp<double>(matrix_block[0][0]->range(),
                                                 matrix_block[1][1]->domain()));

  RCP<const Thyra::LinearOpBase<double> > zero_10
    = rcp(new Thyra::DefaultZeroLinearOp<double>(matrix_block[1][1]->range(),
                                                 matrix_block[0][0]->domain()));

    // now build the block preconditioner

  RCP<const Thyra::LinearOpBase<double> > preconditioner;

  if(use_diagonal_prec)
  {
    preconditioner = Thyra::block2x2(sub_op[0],zero_01,zero_10,sub_op[1]);
  }
  else
  {
    RCP<const Thyra::LinearOpBase<double> > eye_00
      = Teuchos::rcp(new Thyra::DefaultIdentityLinearOp<double>(matrix_block[0][0]->range()));

    RCP<const Thyra::LinearOpBase<double> > eye_11
      = Teuchos::rcp(new Thyra::DefaultIdentityLinearOp<double>(matrix_block[1][1]->range()));

    RCP<const Thyra::LinearOpBase<double> > mAinvB1, mB2Ainv;

    mAinvB1 = Thyra::scale(-1.0, Thyra::multiply(sub_op[0],matrix_block[0][1]) );
    mB2Ainv = Thyra::scale(-1.0, Thyra::multiply(matrix_block[1][0],sub_op[0]) );

    RCP<const Thyra::LinearOpBase<double> > Linv,Dinv,Uinv,Eye;

    Linv = Thyra::block2x2(eye_00,zero_01,mB2Ainv,eye_11);
    Dinv = Thyra::block2x2(sub_op[0],zero_01,zero_10,sub_op[1]);
    Uinv = Thyra::block2x2(eye_00,mAinvB1,zero_10,eye_11);

    //preconditioner = Thyra::multiply(Uinv,Dinv);
    //preconditioner = Thyra::multiply(Dinv,Linv);
    preconditioner = Thyra::multiply(Uinv,Dinv,Linv);
  }

  GEOSX_MARK_END(PRECONDITIONER);
  double setupTime = clock.stop();

    // define solver strategy for blocked system. this is
    // similar but slightly different from the sub operator
    // construction, since now we have a user defined preconditioner

  {
    RCP<Teuchos::ParameterList> list = rcp(new Teuchos::ParameterList("list"));
    
      list->set("Linear Solver Type","AztecOO");
      list->set("Preconditioner Type","None"); // will use user-defined P
      list->sublist("Linear Solver Types").sublist("AztecOO").sublist("Forward Solve").set("Max Iterations",params->m_maxIters);
      list->sublist("Linear Solver Types").sublist("AztecOO").sublist("Forward Solve").set("Tolerance",params->m_krylovTol);

      if(use_bicgstab)
        list->sublist("Linear Solver Types").sublist("AztecOO").sublist("Forward Solve").sublist("AztecOO Settings").set("Aztec Solver","BiCGStab");
      else
        list->sublist("Linear Solver Types").sublist("AztecOO").sublist("Forward Solve").sublist("AztecOO Settings").set("Aztec Solver","GMRES");

      if( params->getLogLevel()>=2 )
        list->sublist("Linear Solver Types").sublist("AztecOO").sublist("Forward Solve").sublist("AztecOO Settings").set("Output Frequency",1);

    Stratimikos::DefaultLinearSolverBuilder builder;
    builder.setParameterList(list);

    RCP<const Thyra::LinearOpWithSolveFactoryBase<double> > strategy = createLinearSolveStrategy(builder);
    RCP<Thyra::LinearOpWithSolveBase<double> > solver = strategy->createOp();

    Thyra::initializePreconditionedOp<double>(*strategy,
                                               matrix,
                                               Thyra::rightPrec<double>(preconditioner),
                                               solver.ptr());

    clock.start(true);
    GEOSX_MARK_BEGIN(SOLVER);

      // !!!! Actual Solve !!!!
      Thyra::SolveStatus<double> status = solver->solve(Thyra::NOTRANS,*rhs,lhs.ptr());

    GEOSX_MARK_END(SOLVER);
    double solveTime = clock.stop();
    params->m_numKrylovIter = status.extraParameters->get<int>("Iteration Count");

    if( getLogLevel()>=2 )
    {
      GEOSX_LOG_RANK_0("\t\tLinear Solver | Iter = " << params->m_numKrylovIter <<
                      " | TargetReduction " << params->m_krylovTol <<
                      " | AuxTime " << auxTime <<
                      " | SetupTime " << setupTime <<
                      " | SolveTime " << solveTime );
    }

    p_solution[1]->Scale(m_pressureScaling);
    p_rhs[1]->Scale(1/(m_pressureScaling*m_densityScaling));
  }

  delete schurApproxPP;

  //TODO: remove all this once everything is working
  if( getLogLevel() == 2 )
  {
    /*
    ParallelVector permutedSol;
    ParallelVector const & solution = m_solidSolver->getSystemSolution();
    permutedSol.createWithLocalSize(m_solidSolver->getSystemMatrix().localRows(), MPI_COMM_GEOSX);
    m_permutationMatrix0.multiply(solution, permutedSol);
    permutedSol.close();
    */

    /*
    GEOSX_LOG_RANK_0("***********************************************************");
    GEOSX_LOG_RANK_0("solution0");
    GEOSX_LOG_RANK_0("***********************************************************");
    solution.print(std::cout);
    std::cout<<std::endl;
    MPI_Barrier(MPI_COMM_GEOSX);

    GEOSX_LOG_RANK_0("***********************************************************");
    GEOSX_LOG_RANK_0("solution0");
    GEOSX_LOG_RANK_0("***********************************************************");
    permutedSol.print(std::cout);

    GEOSX_LOG_RANK_0("***********************************************************");
    GEOSX_LOG_RANK_0("solution1");
    GEOSX_LOG_RANK_0("***********************************************************");
    p_solution[1]->Print(std::cout);
    */
  }
}

real64
HydrofractureSolver::ScalingForSystemSolution( DomainPartition const * const domain,
                                                 DofManager const & GEOSX_UNUSED_ARG( dofManager ),
                                                 ParallelVector const & GEOSX_UNUSED_ARG( solution ) )
{
  return m_solidSolver->ScalingForSystemSolution( domain,
                                                  m_solidSolver->getDofManager(),
                                                  m_solidSolver->getSystemSolution() );
}

REGISTER_CATALOG_ENTRY( SolverBase, HydrofractureSolver, std::string const &, Group * const )
} /* namespace geosx */