/**
 * Copyright (c) 2013      Los Alamos National Security, LLC.
 *                         All rights reserved.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "quo-mpi.h"

#include <stdlib.h>
#include <stdio.h>
#ifdef HAVE_STDBOOL_H
#include <stdbool.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>

#include "mpi.h"

/* don't forget that the upper layer will make sure that all the right stuff
 * will be called in the right order, so we don't have to be so careful
 * about checking if everything has been setup before continuing with the
 * operation. */

/**
 * maintains pid to smprank mapping.
 */
typedef struct pid_smprank_map_t {
    long pid;
    int smprank;
} pid_smprank_map_t;

/* ////////////////////////////////////////////////////////////////////////// */
/* quo_mpi_t type definition */
struct quo_mpi_t {
    /* whether or not mpi is initialized */
    int mpi_inited;
    /* my host's name */
    char hostname[MPI_MAX_PROCESSOR_NAME];
    /* communication channel for libquo mpi bits - dup of MPI_COMM_WORLD */
    MPI_Comm commchan;
    /* node communicator */
    MPI_Comm smpcomm;
    /* number of nodes in the current job */
    int nnodes;
    /* my rank */
    int rank;
    /* number of ranks in comm world */
    int nranks;
    /* my smp (node) rank */
    int smprank;
    /* number of ranks that share a node with me - |pid_smprank_map| */
    int nsmpranks;
    /* pid to smprank map for all ranks that share a node with me */
    pid_smprank_map_t *pid_smprank_map;
    /* array of comm world ranks that share a node with me (includes me) */
    int *node_ranks;
};

/* ////////////////////////////////////////////////////////////////////////// */
/* ////////////////////////////////////////////////////////////////////////// */
/* private utility routines */
/* ////////////////////////////////////////////////////////////////////////// */
/* ////////////////////////////////////////////////////////////////////////// */

/* ////////////////////////////////////////////////////////////////////////// */
static int
cmp_uli(const void *p1,
        const void *p2)
{
    return (*(unsigned long int *)p1 - *(unsigned long int *)p2);
}

/* ////////////////////////////////////////////////////////////////////////// */
static int
get_netnum(const char *hstn,
           unsigned long int *net_num)
{
    struct hostent *host = NULL;

    if (!hstn || !net_num) return QUO_ERR_INVLD_ARG;
    if (NULL == (host = gethostbyname(hstn))) {
        fprintf(stderr, QUO_ERR_PREFIX"%s failed. Cannot continue.\n",
                "gethostbyname");
        return QUO_ERR_SYS;
    }
    /* htonl used here because nodes could be different architectures */
    *net_num = htonl(inet_network(inet_ntoa(*(struct in_addr *)host->h_addr)));
    return QUO_SUCCESS;
}

/* ////////////////////////////////////////////////////////////////////////// */
static int
get_my_color(unsigned long int *net_nums,
             int net_num_len,
             unsigned long int my_net_num,
             int *my_color)
{
    int i = 0, node_i = 0;
    unsigned long int prev_num;

    if (!net_nums || !my_color) return QUO_ERR_INVLD_ARG;
    qsort(net_nums, (size_t)net_num_len, sizeof(unsigned long int), cmp_uli);
    prev_num = net_nums[0];
    while (i < net_num_len && prev_num != my_net_num) {
        while (net_nums[i] == prev_num) {
            ++i;
        }
        ++node_i;
        prev_num = net_nums[i];
    }
    *my_color = node_i;
    return QUO_SUCCESS;
}

