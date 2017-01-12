#define ENABLE_CUDA

#include "cuda_runtime.h"
#include "DelaunayMD.h"
#include "DelaunayMD.cuh"

/*!
A simple constructor that sets many of the class member variables to zero
*/
DelaunayMD::DelaunayMD() :
    cellsize(1.25), timestep(0),repPerFrame(0.0),skippedFrames(0),
    neighMax(0),neighMaxChange(false),GlobalFixes(0)
    {
    //set cellsize to about unity...magic number should be of order 1
    //when the box area is of order N (i.e. on average one particle per bin)

    };

//a function that takes care of the initialization of the class.
void DelaunayMD::initializeDelMD(int n)
    {
    GPUcompute = true;

    //set particle number and box
    Ncells = n;
    Dscalar boxsize = sqrt((Dscalar)Ncells);
    Box.setSquare(boxsize,boxsize);

    //set circumcenter array size
    circumcenters.resize(2*(Ncells+10));
    NeighIdxs.resize(6*(Ncells+10));

    points.resize(Ncells);
    repair.resize(Ncells);
    randomizePositions(boxsize,boxsize);

    //initialize spatial sorting, but do not sort by default
    itt.resize(Ncells);
    tti.resize(Ncells);
    idxToTag.resize(Ncells);
    tagToIdx.resize(Ncells);
    for (int ii = 0; ii < Ncells; ++ii)
        {
        itt[ii]=ii;
        tti[ii]=ii;
        idxToTag[ii]=ii;
        tagToIdx[ii]=ii;
        };

    //cell list initialization
    celllist.setNp(Ncells);
    celllist.setBox(Box);
    celllist.setGridSize(cellsize);

    //DelaunayLoc initialization
    gpubox Bx(boxsize,boxsize);
    delLoc.setBox(Bx);
    resetDelLocPoints();

    //make a full triangulation
    completeRetriangulationPerformed = 1;
    cellNeighborNum.resize(Ncells);
    globalTriangulationCGAL();
    };

//just randomly initialize points by uniformly sampling between (0,0) and (boxx,boxy)
void DelaunayMD::randomizePositions(Dscalar boxx, Dscalar boxy)
    {
    int randmax = 100000000;
    ArrayHandle<Dscalar2> h_points(points,access_location::host, access_mode::overwrite);
    for (int ii = 0; ii < Ncells; ++ii)
        {
        Dscalar x =EPSILON+boxx/(Dscalar)(randmax+1)* (Dscalar)(rand()%randmax);
        Dscalar y =EPSILON+boxy/(Dscalar)(randmax+1)* (Dscalar)(rand()%randmax);
        h_points.data[ii].x=x;
        h_points.data[ii].y=y;
        };
    };

//Always called after spatial sorting is performed, reIndexArrays shuffles the order of an array based on the spatial sort order
void DelaunayMD::reIndexArray(GPUArray<Dscalar2> &array)
    {
    GPUArray<Dscalar2> TEMP = array;
    ArrayHandle<Dscalar2> temp(TEMP,access_location::host,access_mode::read);
    ArrayHandle<Dscalar2> ar(array,access_location::host,access_mode::readwrite);
    for (int ii = 0; ii < Ncells; ++ii)
        {
        ar.data[ii] = temp.data[itt[ii]];
        };
    };

void DelaunayMD::reIndexArray(GPUArray<Dscalar> &array)
    {
    GPUArray<Dscalar> TEMP = array;
    ArrayHandle<Dscalar> temp(TEMP,access_location::host,access_mode::read);
    ArrayHandle<Dscalar> ar(array,access_location::host,access_mode::readwrite);
    for (int ii = 0; ii < Ncells; ++ii)
        {
        ar.data[ii] = temp.data[itt[ii]];
        };
    };

void DelaunayMD::reIndexArray(GPUArray<int> &array)
    {
    GPUArray<int> TEMP = array;
    ArrayHandle<int> temp(TEMP,access_location::host,access_mode::read);
    ArrayHandle<int> ar(array,access_location::host,access_mode::readwrite);
    for (int ii = 0; ii < Ncells; ++ii)
        {
        ar.data[ii] = temp.data[itt[ii]];
        };
    };

