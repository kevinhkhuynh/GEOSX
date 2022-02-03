/*
 * ------------------------------------------------------------------------------------------------------------
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (c) 2018-2020 Lawrence Livermore National Security LLC
 * Copyright (c) 2018-2020 The Board of Trustees of the Leland Stanford Junior University
 * Copyright (c) 2018-2020 TotalEnergies
 * Copyright (c) 2019-     GEOSX Contributors
 * All rights reserved
 *
 * See top level LICENSE, COPYRIGHT, CONTRIBUTORS, NOTICE, and ACKNOWLEDGEMENTS files for details.
 * ------------------------------------------------------------------------------------------------------------
 */

#include "constitutive/fluid/MultiFluidBase.hpp"
#include "finiteVolume/FiniteVolumeManager.hpp"
#include "finiteVolume/FluxApproximationBase.hpp"
#include "mainInterface/initialization.hpp"
#include "mainInterface/GeosxState.hpp"
#include "physicsSolvers/PhysicsSolverManager.hpp"
#include "physicsSolvers/fluidFlow/CompositionalMultiphaseBaseExtrinsicData.hpp"
#include "physicsSolvers/fluidFlow/CompositionalMultiphaseFVM.hpp"
#include "physicsSolvers/fluidFlow/FlowSolverBaseExtrinsicData.hpp"
#include "unitTests/fluidFlowTests/testCompFlowUtils.hpp"

using namespace geosx;
using namespace geosx::dataRepository;
using namespace geosx::constitutive;
using namespace geosx::constitutive::multifluid;
using namespace geosx::testing;

CommandLineOptions g_commandLineOptions;

// Sphinx start after input XML

