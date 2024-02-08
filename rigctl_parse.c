/*
 * Based on rigctl_parse.c, by
 *                  (C) Stephane Fillod 2000-2011
 *                  (C) Nate Bargmann 2003,2006,2008,2010,2011,2012,2013
 *                  (C) Terry Embry 2008-2009
 *                  (C) The Hamlib Group 2002,2006,2007,2008,2009,2010,2011
 *                  (C) Rafael Diniz 2024
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License along
 *   with this program; if not, write to the Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <hamlib/config.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>


#include <hamlib/rig.h>
/* Hash table implementation See:  http://uthash.sourceforge.net/ */
#include <uthash.h>

#include "riglist.h"
#include "rigctl_parse.h"

/* Structure for hash table provided by uthash.h
 *
 * Structure and hash functions patterned after/copied from example.c
 * distributed with the uthash package. See:  http://uthash.sourceforge.net/
 */
struct mod_lst
{
    int id;                 /* caps->rig_model This is the hash key */
    char mfg_name[32];      /* caps->mfg_name */
    char model_name[32];    /* caps->model_name */
    char version[32];       /* caps->version */
    char status[32];        /* caps->status */
    char macro_name[32];        /* caps->status */
    UT_hash_handle hh;      /* makes this structure hashable */
};


/* Hash declaration.  Must be initialized to NULL */
struct mod_lst *models = NULL;

/* Add model information to the hash */
void hash_add_model(int id,
                    const char *mfg_name,
                    const char *model_name,
                    const char *version,
                    const char *status,
                    const char *macro_name)
{
    struct mod_lst *s;

    s = (struct mod_lst *)calloc(1, sizeof(struct mod_lst));

    s->id = id;
    SNPRINTF(s->mfg_name, sizeof(s->mfg_name), "%s", mfg_name);
    SNPRINTF(s->model_name, sizeof(s->model_name), "%s", model_name);
    SNPRINTF(s->version, sizeof(s->version), "%s", version);
    SNPRINTF(s->status, sizeof(s->status), "%s", status);
    SNPRINTF(s->macro_name, sizeof(s->macro_name), "%s", macro_name);

    HASH_ADD_INT(models, id, s);    /* id: name of key field */
}

/* Hash sorting functions */
int hash_model_id_sort(struct mod_lst *a, struct mod_lst *b)
{
    return (a->id > b->id);
}


void hash_sort_by_model_id()
{
    if (models != NULL)
    {
        HASH_SORT(models, hash_model_id_sort);
    }
    else
    {
        rig_debug(RIG_DEBUG_ERR, "%s: models empty?\n", __func__);
    }
}


/* Delete hash */
void hash_delete_all()
{
    struct mod_lst *current_model, *tmp;

    HASH_ITER(hh, models, current_model, tmp)
    {
        HASH_DEL(models, current_model);    /* delete it (models advances to next) */
        free(current_model);                /* free it */
    }
}


static int hash_model_list(const struct rig_caps *caps, void *data)
{
    hash_add_model(caps->rig_model,
                   caps->mfg_name,
                   caps->model_name,
                   caps->version,
                   caps->macro_name,
                   rig_strstatus(caps->status));

    return 1;  /* !=0, we want them all ! */
}


void print_model_list()
{
    struct mod_lst *s;

    for (s = models; s != NULL; s = (struct mod_lst *)(s->hh.next))
    {
        printf("%6d  %-23s%-24s%-16s%-12s%s\n",
               s->id,
               s->mfg_name,
               s->model_name,
               s->version,
               s->macro_name,
               s->status);
    }
}


void list_models()
{
    int status;

    rig_load_all_backends();

    printf(" Rig #  Mfg                    Model                   Version         Status      Macro\n");
    status = rig_list_foreach(hash_model_list, NULL);

    if (status != RIG_OK)
    {
        printf("rig_list_foreach: error = %s \n", rigerror(status));
        exit(2);
    }

    hash_sort_by_model_id();
    print_model_list();
    hash_delete_all();
}

