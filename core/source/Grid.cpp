#include "sim.h"
#include "particles.h"
#include "fieldSolve.h"
#include "electrostatic.h"
#include "laserSolve.h"
#include "fluid.h"
#include "quantum.h"
#include "solidState.h"
//#include <unistd.h>

//////////////////////
//  TEXT PROCESSOR  //
//////////////////////


void ReduceInputFile(std::ifstream& inFile,std::stringstream& out)
{
	bool ignoreUntilMark,ignoreUntilEOL;
	char charNow,nextChar;

	ignoreUntilMark = false;
	ignoreUntilEOL = false;
	while (inFile.get(charNow))
	{
		if (charNow=='/' || charNow=='*' || charNow=='\n' || charNow=='\r')
		{
			if (charNow=='/')
			{
				inFile.get(nextChar);
				if (nextChar=='/')
					ignoreUntilEOL = true;
				if (nextChar=='*' && !ignoreUntilEOL)
					ignoreUntilMark = true;
				if (nextChar!='/' && nextChar!='*')
				{
					if (!ignoreUntilMark && !ignoreUntilEOL)
						out << charNow;
					inFile.putback(nextChar);
				}
			}
			if (charNow=='*')
			{
				inFile.get(nextChar);
				if (nextChar=='/')
					ignoreUntilMark = false;
				else
				{
					if (!ignoreUntilMark && !ignoreUntilEOL)
						out << charNow;
					inFile.putback(nextChar);
				}
			}
			if (charNow=='\n' || charNow=='\r')
			{
				if (!ignoreUntilMark)
					out << charNow;
				ignoreUntilEOL = false;
			}
		}
		else
		{
			if (!ignoreUntilMark && !ignoreUntilEOL)
			{
				if (charNow==',' || charNow=='(' || charNow==')')
					charNow = ' ';
				if (charNow=='{' || charNow=='}' || charNow=='=')
					out << ' ';
				out << charNow;
				if (charNow=='{' || charNow=='}' || charNow=='=')
					out << ' ';
			}
		}
	}
}

void PreprocessInputFile(std::ifstream& inFile,std::stringstream& out)
{
	// This routine erroneously adds closing braces to the end of "out".
	// This does not corrupt subsequent parsing, however.
	std::ifstream *includedFile;
	std::stringstream stage1,stage2,stage3;
	std::string word;
	tw::Float unitDensityCGS;

	// Handle included files, strip comments, clean whitespace
	ReduceInputFile(inFile,stage1);
	stage1.seekg(0);
	while (!stage1.eof())
	{
		stage1 >> word;
		if (word=="include")
		{
			stage1 >> word;
			includedFile = new std::ifstream(word.c_str());
			if (includedFile->rdstate() & std::ios::failbit)
				throw tw::FatalError("couldn't open " + word);
			ReduceInputFile(*includedFile,stage2);
			delete includedFile;
		}
		else
		{
			stage2 << word << " ";
		}
	}

	// Set up the unit converter
	stage2.seekg(0);
	while (!stage2.eof())
	{
		stage2 >> word;
		stage3 << word << " ";
		if (word=="unit")
		{
			stage2 >> word;
			stage3 << word << " ";
			if (word=="density")
			{
				stage2 >> word >> unitDensityCGS;
				stage3 << " = " << unitDensityCGS << " ";
			}
		}
	}
	UnitConverter uc(unitDensityCGS);

	// Unit conversion macro substitution
	stage3.seekg(0);
	while (!stage3.eof())
	{
		stage3 >> word;
		if (word[0]=='%')
			tw::input::NormalizeInput(uc,word);
		out << word << " ";
	}
}

void ExitInputFileBlock(std::stringstream& inputString)
{
	std::string word;
	tw::Int leftCount=0;
	tw::Int rightCount=0;
	do
	{
		inputString >> word;
		if (word=="{")
			leftCount++;
		if (word=="}")
			rightCount++;
	} while (leftCount==0 || leftCount>rightCount);
}




///////////////////////////
//  NON-UNIFORM REGIONS  //
///////////////////////////


NonUniformRegion::NonUniformRegion(tw::Int first,tw::Int last,tw::Float length,tw::Float dz0)
{
	i1 = first;
	i2 = last;
	ih = (i1+i2)/2;
	L = length;
	dz = dz0;
	N = i2 - i1 + 1;

	tw::Int i;
	gridSum = 0.0;
	for (i=1;i<=N;i++)
		gridSum += QuinticPulse(tw::Float(i-1)/tw::Float(N-1));
}

tw::Float NonUniformRegion::AddedCellWidth(tw::Int globalCell)
{
	tw::Float A = (1.0/gridSum)*(L/dz - N);
	if (globalCell>=i1 && globalCell<=i2)
		return dz*A*QuinticPulse(tw::Float(globalCell-i1)/tw::Float(N-1));
	else
		return 0.0;
}

tw::Float NonUniformRegion::ACoefficient(tw::Float length)
{
	return (1.0/gridSum)*(length/dz - N);
}

void Grid::SetCellWidthsAndLocalSize()
{
	tw::Int i,j;

	// Overwrite uniform cell widths with requested variable widths.
	// Correct the local domain size to reflect the variable cell widths.

	if (radialProgressionFactor!=1.0)
	{
		for (i=cornerCell[1]-1;i<=cornerCell[1]+dim[1];i++)
		{
			if (i > globalCells[1]/3)
				dX(i-cornerCell[1]+1,1) = spacing.x*pow(radialProgressionFactor,tw::Float(i-globalCells[1]/3));
		}
	}

	if (dim[3]>1 && region.size())
	{
		for (i=lb[3];i<=ub[3];i++)
		{
			dX(i,3) = spacing.z;
			for (j=0;j<region.size();j++)
				dX(i,3) += region[j]->AddedCellWidth(cornerCell[3] - 1 + i);
		}
	}

	size = 0.0;
	for (i=1;i<=dim[1];i++)
		size.x += dX(i,1);
	for (i=1;i<=dim[2];i++)
		size.y += dX(i,2);
	for (i=1;i<=dim[3];i++)
		size.z += dX(i,3);
}

void Grid::SetGlobalSizeAndLocalCorner()
{
	// Perform message passing to determine the effect of non-uniform cell widths
	// on the global domain size and the coordinates of the local domain corner.
	// Assumes local domain sizes are already calculated.

	tw::Int axis,src,dst;
	tw::Float inData,outData;
	tw::Float lsize[4] = { 0.0 , size.x , size.y , size.z };
	tw::Float gcorn[4] = { 0.0 , globalCorner.x , globalCorner.y , globalCorner.z };
	tw::Float lcorn[4];
	tw::Float gsize[4];

	for (axis=1;axis<=3;axis++)
	{
		finiteStrip[axis].Shift(1,1,&src,&dst);
		if (domainIndex[axis]==0)
		{
			finiteStrip[axis].Send(&lsize[axis],sizeof(tw::Float),dst);
			lcorn[axis] = gcorn[axis];
			outData = lsize[axis]; // in case domains=1
		}
		else
		{
			finiteStrip[axis].Recv(&inData,sizeof(tw::Float),src);
			lcorn[axis] = gcorn[axis] + inData;
			outData = inData + lsize[axis];
			if (domainIndex[axis]!=domains[axis]-1)
				finiteStrip[axis].Send(&outData,sizeof(tw::Float),dst);
		}
		finiteStrip[axis].Shift(1,-1,&src,&dst);
		if (domainIndex[axis]==domains[axis]-1)
		{
			gsize[axis] = outData;
			finiteStrip[axis].Send(&gsize[axis],sizeof(tw::Float),dst);
		}
		else
		{
			finiteStrip[axis].Recv(&inData,sizeof(tw::Float),src);
			gsize[axis] = inData;
			if (domainIndex[axis]!=0)
				finiteStrip[axis].Send(&gsize[axis],sizeof(tw::Float),dst);
		}
		((tw::Float*)&corner)[axis-1] = lcorn[axis];
		((tw::Float*)&globalSize)[axis-1] = gsize[axis];
	}
}