/* ////////////////////////////////////////////////////////////////////////// */
static int
smprank_setup(quo_mpi_t *mpi)
{
    int rc = QUO_ERR, mycolor = 0, nnode_contrib = 0;
    unsigned long int my_netnum = 0, *netnums = NULL;

    if (!mpi) return QUO_ERR_INVLD_ARG;
    if (NULL == (netnums = calloc(mpi->nranks, sizeof(*netnums)))) {
        return QUO_ERR_OOR;
    }
    if (QUO_SUCCESS != (rc = get_netnum(mpi->hostname, &my_netnum))) {
        /* rc set */
        goto out;
    }
    /* get everyone else's netnum */
    if (MPI_SUCCESS != (rc = MPI_Allgather(&my_netnum, 1, MPI_UNSIGNED_LONG,
                                           netnums, 1, MPI_UNSIGNED_LONG,
                                           mpi->commchan))) {
        rc = QUO_ERR_MPI;
        goto out;
    }
    if (QUO_SUCCESS != (rc = get_my_color(netnums, mpi->nranks, my_netnum,
                                          &mycolor))) {
        goto out;
    }
    /* split into local node groups */
    if (MPI_SUCCESS != (rc = MPI_Comm_split(mpi->commchan, mycolor, mpi->rank,
                                            &(mpi->smpcomm)))) {
        goto out;
    }
    /* get basic smpcomm info */
    if (MPI_SUCCESS != MPI_Comm_size(mpi->smpcomm, &(mpi->nsmpranks))) {
        rc = QUO_ERR_MPI;
        goto out;
    }
    if (MPI_SUCCESS != MPI_Comm_rank(mpi->smpcomm, &(mpi->smprank))) {
        rc = QUO_ERR_MPI;
        goto out;
    }
    /* calculate how many nodes are in our allocation */
    nnode_contrib = (0 == mpi->smprank) ? 1 : 0;
    if (MPI_SUCCESS != MPI_Allreduce(&nnode_contrib, &mpi->nnodes, 1, MPI_INT,
                                     MPI_SUM, mpi->commchan)) {
        rc = QUO_ERR_MPI;
        goto out;
    }
out:
    if (netnums) free(netnums);
    return rc;
}

/* ////////////////////////////////////////////////////////////////////////// */
/** communication channel setup used for quo communication. */
static int
commchan_setup(quo_mpi_t *mpi)
{
    if (!mpi) return QUO_ERR_INVLD_ARG;
    if (MPI_SUCCESS != MPI_Comm_dup(MPI_COMM_WORLD, &(mpi->commchan))) {
        return QUO_ERR_MPI;
    }
    return QUO_SUCCESS;
}

/* ////////////////////////////////////////////////////////////////////////// */
static int
init_setup(quo_mpi_t *mpi)
{
    int rc = QUO_ERR, hostname_len = 0;

    if (!mpi) return QUO_ERR_INVLD_ARG;
    if (QUO_SUCCESS != (rc = commchan_setup(mpi))) goto out;
    /* gather some basic info that we need */
    if (MPI_SUCCESS != MPI_Comm_size(mpi->commchan, &(mpi->nranks))) {
        rc = QUO_ERR_MPI;
        goto out;
    }
    if (MPI_SUCCESS != MPI_Comm_rank(mpi->commchan, &(mpi->rank))) {
        rc = QUO_ERR_MPI;
        goto out;
    }
    /* get my host's name */
    if (MPI_SUCCESS != MPI_Get_processor_name(mpi->hostname, &hostname_len)) {
        rc = QUO_ERR_MPI;
        goto out;
    }
out:
    return rc;
}

/* ////////////////////////////////////////////////////////////////////////// */
/**
 * pid_smprank_map allocation, setup, and exchange.
 */
