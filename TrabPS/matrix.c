// to compile: mpicc matrix.c -o matrix -lm -Wall
// to run: mpirun -np order*order matrix <matrix order>
/*
Objective:
This is a matrix multiplication algorithm in MPI.
It does not aim to be faster; but exemplify the use of some MPI resources, such as:
different primitives related to collective communication, spawn of new processes, 
split groups of processes, overlapping of groups and non-blocking standard sends. 

Some details:
There are (order^3) processes. 
(Order^2) processes are started up from mpirun and then more (order-1) processes are spawned by MPI_Comm_spawn
The process with rank 0 (MPI_COMM_WORLD) has specific duties.
*/

#include<string.h>
#include<stdlib.h>
#include<stdio.h>
#include<math.h>
#include"mpi.h"

int
main(int argc, char **argv)
{
	int     my_rank, num_proc, *array_of_errcodes, *c;
    int 	order, i, j, k, color, row_color, column_color;
//  int 	my_rank_x, num_proc_x;  // used just when debugging
    int 	maxprocs, root, key, cij_result;
    int 	*row_array, *column_array, a_oper, b_oper, sendbuf, count, tag;
      
    char 	command[20], **argv_spawn;
    
//  char 	filename[20]; // used just when debugging
//  FILE 	*trace; // used just when debugging

    MPI_Comm	*row_comm, *column_comm, intercomm, cij_comm;
    MPI_Status	status;
    MPI_Request *mpirequest_mr, *mpirequest_rc, *mpirequest_cc;
    
    if (argc != 2) {
	    printf("Wrong number of arguments. Please try again with mpirun -np order*order matrix <order> \n");
        fflush(0);
	    exit(0);
    }//end-if

    // order of our square matrices (A, B and C)
    // this is not necessary indeed, but exemplify how to use argc/argv in a MPI main program.
    order = atoi(argv[1]);
   
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &num_proc);

    // checking...
    if (sqrt(num_proc) != order)  {
	    printf("Ops... something is wrong! Number of processes must be order^2! Pay attention and try again!\n");
        fflush(0);
	    MPI_Finalize();
	    exit(0);
    }//end-if