////////////
//  GRID  //
////////////


Grid::Grid()
{
	clippingRegion.push_back(new EntireRegion(clippingRegion));

	gridGeometry = cartesian;

	dt0 = 0.1;
	dt = 0.1;
	dth = 0.05;
	dtMin = tw::small_pos;
	dtMax = tw::big_pos;
	elapsedTime = 0.0;
	elapsedTimeMax = tw::big_pos;
	signalPosition = 0.0;
	windowPosition = 0.0;
	signalSpeed = 1.0;
	antiSignalPosition = 0.0;
	antiWindowPosition = 0.0;

	radialProgressionFactor = 1.0;

	appendMode = true;
	fullOutput = false;
	neutralize = true;
	smoothing = 0;
	compensation = 0;
	movingWindow = false;
	restarted = false;
	completed = false;
	adaptiveTimestep = false;
	adaptiveGrid = false;

	stepNow = 1;
	stepsToTake = 32;
	lastTime = 0;
	binaryFormat = 3;

	dumpPeriod = 0;
	sortPeriod = 0;
	sortX = sortY = 0;
	sortZ = 1;

	bc0[1] = cyclic;
	bc1[1] = cyclic;
	bc0[2] = cyclic;
	bc1[2] = cyclic;
	bc0[3] = absorbing;
	bc1[3] = absorbing;

	#ifdef USE_OPENCL
	waveBuffer = NULL;
	#endif
}

Grid::~Grid()
{
	tw::Int i;

	for (i=0;i<wave.size();i++)
		delete wave[i];
	for (i=0;i<pulse.size();i++)
		delete pulse[i];
	for (i=0;i<energyDiagnostic.size();i++)
		delete energyDiagnostic[i];
	for (i=0;i<pointDiagnostic.size();i++)
		delete pointDiagnostic[i];
	for (i=0;i<boxDiagnostic.size();i++)
		delete boxDiagnostic[i];
	for (i=0;i<conductor.size();i++)
		delete conductor[i];
	for (i=0;i<clippingRegion.size();i++)
		delete clippingRegion[i];
	for (i=0;i<region.size();i++)
		delete region[i];

	for (i=0;i<computeTool.size();i++)
		delete computeTool[i];
	for (i=0;i<module.size();i++)
		delete module[i];

	if (uniformDeviate!=NULL)
		delete uniformDeviate;
	if (gaussianDeviate!=NULL)
		delete gaussianDeviate;

	if (dynamic_cast<std::ofstream*>(tw_out))
		((std::ofstream*)tw_out)->close();

	#ifdef USE_OPENCL
	if (waveBuffer!=NULL)
		clReleaseMemObject(waveBuffer);
	#endif
}

void Grid::Run()
{
	std::ofstream twstat;
	if (strip[0].Get_rank()==0)
	{
		twstat.open("twstat");
		twstat << "TurboWAVE is initializing.";
		twstat.close();
	}

	try
	{
		(*tw_out) << std::endl << "*** Prepare Simulation ***" << std::endl << std::endl;

		PrepareSimulation();

		(*tw_out) << std::endl << "*** Begin Simulation ***" << std::endl << std::endl;

		tw::Int startTime = GetSeconds();
		lastTime = startTime;

		if (GetSeconds()<0)
		{
			(*tw_out) << std::endl << "WARNING: System clock is not responding properly." << std::endl << std::endl;
		}

		(*tw_out) << "Current status can be viewed in 'twstat' file or by pressing enter key." << std::endl;
		(*tw_out) << "Enter 'help' for list of interactive commands." << std::endl << std::endl;

		while (stepNow <= stepsToTake && elapsedTime < elapsedTimeMax)
		{
			if ((GetSeconds() > lastTime + 5) && strip[0].Get_rank()==0)
			{
				twstat.open("twstat");
				InteractiveCommand("status",&twstat);
				twstat.close();
				lastTime = GetSeconds();
			}
			FundamentalCycle();
		}

		(*tw_out) << "Completed " << stepNow - 1 << " steps in " << GetSeconds() - startTime << " seconds." << std::endl;
		(*tw_out) << "Simulated elapsed time = " << elapsedTime << std::endl;
		if (strip[0].Get_rank()==0)
		{
			twstat.open("twstat");
			twstat << "Completed " << stepNow - 1 << " steps in " << GetSeconds() - startTime << " seconds." << std::endl;
			twstat << "Simulated elapsed time = " << elapsedTime << std::endl;
			twstat.close();
		}
	}
	catch (tw::FatalError& e)
	{
		(*tw_out) << "FATAL ERROR: " << e.what() << std::endl;
		(*tw_out) << "Simulation failed --- exiting now." << std::endl;
		#ifdef USE_TW_MPI
		if (tw_out != &std::cout)
		{
			std::cout << "FATAL ERROR: " << e.what() << std::endl;
			std::cout << "Simulation failed --- exiting now." << std::endl;
		}
		#endif
		if (strip[0].Get_rank()==0)
		{
			twstat.open("twstat");
			twstat << "The simulation failed. For more info see stdout." << std::endl;
			twstat.close();
		}
		completed = true;
		exit(1);
	}

	completed = true;
}

void Grid::SetupGeometry()
{
	// This routine assumes that MetricSpace::width, and MetricSpace::corner are valid
	switch (gridGeometry)
	{
		case cartesian:
			if (stepNow==1)
				(*tw_out) << "Using CARTESIAN Grid" << std::endl;
			SetCartesianGeometry();
			break;
		case cylindrical:
			if (stepNow==1)
				(*tw_out) << "Using CYLINDRICAL Grid" << std::endl;
			SetCylindricalGeometry();
			break;
		case spherical:
			if (stepNow==1)
				(*tw_out) << "Using SPHERICAL Grid" << std::endl;
			SetSphericalGeometry();
			break;
	}
}

