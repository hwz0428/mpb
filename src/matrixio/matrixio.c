/* Copyright (C) 1999 Massachusetts Institute of Technology.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* matrixio.c: This file layers a "matrixio" abstraction on top of the
   HDF5 binary i/o interface.  This abstraction should make HDF5 much
   easier to use for our purposes, and could also conceivably allow
   us to replace HDF5 with some other file format (e.g. HDF4). */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <check.h>
#include <scalar.h>
#include <mpiglue.h>
#include <mpi_utils.h>

#include "matrixio.h"

static void add_string_attr(matrixio_id id,
			    const char *name, const char *val)
{
     hid_t type_id;
     hid_t space_id;
     hid_t attr_id;

     if (!mpi_is_master())
	  return; /* only one process should add attributes */
     
     if (!val || !name || !name[0] || !val[0])
	  return; /* don't try to create empty attributes */
     
     type_id = H5Tcopy(H5T_C_S1);
     H5Tset_size(type_id, strlen(val) + 1);

     space_id = H5Screate(H5S_SCALAR);

     attr_id = H5Acreate(id, name, type_id, space_id, H5P_DEFAULT);
     CHECK(id >= 0, "error creating HDF attr");

     H5Awrite(attr_id, type_id, (void*) val);

     H5Aclose(attr_id);
     H5Sclose(space_id);
     H5Tclose(type_id);
}

#define FNAME_SUFFIX ".h5"  /* standard HDF5 filename suffix */

static char *add_fname_suffix(const char *fname)
{
     int oldlen = strlen(fname);
     int suflen = strlen(FNAME_SUFFIX);
     char *new_fname;

     CHECK(fname, "null filename!");

     new_fname = (char *) malloc(oldlen + suflen + 1);
     CHECK(new_fname, "out of memory!");

     strcpy(new_fname, fname);

     /* only add suffix if it is not already there: */
     if (strstr(new_fname, FNAME_SUFFIX) != new_fname + oldlen - suflen)
	  strcat(new_fname, FNAME_SUFFIX);

     return new_fname;
}

matrixio_id matrixio_create(const char *fname)
{
     char *new_fname;
     matrixio_id id;
     hid_t access_props;

     access_props = H5Pcreate (H5P_FILE_ACCESS);
     
#ifdef HAVE_MPI
     H5Pset_mpi(access_props, MPI_COMM_WORLD, MPI_INFO_NULL);
#endif

     new_fname = add_fname_suffix(fname);

     id = H5Fcreate(new_fname, H5F_ACC_TRUNC, H5P_DEFAULT, access_props);
     CHECK(id >= 0, "error creating HDF output file");

     free(new_fname);

     H5Pclose(access_props);

     return id;
}

matrixio_id matrixio_open(const char *fname)
{
     char *new_fname;
     matrixio_id id;
     hid_t access_props;

     access_props = H5Pcreate (H5P_FILE_ACCESS);
     
#ifdef HAVE_MPI
     H5Pset_mpi(access_props, MPI_COMM_WORLD, MPI_INFO_NULL);
#endif

     new_fname = add_fname_suffix(fname);

     id = H5Fopen(new_fname, H5F_ACC_RDONLY, access_props);
     CHECK(id >= 0, "error opening HDF input file");

     free(new_fname);

     H5Pclose(access_props);

     return id;
}

void matrixio_close(matrixio_id id)
{
     H5Fclose(id);
}

matrixio_id matrixio_create_sub(matrixio_id id, 
				const char *name, const char *description)
{
     matrixio_id sub_id;

     /* when running a parallel job, only the master process creates the
	group.  It flushes the group to disk and then the other processes
	open the group.  Is this the right thing to do, or is the
        H5Gcreate function parallel-aware? */

     if (mpi_is_master()) {
	  sub_id = H5Gcreate(id, name, 0 /* ==> default size */ );
	  add_string_attr(sub_id, "description", description);
	  
	  H5Fflush(sub_id, H5F_SCOPE_GLOBAL);

	  MPI_Barrier(MPI_COMM_WORLD);
     }
     else {
	  MPI_Barrier(MPI_COMM_WORLD);

	  sub_id = H5Gopen(id, name);
     }

     return sub_id;
}

void matrixio_close_sub(matrixio_id id)
{
     H5Gclose(id);
}

matrixio_id matrixio_create_dataset(matrixio_id id,
				    const char *name, const char *description,
				    int rank, const int *dims)
{
     int i;
     hid_t space_id, type_id, data_id;
     hsize_t *dims_copy;

     CHECK(rank > 0, "non-positive rank");

     dims_copy = (hsize_t *) malloc(sizeof(hsize_t) * rank);
     CHECK(dims_copy, "out of memory!");
     for (i = 0; i < rank; ++i)
          dims_copy[i] = dims[i];

     space_id = H5Screate_simple(rank, dims_copy, NULL);

     free(dims_copy);

#ifdef SCALAR_SINGLE_PREC
     type_id = H5T_NATIVE_FLOAT;
#else
     type_id = H5T_NATIVE_DOUBLE;
#endif
     
     data_id = H5Dcreate(id, name, type_id, space_id, H5P_DEFAULT);

     H5Sclose(space_id);  /* the dataset should have its own copy now */
     
     add_string_attr(data_id, "description", description);

     return data_id;
}

void matrixio_close_dataset(matrixio_id data_id)
{
     H5Dclose(data_id);
}