//  These files are used for tracing purposes. By default, just process 0 has right to print on stdout.
//  Observe the file names. There will be a different file for each process, according to their names.
//  sprintf(filename,"rank_%d.trace", my_rank);
//  trace = fopen(filename, "w");
    
    // determining the row and the column for this process (row oriented)
    row_color = my_rank / order;
    column_color = my_rank % order;

    // creating intra-comunicators for each row and column
    // these new groups will be used to send rows and columns for each Cij process.
    // observe that process rank 0 belongs to all groups.
    row_comm = (MPI_Comm *) calloc(order, sizeof(MPI_Comm));
    column_comm = (MPI_Comm *) calloc(order, sizeof(MPI_Comm));

    mpirequest_mr = (MPI_Request *) calloc(order, sizeof(MPI_Request));
    mpirequest_rc = (MPI_Request *) calloc(order, sizeof(MPI_Request));
    mpirequest_cc = (MPI_Request *) calloc(order, sizeof(MPI_Request));
       
    
    // used by MPI_Comm_spawn
    array_of_errcodes = (int *) calloc(order, sizeof(int));
 
    // used by each Cij process to multiply (operations done by the n processes under each Cij process)
    row_array = (int *) calloc(order, sizeof(int));
    column_array = (int *) calloc(order, sizeof(int));

    // process with rank 0 is the responsible for creating and filling up matrices A and B
    // it also broadcasts the respective rows and columns for Cij processes
    if (my_rank == 0) {
	    int  *a, *b;
	
    	MPI_Datatype dt_col;
  
    	// creating matrices
    	a = (int *) calloc(num_proc, sizeof(int));
    	b = (int *) calloc(num_proc, sizeof(int));
    	c = (int *) calloc(num_proc, sizeof(int));

    	// filling up them
    	k = 0;
    	for(i = 0; i < order; i++) {
            for(j=0; j < order; j++)  {
                a[i*order+j] = b[i*order+j] = k++;
    	        // row_array and column_array are filled up here because they are used during the multiplication
	            // other processes used them, then they will be used in the process 0 as well.
	            if (i == 0)
    		        row_array[j] = (k-1);
	            if (j == 0)
    		        column_array[i] = (k-1);
	           // printf("a[%d][%d]= %d x b[%d][%d]=%d \n", i, j, a[i*order+j], i, j, b[i*order+j]);
    	       // fflush(0);
	    
	        } // end-for-j
	    }// end-for-i

	    // creating intra-communicators for rows and columns
	    // process with rank 0 belongs to all groups of rows and columns, in order to allow collective communication
	    // a loop is necessary because MPI_Comm_split does not allow overlapping groups in each call. 
	    // In this case we need the overlapped groups, with process with rank 0 belonging to all.
	    key = my_rank;
	    for(color = 0; color < order; color++) {
    	    MPI_Comm_split(MPI_COMM_WORLD, color, key, &row_comm[color]);
	        MPI_Comm_split(MPI_COMM_WORLD, color, key, &column_comm[color]);
	    }//end-for-color

		
        //	printing just for debugging purposes
        //	for(i=0; i < order; i++) {
        //	  MPI_Comm_rank(row_comm[i], &my_rank_x);
        //	  MPI_Comm_size(row_comm[i], &num_proc_x);
        //	  printf("Rank 0 => row_comm[%d] size:%2d and rank%2d \n", i, num_proc_x, my_rank_x);
        //	  fflush(0);
        //	  MPI_Comm_rank(column_comm[i], &my_rank_x);
        //	  MPI_Comm_size(column_comm[i], &num_proc_x);
        //	  printf("Rank 0 => column_comm[%d] size:%2d and rank%2d \n", i, num_proc_x, my_rank_x);
        //        fflush(0);
        //	}//end-for-i
	
    	// send rows of A for groups of processes in a row
    	root = 0;
    	for(i = 0; i < order; i++)
	        MPI_Bcast(&a[i*order], order, MPI_INT, root, row_comm[i]);

	    // creating a new data type to send columns of B
	    // prototyping: MPI_Type_vector(count, block_length, stride, old_type, &new_type);
	    MPI_Type_vector(order, 1, order, MPI_INT, &dt_col);
	    MPI_Type_commit(&dt_col);
    	
        // send columns of B for groups of processes in a column
	    root = 0;
	    for(i = 0; i < order; i++)
	        MPI_Bcast(&b[i], 1, dt_col, root, column_comm[i]);
    }// end-if
    else 
	{ // Cij processes with rank != 0
		// processes with MPI_UNDEFINED are excluded from the new communicator/group
		// in this loop the Cij process are split in groups, according to their positions 
		for(i = 0; i < order; i++) 
		{
			if (i != row_color) 
				color = MPI_UNDEFINED;
			else
				color = i;
			
			MPI_Comm_split(MPI_COMM_WORLD, color, my_rank, &row_comm[i]);
	
			if (i != column_color) 
				color = MPI_UNDEFINED;
			else
				color = i;
				MPI_Comm_split(MPI_COMM_WORLD, color, my_rank, &column_comm[i]);
		} // end-for-i

//      printing just for debugging purposes
//      MPI_Comm_rank(row_comm[row_color], &my_rank_x);
//      MPI_Comm_size(row_comm[row_color], &num_proc_x);
//      fprintf(trace, "Rank %d => row_comm[%d] size:%d and rank%2d \n", my_rank, row_color, num_proc_x, my_rank_x);
//      fflush(0);
//      MPI_Comm_rank(column_comm[column_color], &my_rank_x);
//      MPI_Comm_size(column_comm[column_color], &num_proc_x);
//      fprintf(trace, "Rank %d => column_comm[%d] size:%d and rank%2d \n", my_rank, column_color, num_proc_x, my_rank_x);
//      fflush(0);

        // receiving row of A and column of B from process with rank 0
        MPI_Bcast(row_array, order, MPI_INT, 0, row_comm[row_color]);
        MPI_Bcast(column_array, order, MPI_INT, 0, column_comm[column_color]);

//      printing just for debugging purposes
//      for(i = 0; i < order; i++)   {
//	        fprintf(trace, "Rank %d => row_array[%d]=%d column[%d]=%d \n", my_rank, i, row_array[i], i, column_array[i]);
//	        fflush(0);
//      }// end-for

    } // end-else
  
    // now all processes, including rank 0, are executing this code.
    
    // splitting MPI_COMM_WORLD in "num_proc" new groups because MPI_Comm_spawn 
    // is a collective primitive and each process needs creating N processes
    // each one of these (order^2) groups will interact with "order-1" new processes as well.
    color = my_rank;
    key = my_rank;
    MPI_Comm_split(MPI_COMM_WORLD, color, key, &cij_comm);

    // creating "order-1" processes for **each C_ij process**. 
    // MPI_Comm_spawn is a collective primitive, but cij_comm communicator has only one process belonging to.
    // remember that there are "order^2" Cij processes running this code now. 
    // Each one spawn "order-1" processes and has one different intercomm.
    strcpy(command, "multiply_pair");
    argv_spawn = (char **)0;
    maxprocs = order - 1;
    root = 0;
    MPI_Comm_spawn(command, argv_spawn, maxprocs, MPI_INFO_NULL, root, cij_comm, &intercomm, array_of_errcodes);

    // these three non-blocking messages are not necessary indeed. They are here just for debugging processes in multiply_pair code.
    // we are also using them to exemplify non-blocking (standard) sends.
    tag = 0;
    for(i = 0; i < order-1; i++) {
		MPI_Isend(&my_rank,1,MPI_INT, i, tag, intercomm, &mpirequest_mr[i]);
		MPI_Isend(&row_color, 1, MPI_INT, i, tag, intercomm, &mpirequest_rc[i]);
		MPI_Isend(&column_color, 1, MPI_INT, i, tag, intercomm, &mpirequest_cc[i]);
    }//end-for-i
    
    // here we could have started other computing or communication
    
    for(i = 0; i < order-1; i++) {
		MPI_Wait(&mpirequest_mr[i], &status);
		MPI_Wait(&mpirequest_rc[i], &status);
		MPI_Wait(&mpirequest_cc[i], &status);
    }//end-for-i
    