static int
pid_smprank_xchange(quo_mpi_t *mpi)
{
    int rc = QUO_SUCCESS;
    pid_smprank_map_t my_info;
    my_info.pid = (long)getpid();
    my_info.smprank = mpi->smprank;

    if (!mpi) return QUO_ERR_INVLD_ARG;
    if (NULL == (mpi->pid_smprank_map = calloc(mpi->nsmpranks,
                                               sizeof(pid_smprank_map_t)))) {
        return QUO_ERR_OOR;
    }
    /* ////////////////////////////////////////////////////////////////////// */
    /* if you update pid_smprank_map, then update this code                   */
    /* ////////////////////////////////////////////////////////////////////// */
    /* number of items in the struct */
    int nitems = 2;
    int block_lens[2] = {1, 1};
    /* array of base mpi types that the struct is made up of */
    MPI_Datatype types[2] = {MPI_LONG, MPI_INT};
    /* the mpi datatype that we are creating */
    MPI_Datatype pid_smprank_type;
    /* type offsets */
    MPI_Aint offsets[2];
    offsets[0] = offsetof(pid_smprank_map_t, pid);
    offsets[1] = offsetof(pid_smprank_map_t, smprank);
    /* create the thing */
    if (MPI_SUCCESS != MPI_Type_create_struct(nitems,
                                              block_lens,
                                              offsets,
                                              types,
                                              &pid_smprank_type)) {
        rc = QUO_ERR_MPI;
        goto out;
    }
    if (MPI_SUCCESS != MPI_Type_commit(&pid_smprank_type)) {
        rc = QUO_ERR_MPI;
        goto out;
    }
    /* now exchange the data between all ranks on the node (via smpcomm) */
    if (MPI_SUCCESS != MPI_Allgather(&my_info, 1, pid_smprank_type,
                                     mpi->pid_smprank_map, 1, pid_smprank_type,
                                     mpi->smpcomm)) {
        return QUO_ERR_MPI;
    }
out:
    /* error path */
    if (QUO_SUCCESS != rc) {
        if (mpi->pid_smprank_map) {
            free(mpi->pid_smprank_map);
            mpi->pid_smprank_map = NULL;
        }
    }
    /* no longer needed */
    if (MPI_SUCCESS != MPI_Type_free(&pid_smprank_type)) rc = QUO_ERR_MPI;
    return rc;
}

/* ////////////////////////////////////////////////////////////////////////// */
static int
node_rank_xchange(quo_mpi_t *mpi)
{
    int rc = QUO_SUCCESS;

    if (!mpi) return QUO_ERR_INVLD_ARG;
    mpi->node_ranks = calloc(mpi->nsmpranks, sizeof(int));
    if (!mpi->node_ranks) return QUO_ERR_OOR;
    if (MPI_SUCCESS != MPI_Allgather(&(mpi->rank), 1, MPI_INT,
                                     mpi->node_ranks, 1, MPI_INT,
                                     mpi->smpcomm)) {
        rc = QUO_ERR_MPI;
        goto out;
    }
out:
    if (QUO_SUCCESS != rc) {
        if (mpi->node_ranks) {
            free(mpi->node_ranks);
            mpi->node_ranks = NULL;
        }
    }
    return rc;
}

/* ////////////////////////////////////////////////////////////////////////// */
int
quo_mpi_smprank2pid(quo_mpi_t *mpi,
                    int smprank,
                    pid_t *out_pid)
{
    if (!mpi || !out_pid) return QUO_ERR_INVLD_ARG;
    *out_pid = 0;
    /* slow. update if too slow... */
    for (int i = 0; i < mpi->nsmpranks; ++i) {
        if (mpi->pid_smprank_map[i].smprank == smprank) {
            *out_pid = (pid_t)mpi->pid_smprank_map[i].pid;
            return QUO_SUCCESS;
        }
    }
    return QUO_ERR_NOT_FOUND;
}

/* ////////////////////////////////////////////////////////////////////////// */
int
quo_mpi_construct(quo_mpi_t **nmpi)
{
    quo_mpi_t *m = NULL;

    if (!nmpi) return QUO_ERR_INVLD_ARG;
    if (NULL == (m = calloc(1, sizeof(*m)))) {
        QUO_OOR_COMPLAIN();
        return QUO_ERR_OOR;
    }
    *nmpi = m;
    return MPI_SUCCESS;
}