char const * xmlInput =
  "<Problem>\n"
  "  <Solvers gravityVector=\"{ 0.0, 0.0, -9.81 }\">\n"
  "    <CompositionalMultiphaseFVM name=\"compflow\"\n"
  "                                 logLevel=\"0\"\n"
  "                                 discretization=\"fluidTPFA\"\n"
  "                                 targetRegions=\"{region}\"\n"
  "                                 fluidNames=\"{fluid}\"\n"
  "                                 solidNames=\"{rock}\"\n"
  "                                 permeabilityNames=\"{rockPerm}\"\n"
  "                                 relPermNames=\"{relperm}\"\n"
  "                                 capPressureNames=\"{cappressure}\"\n"
  "                                 temperature=\"297.15\"\n"
  "                                 useMass=\"1\">\n"
  "                                 \n"
  "      <NonlinearSolverParameters newtonTol=\"1.0e-6\"\n"
  "                                 newtonMaxIter=\"2\"/>\n"
  "      <LinearSolverParameters solverType=\"gmres\"\n"
  "                              krylovTol=\"1.0e-10\"/>\n"
  "    </CompositionalMultiphaseFVM>\n"
  "  </Solvers>\n"
  "  <Mesh>\n"
  "    <InternalMesh name=\"mesh\"\n"
  "                  elementTypes=\"{C3D8}\" \n"
  "                  xCoords=\"{0, 3}\"\n"
  "                  yCoords=\"{0, 1}\"\n"
  "                  zCoords=\"{0, 1}\"\n"
  "                  nx=\"{3}\"\n"
  "                  ny=\"{1}\"\n"
  "                  nz=\"{1}\"\n"
  "                  cellBlockNames=\"{cb1}\"/>\n"
  "  </Mesh>\n"
  "  <NumericalMethods>\n"
  "    <FiniteVolume>\n"
  "      <TwoPointFluxApproximation name=\"fluidTPFA\"\n"
  "                                 fieldName=\"pressure\"\n"
  "                                 coefficientName=\"permeability\"\n"
  "                                 coefficientModelNames=\"{rockPerm}\"/>\n"
  "    </FiniteVolume>\n"
  "  </NumericalMethods>\n"
  "  <ElementRegions>\n"
  "    <CellElementRegion name=\"region\" cellBlocks=\"{cb1}\" materialList=\"{fluid, rock, relperm, cappressure}\" />\n"
  "  </ElementRegions>\n"
  "  <Constitutive>\n"
  "    <CompositionalMultiphaseFluid name=\"fluid\"\n"
  "                                  phaseNames=\"{oil, gas}\"\n"
  "                                  equationsOfState=\"{PR, PR}\"\n"
  "                                  componentNames=\"{N2, C10, C20, H2O}\"\n"
  "                                  componentCriticalPressure=\"{34e5, 25.3e5, 14.6e5, 220.5e5}\"\n"
  "                                  componentCriticalTemperature=\"{126.2, 622.0, 782.0, 647.0}\"\n"
  "                                  componentAcentricFactor=\"{0.04, 0.443, 0.816, 0.344}\"\n"
  "                                  componentMolarWeight=\"{28e-3, 134e-3, 275e-3, 18e-3}\"\n"
  "                                  componentVolumeShift=\"{0, 0, 0, 0}\"\n"
  "                                  componentBinaryCoeff=\"{ {0, 0, 0, 0},\n"
  "                                                          {0, 0, 0, 0},\n"
  "                                                          {0, 0, 0, 0},\n"
  "                                                          {0, 0, 0, 0} }\"/>\n"
  "    <CompressibleSolidConstantPermeability name=\"rock\"\n"
  "        solidModelName=\"nullSolid\"\n"
  "        porosityModelName=\"rockPorosity\"\n"
  "        permeabilityModelName=\"rockPerm\"/>\n"
  "   <NullModel name=\"nullSolid\"/> \n"
  "   <PressurePorosity name=\"rockPorosity\"\n"
  "                     defaultReferencePorosity=\"0.05\"\n"
  "                     referencePressure = \"0.0\"\n"
  "                     compressibility=\"1.0e-9\"/>\n"
  "    <BrooksCoreyRelativePermeability name=\"relperm\"\n"
  "                                     phaseNames=\"{oil, gas}\"\n"
  "                                     phaseMinVolumeFraction=\"{0.1, 0.15}\"\n"
  "                                     phaseRelPermExponent=\"{2.0, 2.0}\"\n"
  "                                     phaseRelPermMaxValue=\"{0.8, 0.9}\"/>\n"
  "    <BrooksCoreyCapillaryPressure name=\"cappressure\"\n"
  "                                  phaseNames=\"{oil, gas}\"\n"
  "                                  phaseMinVolumeFraction=\"{0.2, 0.05}\"\n"
  "                                  phaseCapPressureExponentInv=\"{4.25, 3.5}\"\n"
  "                                  phaseEntryPressure=\"{0., 1e8}\"\n"
  "                                  capPressureEpsilon=\"0.0\"/> \n"
  "  <ConstantPermeability name=\"rockPerm\"\n"
  "                        permeabilityComponents=\"{2.0e-16, 2.0e-16, 2.0e-16}\"/> \n"
  "  </Constitutive>\n"
  "  <FieldSpecifications>\n"
  "    <FieldSpecification name=\"initialPressure\"\n"
  "               initialCondition=\"1\"\n"
  "               setNames=\"{all}\"\n"
  "               objectPath=\"ElementRegions/region/cb1\"\n"
  "               fieldName=\"pressure\"\n"
  "               functionName=\"initialPressureFunc\"\n"
  "               scale=\"5e6\"/>\n"
  "    <FieldSpecification name=\"initialComposition_N2\"\n"
  "               initialCondition=\"1\"\n"
  "               setNames=\"{all}\"\n"
  "               objectPath=\"ElementRegions/region/cb1\"\n"
  "               fieldName=\"globalCompFraction\"\n"
  "               component=\"0\"\n"
  "               scale=\"0.099\"/>\n"
  "    <FieldSpecification name=\"initialComposition_C10\"\n"
  "               initialCondition=\"1\"\n"
  "               setNames=\"{all}\"\n"
  "               objectPath=\"ElementRegions/region/cb1\"\n"
  "               fieldName=\"globalCompFraction\"\n"
  "               component=\"1\"\n"
  "               scale=\"0.3\"/>\n"
  "    <FieldSpecification name=\"initialComposition_C20\"\n"
  "               initialCondition=\"1\"\n"
  "               setNames=\"{all}\"\n"
  "               objectPath=\"ElementRegions/region/cb1\"\n"
  "               fieldName=\"globalCompFraction\"\n"
  "               component=\"2\"\n"
  "               scale=\"0.6\"/>\n"
  "    <FieldSpecification name=\"initialComposition_H20\"\n"
  "               initialCondition=\"1\"\n"
  "               setNames=\"{all}\"\n"
  "               objectPath=\"ElementRegions/region/cb1\"\n"
  "               fieldName=\"globalCompFraction\"\n"
  "               component=\"3\"\n"
  "               scale=\"0.001\"/>\n"
  "  </FieldSpecifications>\n"
  "  <Functions>\n"
  "    <TableFunction name=\"initialPressureFunc\"\n"
  "                   inputVarNames=\"{elementCenter}\"\n"
  "                   coordinates=\"{0.0, 3.0}\"\n"
  "                   values=\"{1.0, 0.5}\"/>\n"
  "  </Functions>"
  "</Problem>";