//take the current location of the points and sort them according the their order along a 2D Hilbert curve
void DelaunayMD::spatiallySortPoints()
    {
    //itt and tti are the changes that happen in the current sort
    //idxToTag and tagToIdx relate the current indexes to the original ones
    HilbertSorter hs(Box);

    vector<pair<int,int> > idxSorter(Ncells);

    //sort points by Hilbert Curve location
    ArrayHandle<Dscalar2> h_p(points,access_location::host, access_mode::readwrite);
    for (int ii = 0; ii < Ncells; ++ii)
        {
        idxSorter[ii].first=hs.getIdx(h_p.data[ii]);
        idxSorter[ii].second = ii;
        };
    sort(idxSorter.begin(),idxSorter.end());

    //update tti and itt
    for (int ii = 0; ii < Ncells; ++ii)
        {
        int newidx = idxSorter[ii].second;
        itt[ii] = newidx;
        tti[newidx] = ii;
        };

    //update points, idxToTag, and tagToIdx
    vector<int> tempi = idxToTag;
    for (int ii = 0; ii < Ncells; ++ii)
        {
        idxToTag[ii] = tempi[itt[ii]];
        tagToIdx[tempi[itt[ii]]] = ii;
        };
    reIndexArray(points);

    };

//The GPU moves the location of points in the GPU memory... this gets a local copy that can be used by the DelaunayLoc class
void DelaunayMD::resetDelLocPoints()
    {
    ArrayHandle<Dscalar2> h_points(points,access_location::host, access_mode::read);
    delLoc.setPoints(h_points,Ncells);
    delLoc.initialize(cellsize);
    };

//update which cell every particle belongs to (for spatial location)
void DelaunayMD::updateCellList()
    {

    if(GPUcompute)
        {
        celllist.computeGPU(points);
        cudaError_t code = cudaGetLastError();
        if(code!=cudaSuccess)
            {
            printf("cell list computation GPUassert: %s \n", cudaGetErrorString(code));
            throw std::exception();
            };
        }
    else
        {
        vector<Dscalar> psnew(2*Ncells);
        ArrayHandle<Dscalar2> h_points(points,access_location::host, access_mode::read);
        for (int ii = 0; ii < Ncells; ++ii)
            {
            psnew[2*ii] =  h_points.data[ii].x;
            psnew[2*ii+1]= h_points.data[ii].y;
            };
        celllist.setParticles(psnew);
        celllist.compute();

        };

    };

//take a vector of displacements, modify the position of the particles, and put them back in the box...all on the CPU
void DelaunayMD::movePointsCPU(GPUArray<Dscalar2> &displacements)
    {
    ArrayHandle<Dscalar2> h_p(points,access_location::host,access_mode::readwrite);
    ArrayHandle<Dscalar2> h_d(displacements,access_location::host,access_mode::read);
    for (int idx = 0; idx < Ncells; ++idx)
        {
        h_p.data[idx].x += h_d.data[idx].x;
        h_p.data[idx].y += h_d.data[idx].y;
        Box.putInBoxReal(h_p.data[idx]);
        };
    };

//take a vector of displacements, modify the position of the particles, and put them back in the box...GPU
void DelaunayMD::movePoints(GPUArray<Dscalar2> &displacements)
    {
    ArrayHandle<Dscalar2> d_p(points,access_location::device,access_mode::readwrite);
    ArrayHandle<Dscalar2> d_d(displacements,access_location::device,access_mode::readwrite);
    gpu_move_particles(d_p.data,d_d.data,Ncells,Box);
    cudaError_t code = cudaGetLastError();
    if(code!=cudaSuccess)
        {
        printf("movePoints GPUassert: %s \n", cudaGetErrorString(code));
        throw std::exception();
        };
    };