void Grid::PrepareSimulation()
{
	std::ofstream twstat;
	tw::Int i;

	#ifdef USE_OPENCL
	PrintGPUInformation();
	#endif

	if (!restarted)
		GridFromInputFile();

	ReadInputFile();

	// Grid initialization done during reading of input file
	// because Module constructors are allowed to assume the Grid is fully specified

	// Initialize Regions

	if (!restarted)
		for (i=1;i<clippingRegion.size();i++)
			clippingRegion[i]->Initialize(*this,this);
	// region 0 is not saved in restart file
	clippingRegion[0]->Initialize(*this,this);

	// Initialize Injection Objects

	if (!restarted)
	{
		for (i=0;i<wave.size();i++)
			wave[i]->Initialize();
		for (i=0;i<pulse.size();i++)
			pulse[i]->Initialize();
		for (i=0;i<conductor.size();i++)
			conductor[i]->Initialize(*this);
	}
	#ifdef USE_OPENCL
	cl_int err;
	std::valarray<tw::Float> packed_waves(15*wave.size());
	for (i=0;i<wave.size();i++)
	{
		packed_waves[15*i] = wave[i]->a0;
		packed_waves[15*i+1] = wave[i]->w;
		for (tw::Int c=0;c<3;c++)
		{
			packed_waves[15*i+2+c] = wave[i]->laserFrame.u[c];
			packed_waves[15*i+5+c] = wave[i]->laserFrame.w[c];
			packed_waves[15*i+8+c] = wave[i]->focusPosition[c];
		}
		packed_waves[15*i+11] = wave[i]->pulseShape.t1;
		packed_waves[15*i+12] = wave[i]->pulseShape.t2;
		packed_waves[15*i+13] = wave[i]->pulseShape.t3;
		packed_waves[15*i+14] = wave[i]->pulseShape.t4;
	}
	waveBuffer = clCreateBuffer(context,CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,sizeof(tw::Float)*packed_waves.size(),&packed_waves[0],&err);
	#endif

	// Sort Modules

	ModuleComparator comparatorObject;
	std::sort(module.begin(),module.end(),comparatorObject);

	// Initialize Computational Tools
	// Must precede module initialization

	(*tw_out) << std::endl << "Initializing Compute Tools..." << std::endl << std::endl;

	for (i=0;i<computeTool.size();i++)
	{
		(*tw_out) << "Tool: " << computeTool[i]->name << std::endl;
		computeTool[i]->Initialize();
		computeTool[i]->WarningMessage(tw_out);
	}
	if (computeTool.size()==0)
		(*tw_out) << "(no tools)" << std::endl;

	// Initialize Modules

	(*tw_out) << std::endl << "Initializing Modules..." << std::endl << std::endl;

	for (i=0;i<module.size();i++)
		module[i]->ExchangeResources();

	for (i=0;i<module.size();i++)
	{
		(*tw_out) << "Module: " << module[i]->name << std::endl;
		module[i]->Initialize();
		module[i]->WarningMessage(tw_out);
	}
}

#ifdef USE_OPENCL

void Grid::PrintGPUInformation()
{
	cl_ulong ninfo;

	*tw_out << initMessage;

	*tw_out << "GPU INFORMATION" << std::endl;
	*tw_out << "--------------------------------------------" << std::endl;

	clGetDeviceInfo(gpu,CL_DEVICE_GLOBAL_MEM_SIZE,sizeof(ninfo),&ninfo,NULL);
	*tw_out << "Global memory: " << ninfo << std::endl;

	clGetDeviceInfo(gpu,CL_DEVICE_LOCAL_MEM_SIZE,sizeof(ninfo),&ninfo,NULL);
	*tw_out << "Local memory: " << ninfo << std::endl;

	clGetDeviceInfo(gpu,CL_DEVICE_MAX_WORK_GROUP_SIZE,sizeof(ninfo),&ninfo,NULL);
	*tw_out << "Maximum work group size: " << ninfo << std::endl;

	clGetDeviceInfo(gpu,CL_DEVICE_PREFERRED_VECTOR_WIDTH_FLOAT,sizeof(ninfo),&ninfo,NULL);
	*tw_out << "Float vector width: " << ninfo << std::endl;

	clGetDeviceInfo(gpu,CL_DEVICE_PREFERRED_VECTOR_WIDTH_DOUBLE,sizeof(ninfo),&ninfo,NULL);
	*tw_out << "Double vector width: " << ninfo << std::endl;

	*tw_out << "--------------------------------------------" << std::endl << std::endl;

}

#endif

void Grid::InteractiveCommand(const std::string& cmd,std::ostream *theStream)
{
	if (cmd=="help" || cmd=="?")
	{
		*theStream << "--- List of Interactive Commands ---" << std::endl;
		*theStream << "status or [enter] : print current step and other status indicators" << std::endl;
		*theStream << "metrics : print grid and time step metrics for this simulation" << std::endl;
		*theStream << "list : list modules and compute tools and their ID numbers" << std::endl;
		//*theStream << "peek [x] [y] [z] : print current data at cell x,y,z" << std::endl;
		*theStream << "Ctrl-C : abort the simulation" << std::endl;
		*theStream << std::endl;
	}
	if (cmd=="status" || cmd=="")
	{
		*theStream << "Current step: " << stepNow << std::endl;
		*theStream << "Current step size: " << dt << std::endl;
		*theStream << "Current elapsed time: " << elapsedTime << std::endl;
		for (tw::Int i=0;i<module.size();i++)
			module[i]->StatusMessage(theStream);
		for (tw::Int i=0;i<computeTool.size();i++)
			computeTool[i]->StatusMessage(theStream);
		*theStream << std::endl;
	}
	if (cmd=="list")
	{
		*theStream << "--- List of Modules ---" << std::endl;
		for (tw::Int i=0;i<module.size();i++)
			*theStream << i << " = " << module[i]->name << std::endl;
		*theStream << "--- List of Tools ---" << std::endl;
		for (tw::Int i=0;i<computeTool.size();i++)
			*theStream << i << " = " << computeTool[i]->name << std::endl;
		*theStream << std::endl;
	}
	if (cmd=="metrics")
	{
		*theStream << "Steps to take: " << stepsToTake << std::endl;
		*theStream << "Steps remaining: " << stepsToTake - stepNow << std::endl;
		*theStream << "Global grid size: " << globalCells[1] << "," << globalCells[2] << "," << globalCells[3] << std::endl;
		*theStream << "Local grid size: " << localCells[1] << "," << localCells[2] << "," << localCells[3] << std::endl;
		*theStream << "MPI Domains: " << domains[1] << "," << domains[2] << "," << domains[3] << std::endl;
		*theStream << std::endl;
	}
	if (cmd.find("peek")!=std::string::npos)
	{
		*theStream << "Not implemented yet.";
		*theStream << std::endl;
	}
}

void Grid::FundamentalCycle()
{
	tw::Int i;

	Diagnose();

	for (i=0;i<module.size();i++)
		module[i]->Reset();

	for (i=0;i<module.size();i++)
		module[i]->Update();

	elapsedTime += dt;
	signalPosition += signalSpeed*dt;
	antiSignalPosition -= signalSpeed*dt;
	stepNow++;

	if (adaptiveGrid)
		for (i=0;i<module.size();i++)
			module[i]->AdaptGrid();

	if (movingWindow && signalPosition>=(windowPosition + spacing.z) && dim[3]>1)
		MoveWindow();

	if (!movingWindow && antiSignalPosition<=(antiWindowPosition - spacing.z) && dim[3]>1)
		AntiMoveWindow();
}

ComputeTool* Grid::AddPrivateTool(tw_tool whichTool)
{
	computeTool.push_back(ComputeTool::CreateObjectFromType(whichTool,this,this,false));
	computeTool.back()->refCount++;
	return computeTool.back();
}

ComputeTool* Grid::AddSharedTool(tw_tool whichTool)
{
	int i;
	for (i=0;i<computeTool.size();i++)
		if (computeTool[i]->typeCode==whichTool && computeTool[i]->sharedTool)
		{
			computeTool[i]->refCount++;
			return computeTool[i];
		}

	computeTool.push_back(ComputeTool::CreateObjectFromType(whichTool,this,this,true));
	computeTool.back()->refCount++;
	return computeTool.back();
}