// Sphinx end before input XML

void testCompositionNumericalDerivatives( CompositionalMultiphaseFVM & solver,
                                          DomainPartition & domain,
                                          real64 const perturbParameter,
                                          real64 const relTol )
{
  localIndex const NC = solver.numFluidComponents();


  solver.forMeshTargets( domain.getMeshBodies(),
                         [&]( string const,
                              MeshLevel & mesh,
                              arrayView1d< string const > const & regionNames )
  {
    ElementRegionManager & elementRegionManager = mesh.getElemManager();
    elementRegionManager.forElementSubRegions( regionNames,
                                               [&]( localIndex const,
                                                    ElementSubRegionBase & subRegion )
    {
      SCOPED_TRACE( subRegion.getParent().getParent().getName() + "/" + subRegion.getName() );

      string const & fluidName = subRegion.getReference< string >( CompositionalMultiphaseFVM::viewKeyStruct::fluidNamesString() );
      MultiFluidBase const & fluid = subRegion.getConstitutiveModel< MultiFluidBase >( fluidName );
      arrayView1d< string const > const & components = fluid.componentNames();

      arrayView2d< real64, compflow::USD_COMP > & compDens =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::globalCompDensity >();

      arrayView2d< real64, compflow::USD_COMP > & dCompDens =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::deltaGlobalCompDensity >();

      arrayView2d< real64, compflow::USD_COMP > & compFrac =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::globalCompFraction >();

      arrayView3d< real64, compflow::USD_COMP_DC > & dCompFrac_dCompDens =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::dGlobalCompFraction_dGlobalCompDensity >();

      // reset the solver state to zero out variable updates
      solver.resetStateToBeginningOfStep( domain );

      // make a copy of unperturbed values of component fractions
      array2d< real64, compflow::LAYOUT_COMP > compFracOrig( subRegion.size(), NC );
      compFracOrig.setValues< serialPolicy >( compFrac );

      // update component density and check derivatives
      for( localIndex jc = 0; jc < NC; ++jc )
      {
        // reset the solver state to zero out variable updates (resetting the whole domain is overkill...)
        solver.resetStateToBeginningOfStep( domain );

        // perturb a single component density in each cell
        forAll< serialPolicy >( subRegion.size(), [=] ( localIndex const ei )
        {
          real64 const dRho = perturbParameter * ( compDens[ei][jc] + perturbParameter );
          dCompDens[ei][jc] = dRho;
        } );

        // recompute component fractions
        solver.updateComponentFraction( subRegion );

        // check values in each cell
        forAll< serialPolicy >( subRegion.size(), [=] ( localIndex const ei )
        {
          SCOPED_TRACE( "Element " + std::to_string( ei ) );

          auto dZ_dRho = invertLayout( dCompFrac_dCompDens[ei].toSliceConst(), NC, NC );
          string var = "compDens[" + components[jc] + "]";

          checkDerivative( compFrac[ei].toSliceConst(),
                           compFracOrig[ei].toSliceConst(),
                           dZ_dRho[jc].toSliceConst(),
                           dCompDens[ei][jc],
                           relTol,
                           "compFrac",
                           var,
                           components );
        } );
      }
    } );
  } );
}