//the function calls the DelaunayLoc and DelaunayNP classes to determine the Delaunay triangulation of the entire periodic domain
void DelaunayMD::fullTriangulation()
    {
    resetDelLocPoints();
    cout << "Resetting complete triangulation" << endl;
    //get neighbors of each cell in CW order

    ArrayHandle<int> neighnum(cellNeighborNum,access_location::host,access_mode::overwrite);
    ArrayHandle<int> h_repair(repair,access_location::host,access_mode::overwrite);
    vector< vector<int> > allneighs(Ncells);
    int totaln = 0;
    int nmax = 0;
    for(int nn = 0; nn < Ncells; ++nn)
        {
        vector<int> neighTemp;
        delLoc.getNeighbors(nn,neighTemp);
        allneighs[nn]=neighTemp;
        neighnum.data[nn] = neighTemp.size();
        totaln += neighTemp.size();
        if (neighTemp.size() > nmax) nmax= neighTemp.size();
        h_repair.data[nn]=0;
        };
    if (nmax%2 ==0)
        neighMax = nmax + 2;
    else
        neighMax = nmax + 1;
    cellNeighbors.resize(neighMax*Ncells);

    //store data in gpuarray
    n_idx = Index2D(neighMax,Ncells);
    ArrayHandle<int> ns(cellNeighbors,access_location::host,access_mode::overwrite);
    for (int nn = 0; nn < Ncells; ++nn)
        {
        int imax = neighnum.data[nn];
        for (int ii = 0; ii < imax; ++ii)
            {
            int idxpos = n_idx(ii,nn);
            ns.data[idxpos] = allneighs[nn][ii];
            };
        };

    if(totaln != 6*Ncells)
        {
        printf("CPU neighbor creation failed to match topology! NN = %i \n",totaln);
        char fn[256];
        sprintf(fn,"failed.txt");
        ofstream output(fn);
        getCircumcenterIndices();
        writeTriangulation(output);
        throw std::exception();
        };
    getCircumcenterIndices();
    };

/*!
This function calls the DelaunayCGAL class to determine the Delaunay triangulation of the entire
square periodic domain this method is, obviously, better than the version written by DMS, so
should be the default option. In addition to performing a triangulation, the function also automatically
calls updateNeighIdxs and getCircumcenterIndices/
*/
void DelaunayMD::globalTriangulationCGAL(bool verbose)
    {
    GlobalFixes +=1;
    completeRetriangulationPerformed = 1;
    DelaunayCGAL dcgal;
    ArrayHandle<Dscalar2> h_points(points,access_location::host, access_mode::read);
    vector<pair<Point,int> > Psnew(Ncells);
    for (int ii = 0; ii < Ncells; ++ii)
        {
        Psnew[ii]=make_pair(Point(h_points.data[ii].x,h_points.data[ii].y),ii);
        };
    Dscalar b1,b2,b3,b4;
    Box.getBoxDims(b1,b2,b3,b4);
    dcgal.PeriodicTriangulation(Psnew,b1);

    ArrayHandle<int> neighnum(cellNeighborNum,access_location::host,access_mode::overwrite);
    ArrayHandle<int> h_repair(repair,access_location::host,access_mode::overwrite);

    int oldNmax = neighMax;
    int totaln = 0;
    int nmax = 0;
    for(int nn = 0; nn < Ncells; ++nn)
        {
        neighnum.data[nn] = dcgal.allneighs[nn].size();
        totaln += dcgal.allneighs[nn].size();
        if (dcgal.allneighs[nn].size() > nmax) nmax= dcgal.allneighs[nn].size();
        h_repair.data[nn]=0;
        };
    if (nmax%2 == 0)
        neighMax = nmax+2;
    else
        neighMax = nmax+1;

    n_idx = Index2D(neighMax,Ncells);
    if(neighMax != oldNmax)
        {
        cellNeighbors.resize(neighMax*Ncells);
        neighMaxChange = true;
        };
    updateNeighIdxs();

    //store data in gpuarrays
    {
    ArrayHandle<int> ns(cellNeighbors,access_location::host,access_mode::overwrite);

    for (int nn = 0; nn < Ncells; ++nn)
        {
        int imax = neighnum.data[nn];
        for (int ii = 0; ii < imax; ++ii)
            {
            int idxpos = n_idx(ii,nn);
            ns.data[idxpos] = dcgal.allneighs[nn][ii];
            };
        };

    if(verbose)
        cout << "global new Nmax = " << neighMax << "; total neighbors = " << totaln << endl;cout.flush();
    };

    getCircumcenterIndices(true);

    if(totaln != 6*Ncells)
        {
        printf("global CPU neighbor failed! NN = %i\n",totaln);
        char fn[256];
        sprintf(fn,"failed.txt");
        ofstream output(fn);
        writeTriangulation(output);
        throw std::exception();
        };
    };

//this function updates the NeighIdx data structure, which helps cut down on the number of inactive threads in the force set computation function.
void DelaunayMD::updateNeighIdxs()
    {
    ArrayHandle<int> neighnum(cellNeighborNum,access_location::host,access_mode::read);
    ArrayHandle<int2> h_nidx(NeighIdxs,access_location::host,access_mode::overwrite);
    int idx = 0;
    for (int ii = 0; ii < Ncells; ++ii)
        {
        int nmax = neighnum.data[ii];
        for (int nn = 0; nn < nmax; ++nn)
            {
            h_nidx.data[idx].x = ii;
            h_nidx.data[idx].y = nn;
            idx+=1;
            };
        };
    NeighIdxNum = idx;
    };