bool Grid::RemoveTool(ComputeTool *theTool)
{
	int i,toolIndex = -1;
	for (i=0;i<computeTool.size();i++)
		if (computeTool[i]==theTool)
			toolIndex = i;

	if (toolIndex==-1)
		return false;

	computeTool[toolIndex]->refCount--;
	if (computeTool[toolIndex]->refCount==0)
	{
		delete computeTool[toolIndex];
		computeTool.erase(computeTool.begin()+toolIndex);
	}
	return true;
}

void Grid::SetupTimeInfo(tw::Float dt)
{
	this->dt = dt;
	dth = 0.5*dt;

	tw::Int i;
	for (i=0;i<module.size();i++)
	{
		module[i]->dt = dt;
		module[i]->dth = dth;
		module[i]->dti = 1.0/dt;
	}
}

void Grid::MoveWindow()
{
	tw::Int i;
	windowPosition += spacing.z;
	corner.z += spacing.z;
	globalCorner.z += spacing.z;
	for (i=lb[3];i<=ub[3];i++)
		X(i,3) += spacing.z;

	for (i=0;i<clippingRegion.size();i++)
		if (clippingRegion[i]->moveWithWindow)
			clippingRegion[i]->Translate(tw::vec3(0,0,spacing.z));
		else
			clippingRegion[i]->Initialize(*this,this);

	for (i=0;i<module.size();i++)
		module[i]->MoveWindow();
}

void Grid::AntiMoveWindow()
{
	tw::Int i;
	antiWindowPosition -= spacing.z;

	for (i=0;i<module.size();i++)
		module[i]->AntiMoveWindow();
}

void Grid::ReadData(std::ifstream& inFile)
{
	tw::Int i;
	tw::Int num;
	Chemistry *chemBoss = NULL;
	Kinetics *parBoss = NULL;

	Task::ReadData(inFile);
	MetricSpace::ReadData(inFile);
	inFile.read((char *)&gridGeometry,sizeof(tw_geometry));
	inFile.read((char *)&unitDensityCGS,sizeof(tw::Float));
	inFile.read((char *)&dt0,sizeof(tw::Float));
	inFile.read((char *)&dt,sizeof(tw::Float));
	inFile.read((char *)&dth,sizeof(tw::Float));
	inFile.read((char *)&dtMin,sizeof(tw::Float));
	inFile.read((char *)&dtMax,sizeof(tw::Float));
	inFile.read((char *)&elapsedTime,sizeof(tw::Float));
	inFile.read((char *)&elapsedTimeMax,sizeof(tw::Float));
	inFile.read((char *)&signalPosition,sizeof(tw::Float));
	inFile.read((char *)&signalSpeed,sizeof(tw::Float));
	inFile.read((char *)&windowPosition,sizeof(tw::Float));
	inFile.read((char *)&antiSignalPosition,sizeof(tw::Float));
	inFile.read((char *)&antiWindowPosition,sizeof(tw::Float));
	inFile.read((char *)&movingWindow,sizeof(bool));
	inFile.read((char *)&adaptiveTimestep,sizeof(bool));
	inFile.read((char *)&adaptiveGrid,sizeof(bool));
	inFile.read((char *)&appendMode,sizeof(bool));
	inFile.read((char *)&fullOutput,sizeof(bool));
	inFile.read((char *)&neutralize,sizeof(bool));
	inFile.read((char *)&smoothing,sizeof(tw::Int));
	inFile.read((char *)&compensation,sizeof(tw::Int));
	inFile.read((char *)&stepsToTake,sizeof(tw::Int));
	inFile.read((char *)&dumpPeriod,sizeof(tw::Int));
	inFile.read((char *)&sortPeriod,sizeof(tw::Int));
	inFile.read((char *)&sortX,sizeof(tw::Int));
	inFile.read((char *)&sortY,sizeof(tw::Int));
	inFile.read((char *)&sortZ,sizeof(tw::Int));
	inFile.read((char *)&binaryFormat,sizeof(tw::Int));
	inFile.read((char *)bc0,sizeof(tw_boundary_spec)*4);
	inFile.read((char *)bc1,sizeof(tw_boundary_spec)*4);
	inFile.read((char *)&radialProgressionFactor,sizeof(tw::Float));

	(*tw_out) << "Local Grid = " << dim[1] << "x" << dim[2] << "x" << dim[3] << std::endl;
	#ifdef USE_OPENCL
	InitializeMetricsBuffer(context,dt);
	#endif

	inFile.read((char *)&num,sizeof(tw::Int));
	for (i=0;i<num;i++)
	{
		(*tw_out) << "Add Nonuniform Region" << std::endl;
		region.push_back(new NonUniformRegion(1,2,1.0,1.0));
		inFile.read((char*)region.back(),sizeof(NonUniformRegion));
	}

	inFile.read((char *)&num,sizeof(tw::Int));
	for (i=1;i<num;i++) // don't read index 0, it is created by constructor
	{
		clippingRegion.push_back(Region::CreateObjectFromFile(clippingRegion,inFile));
		(*tw_out) << "Add Region " << clippingRegion.back()->name << std::endl;
	}

	if (uniformDeviate!=NULL) delete uniformDeviate;
	uniformDeviate = new UniformDeviate(1);
	uniformDeviate->ReadData(inFile);

	if (gaussianDeviate!=NULL) delete gaussianDeviate;
	gaussianDeviate = new GaussianDeviate(1);
	gaussianDeviate->ReadData(inFile);

	inFile.read((char *)&num,sizeof(tw::Int));
	for (i=0;i<num;i++)
	{
		module.push_back(Module::CreateObjectFromFile(inFile,this));
		if (module.back()->typeCode==kinetics)
			parBoss = (Kinetics*)module.back();
		if (module.back()->typeCode==chemistry)
			chemBoss = (Chemistry*)module.back();
		if (module.back()->typeCode==species)
		{
			parBoss->species.push_back((Species*)module.back());
			((Species*)module.back())->parBoss = parBoss;
		}
		if (module.back()->typeCode==chemical)
		{
			chemBoss->chemical.push_back((Chemical*)module.back());
			chemBoss->group.back()->chemical.push_back((Chemical*)module.back());
			((Chemical*)module.back())->chemBoss = chemBoss;
			((Chemical*)module.back())->group = chemBoss->group.back();
		}
		if (module.back()->typeCode==equilibriumGroup)
		{
			chemBoss->group.push_back((EquilibriumGroup*)module.back());
			((EquilibriumGroup*)module.back())->chemBoss = chemBoss;
		}
		(*tw_out) << "Installed Module " << module.back()->name << std::endl;
	}

	inFile.read((char *)&num,sizeof(tw::Int));
	for (i=0;i<num;i++)
	{
		(*tw_out) << "Add Explicit Wave" << std::endl;
		wave.push_back(new Wave(gaussianDeviate));
		wave.back()->ReadData(inFile);
	}

	inFile.read((char *)&num,sizeof(tw::Int));
	for (i=0;i<num;i++)
	{
		(*tw_out) << "Add PGC Pulse" << std::endl;
		pulse.push_back(new Pulse(gaussianDeviate));
		pulse.back()->ReadData(inFile);
	}

	inFile.read((char *)&num,sizeof(tw::Int));
	for (i=0;i<num;i++)
	{
		(*tw_out) << "Add Conductor" << std::endl;
		conductor.push_back(new Conductor(clippingRegion));
		conductor.back()->ReadData(inFile);
	}

	inFile.read((char *)&num,sizeof(tw::Int));
	for (i=0;i<num;i++)
	{
		(*tw_out) << "Add Energy Series" << std::endl;
		energyDiagnostic.push_back(new EnergySeriesDescriptor(clippingRegion));
		energyDiagnostic.back()->ReadData(inFile);
	}

	inFile.read((char *)&num,sizeof(tw::Int));
	for (i=0;i<num;i++)
	{
		(*tw_out) << "Add Point Series" << std::endl;
		pointDiagnostic.push_back(new PointSeriesDescriptor(clippingRegion));
		pointDiagnostic.back()->ReadData(inFile);
	}

	inFile.read((char *)&num,sizeof(tw::Int));
	for (i=0;i<num;i++)
	{
		(*tw_out) << "Add Box Diagnostic" << std::endl;
		boxDiagnostic.push_back(new GridDataDescriptor(clippingRegion));
		boxDiagnostic.back()->ReadData(inFile);
	}
}