void testPhaseVolumeFractionNumericalDerivatives( CompositionalMultiphaseFVM & solver,
                                                  DomainPartition & domain,
                                                  real64 const perturbParameter,
                                                  real64 const relTol )
{
  localIndex const NC = solver.numFluidComponents();
  localIndex const NP = solver.numFluidPhases();

  solver.forMeshTargets( domain.getMeshBodies(),
                         [&]( string const,
                              MeshLevel & mesh,
                              arrayView1d< string const > const & regionNames )
  {
    ElementRegionManager & elementRegionManager = mesh.getElemManager();
    elementRegionManager.forElementSubRegions( regionNames,
                                               [&]( localIndex const,
                                                    ElementSubRegionBase & subRegion )
    {
      SCOPED_TRACE( subRegion.getParent().getParent().getName() + "/" + subRegion.getName() );

      string const & fluidName = subRegion.getReference< string >( CompositionalMultiphaseFVM::viewKeyStruct::fluidNamesString() );
      MultiFluidBase const & fluid = subRegion.getConstitutiveModel< MultiFluidBase >( fluidName );
      arrayView1d< string const > const & components = fluid.componentNames();
      arrayView1d< string const > const & phases = fluid.phaseNames();

      arrayView1d< real64 > & pres =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::pressure >();

      arrayView1d< real64 > & dPres =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::deltaPressure >();

      arrayView2d< real64, compflow::USD_COMP > & compDens =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::globalCompDensity >();

      arrayView2d< real64, compflow::USD_COMP > & dCompDens =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::deltaGlobalCompDensity >();

      arrayView2d< real64, compflow::USD_PHASE > & phaseVolFrac =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::phaseVolumeFraction >();

      arrayView2d< real64, compflow::USD_PHASE > & dPhaseVolFrac_dPres =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::dPhaseVolumeFraction_dPressure >();

      arrayView3d< real64, compflow::USD_PHASE_DC > & dPhaseVolFrac_dCompDens =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::dPhaseVolumeFraction_dGlobalCompDensity >();

      // reset the solver state to zero out variable updates
      solver.resetStateToBeginningOfStep( domain );

      // make a copy of unperturbed values of component fractions
      array2d< real64, compflow::LAYOUT_PHASE > phaseVolFracOrig( subRegion.size(), NP );
      phaseVolFracOrig.setValues< serialPolicy >( phaseVolFrac );

      // update pressure and check derivatives
      {
        // perturb pressure in each cell
        forAll< serialPolicy >( subRegion.size(), [=] ( localIndex const ei )
        {
          real64 const dP = perturbParameter * ( pres[ei] + perturbParameter );
          dPres[ei] = dP;
        } );

        // recompute component fractions
        solver.updateFluidState( subRegion );

        // check values in each cell
        forAll< serialPolicy >( subRegion.size(), [=, &phaseVolFracOrig] ( localIndex const ei )
        {
          SCOPED_TRACE( "Element " + std::to_string( ei ) );

          checkDerivative( phaseVolFrac[ei].toSliceConst(),
                           phaseVolFracOrig[ei].toSliceConst(),
                           dPhaseVolFrac_dPres[ei].toSliceConst(),
                           dPres[ei],
                           relTol,
                           "phaseVolFrac",
                           "Pres",
                           phases );
        } );
      }

      // update component density and check derivatives
      for( localIndex jc = 0; jc < NC; ++jc )
      {
        // reset the solver state to zero out variable updates (resetting the whole domain is overkill...)
        solver.resetStateToBeginningOfStep( domain );

        // perturb a single component density in each cell
        forAll< serialPolicy >( subRegion.size(), [=] ( localIndex const ei )
        {
          real64 const dRho = perturbParameter * ( compDens[ei][jc] + perturbParameter );
          dCompDens[ei][jc] = dRho;
        } );

        // recompute component fractions
        solver.updateFluidState( subRegion );

        // check values in each cell
        forAll< serialPolicy >( subRegion.size(), [=, &phaseVolFracOrig] ( localIndex const ei )
        {
          SCOPED_TRACE( "Element " + std::to_string( ei ) );

          auto dS_dRho = invertLayout( dPhaseVolFrac_dCompDens[ei].toSliceConst(), NP, NC );
          string var = "compDens[" + components[jc] + "]";

          checkDerivative( phaseVolFrac[ei].toSliceConst(),
                           phaseVolFracOrig[ei].toSliceConst(),
                           dS_dRho[jc].toSliceConst(),
                           dCompDens[ei][jc],
                           relTol,
                           "phaseVolFrac",
                           var,
                           phases );
        } );
      }
    } );
  } );
}

