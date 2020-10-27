/*
 * ------------------------------------------------------------------------------------------------------------
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (c) 2018-2020 Lawrence Livermore National Security LLC
 * Copyright (c) 2018-2020 The Board of Trustees of the Leland Stanford Junior University
 * Copyright (c) 2018-2020 Total, S.A
 * Copyright (c) 2019-     GEOSX Contributors
 * All rights reserved
 *
 * See top level LICENSE, COPYRIGHT, CONTRIBUTORS, NOTICE, and ACKNOWLEDGEMENTS files for details.
 * ------------------------------------------------------------------------------------------------------------
 */

#ifndef GEOSX_RAJAINTERFACE_RAJAINTERFACE_HPP
#define GEOSX_RAJAINTERFACE_RAJAINTERFACE_HPP

// Source includes
#include "common/DataTypes.hpp"

// TPL includes
#include <RAJA/RAJA.hpp>

namespace geosx
{


using serialPolicy = RAJA::loop_exec;
using serialReduce = RAJA::seq_reduce;
using serialAtomic = RAJA::seq_atomic;

#if defined(GEOSX_USE_OPENMP)

using parallelHostPolicy = RAJA::omp_parallel_for_exec;
using parallelHostReduce = RAJA::omp_reduce;
using parallelHostAtomic = RAJA::builtin_atomic;

#else

using parallelHostPolicy = serialPolicy;
using parallelHostReduce = serialReduce;
using parallelHostAtomic = serialAtomic;

#endif

using parallelDeviceEvent = RAJA::resources::Event;

#if defined(GEOSX_USE_CUDA)

template< unsigned long BLOCK_SIZE = 256 >
using parallelDevicePolicy = RAJA::cuda_exec< BLOCK_SIZE >;
template< unsigned long BLOCK_SIZE = 256 >
using parallelDeviceAsync = RAJA::cuda_exec_async< BLOCK_SIZE >;
using parallelDeviceReduce = RAJA::cuda_reduce;
using parallelDeviceAtomic = RAJA::cuda_atomic;

using parallelDeviceStream = RAJA::resources::Cuda;

#else

template< unsigned long BLOCK_SIZE = 0 >
using parallelDevicePolicy = parallelHostPolicy;
template< unsigned long BLOCK_SIZE = 0 >
using parallelDeviceAsync = parallelHostPolicy;
using parallelDeviceReduce = parallelHostReduce;
using parallelDeviceAtomic = parallelHostAtomic;

using parallelDeviceStream = RAJA::resources::Host;

#endif

namespace internalRajaInterface
{
template< typename >
struct PolicyMap;

template<>
struct PolicyMap< serialPolicy >
{
  using atomic = serialAtomic;
  using reduce = serialReduce;
};

#if defined(GEOSX_USE_OPENMP)
template<>
struct PolicyMap< RAJA::omp_parallel_for_exec >
{
  using atomic = RAJA::builtin_atomic;
  using reduce = RAJA::omp_reduce;
};
#endif

#if defined(GEOSX_USE_CUDA)
template< unsigned long BLOCK_SIZE >
struct PolicyMap< RAJA::cuda_exec< BLOCK_SIZE > >
{
  using atomic = RAJA::cuda_atomic;
  using reduce = RAJA::cuda_reduce;
};
#endif
}


template< typename POLICY >
using ReducePolicy = typename internalRajaInterface::PolicyMap< POLICY >::reduce;

template< typename POLICY >
using AtomicPolicy = typename internalRajaInterface::PolicyMap< POLICY >::atomic;



template< typename POLICY, typename LAMBDA >
RAJA_INLINE void forAll( const localIndex end, LAMBDA && body )
{
  RAJA::forall< POLICY >( RAJA::TypedRangeSegment< localIndex >( 0, end ), std::forward< LAMBDA >( body ) );
}

template< typename POLICY, typename RESOURCE, typename LAMBDA >
RAJA_INLINE parallelDeviceEvent forAll( RESOURCE && stream, const localIndex end, LAMBDA && body )
{
  return RAJA::forall< POLICY >( std::forward< RESOURCE >( stream ),
                                 RAJA::TypedRangeSegment< localIndex >( 0, end ),
                                 std::forward< LAMBDA >( body ) );
}

} // namespace geosx

#endif // GEOSX_RAJAINTERFACE_RAJAINTERFACE_HPP