/*!
Converts the neighbor list data structure into a list of the three particle indices defining
all of the circumcenters in the triangulation. Keeping this version of the topology on the GPU
allows for fast testing of what points need to be retriangulated.
*/
void DelaunayMD::getCircumcenterIndices(bool secondtime, bool verbose)
    {
    ArrayHandle<int> neighnum(cellNeighborNum,access_location::host,access_mode::read);
    ArrayHandle<int> ns(cellNeighbors,access_location::host,access_mode::read);
    ArrayHandle<int3> h_ccs(circumcenters,access_location::host,access_mode::overwrite);

    int totaln = 0;
    int cidx = 0;
    bool fail = false;
    for (int nn = 0; nn < Ncells; ++nn)
        {
        int nmax = neighnum.data[nn];
        totaln+=nmax;
        for (int jj = 0; jj < nmax; ++jj)
            {
            if (fail) continue;

            int n1 = ns.data[n_idx(jj,nn)];
            int ne2 = jj + 1;
            if (jj == nmax-1)  ne2=0;
            int n2 = ns.data[n_idx(ne2,nn)];
            if (nn < n1 && nn < n2)
                {
                h_ccs.data[cidx].x = nn;
                h_ccs.data[cidx].y = n1;
                h_ccs.data[cidx].z = n2;
                cidx+=1;
                };
            };
        };
    NumCircumCenters = cidx;
    if((totaln != 6*Ncells || cidx != 2*Ncells) && !secondtime)
        globalTriangulationCGAL();
    if((totaln != 6*Ncells || cidx != 2*Ncells) && secondtime)
        {
        char fn[256];
        sprintf(fn,"failed.txt");
        ofstream output(fn);
        writeTriangulation(output);
        printf("step: %i  getCCs failed, %i out of %i ccs, %i out of %i neighs \n",timestep,cidx,2*Ncells,totaln,6*Ncells);
        globalTriangulationCGAL();
        throw std::exception();
        };
    };

/*!
Given a list of particle indices that need to be repaired, call CGAL to figure out their neighbors
and then update the relevant data structures.
*/
void DelaunayMD::repairTriangulation(vector<int> &fixlist)
    {
    int fixes = fixlist.size();
    repPerFrame += ((Dscalar) fixes/(Dscalar)Ncells);
    resetDelLocPoints();

    ArrayHandle<int> neighnum(cellNeighborNum,access_location::host,access_mode::readwrite);

    //First, retriangulate the target points, and check if the neighbor list needs to be reset
    //the structure you want is vector<vector<int> > allneighs(fixes), but below a flattened version is implemented

    vector<int> allneighs;
    allneighs.reserve(fixes*neighMax);
    vector<int> allneighidxstart(fixes);
    vector<int> allneighidxstop(fixes);

    //vector<vector<int> > allneighs(fixes);
    vector<int> neighTemp;
    neighTemp.reserve(10);

    bool resetCCidx = false;
    bool LocalFailure = false;
    bool localTest = false;
    for (int ii = 0; ii < fixes; ++ii)
        {
        int pidx = fixlist[ii];
        localTest = delLoc.getNeighborsCGAL(pidx,neighTemp);
        if(!localTest)
            {
            LocalFailure = true;
            cout << "local triangulation failed...attempting a global triangulation to save the day" << endl << "Note that a particle position has probably become NaN, in which case CGAL will give an assertion violation" << endl;
            break;
            };

        allneighidxstart[ii] = allneighs.size();
        for (int nn = 0; nn < neighTemp.size(); ++nn)
            {
            allneighs.push_back(neighTemp[nn]);
            };
        //allneighs[ii]=neighTemp;

        allneighidxstop[ii] = allneighs.size();
        if(neighTemp.size() > neighMax)
            {
            resetCCidx = true;
            };
        };

    //if needed, regenerate the "neighs" structure...hopefully don't do this too much
    if(resetCCidx || LocalFailure)
        {
        if(resetCCidx)
            neighMaxChange = true;
        globalTriangulationCGAL();
        return;
        };

    //now, edit the right entries of the neighborlist and neighbor size list
    ArrayHandle<int> ns(cellNeighbors,access_location::host,access_mode::readwrite);
    for (int nn = 0; nn < fixes; ++nn)
        {
        int pidx = fixlist[nn];
        //int imax = allneighs[nn].size();
        int imax = allneighidxstop[nn]-allneighidxstart[nn];
        neighnum.data[pidx] = imax;
        for (int ii = 0; ii < imax; ++ii)
            {
            int idxpos = n_idx(ii,pidx);
            //ns.data[idxpos] = allneighs[nn][ii];
            ns.data[idxpos] = allneighs[ii+allneighidxstart[nn]];
            };
        };

    //finally, update the NeighIdx list and Circumcenter list
    updateNeighIdxs();
    getCircumcenterIndices();
    };