void testPhaseMobilityNumericalDerivatives( CompositionalMultiphaseFVM & solver,
                                            DomainPartition & domain,
                                            real64 const perturbParameter,
                                            real64 const relTol )
{
  localIndex const NC = solver.numFluidComponents();
  localIndex const NP = solver.numFluidPhases();

  solver.forMeshTargets( domain.getMeshBodies(),
                         [&]( string const,
                              MeshLevel & mesh,
                              arrayView1d< string const > const & regionNames )
  {
    ElementRegionManager & elementRegionManager = mesh.getElemManager();
    elementRegionManager.forElementSubRegions( regionNames,
                                               [&]( localIndex const,
                                                    ElementSubRegionBase & subRegion )
    {
      SCOPED_TRACE( subRegion.getParent().getParent().getName() + "/" + subRegion.getName() );

      string const & fluidName = subRegion.getReference< string >( CompositionalMultiphaseFVM::viewKeyStruct::fluidNamesString() );
      MultiFluidBase const & fluid = subRegion.getConstitutiveModel< MultiFluidBase >( fluidName );
      arrayView1d< string const > const & components = fluid.componentNames();
      arrayView1d< string const > const & phases = fluid.phaseNames();

      arrayView1d< real64 > & pres =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::pressure >();

      arrayView1d< real64 > & dPres =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::deltaPressure >();

      arrayView2d< real64, compflow::USD_COMP > & compDens =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::globalCompDensity >();

      arrayView2d< real64, compflow::USD_COMP > & dCompDens =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::deltaGlobalCompDensity >();

      arrayView2d< real64, compflow::USD_PHASE > & phaseMob =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::phaseMobility >();

      arrayView2d< real64, compflow::USD_PHASE > & dPhaseMob_dPres =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::dPhaseMobility_dPressure >();

      arrayView3d< real64, compflow::USD_PHASE_DC > & dPhaseMob_dCompDens =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::dPhaseMobility_dGlobalCompDensity >();

      // reset the solver state to zero out variable updates
      solver.resetStateToBeginningOfStep( domain );

      // make a copy of unperturbed values of component fractions
      array2d< real64, compflow::LAYOUT_PHASE > phaseVolFracOrig( subRegion.size(), NP );
      phaseVolFracOrig.setValues< serialPolicy >( phaseMob );

      // update pressure and check derivatives
      {
        // perturb pressure in each cell
        forAll< serialPolicy >( subRegion.size(), [=] ( localIndex const ei )
        {
          real64 const dP = perturbParameter * ( pres[ei] + perturbParameter );
          dPres[ei] = dP;
        } );

        // recompute component fractions
        solver.updateFluidState( subRegion );

        // check values in each cell
        forAll< serialPolicy >( subRegion.size(), [=, &phaseVolFracOrig] ( localIndex const ei )
        {
          SCOPED_TRACE( "Element " + std::to_string( ei ) );

          checkDerivative( phaseMob[ei].toSliceConst(),
                           phaseVolFracOrig[ei].toSliceConst(),
                           dPhaseMob_dPres[ei].toSliceConst(),
                           dPres[ei],
                           relTol,
                           "phaseVolFrac",
                           "Pres",
                           phases );
        } );
      }

      // update component density and check derivatives
      for( localIndex jc = 0; jc < NC; ++jc )
      {
        // reset the solver state to zero out variable updates (resetting the whole domain is overkill...)
        solver.resetStateToBeginningOfStep( domain );

        // perturb a single component density in each cell
        forAll< serialPolicy >( subRegion.size(), [=] ( localIndex const ei )
        {
          real64 const dRho = perturbParameter * ( compDens[ei][jc] + perturbParameter );
          dCompDens[ei][jc] = dRho;
        } );

        // recompute component fractions
        solver.updateFluidState( subRegion );

        // check values in each cell
        forAll< serialPolicy >( subRegion.size(), [=, &phaseVolFracOrig] ( localIndex const ei )
        {
          SCOPED_TRACE( "Element " + std::to_string( ei ) );

          auto dS_dRho = invertLayout( dPhaseMob_dCompDens[ei].toSliceConst(), NP, NC );
          string var = "compDens[" + components[jc] + "]";

          checkDerivative( phaseMob[ei].toSliceConst(),
                           phaseVolFracOrig[ei].toSliceConst(),
                           dS_dRho[jc].toSliceConst(),
                           dCompDens[ei][jc],
                           relTol,
                           "phaseMob",
                           var,
                           phases );
        } );
      }
    } );
  } );
}

