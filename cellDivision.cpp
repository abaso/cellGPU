#include "std_include.h"
#include "cuda_runtime.h"
#include "cuda_profiler_api.h"

#define ENABLE_CUDA

#include "avm2d.h"
#include "voronoi2d.h"
#include "selfPropelledCellVertexDynamics.h"
#include "brownianParticleDynamics.h"
#include "DatabaseNetCDFAVM.h"
#include "DatabaseNetCDFSPV.h"
/*!
This file demonstrates simulations in the vertex or voronoi models in which a cell divides
*/
int main(int argc, char*argv[])
{
    int numpts = 200;
    int USE_GPU = 0;
    int USE_TENSION = 0;
    int c;
    int tSteps = 5;
    int initSteps = 0;

    Dscalar dt = 0.01;
    Dscalar p0 = 3.84;
    Dscalar a0 = 1.0;
    Dscalar v0 = 0.01;
    Dscalar Dr = 1.0;
    Dscalar gamma = 0.0;

    int program_switch = 0;
    while((c=getopt(argc,argv,"n:g:m:s:r:a:i:v:b:x:y:z:p:t:e:d:")) != -1)
        switch(c)
        {
            case 'n': numpts = atoi(optarg); break;
            case 't': tSteps = atoi(optarg); break;
            case 'g': USE_GPU = atoi(optarg); break;
            case 'x': USE_TENSION = atoi(optarg); break;
            case 'i': initSteps = atoi(optarg); break;
            case 'z': program_switch = atoi(optarg); break;
            case 'e': dt = atof(optarg); break;
            case 's': gamma = atof(optarg); break;
            case 'p': p0 = atof(optarg); break;
            case 'a': a0 = atof(optarg); break;
            case 'v': v0 = atof(optarg); break;
            case 'd': Dr = atof(optarg); break;
            case '?':
                    if(optopt=='c')
                        std::cerr<<"Option -" << optopt << "requires an argument.\n";
                    else if(isprint(optopt))
                        std::cerr<<"Unknown option '-" << optopt << "'.\n";
                    else
                        std::cerr << "Unknown option character.\n";
                    return 1;
            default:
                       abort();
        };
    clock_t t1,t2;

    bool reproducible = true;
    bool initializeGPU = true;
    if (USE_GPU >= 0)
        {
        bool gpu = chooseGPU(USE_GPU);
        if (!gpu) return 0;
        cudaSetDevice(USE_GPU);
        }
    else
        initializeGPU = false;

    char dataname[256];
    sprintf(dataname,"../test.nc");
    char dataname2[256];
    sprintf(dataname2,"../test2.nc");

    //program_switch >= 0 --> thermal voronoi model
    if(program_switch >=0)
        {

        };

    //program_switch < 0 --> self-propelled vertex model
    if(program_switch <0)
        {
        int Nvert = 2*numpts;
        AVMDatabaseNetCDF ncdat(Nvert,dataname,NcFile::Replace);
        AVMDatabaseNetCDF ncdat2(Nvert+2,dataname2,NcFile::Replace);
        bool runSPV = false;

        EOMPtr spp = make_shared<selfPropelledCellVertexDynamics>(numpts,Nvert);
        ForcePtr avm = make_shared<AVM2D>(numpts,1.0,4.0,reproducible,runSPV);
        avm->setCellPreferencesUniform(1.0,p0);
        avm->setv0Dr(v0,1.0);

        shared_ptr<AVM2D> AVM = dynamic_pointer_cast<AVM2D>(avm);
        AVM->setT1Threshold(0.04);

        SimulationPtr sim = make_shared<Simulation>();
        sim->setConfiguration(avm);
        sim->setEquationOfMotion(spp,avm);
        sim->setIntegrationTimestep(dt);
        sim->setSortPeriod(initSteps/10);
        //set appropriate CPU and GPU flags
        sim->setCPUOperation(!initializeGPU);
        sim->setReproducible(reproducible);
        for (int timestep = 0; timestep < initSteps+1; ++timestep)
            {
            sim->performTimestep();
            if(timestep%((int)(1/dt))==0)
                {
        //        cout << timestep << endl;
        //        avm.reportAP();
        //        avm.reportMeanVertexForce();
                };
            if(program_switch < -1 && timestep%((int)(1/dt))==0)
                {
                cout << timestep << endl;
                ncdat.WriteState(AVM);
                };
            };
        vector<int> cdtest(3); cdtest[0]=10; cdtest[1] = 0; cdtest[2] = 2;
        avm->cellDivision(cdtest);

        t1=clock();

        for (int timestep = 0; timestep < tSteps; ++timestep)
            {
            sim->performTimestep();
            if(program_switch <-2 && timestep%((int)(1/dt))==0)
                avm->cellDivision(cdtest);
            if(program_switch == -2 && timestep%((int)(1/dt))==0)
                {
                cout << timestep << endl;
                ncdat2.WriteState(AVM);
                };
            };

        t2=clock();
        cout << "final number of vertices = " <<AVM->getNumberOfDegreesOfFreedom() << endl;
        cout << "timestep time per iteration currently at " <<  (t2-t1)/(Dscalar)CLOCKS_PER_SEC/tSteps << endl << endl;

        avm->reportMeanVertexForce();
        cout << "Mean q = " << avm->reportq() << endl;
        };


    if(initializeGPU)
        cudaDeviceReset();

    return 0;
    };