/*!
Call the GPU to test each circumcenter to see if it is still empty (i.e., how much of the
triangulation from the last time step is still valid?). Note that because gpu_test_circumcenters
*always* copies at least a single integer back and forth (to answer the question "did any
circumcircle come back non-empty?" for the cpu) this function is always an implicit cuda
synchronization event. At least until non-default streams are added to the code.
*/
void DelaunayMD::testTriangulation()
    {
    //first, update the cell list
    updateCellList();

    //access data handles
    ArrayHandle<Dscalar2> d_pt(points,access_location::device,access_mode::read);

    ArrayHandle<unsigned int> d_cell_sizes(celllist.cell_sizes,access_location::device,access_mode::read);
    ArrayHandle<int> d_c_idx(celllist.idxs,access_location::device,access_mode::read);

    ArrayHandle<int> d_repair(repair,access_location::device,access_mode::readwrite);
    ArrayHandle<int3> d_ccs(circumcenters,access_location::device,access_mode::read);

    int NumCircumCenters = Ncells*2;
    gpu_test_circumcenters(d_repair.data,
                           d_ccs.data,
                           NumCircumCenters,
                           d_pt.data,
                           d_cell_sizes.data,
                           d_c_idx.data,
                           Ncells,
                           celllist.getXsize(),
                           celllist.getYsize(),
                           celllist.getBoxsize(),
                           Box,
                           celllist.cell_indexer,
                           celllist.cell_list_indexer,
                           anyCircumcenterTestFailed
                           );
    };

/*!
perform the same check on the CPU... because of the cost of checking circumcircles and the
relatively poor performance of the 1-ring calculation in DelaunayLoc, it is sometimes better
to just re-triangulate the entire point set with CGAL. At the moment that is the default
behavior of the cpu branch.
*/
void DelaunayMD::testTriangulationCPU()
    {
    anyCircumcenterTestFailed=0;
    if (globalOnly)
        {
        globalTriangulationCGAL();
        skippedFrames -= 1;
        }
    else
        {
        resetDelLocPoints();

        ArrayHandle<int> h_repair(repair,access_location::host,access_mode::readwrite);
        ArrayHandle<int> neighnum(cellNeighborNum,access_location::host,access_mode::readwrite);
        ArrayHandle<int> ns(cellNeighbors,access_location::host,access_mode::readwrite);
        anyCircumcenterTestFailed = 0;
        for (int nn = 0; nn < Ncells; ++nn)
            {
            h_repair.data[nn] = 0;
            vector<int> neighbors;
            neighbors.reserve(neighMax);
            for (int ii = 0; ii < neighnum.data[nn];++ii)
                    {
                    int idxpos = n_idx(ii,nn);
                    neighbors.push_back(ns.data[idxpos]);
                    };

            bool good = delLoc.testPointTriangulation(nn,neighbors,false);
            if(!good)
                {
                h_repair.data[nn] = 1;
                anyCircumcenterTestFailed=1;
                };
            };
        };
    };