void Grid::WriteData(std::ofstream& outFile)
{
	tw::Int i;

	Task::WriteData(outFile);
	MetricSpace::WriteData(outFile);
	outFile.write((char *)&gridGeometry,sizeof(tw_geometry));
	outFile.write((char *)&unitDensityCGS,sizeof(tw::Float));
	outFile.write((char *)&dt0,sizeof(tw::Float));
	outFile.write((char *)&dt,sizeof(tw::Float));
	outFile.write((char *)&dth,sizeof(tw::Float));
	outFile.write((char *)&dtMin,sizeof(tw::Float));
	outFile.write((char *)&dtMax,sizeof(tw::Float));
	outFile.write((char *)&elapsedTime,sizeof(tw::Float));
	outFile.write((char *)&elapsedTimeMax,sizeof(tw::Float));
	outFile.write((char *)&signalPosition,sizeof(tw::Float));
	outFile.write((char *)&signalSpeed,sizeof(tw::Float));
	outFile.write((char *)&windowPosition,sizeof(tw::Float));
	outFile.write((char *)&antiSignalPosition,sizeof(tw::Float));
	outFile.write((char *)&antiWindowPosition,sizeof(tw::Float));
	outFile.write((char *)&movingWindow,sizeof(bool));
	outFile.write((char *)&adaptiveTimestep,sizeof(bool));
	outFile.write((char *)&adaptiveGrid,sizeof(bool));
	outFile.write((char *)&appendMode,sizeof(bool));
	outFile.write((char *)&fullOutput,sizeof(bool));
	outFile.write((char *)&neutralize,sizeof(bool));
	outFile.write((char *)&smoothing,sizeof(tw::Int));
	outFile.write((char *)&compensation,sizeof(tw::Int));
	outFile.write((char *)&stepsToTake,sizeof(tw::Int));
	outFile.write((char *)&dumpPeriod,sizeof(tw::Int));
	outFile.write((char *)&sortPeriod,sizeof(tw::Int));
	outFile.write((char *)&sortX,sizeof(tw::Int));
	outFile.write((char *)&sortY,sizeof(tw::Int));
	outFile.write((char *)&sortZ,sizeof(tw::Int));
	outFile.write((char *)&binaryFormat,sizeof(tw::Int));
	outFile.write((char *)bc0,sizeof(tw_boundary_spec)*4);
	outFile.write((char *)bc1,sizeof(tw_boundary_spec)*4);
 	outFile.write((char *)&radialProgressionFactor,sizeof(tw::Float));

	i = region.size();
	outFile.write((char *)&i,sizeof(tw::Int));
	for (i=0;i<region.size();i++)
		outFile.write((char*)region[i],sizeof(NonUniformRegion));

	i = clippingRegion.size();
	outFile.write((char *)&i,sizeof(tw::Int));
	for (i=1;i<clippingRegion.size();i++) // don't write index 0, it is created by constructor
	clippingRegion[i]->WriteData(outFile);

	uniformDeviate->WriteData(outFile);
	gaussianDeviate->WriteData(outFile);

	i = module.size();
	outFile.write((char *)&i,sizeof(tw::Int));
	for (i=0;i<module.size();i++)
		module[i]->WriteData(outFile);

	i = wave.size();
	outFile.write((char *)&i,sizeof(tw::Int));
	for (i=0;i<wave.size();i++)
		wave[i]->WriteData(outFile);

	i = pulse.size();
	outFile.write((char *)&i,sizeof(tw::Int));
	for (i=0;i<pulse.size();i++)
		pulse[i]->WriteData(outFile);

	i = conductor.size();
	outFile.write((char *)&i,sizeof(tw::Int));
	for (i=0;i<conductor.size();i++)
		conductor[i]->WriteData(outFile);

	i = energyDiagnostic.size();
	outFile.write((char *)&i,sizeof(tw::Int));
	for (i=0;i<energyDiagnostic.size();i++)
		energyDiagnostic[i]->WriteData(outFile);

	i = pointDiagnostic.size();
	outFile.write((char *)&i,sizeof(tw::Int));
	for (i=0;i<pointDiagnostic.size();i++)
		pointDiagnostic[i]->WriteData(outFile);

	i = boxDiagnostic.size();
	outFile.write((char *)&i,sizeof(tw::Int));
	for (i=0;i<boxDiagnostic.size();i++)
		boxDiagnostic[i]->WriteData(outFile);
}


///////////////////////////
//  Read the Input File  //
///////////////////////////

void Grid::OpenInputFile(std::ifstream& inFile)
{
	std::string fileName;
	fileName = InputPathName() + "stdin";
	inFile.open(fileName.c_str());
	if (!(inFile.rdstate() & std::ios::failbit))
		return;
	fileName = InputPathName() + "stdin.txt";
	inFile.open(fileName.c_str());
	if (!(inFile.rdstate() & std::ios::failbit))
		return;
	throw tw::FatalError("couldn't open stdin");
}

