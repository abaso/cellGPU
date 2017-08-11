#define ENABLE_CUDA

#include "Simple2DCell.h"
#include "Simple2DCell.cuh"
#include "Simple2DActiveCell.h"
/*! \file Simple2DActiveCell.cpp */

/*!
An extremely simple constructor that does nothing, but enforces default GPU operation
*/
Simple2DActiveCell::Simple2DActiveCell() :
    Timestep(0), deltaT(0.01)
    {
    };

/*!
Calls the spatial vertex sorting routine in Simple2DCell, and re-indexes the arrays for the cell
RNGS, as well as the cell motility and cellDirector arrays
*/
void Simple2DActiveCell::spatiallySortVerticesAndCellActivity()
    {
    spatiallySortVertices();
    reIndexCellArray(Motility);
    reIndexCellArray(cellDirectors);
    };

/*!
Calls the spatial vertex sorting routine in Simple2DCell, and re-indexes the arrays for the cell
RNGS, as well as the cell motility and cellDirector arrays
*/
void Simple2DActiveCell::spatiallySortCellsAndCellActivity()
    {
    spatiallySortCells();
    reIndexCellArray(Motility);
    reIndexCellArray(cellDirectors);
    };

/*!
Assign cell directors via a simple, reproducible RNG
*/
void Simple2DActiveCell::setCellDirectorsRandomly()
    {
    cellDirectors.resize(Ncells);
    noise.Reproducible = Reproducible;
    ArrayHandle<Dscalar> h_cd(cellDirectors,access_location::host, access_mode::overwrite);
    for (int ii = 0; ii < Ncells; ++ii)
        h_cd.data[ii] =noise.getRealUniform(0.0,2.0*PI);
    };

/*!
\param v0new the new value of velocity for all cells
\param drnew the new value of the rotational diffusion of cell directors for all cells
*/
void Simple2DActiveCell::setv0Dr(Dscalar v0new,Dscalar drnew)
    {
    Motility.resize(Ncells);
    v0=v0new;
    Dr=drnew;
    if (true)
        {
        ArrayHandle<Dscalar2> h_mot(Motility,access_location::host,access_mode::overwrite);
        for (int ii = 0; ii < Ncells; ++ii)
            {
            h_mot.data[ii].x = v0new;
            h_mot.data[ii].y = drnew;
            };
        };
    };

/*!
\param v0s the per-particle vector of what all velocities will be
\param drs the per-particle vector of what all rotational diffusions will be
*/
void Simple2DActiveCell::setCellMotility(vector<Dscalar> &v0s,vector<Dscalar> &drs)
    {
    Motility.resize(Ncells);
    ArrayHandle<Dscalar2> h_mot(Motility,access_location::host,access_mode::overwrite);
    for (int ii = 0; ii < Ncells; ++ii)
        {
        h_mot.data[ii].x = v0s[ii];
        h_mot.data[ii].y = drs[ii];
        };
    };

/*!
This function supports cellDivisions, updating data structures in Simple2DActiveCell
This function will first call Simple2DCell's routine, and then
grows the cellDirectors and Motility arrays, and assign the new cell
(the last element of those arrays) the values of the cell given by parameters[0]
Note that dParams does nothing
 */
void Simple2DActiveCell::cellDivision(const vector<int> &parameters, const vector<Dscalar> &dParams)
    {
    //The Simple2DCell routine will increment Ncells by one, and then update other data structures
    Simple2DCell::cellDivision(parameters);
    int cellIdx = parameters[0];
    growGPUArray(cellDirectors,1);
    growGPUArray(Motility,1);
    noise.Reproducible = Reproducible;
        {//arrayhandle scope
        ArrayHandle<Dscalar2> h_mot(Motility); h_mot.data[Ncells-1] = h_mot.data[cellIdx];
        ArrayHandle<Dscalar> h_cd(cellDirectors); h_cd.data[Ncells-1] = noise.getRealUniform(0.,2*PI);
        };
    };