//  printing just for debugging purposes
//  for(i = 0; i < order; i++)   {
//	    fprintf(trace, "Rank %d => row_array[%d]=%d column[%d]=%d \n", my_rank, i, row_array[i], i, column_array[i]);
//	    fflush(0);
//  }// end-for

    // sending a and b operands for each new process, responsible for calculating the multiplication and after making reduction
    MPI_Scatter(&row_array   [1], 1, MPI_INT, &a_oper, 1, MPI_INT, MPI_ROOT, intercomm);
    MPI_Scatter(&column_array[1], 1, MPI_INT, &b_oper, 1, MPI_INT, MPI_ROOT, intercomm);

    sendbuf = row_array[0] * column_array[0];

    // determining the ROOT process for reduce operation. It is an operation with an inter-communicator.
    // MPI_ROOT must be used for root process.
    count = 1;
    //if((my_rank % order) == 0)
    //  root = MPI_ROOT;
    //else
    //  root = 0;
    MPI_Reduce(&sendbuf, &cij_result, count, MPI_INT, MPI_SUM, MPI_ROOT, intercomm);

//  printing just for debugging purposes
//  fprintf(trace, "Rank %d => sendbuf=%d cij_result=%d \n", my_rank, sendbuf, cij_result);
//  fflush(0);

    // reduction evolving inter-communicators does not allow the target process to participate with a value.
    // Then the sum for the "father/mother" process must be made here.
    cij_result += sendbuf;
//  printing just for debugging purposes
//  fprintf(trace, "Rank %d => Antes do Gather=> sendbuf=%d cij_result=%d \n", my_rank, sendbuf, cij_result);
//  fflush(0);
    
    // Process with my_rank == 0 receives all the c_ij already evaluated in their Cij respective positions.
    count = 1;
    root = 0,
    MPI_Gather(&cij_result, count, MPI_INT, c, count, MPI_INT, root, MPI_COMM_WORLD);

    // just process with rank 0, in MPI_COMM_WORLD will print the resulting C matrix
    if(my_rank == 0) {
	    for(i = 0; i < order; i++) {
	        for(j=0; j < order; j++)  {
	            printf("c[%d][%d]= %d \n", i, j, c[i*order+j]);
	            fflush(0);
	        }//end-for-j
	    }//end-for-i
    }// end-if

    // ...
    // that's all folks!
    MPI_Finalize();
    exit(0);
}// end-main