template< typename LAMBDA >
void testNumericalJacobian( CompositionalMultiphaseFVM & solver,
                            DomainPartition & domain,
                            real64 const perturbParameter,
                            real64 const relTol,
                            LAMBDA assembleFunction )
{
  localIndex const NC = solver.numFluidComponents();

  CRSMatrix< real64, globalIndex > const & jacobian = solver.getLocalMatrix();
  array1d< real64 > residual( jacobian.numRows() );
  DofManager const & dofManager = solver.getDofManager();

  // assemble the analytical residual
  solver.resetStateToBeginningOfStep( domain );

  residual.zero();
  jacobian.zero();

  assembleFunction( jacobian.toViewConstSizes(), residual.toView() );
  residual.move( LvArray::MemorySpace::host, false );

  // copy the analytical residual
  array1d< real64 > residualOrig( residual );

  // create the numerical jacobian
  jacobian.move( LvArray::MemorySpace::host );
  CRSMatrix< real64, globalIndex > jacobianFD( jacobian );
  jacobianFD.zero();

  string const dofKey = dofManager.getKey( CompositionalMultiphaseFVM::viewKeyStruct::elemDofFieldString() );

  solver.forMeshTargets( domain.getMeshBodies(),
                         [&]( string const,
                              MeshLevel & mesh,
                              arrayView1d< string const > const & regionNames )
  {
    ElementRegionManager & elementRegionManager = mesh.getElemManager();
    elementRegionManager.forElementSubRegions( regionNames,
                                               [&]( localIndex const,
                                                    ElementSubRegionBase & subRegion )
    {
      arrayView1d< integer const > const & elemGhostRank = subRegion.ghostRank();

      arrayView1d< globalIndex const > const & dofNumber =
        subRegion.getReference< array1d< globalIndex > >( dofKey );

      arrayView1d< real64 const > const & pres =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::pressure >();
      pres.move( LvArray::MemorySpace::host, false );

      arrayView1d< real64 > const & dPres =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::deltaPressure >();

      arrayView2d< real64 const, compflow::USD_COMP > const & compDens =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::globalCompDensity >();
      compDens.move( LvArray::MemorySpace::host, false );

      arrayView2d< real64, compflow::USD_COMP > const & dCompDens =
        subRegion.getExtrinsicData< extrinsicMeshData::flow::deltaGlobalCompDensity >();

      for( localIndex ei = 0; ei < subRegion.size(); ++ei )
      {
        if( elemGhostRank[ei] >= 0 )
        {
          continue;
        }

        real64 totalDensity = 0.0;
        for( localIndex ic = 0; ic < NC; ++ic )
        {
          totalDensity += compDens[ei][ic];
        }

        {
          solver.resetStateToBeginningOfStep( domain );

          real64 const dP = perturbParameter * ( pres[ei] + perturbParameter );
          dPres.move( LvArray::MemorySpace::host, true );
          dPres[ei] = dP;
#if defined(GEOSX_USE_CUDA)
          dPres.move( LvArray::MemorySpace::cuda, false );
#endif


          solver.forMeshTargets( domain.getMeshBodies(),
                                 [&]( string const,
                                      MeshLevel & mesh,
                                      arrayView1d< string const > const & regionNames )
          {
            ElementRegionManager & elementRegionManager = mesh.getElemManager();
            elementRegionManager.forElementSubRegions( regionNames,
                                                       [&]( localIndex const,
                                                            ElementSubRegionBase & subRegion2 )
            {
              solver.updateFluidState( subRegion2 );
            } );
          } );

          residual.zero();
          jacobian.zero();
          assembleFunction( jacobian.toViewConstSizes(), residual.toView() );

          fillNumericalJacobian( residual.toViewConst(),
                                 residualOrig.toViewConst(),
                                 dofNumber[ei],
                                 dP,
                                 jacobianFD.toViewConstSizes() );
        }

        for( localIndex jc = 0; jc < NC; ++jc )
        {
          solver.resetStateToBeginningOfStep( domain );

          real64 const dRho = perturbParameter * totalDensity;
          dCompDens.move( LvArray::MemorySpace::host, true );
          dCompDens[ei][jc] = dRho;

          solver.forMeshTargets( domain.getMeshBodies(),
                                 [&]( string const,
                                      MeshLevel & mesh,
                                      arrayView1d< string const > const & regionNames )
          {
            ElementRegionManager & elementRegionManager = mesh.getElemManager();
            elementRegionManager.forElementSubRegions( regionNames,
                                                       [&]( localIndex const,
                                                            ElementSubRegionBase & subRegion2 )
            {
              solver.updateFluidState( subRegion2 );
            } );
          } );

          residual.zero();
          jacobian.zero();
          assembleFunction( jacobian.toViewConstSizes(), residual.toView() );

          fillNumericalJacobian( residual.toViewConst(),
                                 residualOrig.toViewConst(),
                                 dofNumber[ei] + jc + 1,
                                 dRho,
                                 jacobianFD.toViewConstSizes() );
        }
      }
    } );
  } );

  // assemble the analytical jacobian
  solver.resetStateToBeginningOfStep( domain );

  residual.zero();
  jacobian.zero();
  assembleFunction( jacobian.toViewConstSizes(), residual.toView() );

  compareLocalMatrices( jacobian.toViewConst(), jacobianFD.toViewConst(), relTol );
}

