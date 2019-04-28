/*! \file
Copyright (c) 2003, The Regents of the University of California, through
Lawrence Berkeley National Laboratory (subject to receipt of any required
approvals from U.S. Dept. of Energy)

All rights reserved.

The source code is distributed under BSD license, see the file License.txt
at the top-level directory.
*/

#include "superlu_zdefs.h"

#if 0
#include "pdgstrf3d.h"
#include "trfCommWrapper.h"
#endif

#ifdef __INTEL_COMPILER
#include "mkl.h"
#else
#include "cblas.h"
#endif 

int_t zDiagFactIBCast(int_t k,  int_t k0,      // supernode to be factored
                     doublecomplex *BlockUFactor,
                     doublecomplex *BlockLFactor,
                     int_t* IrecvPlcd_D,
                     MPI_Request *U_diag_blk_recv_req,
                     MPI_Request *L_diag_blk_recv_req,
                     MPI_Request *U_diag_blk_send_req,
                     MPI_Request *L_diag_blk_send_req,
                     gridinfo_t *grid,
                     superlu_dist_options_t *options,
                     double thresh,
                     LUstruct_t *LUstruct,
                     SuperLUStat_t *stat, int *info,
                     SCT_t *SCT,
		     int tag_ub
                    )
{
    // unpacking variables
    Glu_persist_t *Glu_persist = LUstruct->Glu_persist;
    LocalLU_t *Llu = LUstruct->Llu;
    int_t* xsup = Glu_persist->xsup;

    int_t iam = grid->iam;
    int_t Pc = grid->npcol;
    int_t Pr = grid->nprow;
    int_t myrow = MYROW (iam, grid);
    int_t mycol = MYCOL (iam, grid);
    int_t pkk = PNUM (PROW (k, grid), PCOL (k, grid), grid);
    int_t krow = PROW (k, grid);
    int_t kcol = PCOL (k, grid);

    //xsup for supersize

    /*Place Irecvs first*/
    // if (IrecvPlcd_D[k] == 0 )
    // {
    int_t nsupc = SuperSize (k);
    if (mycol == kcol && iam != pkk)
    {
        zIRecv_UDiagBlock(k0, BlockUFactor,  /*pointer for the diagonal block*/
                         nsupc * nsupc, krow,
                         U_diag_blk_recv_req, grid, SCT, tag_ub);
    }

    if (myrow == krow && iam != pkk)
    {
        zIRecv_LDiagBlock(k0, BlockLFactor,  /*pointer for the diagonal block*/
                         nsupc * nsupc, kcol,
                         L_diag_blk_recv_req, grid, SCT, tag_ub);
    }
    IrecvPlcd_D[k] = 1;
    // }

    /*DiagFact and send */
    // if ( factored_D[k] == 0 )
    // {

    // int_t pkk = PNUM (PROW (k, grid), PCOL (k, grid), grid);
    // int_t krow = PROW (k, grid);
    // int_t kcol = PCOL (k, grid);
    /*factorize the leaf node and broadcast them
     process row and process column*/
    if (iam == pkk)
    {
        // printf("Entering factorization %d\n", k);
        // int_t offset = (k0 - k_st); // offset is input
        /*factorize A[kk]*/
        Local_Dgstrf2(options, k, thresh,
                      BlockUFactor, /*factored U is over writen here*/
                      Glu_persist, grid, Llu, stat, info, SCT);

        /*Pack L[kk] into blockLfactor*/
        dPackLBlock(k, BlockLFactor, Glu_persist, grid, Llu);

        /*Isend U blocks to the process row*/
        int_t nsupc = SuperSize(k);
        zISend_UDiagBlock(k0, BlockLFactor,
                         nsupc * nsupc, U_diag_blk_send_req , grid, tag_ub);

        /*Isend L blocks to the process col*/
        zISend_LDiagBlock(k0, BlockLFactor,
                         nsupc * nsupc, L_diag_blk_send_req, grid, tag_ub);
        SCT->commVolFactor += 1.0 * nsupc * nsupc * (Pr + Pc);
    }
    // }
    return 0;
}

