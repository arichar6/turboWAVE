#ifndef _did_definitions

/////////////////////////////////////////////////////////////////////
//                                                                 //
// This file contains definitions of basic data types and provides //
// a generalized interface for platform specific features.         //
// To compile on a specific platform comment/uncomment the define  //
// directives immediately below.                                   //
//                                                                 //
/////////////////////////////////////////////////////////////////////

#define USE_DESKTOP
//#define USE_CRAY

/////////////////////////////////////////////////////////////////////
//                                                                 //
// Uncomment the following to enable OpenCL or OpenMP              //
// This is used mainly for GPGPU or MIC acceleration               //
//                                                                 //
/////////////////////////////////////////////////////////////////////

//#define USE_OPENCL
#define USE_OPENMP

/////////////////////////////////////////////////////////////////////
//                                                                 //
// Coordinate system is now fully controllable from input file.    //
//                                                                 //
/////////////////////////////////////////////////////////////////////

#include <stdint.h>
#include <memory.h>
#include <cmath>
#include <complex>
#include <iostream>
#include <fstream>
#include <sstream>
#include <ctime>
#include <vector>
#include <valarray>
#include <algorithm>
#include <limits>
#include <exception>
#include <string>
#ifdef USE_OPENMP
#include <omp.h>
#endif

namespace tw
{
	typedef double Float;
	typedef int32_t Int;
	typedef uint32_t Uint;
	typedef std::complex<tw::Float> Complex;
	static const tw::Int cache_align_bytes = 64;
	static const tw::Int vec_align_bytes = 64; // if not matched to hardware can lead to failures
	static const tw::Int max_bundle_size = 16; // must be multiple of vec_align_bytes / 4
	static const tw::Float small_neg = -1e9*std::numeric_limits<tw::Float>::min();
	static const tw::Float small_pos = 1e9*std::numeric_limits<tw::Float>::min();
	static const tw::Float big_neg = -1e-9*std::numeric_limits<tw::Float>::max();
	static const tw::Float big_pos = 1e-9*std::numeric_limits<tw::Float>::max();
	class FatalError : public std::exception
	{
		char messg[256];
		public:
		FatalError(const std::string& s) { strcpy(messg,s.c_str()); }
		virtual const char* what() const throw()
		{
			return messg;
		}
	};
}
// Define a strongly typed boolean for better type checking in some situations
enum class strongbool { yes , no };

static const tw::Float pi = 3.1415926535897932385;
static const tw::Complex ii = tw::Complex(0,1);
// The standard library doesn't allow binary operands to be std::complex<float> and double.
// This presents problems due to the fact that all floating point literals are doubles.
// Therefore, we define some trivial constants for convenience:
static const tw::Float half = 0.5;
static const tw::Float one = 1.0;
static const tw::Float two = 2.0;
static const tw::Float root2 = std::sqrt(two);
namespace mks
{
	static const tw::Float c=2.9979e8,qe=1.6022e-19,me=9.1094e-31,eps0=8.8542e-12,kB=1.3807e-23,hbar=1.0546e-34;
}

/////////////////////////////////
//                             //
//  ITEMS FOR OpenMP PROGRAMS  //
//                             //
/////////////////////////////////

#ifdef USE_OPENMP
namespace tw
{
	inline tw::Int GetOMPThreadNum() { return omp_get_thread_num(); }
	inline tw::Int GetOMPNumThreads() { return omp_get_num_threads(); }
	inline tw::Int GetOMPMaxThreads() { return omp_get_max_threads(); }
	inline void GetOMPTaskLoopRange(tw::Int task_id,tw::Int num,tw::Int num_tasks,tw::Int *first,tw::Int *last)
	{
		tw::Int locNum = num / num_tasks;
		*first = task_id * locNum;
		*last = *first + locNum - 1;
		*last += task_id==num_tasks-1 ? num % num_tasks : 0;
	}
}
#else
namespace tw
{
	inline tw::Int GetOMPThreadNum() { return 0; }
	inline tw::Int GetOMPNumThreads() { return 1; }
	inline tw::Int GetOMPMaxThreads() { return 1; }
	inline void GetOMPTaskLoopRange(tw::Int task_id,tw::Int num,tw::Int num_tasks,tw::Int *first,tw::Int *last)
	{
		// If no OpenMP, still split tasks for serial execution, if requested.
		// To suppress loop splitting in serial code, must pass in task_id=0 and num_tasks=1.
		tw::Int locNum = num / num_tasks;
		*first = task_id * locNum;
		*last = *first + locNum - 1;
		*last += task_id==num_tasks-1 ? num % num_tasks : 0;
	}
}
#endif

/////////////////////////////////
//                             //
//  ITEMS FOR OpenCL PROGRAMS  //
//                             //
/////////////////////////////////