class CompositionalMultiphaseFlowTest : public ::testing::Test
{
public:

  CompositionalMultiphaseFlowTest():
    state( std::make_unique< CommandLineOptions >( g_commandLineOptions ) )
  {}

protected:

  void SetUp() override
  {
    setupProblemFromXML( state.getProblemManager(), xmlInput );
    solver = &state.getProblemManager().getPhysicsSolverManager().getGroup< CompositionalMultiphaseFVM >( "compflow" );

    DomainPartition & domain = state.getProblemManager().getDomainPartition();

    solver->setupSystem( domain,
                         solver->getDofManager(),
                         solver->getLocalMatrix(),
                         solver->getSystemRhs(),
                         solver->getSystemSolution() );

    solver->implicitStepSetup( time, dt, domain );
  }

  static real64 constexpr time = 0.0;
  static real64 constexpr dt = 1e4;
  static real64 constexpr eps = std::numeric_limits< real64 >::epsilon();

  GeosxState state;
  CompositionalMultiphaseFVM * solver;
};

real64 constexpr CompositionalMultiphaseFlowTest::time;
real64 constexpr CompositionalMultiphaseFlowTest::dt;
real64 constexpr CompositionalMultiphaseFlowTest::eps;

TEST_F( CompositionalMultiphaseFlowTest, derivativeNumericalCheck_composition )
{
  real64 const perturb = std::sqrt( eps );
  real64 const tol = 1e-4;

  DomainPartition & domain = state.getProblemManager().getDomainPartition();

  testCompositionNumericalDerivatives( *solver, domain, perturb, tol );
}