/* ////////////////////////////////////////////////////////////////////////// */
int
quo_mpi_init(quo_mpi_t *mpi)
{
    int rc = QUO_ERR;

    if (!mpi) return QUO_ERR_INVLD_ARG;
    if (MPI_SUCCESS != MPI_Initialized(&(mpi->mpi_inited))) return QUO_ERR_MPI;
    /* if mpi isn't initialized, then we can't continue */
    if (!mpi->mpi_inited) {
        fprintf(stderr, QUO_ERR_PREFIX"MPI has not been initialized and %s "
                "uses MPI. Cannot continue.\n", PACKAGE);
        rc = QUO_ERR_MPI;
        goto err;
    }
    /* if we are here, then mpi is initialized */
    mpi->mpi_inited = 1;
    /* first perform basic initialization */
    if (QUO_SUCCESS != (rc = init_setup(mpi))) goto err;
    /* setup node rank info */
    if (QUO_SUCCESS != (rc = smprank_setup(mpi))) goto err;
    /* mpi is setup and we know about our node neighbors and all the jive, so
     * setup and exchange node pids and node ranks. */
    if (QUO_SUCCESS != (rc = pid_smprank_xchange(mpi))) goto err;
    /* now cache the MPI_COMM_WORLD ranks that are the node with me */
    if (QUO_SUCCESS != (rc = node_rank_xchange(mpi))) goto err;
    return QUO_SUCCESS;
err:
    quo_mpi_destruct(mpi);
    return rc;
}

/* ////////////////////////////////////////////////////////////////////////// */
int
quo_mpi_destruct(quo_mpi_t *mpi)
{
    int nerrs = 0;

    if (!mpi) return QUO_ERR_INVLD_ARG;
    if (mpi->mpi_inited) {
        if (MPI_SUCCESS != MPI_Comm_free(&(mpi->commchan))) nerrs++;
        if (MPI_SUCCESS != MPI_Comm_free(&(mpi->smpcomm))) nerrs++;
    }
    if (mpi->pid_smprank_map) {
        free(mpi->pid_smprank_map);
        mpi->pid_smprank_map = NULL;
    }
    if (mpi->node_ranks) {
        free(mpi->node_ranks);
        mpi->node_ranks = NULL;
    }
    free(mpi); mpi = NULL;
    return nerrs == 0 ? QUO_SUCCESS : QUO_ERR_MPI;
}

/* ////////////////////////////////////////////////////////////////////////// */
int
quo_mpi_nnodes(const quo_mpi_t *mpi,
               int *nnodes)
{
    if (!mpi || !nnodes) return QUO_ERR_INVLD_ARG;
    *nnodes = mpi->nnodes;
    return QUO_SUCCESS;
}

/* ////////////////////////////////////////////////////////////////////////// */
int
quo_mpi_nnoderanks(const quo_mpi_t *mpi,
                   int *nnoderanks)
{
    if (!mpi || !nnoderanks) return QUO_ERR_INVLD_ARG;
    *nnoderanks = mpi->nsmpranks;
    return QUO_SUCCESS;
}

/* ////////////////////////////////////////////////////////////////////////// */
/* SKG - i think we can get this info from MPI-3. so in the MPI-3 case, just use
 * the mpi interface to get this info -- should be much faster and more
 * scalable. */
int
quo_mpi_noderank(const quo_mpi_t *mpi,
                 int *noderank)
{
    if (!mpi || !noderank) return QUO_ERR_INVLD_ARG;
    *noderank = mpi->smprank;
    return QUO_SUCCESS;
}

/* ////////////////////////////////////////////////////////////////////////// */
int
quo_mpi_ranks_on_node(const quo_mpi_t *mpi,
                      int *out_nranks,
                      int **out_ranks)
{
    int *ta = NULL;
    if (!mpi || !out_nranks || !out_ranks) return QUO_ERR_INVLD_ARG;
    *out_nranks = mpi->nsmpranks; *out_ranks = NULL;
    if (NULL == (ta = calloc(mpi->nsmpranks, sizeof(int)))) return QUO_ERR_OOR;
    (void)memmove(ta, mpi->node_ranks, mpi->nsmpranks * sizeof(int));
    *out_nranks = mpi->nsmpranks;
    *out_ranks = ta;
    return QUO_SUCCESS;
}