#ifdef USE_OPENCL

	#ifdef __APPLE__
		#include <OpenCL/cl.h>
	#else
		#include <CL/cl.h>
	#endif

	inline std::string CLDoublePragma(cl_device_id theDevice)
	{
		char buff[4096];
		size_t buffSize;
		std::string extensionList;
		if (sizeof(tw::Float)==4)
			return "";
		else
		{
			clGetDeviceInfo(theDevice,CL_DEVICE_EXTENSIONS,sizeof(buff),buff,&buffSize);
			extensionList = std::string(buff,buffSize-1);
			extensionList = buff;
			if (extensionList.find("cl_amd_fp64")!=std::string::npos)
				return "#pragma OPENCL EXTENSION cl_amd_fp64 : enable\n";
			if (extensionList.find("cl_khr_fp64")!=std::string::npos)
				return "#pragma OPENCL EXTENSION cl_khr_fp64 : enable\n";
			return "";
		}
	}
	inline std::string CLBasicTypes()
	{
		std::string types_string;
		if (sizeof(tw::Float)==4)
			types_string = "typedef float tw_Float;typedef float2 tw_Complex;typedef float4 tw_vec4; ";
		else
			types_string = "typedef double tw_Float;typedef double2 tw_Complex;typedef double4 tw_vec4; ";
		if (sizeof(tw::Int)==4)
			types_string += "typedef int tw_Int;typedef int4 tw_cell_id;";
		else
			types_string += "typedef long tw_Int;typedef int4 tw_cell_id;";
		return types_string;
	}
	inline std::string CLDiscreteSpace()
	{
		return "typedef struct { tw_Int xDim,yDim,zDim,xN0,xN1,yN0,yN1,zN0,zN1; } tw_DiscreteSpace;";
	}
	inline std::string CLMetrics()
	{
		return "typedef struct { tw_Float dt,dx,dy,dz; tw_Float to,xo,yo,zo; tw_Float car,cyl,sph,par; } tw_Metrics;";
	}
	inline std::string CLStrip()
	{
		return "typedef struct { tw_Int axis,di,dj,dk; tw_Int stride,dim; } tw_Strip;";
	}
	inline std::string CLLocalProtocol()
	{
		return "#define LOCAL_PROTOCOL_MACRO const tw_Int ig = get_global_id(0); \
			const tw_Int jg = get_global_id(1); \
			const tw_Int kg = get_global_id(2); \
			const tw_Int nx = get_global_size(0) + 2*get_global_offset(0); \
			const tw_Int ny = get_global_size(1) + 2*get_global_offset(1); \
			const tw_Int nz = get_global_size(2) + 2*get_global_offset(2); \
			const tw_Int xs = (nx!=1); \
			const tw_Int ys = (ny!=1)*nx; \
			const tw_Int zs = (nz!=1)*nx*ny; \
			const tw_Int cs = nx*ny*nz; \
			const tw_Int n = ig*xs + jg*ys + kg*zs; \
			tw_cell_id cell = (tw_cell_id)(0,ig,jg,kg); \
			tw_Metrics met; \
			GetMetrics((tw_Float*)&met,met_g);";
	}
	inline std::string CLPointProtocol()
	{
		return "#define POINT_PROTOCOL_MACRO const tw_Int ig = get_global_id(0); \
			const tw_Int jg = get_global_id(1); \
			const tw_Int kg = get_global_id(2); \
			const tw_Int nx = get_global_size(0); \
			const tw_Int ny = get_global_size(1); \
			const tw_Int nz = get_global_size(2); \
			const tw_Int xs = 1; \
			const tw_Int ys = nx; \
			const tw_Int zs = nx*ny; \
			const tw_Int cs = nx*ny*nz; \
			const tw_Int n = ig*xs + jg*ys + kg*zs; \
			tw_cell_id cell = (tw_cell_id)(0,ig,jg,kg);";
	}
	inline std::string CLPointProtocolMetric()
	{
		return "#define POINT_PROTOCOL_METRIC_MACRO const tw_Int ig = get_global_id(0); \
			const tw_Int jg = get_global_id(1); \
			const tw_Int kg = get_global_id(2); \
			const tw_Int nx = get_global_size(0); \
			const tw_Int ny = get_global_size(1); \
			const tw_Int nz = get_global_size(2); \
			const tw_Int xs = 1; \
			const tw_Int ys = nx; \
			const tw_Int zs = nx*ny; \
			const tw_Int cs = nx*ny*nz; \
			const tw_Int n = ig*xs + jg*ys + kg*zs; \
			tw_cell_id cell = (tw_cell_id)(0,ig,jg,kg); \
			tw_Metrics met; \
			GetMetrics((tw_Float*)&met,met_g);";
	}
	inline std::string CLDefinitions(cl_device_id theDevice)
	{
		std::string definitions_string;
		definitions_string = CLDoublePragma(theDevice) + '\n';
		definitions_string += CLBasicTypes() + '\n';
		definitions_string += CLDiscreteSpace() + '\n';
		definitions_string += CLMetrics() + '\n';
		definitions_string += CLStrip() + '\n';
		definitions_string += CLLocalProtocol() + '\n';
		definitions_string += CLPointProtocol() + '\n';
		definitions_string += CLPointProtocolMetric() + '\n';
		return definitions_string;
	}

#endif


//////////////////////////////
//                          //
//    DESKTOP DEFINITIONS   //
//   (Mac, Windows, Linux)  //
//                          //
//////////////////////////////


#ifdef USE_DESKTOP

#define USE_TW_MPI

inline tw::Int GetSeconds()
{
	return tw::Int(time(NULL));
}

#include <thread>
#include <mutex>
#include <condition_variable>
#include "tw_thread_stl.h"
#include "tw_mpi.h"

inline bool LittleEndian()
{
	return true;
}

inline std::string InputPathName()
{
	return "";
}

static const bool parallelFileSystem = false;

#endif



//////////////////////
//                  //
// CRAY DEFINITIONS //
//                  //
//////////////////////


#ifdef USE_CRAY

#include "mpi.h"

inline tw::Int GetSeconds()
{
	return tw::Int(MPI_Wtime());
}

inline std::string InputPathName()
{
	return "";
}

inline bool LittleEndian()
{
	return true;
}

static const bool parallelFileSystem = true;

#endif


#define _did_definitions

#endif