//calls the relevant testing and repairing functions. increments the timestep by one
//the call to testTriangulation will synchronize the gpu via a memcpy of "anyCircumcenterTestFailed" variable
void DelaunayMD::testAndRepairTriangulation(bool verb)
    {
    timestep +=1;

    if (verb) printf("testing triangulation\n");
    if(GPUcompute)
        {
        testTriangulation();
        }
    else
        {
        testTriangulationCPU();
        };

    if(anyCircumcenterTestFailed == 1)
        {
        NeedsFixing.clear();
        ArrayHandle<int> h_repair(repair,access_location::host,access_mode::readwrite);
        if(GPUcompute)
            {
            cudaError_t code = cudaGetLastError();
            if(code!=cudaSuccess)
                {
                printf("testAndRepair preliminary GPUassert: %s \n", cudaGetErrorString(code));
                throw std::exception();
                };
            };

        //add the index and all of its' neighbors
        ArrayHandle<int> neighnum(cellNeighborNum,access_location::host,access_mode::readwrite);
        ArrayHandle<int> ns(cellNeighbors,access_location::host,access_mode::readwrite);
        for (int nn = 0; nn < Ncells; ++nn)
            {
            if (h_repair.data[nn] == 1)
                {
                NeedsFixing.push_back(nn);
                h_repair.data[nn] = 0;
                for (int ii = 0; ii < neighnum.data[nn];++ii)
                    {
                    int idxpos = n_idx(ii,nn);
                    NeedsFixing.push_back(ns.data[idxpos]);
                    };
                };
            };
        sort(NeedsFixing.begin(),NeedsFixing.end());
        NeedsFixing.erase(unique(NeedsFixing.begin(),NeedsFixing.end() ),NeedsFixing.end() );

        if (verb) printf("repairing triangulation via %lu\n",NeedsFixing.size());

        if (NeedsFixing.size() > (Ncells/6))
            {
            completeRetriangulationPerformed = 1;
            globalTriangulationCGAL();
            }
        else
            {
            completeRetriangulationPerformed = 0;
            repairTriangulation(NeedsFixing);
            };
        }
    else
        skippedFrames+=1;
    };

//read a triangulation from a text file...used only for testing purposes. Any other use should call the Database class (see inc/Database.h")
void DelaunayMD::readTriangulation(ifstream &infile)
    {
    string line;
    getline(infile,line);
    stringstream convert(line);
    int nn;
    convert >> nn;
    cout << "Reading in " << nn << "points" << endl;
    int idx = 0;
    int ii = 0;
    ArrayHandle<Dscalar2> p(points,access_location::host,access_mode::overwrite);
    while(getline(infile,line))
        {
        Dscalar val = stof(line);
        if (idx == 0)
            {
            p.data[ii].x=val;
            idx +=1;
            }
        else
            {
            p.data[ii].y=val;
            Box.putInBoxReal(p.data[ii]);
            idx = 0;
            ii += 1;
            };
        };
    };

//similarly, write a text file with particle positions. This is often called when an exception is thrown
void DelaunayMD::writeTriangulation(ofstream &outfile)
    {
    ArrayHandle<Dscalar2> p(points,access_location::host,access_mode::read);
    outfile << Ncells <<endl;
    for (int ii = 0; ii < Ncells ; ++ii)
        outfile << p.data[ii].x <<"\t" <<p.data[ii].y <<endl;
    };

//"repel" calculates the displacement due to a harmonic soft repulsion between neighbors. Mostly for testing purposes, but it could be expanded to full functionality later
void DelaunayMD::repel(GPUArray<Dscalar2> &disp,Dscalar eps)
    {
    ArrayHandle<Dscalar2> p(points,access_location::host,access_mode::read);
    ArrayHandle<Dscalar2> dd(disp,access_location::host,access_mode::overwrite);
    ArrayHandle<int> neighnum(cellNeighborNum,access_location::host,access_mode::read);
    ArrayHandle<int> ns(cellNeighbors,access_location::host,access_mode::read);
    Dscalar2 ftot;ftot.x=0.0;ftot.y=0.0;
    for (int ii = 0; ii < Ncells; ++ii)
        {
        Dscalar2 dtot;dtot.x=0.0;dtot.y=0.0;
        Dscalar2 posi = p.data[ii];
        int imax = neighnum.data[ii];
        for (int nn = 0; nn < imax; ++nn)
            {
            int idxpos = n_idx(nn,ii);
            Dscalar2 posj = p.data[ns.data[idxpos]];
            Dscalar2 d;
            Box.minDist(posi,posj,d);

            Dscalar norm = sqrt(d.x*d.x+d.y*d.y);
            if (norm < 1)
                {
                dtot.x-=2*eps*d.x*(1.0-1.0/norm);
                dtot.y-=2*eps*d.y*(1.0-1.0/norm);
                };
            };
        int randmax = 1000000;
        Dscalar xrand = eps*0.1*(-0.5+1.0/(Dscalar)randmax* (Dscalar)(rand()%randmax));
        Dscalar yrand = eps*0.1*(-0.5+1.0/(Dscalar)randmax* (Dscalar)(rand()%randmax));
        dd.data[ii]=dtot;
        ftot.x+=dtot.x+xrand;
        ftot.y+=dtot.y+yrand;
        };
    };
