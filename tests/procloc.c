/**
 * Copyright (c) 2013      Los Alamos National Security, LLC.
 *                         All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>

#include "quo.h"

int
main(int argc, char **argv)
{
    int qrc = QUO_SUCCESS;
    int erc = EXIT_SUCCESS;
    int qv = 0;
    int qsv = 0;
    quo_t *quo = NULL;
    char *bad_func = NULL;
    int nsockets = 0;

    if (QUO_SUCCESS != (qrc = quo_version(&qv, &qsv))) {
        bad_func = "quo_version";
        goto out;
    }
    if (QUO_SUCCESS != (qrc = quo_init())) {
        bad_func = "quo_init";
        goto out;
    }
    if (QUO_SUCCESS != (qrc = quo_construct(&quo))) {
        bad_func = "quo_construct";
        goto out;
    }
    if (QUO_SUCCESS != (qrc = quo_nsockets(quo, &nsockets))) {
        bad_func = "quo_nsockets";
        goto out;
    }
    if (QUO_SUCCESS != (qrc = quo_destruct(quo))) {
        bad_func = "quo_destruct";
        goto out;
    }

    fprintf(stdout, "sockets: %d\n",
            nsockets);

out:
    if (NULL != bad_func) {
        erc = EXIT_FAILURE;
    }

    return erc;
}