void matrixio_write_real_data(matrixio_id data_id,
			      const int *local_dims, const int *local_start,
			      int stride,
			      real *data)
{
     int rank;
     hsize_t *dims, *maxdims;
     hid_t space_id, type_id, mem_space_id;
     hssize_t *start;
     hsize_t *strides, *count;
     int i;

     /*******************************************************************/
     /* Get dimensions of dataset */
     
     space_id = H5Dget_space(data_id);

     rank = H5Sget_simple_extent_ndims(space_id);
     
     dims = (hsize_t *) malloc(sizeof(hsize_t) * rank);
     maxdims = (hsize_t *) malloc(sizeof(hsize_t) * rank);
     CHECK(dims && maxdims, "out of memory!");

     H5Sget_simple_extent_dims(space_id, dims, maxdims);

     free(maxdims);

#ifdef SCALAR_SINGLE_PREC
     type_id = H5T_NATIVE_FLOAT;
#else
     type_id = H5T_NATIVE_DOUBLE;
#endif

     /*******************************************************************/
     /* Before we can write the data to the data set, we must define
	the dimensions and "selections" of the arrays to be read & written: */

     start = (hssize_t *) malloc(sizeof(hssize_t) * rank);
     strides = (hsize_t *) malloc(sizeof(hsize_t) * rank);
     count = (hsize_t *) malloc(sizeof(hsize_t) * rank);
     CHECK(start && strides && count, "out of memory!");

     for (i = 0; i < rank; ++i) {
	  start[i] = local_start[i];
	  count[i] = local_dims[i];
	  strides[i] = 1;
     }

     H5Sselect_hyperslab(space_id, H5S_SELECT_SET,
			 start, NULL, count, NULL);

     for (i = 0; i < rank; ++i)
	  start[i] = 0;
     strides[rank - 1] = stride;
     count[rank - 1] *= stride;
     mem_space_id = H5Screate_simple(rank, count, NULL);
     count[rank - 1] = local_dims[rank - 1];
     H5Sselect_hyperslab(mem_space_id, H5S_SELECT_SET,
			 start, stride <= 1 ? NULL : strides, count, NULL);

     /*******************************************************************/
     /* Write the data, then free all the stuff we've allocated. */

     H5Dwrite(data_id, type_id, mem_space_id, space_id, H5P_DEFAULT, 
	      (void*) data);

     H5Sclose(mem_space_id);
     free(count);
     free(strides);
     free(start);
     free(dims);
     H5Sclose(space_id);
}

void matrixio_read_real_data(matrixio_id id,
			     const char *name,
			     int rank, const int *dims,
			     int local_dim0, int local_dim0_start,
			     int stride,
			     real *data)
{
     hid_t space_id, type_id, data_id, mem_space_id;
     hssize_t *start;
     hsize_t *dims_copy, *maxdims, *strides, *count;
     int i;

     CHECK(rank > 0, "non-positive rank");

     /*******************************************************************/
     /* Open the data set and check the dimensions: */

     data_id = H5Dopen(id, name);

     space_id = H5Dget_space(data_id);

     CHECK(rank == H5Sget_simple_extent_ndims(space_id),
	   "rank in HDF5 file doesn't match expected rank");
     
     dims_copy = (hsize_t *) malloc(sizeof(hsize_t) * rank);
     maxdims = (hsize_t *) malloc(sizeof(hsize_t) * rank);
     CHECK(dims_copy && maxdims, "out of memory!");

     H5Sget_simple_extent_dims(space_id, dims_copy, maxdims);

     free(maxdims);

     for (i = 0; i < rank; ++i) {
	  CHECK(dims_copy[i] == dims[i],
		"array size in HDF5 file doesn't match expected size");
     }

#ifdef SCALAR_SINGLE_PREC
     type_id = H5T_NATIVE_FLOAT;
#else
     type_id = H5T_NATIVE_DOUBLE;
#endif

     /*******************************************************************/
     /* Before we can read the data from the data set, we must define
	the dimensions and "selections" of the arrays to be read & written: */

     start = (hssize_t *) malloc(sizeof(hssize_t) * rank);
     strides = (hsize_t *) malloc(sizeof(hsize_t) * rank);
     count = (hsize_t *) malloc(sizeof(hsize_t) * rank);
     CHECK(start && strides && count, "out of memory!");

     for (i = 0; i < rank; ++i) {
	  start[i] = 0;
	  strides[i] = 1;
	  count[i] = dims[i];
     }

     dims_copy[0] = local_dim0;
     dims_copy[rank - 1] *= stride;
     start[0] = 0;
     strides[rank - 1] = stride;
     count[0] = local_dim0;
     mem_space_id = H5Screate_simple(rank, dims_copy, NULL);
     H5Sselect_hyperslab(mem_space_id, H5S_SELECT_SET,
			 start, strides, count, NULL);

     start[0] = local_dim0_start;
     count[0] = local_dim0;
     H5Sselect_hyperslab(space_id, H5S_SELECT_SET,
			 start, NULL, count, NULL);

     /*******************************************************************/
     /* Read the data, then free all the H5 identifiers. */

     H5Dread(data_id, type_id, mem_space_id, space_id, H5P_DEFAULT, 
	      (void*) data);

     H5Sclose(mem_space_id);
     free(count);
     free(strides);
     free(start);
     free(dims_copy);
     H5Sclose(space_id);
     H5Dclose(data_id);
}