std::string Grid::InputFileFirstPass()
{
	// The first pass is used to fully initialize the task

	try
	{
		Lock();

		bool foundGrid = false;
		bool foundRestart = false;
		std::stringstream messageOut,fileName;

		int numRanksProvided,worldRank;
		MPI_Comm_size(MPI_COMM_WORLD,&numRanksProvided);
		MPI_Comm_rank(MPI_COMM_WORLD,&worldRank);
		// world rank is suitable for reading task data from restart file
		// because this data is the same in every restart file

		std::ifstream inFile;
		std::stringstream inputString;

		OpenInputFile(inFile);

		PreprocessInputFile(inFile,inputString);
		inFile.close();

		inputString.seekg(0);

		std::string com1,com2,word;

		do
		{
			inputString >> com1;

			if (com1=="threads")
			{
				messageOut << "WARNING: threads directive no longer supported.  Use command line arguments instead." << std::endl;
			}

			if (com1=="affinity")
			{
				tw::input::ReadArray(affinityMask,inputString); // eg, affinity = { 0 2 4 6 }
			}

			if (com1=="hardware")
			{
				inputString >> word >> com1;
				if (com1=="device")
				{
					inputString >> com2;
					if (com2=="string")
						inputString >> word >> deviceSearchString; // eg, hardware acceleration device string = nvidia
					if (com2=="numbers")
						tw::input::ReadArray(deviceIDList,inputString); // eg, hardware acceleration device numbers = { 0 , 1 }
				}
				if (com1=="platform") // eg, hardware acceleration platform string = cuda
				{
					inputString >> word >> word >> platformSearchString;
				}
			}

			if (com1=="new")
			{
				inputString >> com1;
				if (com1=="grid")
				{
					foundGrid = true;

					do
					{
						inputString >> com2;

						if (com2=="dimensions") // eg, dimensions = 32 32 32
						{
							inputString >> word;
							inputString >> globalCells[1] >> globalCells[2] >> globalCells[3];
						}
						if (com2=="decomposition") // eg, decomposition = 8 4 1
						{
							inputString >> word;
							inputString >> domains[1] >> domains[2] >> domains[3];
						}
					} while (com2!="}");
				}
				else
					ExitInputFileBlock(inputString);
			}

			if (com1=="generate")
				ExitInputFileBlock(inputString);

			if (com1=="open")
			{
				inputString >> com2;
				if (com2=="restart")
				{
					foundRestart = true;
					restarted = true;
					inputString >> com2 >> com2;
					fileName.str("");
					fileName << InputPathName() << worldRank << "_" << com2;
					std::ifstream inFile(fileName.str().c_str());
					if (inFile.rdstate() & std::ios::failbit)
						throw tw::FatalError("could not open restart file");
					Task::ReadData(inFile);
					inFile.close();
				}
			}

			if (com1=="stdout") // eg, stdout = full
			{
				inputString >> com2 >> com2;
				if (com2=="full")
					fullOutput = true;
			}

			if (com1=="xboundary" || com1=="yboundary" || com1=="zboundary" ) // eg, xboundary = absorbing absorbing
			{
				tw::input::ReadBoundaryTerm(bc0,bc1,inputString,com1);
				periodic[1] = bc0[1]==cyclic ? 1 : 0;
				periodic[2] = bc0[2]==cyclic ? 1 : 0;
				periodic[3] = bc0[3]==cyclic ? 1 : 0;
			}

		} while (!inputString.eof());

		if (!foundGrid && !foundRestart)
			throw tw::FatalError("neither a grid directive nor a restart file was found");

		// Check integer viability
		int64_t totalCellsPerRank = int64_t(globalCells[1])*int64_t(globalCells[2])*int64_t(globalCells[3])/int64_t(numRanksProvided);
		if (totalCellsPerRank>=pow(2,31) && sizeof(tw::Int)==4)
			throw tw::FatalError("You must recompile turboWAVE with 64 bit integers to handle this many grid cells.");

		// Verify and (if necessary) correct decomposition
		if (NumTasks() != numRanksProvided)
		{
			messageOut << "WARNING: Bad decomposition ";
			tw::Int ax1=1,ax2=2,ax3=3; // to be sorted so ax1 is longest
			for (tw::Int i=1;i<=3;i++)
			{
				domains[i] = 1;
				if (globalCells[i]>=globalCells[1] && globalCells[i]>=globalCells[2] && globalCells[i]>=globalCells[3])
					ax1 = i;
			}
			for (tw::Int i=1;i<=3;i++)
				ax2 = i==ax1 ? ax2 : i;
			for (tw::Int i=1;i<=3;i++)
				ax3 = i==ax1 || i==ax2 ? ax3 : i;
			if (globalCells[ax2]<globalCells[ax3])
				std::swap(ax2,ax3);
			domains[ax1] = numRanksProvided;
			while (globalCells[ax1]%(domains[ax1]*2)!=0 && domains[ax1]>0)
			{
				domains[ax1] /= 2;
				domains[ax2] *= 2;
			}
			messageOut << "(defaulting to " << domains[1] << "x" << domains[2] << "x" << domains[3] << ")" << std::endl;
		}
		messageOut << NumTasks() << "-Way Decomposition" << std::endl;

		// Set up the domain decomposition
		// Grid/restart provide domains[] , globalCells[] , periodic[] as inputs
		// communicators, cornerCell[], localCells[], domainIndex[], n0[] , n1[] are computed
		for (tw::Int i=1;i<=3;i++)
		{
			if (globalCells[i]%domains[i]!=0)
				throw tw::FatalError("global number of cells is not divisible by number of domains along axis");
		}
		Task::Initialize(domains,globalCells,periodic);
		for (tw::Int i=1;i<=3;i++)
		{
			if (localCells[i]%2!=0 && globalCells[i]>1)
				throw tw::FatalError("local number of cells is not even along non-ignorable axis");
		}
		Resize(localCells[1],localCells[2],localCells[3],tw::vec3(0.0,0.0,0.0),tw::vec3(1.0,1.0,1.0),2);

		// Random numbers
		uniformDeviate = new UniformDeviate(1 + strip[0].Get_rank()*(MaxSeed()/numRanksProvided));
		gaussianDeviate = new GaussianDeviate(1 + strip[0].Get_rank()*(MaxSeed()/numRanksProvided) + MaxSeed()/(2*numRanksProvided));

		// Set up standard outputs
		std::stringstream stdoutString;
		stdoutString << strip[0].Get_rank() << "_stdout.txt";
		if (strip[0].Get_rank() == 0)
			tw_out = &std::cout;
		else
		{
			if (fullOutput)
				tw_out = new std::ofstream(stdoutString.str().c_str());
			else
				tw_out = new std::stringstream; // put output into a throw away string
		}

		Unlock();

		return messageOut.str();
	}

	catch (tw::FatalError& e)
	{
		std::cout << "FATAL ERROR: " << e.what() << std::endl;
		std::cout << "Could not start simulation --- exiting now." << std::endl;
		exit(1);
	}
}