int_t zLPanelTrSolve( int_t k,   int_t* factored_L,
		      doublecomplex* BlockUFactor,
		      gridinfo_t *grid,
		      LUstruct_t *LUstruct)
{
    Glu_persist_t *Glu_persist = LUstruct->Glu_persist;
    LocalLU_t *Llu = LUstruct->Llu;
    int_t* xsup = Glu_persist->xsup;

    int_t iam = grid->iam;

    int_t pkk = PNUM (PROW (k, grid), PCOL (k, grid), grid);
    int_t kcol = PCOL (k, grid);
    int_t mycol = MYCOL (iam, grid);
    int_t nsupc = SuperSize(k);

    /*factor the L panel*/
    if (mycol == kcol  && iam != pkk)
    {
        // factored_L[k] = 1;
        int_t lk = LBj (k, grid);
        doublecomplex *lusup = Llu->Lnzval_bc_ptr[lk];
        int_t nsupr;
        if (Llu->Lrowind_bc_ptr[lk])
            nsupr = Llu->Lrowind_bc_ptr[lk][1];
        else
            nsupr = 0;
        /*wait for communication to finish*/

        // Wait_UDiagBlock_Recv( U_diag_blk_recv_req, SCT);
        // int_t flag = 0;
        // while (flag == 0)
        // {
        //     flag = Test_UDiagBlock_Recv( U_diag_blk_recv_req, SCT);
        // }

        int_t l = nsupr;
        doublecomplex* ublk_ptr = BlockUFactor;
        int_t ld_ujrow = nsupc;

        // unsigned long long t1 = _rdtsc();

        // #pragma omp for schedule(dynamic) nowait
#define BL  32
        for (int i = 0; i < CEILING(l, BL); ++i)
        {
            #pragma omp task
            {
                int_t off = i * BL;
                // Sherry: int_t len = MY_MIN(BL, l - i * BL);
                int_t len = SUPERLU_MIN(BL, l - i * BL);
                cblas_dtrsm (CblasColMajor, CblasRight, CblasUpper, CblasNoTrans, CblasNonUnit,
                len, nsupc, 1.0, ublk_ptr, ld_ujrow, &lusup[off], nsupr);
            }
        }
    }

    if (iam == pkk)
    {
        /* if (factored_L[k] == 0)
         { */
        /* code */
        factored_L[k] = 1;
        int_t lk = LBj (k, grid);
        doublecomplex *lusup = Llu->Lnzval_bc_ptr[lk];
        int_t nsupr;
        if (Llu->Lrowind_bc_ptr[lk])
            nsupr = Llu->Lrowind_bc_ptr[lk][1];
        else
            nsupr = 0;

        /*factorize A[kk]*/

        int_t l = nsupr - nsupc;

        doublecomplex* ublk_ptr = BlockUFactor;
        int_t ld_ujrow = nsupc;
        // printf("%d: L update \n",k );

#define BL  32
        // #pragma omp parallel for
        for (int i = 0; i < CEILING(l, BL); ++i)
        {
            int_t off = i * BL;
            // Sherry: int_t len = MY_MIN(BL, l - i * BL);
            int_t len = SUPERLU_MIN(BL, (l - i * BL));
            #pragma omp task
            {
                cblas_dtrsm (CblasColMajor, CblasRight, CblasUpper, CblasNoTrans, CblasNonUnit,
                             len, nsupc, 1.0, ublk_ptr, ld_ujrow, &lusup[nsupc + off], nsupr);
            }
        }
    }

    return 0;
}  /* zLPanelTrSolve */