TEST_F( CompositionalMultiphaseFlowTest, derivativeNumericalCheck_phaseVolumeFraction )
{
  real64 const perturb = std::sqrt( eps );
  real64 const tol = 5e-2; // 5% error margin

  DomainPartition & domain = state.getProblemManager().getDomainPartition();
  testPhaseVolumeFractionNumericalDerivatives( *solver, domain, perturb, tol );
}

TEST_F( CompositionalMultiphaseFlowTest, derivativeNumericalCheck_phaseMobility )
{
  real64 const perturb = std::sqrt( eps );
  real64 const tol = 5e-2; // 5% error margin

  DomainPartition & domain = state.getProblemManager().getDomainPartition();

  testPhaseMobilityNumericalDerivatives( *solver, domain, perturb, tol );
}

TEST_F( CompositionalMultiphaseFlowTest, jacobianNumericalCheck_flux )
{
  real64 const perturb = std::sqrt( eps );
  real64 const tol = 1e-1; // 10% error margin

  DomainPartition & domain = state.getProblemManager().getDomainPartition();

  testNumericalJacobian( *solver, domain, perturb, tol,
                         [&] ( CRSMatrixView< real64, globalIndex const > const & localMatrix,
                               arrayView1d< real64 > const & localRhs )
  {
    solver->assembleFluxTerms( dt, domain, solver->getDofManager(), localMatrix, localRhs );
  } );
}

/*
 * Accumulation numerical test not passing due to some numerical catastrophic cancellation
 * happenning in the kernel for the particular set of initial conditions we're running.
 * The test should be re-enabled and fixed at some point.
 */
#if 0
TEST_F( CompositionalMultiphaseFlowTest, jacobianNumericalCheck_accumulationVolumeBalance )
{
  real64 const perturb = sqrt( eps );
  real64 const tol = 1e-1; // 10% error margin

  DomainPartition & domain = state.getProblemManager().getDomainPartition();

  testNumericalJacobian( *solver, domain, perturb, tol,
                         [&] ( CRSMatrixView< real64, globalIndex const > const & localMatrix,
                               arrayView1d< real64 > const & localRhs )
  {
    solver->assembleAccumulationAndVolumeBalanceTerms( domain, solver->getDofManager(), localMatrix, localRhs );
  } );
}
#endif

int main( int argc, char * * argv )
{
  ::testing::InitGoogleTest( &argc, argv );
  g_commandLineOptions = *geosx::basicSetup( argc, argv );
//  chai::ArrayManager::getInstance()->disableCallbacks();
  int const result = RUN_ALL_TESTS();
  geosx::basicCleanup();
  return result;
}