void Grid::GridFromInputFile()
{
	Lock();

	tw::Int i;
	std::string com1,com2,word;

	// adaptive grid variables
	tw::Int i1,i2;
	tw::Float length;

	corner = globalCorner = size = globalSize = tw::vec3(0.0,0.0,0.0);

	(*tw_out) << "Extract Grid from Input File..." << std::endl << std::endl;

	std::ifstream inFile;
	std::stringstream inputString;
	OpenInputFile(inFile);
	PreprocessInputFile(inFile,inputString);
	inFile.close();

	inputString.seekg(0);

	do
	{
		inputString >> com1;

		if (com1=="new")
		{
			inputString >> com2;

			if (com2=="grid")
			{
				do
				{
					inputString >> com2;
					if (com2=="corner") // eg, corner = 0.0 0.0 0.0
					{
						inputString >> word;
						inputString >> globalCorner.x >> globalCorner.y >> globalCorner.z;
					}
					if (com2=="cell") // eg, cell size = 0.5 0.5 0.5
					{
						inputString >> word >> word;
						inputString >> spacing.x >> spacing.y >> spacing.z;
					}
					if (com2=="geometry") // eg, geometry = cylindrical
					{
						gridGeometry = cartesian;
						inputString >> com2 >> com2;
						if (com2=="cylindrical")
							gridGeometry = cylindrical;
						if (com2=="spherical")
							gridGeometry = spherical;
					}
					if (com2=="radial") // eg, radial progression factor = 1.03
					{
						inputString >> com2 >> com2 >> com2 >> radialProgressionFactor;
					}
					if (com2=="region") // eg, region : start = 1 , end = 100 , length = 1e4
					{
						inputString >> com2 >> com2 >> com2 >> i1 >> com2 >> com2 >> i2 >> com2 >> com2 >> length;
						region.push_back(new NonUniformRegion(i1,i2,length,spacing.z));
					}

					if (com2=="adaptive") // eg, adaptive timestep = yes
					{
						inputString >> com2;
						if (com2=="timestep")
						{
							inputString >> com2 >> com2;
							adaptiveTimestep = (com2=="yes" || com2=="true" || com2=="on");
							(*tw_out) << "Adaptive timestep = " << adaptiveTimestep << std::endl;
						}
						if (com2=="grid")
						{
							inputString >> com2 >> com2;
							adaptiveGrid = (com2=="yes" || com2=="true" || com2=="on");
						}
					}
				} while (com2!="}");

				com2 = "grid";
			}
		}

		if (com1=="timestep") // eg, timestep = 1.0
		{
			inputString >> word;
			inputString >> dt0;
			SetupTimeInfo(dt0);
			(*tw_out) << "Timestep = " << dt << std::endl;
		}

		com1 = "???";

	} while (!inputString.eof());

	Unlock();

	(*tw_out) << "Allocate " << dim[1] << "x" << dim[2] << "x" << dim[3] << " Grid" << std::endl;
	size = spacing * tw::vec3(dim[1],dim[2],dim[3]);
	globalSize = spacing * tw::vec3(globalCells[1],globalCells[2],globalCells[3]);
	corner = globalCorner + tw::vec3(domainIndex[1],domainIndex[2],domainIndex[3]) * size;
	Resize(dim[1],dim[2],dim[3],corner,size,2);
	SetCellWidthsAndLocalSize();
	SetGlobalSizeAndLocalCorner();
	SetupGeometry();
	#ifdef USE_OPENCL
	InitializeMetricsBuffer(context,dt);
	#endif
}