int_t zLPanelUpdate( int_t k,  int_t* IrecvPlcd_D, int_t* factored_L,
                    MPI_Request * U_diag_blk_recv_req,
                    doublecomplex* BlockUFactor,
                    gridinfo_t *grid,
                    LUstruct_t *LUstruct, SCT_t *SCT)
{

    zUDiagBlockRecvWait( k,  IrecvPlcd_D, factored_L,
                         U_diag_blk_recv_req, grid, LUstruct, SCT);

    zLPanelTrSolve( k, factored_L, BlockUFactor, grid, LUstruct );

    return 0;
}  /* zLPanelUpdate */

#define BL  32

int_t zUPanelTrSolve( int_t k,  
                     doublecomplex* BlockLFactor,
                     doublecomplex* bigV,
                     int_t ldt,
                     Ublock_info_t* Ublock_info,
                     gridinfo_t *grid,
                     LUstruct_t *LUstruct,
                     SuperLUStat_t *stat, SCT_t *SCT)
{
    Glu_persist_t *Glu_persist = LUstruct->Glu_persist;
    LocalLU_t *Llu = LUstruct->Llu;
    int_t* xsup = Glu_persist->xsup;
    int_t iam = grid->iam;
    int_t myrow = MYROW (iam, grid);
    int_t pkk = PNUM (PROW (k, grid), PCOL (k, grid), grid);
    int_t krow = PROW (k, grid);
    int_t nsupc = SuperSize(k);

    /*factor the U panel*/
    if (myrow == krow  && iam != pkk)
    {
        int_t lk = LBi (k, grid);         /* Local block number */
        if (!Llu->Unzval_br_ptr[lk])
            return 0;
        /* Initialization. */
        int_t klst = FstBlockC (k + 1);

        int_t *usub = Llu->Ufstnz_br_ptr[lk];  /* index[] of block row U(k,:) */
        doublecomplex *uval = Llu->Unzval_br_ptr[lk];
        int_t nb = usub[0];

        // int_t nsupr = Lsub_buf[1];   /* LDA of lusup[] */
        doublecomplex *lusup = BlockLFactor;

        /* Loop through all the row blocks. to get the iukp and rukp*/
        Trs2_InitUblock_info(klst, nb, Ublock_info, usub, Glu_persist, stat );

        /* Loop through all the row blocks. */
        // #pragma omp for schedule(dynamic,2) nowait
        for (int_t b = 0; b < nb; ++b)
        {
            #pragma omp task
            {
                int_t thread_id = omp_get_thread_num();
                doublecomplex *tempv = bigV +  thread_id * ldt * ldt;
                Trs2_GatherTrsmScatter(klst, Ublock_info[b].iukp, Ublock_info[b].rukp,
				       usub, uval, tempv, nsupc, nsupc, lusup, Glu_persist);
            }
        }
    }

    /*factor the U panel*/
    if (iam == pkk)
    {
        /* code */
        // factored_U[k] = 1;
        int_t *Lsub_buf;
        doublecomplex *Lval_buf;
        int_t lk = LBj (k, grid);
        Lsub_buf = Llu->Lrowind_bc_ptr[lk];
        Lval_buf = Llu->Lnzval_bc_ptr[lk];


        /* calculate U panel */
        // PDGSTRS2 (n, k0, k, Lsub_buf, Lval_buf, Glu_persist, grid, Llu,
        //           stat, HyP->Ublock_info, bigV, ldt, SCT);

        lk = LBi (k, grid);         /* Local block number */
        if (Llu->Unzval_br_ptr[lk])
        {
            /* Initialization. */
            int_t klst = FstBlockC (k + 1);

            int_t *usub = Llu->Ufstnz_br_ptr[lk];  /* index[] of block row U(k,:) */
            doublecomplex *uval = Llu->Unzval_br_ptr[lk];
            int_t nb = usub[0];

            // int_t nsupr = Lsub_buf[1];   /* LDA of lusup[] */
            int_t nsupr = Lsub_buf[1];   /* LDA of lusup[] */
            doublecomplex *lusup = Lval_buf;

            /* Loop through all the row blocks. to get the iukp and rukp*/
            Trs2_InitUblock_info(klst, nb, Ublock_info, usub, Glu_persist, stat );

            /* Loop through all the row blocks. */
            // printf("%d :U update \n", k);
            for (int_t b = 0; b < nb; ++b)
            {
                #pragma omp task
                {
                    int_t thread_id = omp_get_thread_num();
                    doublecomplex *tempv = bigV +  thread_id * ldt * ldt;
                    Trs2_GatherTrsmScatter(klst, Ublock_info[b].iukp, Ublock_info[b].rukp,
					   usub, uval, tempv, nsupc, nsupr, lusup, Glu_persist);
                }

            }
        }
    }

    return 0;
} /* zUPanelTrSolve */

