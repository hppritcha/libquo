/*
 * Copyright (c) 2017      Los Alamos National Security, LLC
 *                         All rights reserved.
 *
 * This file is part of the libquo project. See the LICENSE file at the
 * top-level directory of this distribution.
 */

/**
 * @file quo-xpm.c
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "quo-xpm.h"
#include "quo-private.h"
#include "quo-hwloc.c"
#include "quo-mpi.c"
#include "quo.h"

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STDBOOL_H
#include <stdbool.h>
#endif

/** quo_xpm_t type definition. */
struct quo_xpm_t {
    /** Handle to the QUO context associated with allocation. */
    QUO_t *qc;
    /** Copy of node communicator. */
    MPI_Comm node_comm;
    /** Flag indicating whether or not I am the memory custodian. */
    bool custodian;
    /** Used as a backing store for the cooperative allocation. */
    quo_sm_t *qsm_segment;
    /** Process-local size of memory allocation. */
    size_t local_size;
    /** Node-local size of memory allocation (total). */
    size_t global_size;
    /** Array of local sizes indexed by qid. */
    size_t *local_sizes;
};

/* ////////////////////////////////////////////////////////////////////////// */
static size_t
range_sum(size_t *array,
          int start,
          int end)
{
    size_t res = 0;

    for (int i = start; i < end; ++i) {
        res += array[i];
    }

    return res;
}

/* ////////////////////////////////////////////////////////////////////////// */
static int
mem_segment_create(quo_xpm_t *xpm)
{
    int qrc = QUO_SUCCESS;

    long long int lsize = (long long int)xpm->local_size;
    long long int *lsizes = calloc(xpm->qc->nqid, sizeof(*lsizes));
    if (!lsizes) {
        QUO_OOR_COMPLAIN();
        qrc = QUO_ERR_OOR;
        goto out;
    }
    /* Exchange local sizes across all processes in node-local comm. */
    if (QUO_SUCCESS != (qrc = quo_mpi_allgather(&lsize, 1, MPI_LONG_LONG_INT,
                                                lsizes, 1, MPI_LONG_LONG_INT,
                                                xpm->node_comm))) {
        qrc = QUO_ERR_MPI;
    }
    /* Copy back. */
    for (int i = 0; i < xpm->qc->nqid; ++i) {
        xpm->local_sizes[i] = lsizes[i];
    }
    free(lsizes);

    xpm->global_size = range_sum(xpm->local_sizes, 0, xpm->qc->nqid - 1); 

out:
    return qrc;
}

/* ////////////////////////////////////////////////////////////////////////// */
static int
destruct_xpm(quo_xpm_t *xpm)
{
    if (xpm) {
        (void)quo_sm_destruct(xpm->qsm_segment);
        if (xpm->local_sizes) free(xpm->local_sizes);
        free(xpm);
    }
    /* okay to pass NULL here. just return success */
    return QUO_SUCCESS;
}

/* ////////////////////////////////////////////////////////////////////////// */
static int
construct_xpm(QUO_t *qc,
              size_t local_size,
              quo_xpm_t **new_xpm)
{
    int qrc = QUO_SUCCESS;
    quo_xpm_t *txpm = NULL;

    if (NULL == (txpm = calloc(1, sizeof(*txpm)))) {
        QUO_OOR_COMPLAIN();
        qrc = QUO_ERR_OOR;
        goto out;
    }
    if (NULL == (txpm->local_sizes = calloc(qc->nqid, sizeof(size_t)))) {
        QUO_OOR_COMPLAIN();
        qrc = QUO_ERR_OOR;
        goto out;
    }
    if (QUO_SUCCESS != (qrc = quo_sm_construct(&txpm->qsm_segment))) {
        QUO_ERR_MSGRC("quo_sm_construct", qrc);
        goto out;
    }
    if (QUO_SUCCESS != (qrc = quo_mpi_get_node_comm(qc->mpi, &txpm->node_comm))) {
        QUO_ERR_MSGRC("quo_mpi_get_node_comm", qrc);
        goto out;
    }
    txpm->qc = qc;
    txpm->custodian = (0 == qc->qid) ? true : false;
    txpm->local_size = local_size;
out:
    if (QUO_SUCCESS != qrc) {
        (void)destruct_xpm(txpm);
        *new_xpm = NULL;
    }
    else {
        *new_xpm = txpm;
    }

    return qrc;
}

/* ////////////////////////////////////////////////////////////////////////// */
int
QUO_xpm_allocate(QUO_t *qc,
                 size_t local_size,
                 quo_xpm_t **new_xpm)
{
    int qrc = QUO_SUCCESS;

    if (!qc || !new_xpm) return QUO_ERR_INVLD_ARG;

    /* make sure we are initialized before we continue */
    QUO_NO_INIT_ACTION(qc);

    if (QUO_SUCCESS != (qrc = construct_xpm(qc, local_size, new_xpm))) {
        QUO_ERR_MSGRC("construct_xpm", qrc);
        goto out;
    }
    if (QUO_SUCCESS != (qrc = mem_segment_create(*new_xpm))) {
        QUO_ERR_MSGRC("mem_segment_create", qrc);
        goto out;
    }
out:
    if (QUO_SUCCESS != qrc) {
        (void)destruct_xpm(*new_xpm);
        *new_xpm = NULL;
    }
    return qrc;
}

/* ////////////////////////////////////////////////////////////////////////// */
int
QUO_xpm_free(quo_xpm_t *xpm)
{
    return destruct_xpm(xpm);
}

/* ////////////////////////////////////////////////////////////////////////// */
int
QUO_xpm_view_by_qid(quo_xpm_t *xc,
                    int qid_start,
                    int qid_end,
                    QUO_xpm_view_t *rview)
{
    if (!xc || !rview) return QUO_ERR_INVLD_ARG;
    return QUO_SUCCESS;
}