void Grid::ReadInputFile()
{
	Lock();

	tw::Int i;
	std::string com1,com2,word,filename;

	std::string name;
	tw::Float w0,k0;
	Profile* theProfile;

	Chemistry *chemBoss = NULL;
	Kinetics *parBoss = NULL;
	Electromagnetic *EMBoss = NULL;

	w0 = 0.0;
	k0 = 0.0;

	(*tw_out) << std::endl << "Reading Input File..." << std::endl << std::endl;

	std::ifstream inFile;
	std::stringstream inputString;
	OpenInputFile(inFile);
	PreprocessInputFile(inFile,inputString);
	inFile.close();

	inputString.seekg(0);

	do
	{
		inputString >> com1;

		if (com1=="new")
		{
			inputString >> com2;

			if (com2=="wave")
			{
				wave.push_back(new Wave(gaussianDeviate));
				wave.back()->ReadInputFile(inputString,com2);
				wave.back()->pulseShape.delay += dth;
			}

			if (com2=="pulse")
			{
				pulse.push_back(new Pulse(gaussianDeviate));
				pulse.back()->ReadInputFile(inputString,com2);
				if (w0==0.0)
				{
					(*tw_out) << "WARNING: pulse declared before laser solver" << std::endl;
					pulse.back()->w0 = pulse.back()->w;
					pulse.back()->k0 = pulse.back()->w * pulse.back()->nrefr;
				}
				else
				{
					pulse.back()->w0 = w0;
					pulse.back()->k0 = k0;
				}
			}

			if (com2=="region")
			{
				inputString >> word;
				Region *curr = Region::CreateObjectFromString(clippingRegion,word);
				inputString >> curr->name;
				curr->ReadInputFileBlock(inputString);
				clippingRegion.push_back(curr);
			}

			if (com2=="conductor")
			{
				conductor.push_back(new Conductor(clippingRegion));
				conductor.back()->ReadInputFile(inputString,com2);
			}

			if (com2=="electrostatic") // eg, new electrostatic field solver
			{
				inputString >> word >> word;
				(*tw_out) << "Install Electrostatic Field Solver" << std::endl;
				module.push_back(new Electrostatic(this));
				module.back()->ReadInputFileBlock(inputString);
			}

			if (com2=="coulomb") // eg, new coulomb field solver
			{
				inputString >> word >> word;
				(*tw_out) << "Install Coulomb Solver" << std::endl;
				EMBoss = new CoulombSolver(this);
				module.push_back(EMBoss);
				module.back()->ReadInputFileBlock(inputString);
			}

			if (com2=="PML" || com2=="pml" || com2=="direct")
			{
				inputString >> word >> word;
				(*tw_out) << "Install Direct Solver" << std::endl;
				EMBoss = new DirectSolver(this);
				module.push_back(EMBoss);
				module.back()->ReadInputFileBlock(inputString);
			}

			if (com2=="curvilinear")
			{
				inputString >> word;
				if (word=="direct") // eg, new curvilinear direct solver
				{
					inputString >> word;
					(*tw_out) << "Install Curvilinear Direct Solver" << std::endl;
					EMBoss = new CurvilinearDirectSolver(this);
					module.push_back(EMBoss);
					module.back()->ReadInputFileBlock(inputString);
				}
				if (word=="coulomb") // eg, new curvilinear coulomb solver
				{
					throw tw::FatalError("curvilinear Coulomb not supported this version");
				}
			}

			if (com2=="quasistatic") // eg, new quasistatic laser module
			{
				inputString >> word >> word;
				(*tw_out) << "Install QS Laser Solver" << std::endl;
				module.push_back(new QSSolver(this));
				module.back()->ReadInputFileBlock(inputString);
				w0 = ((LaserSolver*)module.back())->laserFreq;
				k0 = w0;
				if (((LaserSolver*)module.back())->propagator==NULL)
					throw tw::FatalError("no laser propagator was specified");
			}

			if (com2=="pgc" || com2=="PGC") // eg, new PGC laser module
			{
				inputString >> word >> word;
				(*tw_out) << "Install PGC Laser Solver" << std::endl;
				module.push_back(new PGCSolver(this));
				module.back()->ReadInputFileBlock(inputString);
				w0 = ((LaserSolver*)module.back())->laserFreq;
				k0 = w0;
				if (((LaserSolver*)module.back())->propagator==NULL)
					throw tw::FatalError("no laser propagator was specified");
			}

			if (com2=="bound")
			{
				module.push_back(new BoundElectrons(this));
				inputString >> module.back()->name;
				module.back()->ReadInputFileBlock(inputString);
			}

			if (com2=="atomic" || com2=="schroedinger") // eg, new atomic physics module
			{
				inputString >> word >> word;
				module.push_back(new Schroedinger(this));
				module.back()->ReadInputFileBlock(inputString);
			}

			if (com2=="pauli") // eg, new pauli equation module
			{
				inputString >> word >> word;
				module.push_back(new Pauli(this));
				module.back()->ReadInputFileBlock(inputString);
			}

			if (com2=="klein") // eg, new klein gordon module
			{
				inputString >> word >> word;
				module.push_back(new KleinGordon(this));
				module.back()->ReadInputFileBlock(inputString);
			}

			if (com2=="dirac") // eg, new dirac module
			{
				inputString >> word;
				module.push_back(new Dirac(this));
				module.back()->ReadInputFileBlock(inputString);
			}

			if (com2=="fluid")
			{
				module.push_back(new Fluid(this));
				inputString >> module.back()->name;
				module.back()->ReadInputFileBlock(inputString);
			}

			if (com2=="species")
			{
				if (parBoss==NULL)
					module.push_back(parBoss = new Kinetics(this));

				Species *new_species = new Species(this);
				new_species->parBoss = parBoss;
				module.push_back(new_species);
				parBoss->species.push_back(new_species);
				inputString >> new_species->name;
				new_species->ReadInputFileBlock(inputString);
			}

			if (com2=="chemistry")
			{
				module.push_back(chemBoss = new Chemistry(this));
				module.back()->ReadInputFileBlock(inputString);
			}

			if (com2=="group" || com2=="chemical")
			{
				if (chemBoss==NULL)
					module.push_back(chemBoss = new Chemistry(this));

				EquilibriumGroup *new_group = new EquilibriumGroup(this);
				new_group->chemBoss = chemBoss;
				module.push_back(new_group);
				chemBoss->group.push_back(new_group);
				inputString >> new_group->name;
				if (com2=="group")
					new_group->ReadInputFileBlock(inputString);
				else
					new_group->ReadOneChemical(inputString,"");
			}

			if (com2=="reaction" || com2=="thermalization" || com2=="excitation" || com2=="collision")
			{
				if (chemBoss==NULL)
					throw tw::FatalError("encountered reaction/thermalization/excitation/collision before chemicals were defined");
				chemBoss->ReadInputFileTerm(inputString,com2);
			}

			if (com2=="phase" || com2=="orbit" || com2=="detector")
			{
				if (parBoss==NULL)
					throw tw::FatalError("encountered a particle diagnostic before species were defined");
				parBoss->ReadInputFileTerm(inputString,com2);
			}

			if (com2=="far-field") // eg, new far-field diagnostic { ... }
			{
				if (EMBoss==NULL)
					throw tw::FatalError("encountered far-field diagnostic before field solver was defined");
				EMBoss->ReadInputFileTerm(inputString,com2);
			}

			if (com2=="energy") // eg, new energy series { ... }
			{
				inputString >> word >> word;
				energyDiagnostic.push_back(new EnergySeriesDescriptor(clippingRegion));
				energyDiagnostic.back()->ReadInputFile(inputString);
			}

			if (com2=="point") // eg, new point series { ... }
			{
				inputString >> word	>> word;
				pointDiagnostic.push_back(new PointSeriesDescriptor(clippingRegion));
				pointDiagnostic.back()->ReadInputFile(inputString);
			}

			if (com2=="box") // eg, new box series { ... }
			{
				inputString >> word >> word;
				boxDiagnostic.push_back(new GridDataDescriptor(clippingRegion));
				boxDiagnostic.back()->ReadInputFile(inputString);
			}
		}

		if (com1=="generate")
		{
			inputString >> com2;
			inputString >> name >> word;
			theProfile = tw::input::GetProfile(this,name,com2);

			if (theProfile!=NULL)
			{
				(*tw_out) << "Create " << com2 << " " << name << std::endl;
				theProfile->ReadInputFileBlock(inputString,neutralize);
				(*tw_out) << "   Clipping region = " << theProfile->theRgn->name << std::endl;
			}
			else
				(*tw_out) << "WARNING: Couldn't find " << name << std::endl;
		}

		// Outside declarations: must come after the above

		if (com1=="xboundary" || com1=="yboundary" || com1=="zboundary" ) // eg, xboundary = absorbing absorbing
		{
			// already done in first pass, but must take block off string again
			tw::input::ReadBoundaryTerm(bc0,bc1,inputString,com1);
		}

		if (com1=="open") // eg, open restart file dump1
		{
			std::stringstream fileName;
			std::ifstream restartFile;
			inputString >> word >> word >> word;
			fileName << InputPathName() << strip[0].Get_rank() << "_" << word;
			(*tw_out) << "Reading restart file " << fileName.str() << "..." << std::endl;
			restartFile.open(fileName.str().c_str());
			ReadData(restartFile);
			restartFile.close();
		}

		if (com1=="normalize" || com1=="unit") // eg, normalize density to 1e16, or unit density = 1e16
		{
			inputString >> word >> word;
			inputString >> unitDensityCGS;
			(*tw_out) << "Unit of density = " << unitDensityCGS << " cm^-3" << std::endl;
		}

		if (com1=="dtmax") // eg, dtmax = 100.0
		{
			inputString >> word;
			inputString >> dtMax;
			(*tw_out) << "Set Maximum Timestep = " << dtMax << std::endl;
		}

		if (com1=="dtmin") // eg, dtmin = 1.0
		{
			inputString >> word;
			inputString >> dtMin;
			(*tw_out) << "Set Minimum Timestep = " << dtMin << std::endl;
		}

		if (com1=="maxtime") // eg, maxtime = 1e4
		{
			inputString >> word;
			inputString >> elapsedTimeMax;
			(*tw_out) << "Set Maximum Elapsed Time = " << elapsedTimeMax << std::endl;
		}

		if (com1=="steps") // eg, steps = 10
		{
			inputString >> word;
			inputString >> stepsToTake;
			(*tw_out) << "Steps to Take = " << stepsToTake << std::endl;
		}

		if (com1=="dump") // eg, dump period = 1024
		{
			inputString >> word >> word >> dumpPeriod;
			(*tw_out) << "Dump Period = " << dumpPeriod << std::endl;
		}

		if (com1=="sort") // eg, sort = ( 0 , 0 , 1 ) every 100 steps
		{
			inputString >> word >> sortX >> sortY >> sortZ >> word >> sortPeriod >> word;
			(*tw_out) << "Sort Period = " << sortPeriod << std::endl;
		}

		if (com1=="neutralize") // eg, neutralize = yes
		{
			inputString >> word >> word;
			neutralize = (word=="yes" || word=="true" || word=="on");
			(*tw_out) << "Full neutralization = " << neutralize << std::endl;
		}

		if (com1=="window") // eg, window speed = 1
		{
			inputString >> word >> word >> signalSpeed;
			(*tw_out) << "Window Speed = " << signalSpeed << std::endl;
		}

		if (com1=="moving") // eg, moving window = yes
		{
			inputString >> word >> word >> word;
			movingWindow = (word=="yes" || word=="on" || word=="true");
			(*tw_out) << "Moving Window = " << movingWindow << std::endl;
		}

		if (com1=="smoothing" || com1=="smoother") // eg, smoothing = on, or smoothing = 2 (smooth twice)
		{
			inputString >> word >> word;
			char *endptr;
			if (word=="yes" || word=="on" || word=="true")
				smoothing = 4;
			else
				smoothing = strtol(word.c_str(),&endptr,10);
			if (smoothing>0)
				compensation = 1;
		}

		if (com1=="compensation") // eg, compensation = 1
		{
			inputString >> word >> compensation;
		}

		if (com1=="binary")
		{
			inputString >> word >> word >> word;
			if (word=="2d")
				binaryFormat = 2;
			if (word=="3d")
				binaryFormat = 3;
		}

		if (com1=="append")
		{
			inputString >> word >> word >> word;
			appendMode = (word=="on" || word=="true" || word=="yes");
		}

		com1 = "???";

	} while (!inputString.eof());

	if (smoothing==0)
		(*tw_out) << "No Smoothing" << std::endl;
	else
		(*tw_out) << "Smoothing passes = " << smoothing << " , Compensation passes = " << compensation << std::endl;

	Unlock();
}