int_t zUPanelUpdate( int_t k,  int_t* factored_U,
                    MPI_Request * L_diag_blk_recv_req,
                    doublecomplex* BlockLFactor,
                    doublecomplex* bigV,
                    int_t ldt,
                    Ublock_info_t* Ublock_info,
                    gridinfo_t *grid,
                    LUstruct_t *LUstruct,
                    SuperLUStat_t *stat, SCT_t *SCT)
{

    LDiagBlockRecvWait( k, factored_U, L_diag_blk_recv_req, grid);

    zUPanelTrSolve( k, BlockLFactor, bigV, ldt, Ublock_info, grid,
                       LUstruct, stat, SCT);
    return 0;
}

int_t zIBcastRecvLPanel(
    int_t k,
    int_t k0,
    int* msgcnt,
    MPI_Request *send_req,
    MPI_Request *recv_req ,
    int_t* Lsub_buf,
    doublecomplex* Lval_buf,
    int_t * factored,
    gridinfo_t *grid,
    LUstruct_t *LUstruct,
    SCT_t *SCT,
    int tag_ub
)
{
    Glu_persist_t *Glu_persist = LUstruct->Glu_persist;
    LocalLU_t *Llu = LUstruct->Llu;
    int_t* xsup = Glu_persist->xsup;
    int_t** ToSendR = Llu->ToSendR;
    int_t* ToRecv = Llu->ToRecv;
    int_t iam = grid->iam;
    int_t Pc = grid->npcol;
    int_t mycol = MYCOL (iam, grid);
    int_t kcol = PCOL (k, grid);
    int_t** Lrowind_bc_ptr = Llu->Lrowind_bc_ptr;
    doublecomplex** Lnzval_bc_ptr = Llu->Lnzval_bc_ptr;
    /* code */
    if (mycol == kcol)
    {
        /*send the L panel to myrow*/

        int_t lk = LBj (k, grid);     /* Local block number. */
        int_t* lsub = Lrowind_bc_ptr[lk];
        doublecomplex* lusup = Lnzval_bc_ptr[lk];

        zIBcast_LPanel (k, k0, lsub, lusup, grid, msgcnt, send_req,
		       ToSendR, xsup, tag_ub);

        if (lsub)
        {
            int_t nrbl  =   lsub[0]; /*number of L blocks */
            int_t   len   = lsub[1];       /* LDA of the nzval[] */
            int_t len1  = len + BC_HEADER + nrbl * LB_DESCRIPTOR;
            int_t len2  = SuperSize(lk) * len;
            SCT->commVolFactor += 1.0 * (Pc - 1) * (len1 * sizeof(int_t) + len2 * sizeof(doublecomplex));
        }
    }
    else
    {
        /*receive factored L panels*/
        if (ToRecv[k] >= 1)     /* Recv block column L(:,0). */
        {
            /*place Irecv*/
            zIrecv_LPanel (k, k0, Lsub_buf, Lval_buf, grid, recv_req, Llu, tag_ub);
        }
        else
        {
            msgcnt[0] = 0;
        }

    }
    factored[k] = 0;

    return 0;
}

int_t zIBcastRecvUPanel(int_t k, int_t k0, int* msgcnt,
    			     MPI_Request *send_requ,
    			     MPI_Request *recv_requ,
    			     int_t* Usub_buf, doublecomplex* Uval_buf,
    			     gridinfo_t *grid, LUstruct_t *LUstruct,
    			     SCT_t *SCT, int tag_ub)
{
    LocalLU_t *Llu = LUstruct->Llu;

    int_t* ToSendD = Llu->ToSendD;
    int_t* ToRecv = Llu->ToRecv;
    int_t iam = grid->iam;
    int_t Pr = grid->nprow;
    int_t myrow = MYROW (iam, grid);
    int_t krow = PROW (k, grid);

    int_t** Ufstnz_br_ptr = Llu->Ufstnz_br_ptr;
    doublecomplex** Unzval_br_ptr = Llu->Unzval_br_ptr;
    if (myrow == krow)
    {
        /*send U panel to myrow*/
        int_t   lk = LBi (k, grid);
        int_t*  usub = Ufstnz_br_ptr[lk];
        doublecomplex* uval = Unzval_br_ptr[lk];
        zIBcast_UPanel(k, k0, usub, uval, grid, msgcnt,
                        send_requ, ToSendD, tag_ub);
        if (usub)
        {
            /* code */
            int_t lenv = usub[1];
            int_t lens = usub[2];
            SCT->commVolFactor += 1.0 * (Pr - 1) * (lens * sizeof(int_t) + lenv * sizeof(doublecomplex));
        }
    }
    else
    {
        /*receive U panels */
        if (ToRecv[k] == 2)     /* Recv block row U(k,:). */
        {
            zIrecv_UPanel (k, k0, Usub_buf, Uval_buf, Llu, grid, recv_requ, tag_ub);
        }
        else
        {
            msgcnt[2] = 0;
        }
    }

    return 0;
}

int_t zWaitL( int_t k, int* msgcnt, int* msgcntU,
              MPI_Request *send_req, MPI_Request *recv_req,
    	      gridinfo_t *grid, LUstruct_t *LUstruct, SCT_t *SCT)
{
    LocalLU_t *Llu = LUstruct->Llu;
    int_t** ToSendR = Llu->ToSendR;
    int_t* ToRecv = Llu->ToRecv;
    int_t iam = grid->iam;
    int_t mycol = MYCOL (iam, grid);
    int_t kcol = PCOL (k, grid);
    if (mycol == kcol)
    {
        /*send the L panel to myrow*/
        Wait_LSend (k, grid, ToSendR, send_req, SCT);
    }
    else
    {
        /*receive factored L panels*/
        if (ToRecv[k] >= 1)     /* Recv block column L(:,0). */
        {
            /*force wait for I recv to complete*/
            zWait_LRecv( recv_req,  msgcnt, msgcntU, grid, SCT);
        }
    }

    return 0;
}

int_t zWaitU( int_t k, int* msgcnt,
              MPI_Request *send_requ, MPI_Request *recv_requ,
    	      gridinfo_t *grid, LUstruct_t *LUstruct, SCT_t *SCT)
{
    LocalLU_t *Llu = LUstruct->Llu;

    int_t* ToRecv = Llu->ToRecv;
    int_t* ToSendD = Llu->ToSendD;
    int_t iam = grid->iam;
    int_t myrow = MYROW (iam, grid);
    int_t krow = PROW (k, grid);
    if (myrow == krow)
    {
        int_t lk = LBi (k, grid);
        if (ToSendD[lk] == YES)
            Wait_USend(send_requ, grid, SCT);
    }
    else
    {
        /*receive U panels */
        if (ToRecv[k] == 2)     /* Recv block row U(k,:). */
        {
            /*force wait*/
            zWait_URecv( recv_requ, msgcnt, SCT);
        }
    }
    return 0;
}
