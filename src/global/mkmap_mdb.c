/*++
/* NAME
/*	mkmap_mdb 3
/* SUMMARY
/*	create or open database, MDB style
/* SYNOPSIS
/*	#include <mkmap.h>
/*
/*	MKMAP	*mkmap_mdb_open(path)
/*	const char *path;
/*
/* DESCRIPTION
/*	This module implements support for creating MDB databases.
/*
/*	mkmap_mdb_open() takes a file name, appends the ".mdb"
/*	suffix, and does whatever initialization is required
/*	before the OpenLDAP MDB open routine is called.
/*
/*	All errors are fatal.
/* SEE ALSO
/*	dict_mdb(3), MDB dictionary interface.
/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	Howard Chu
/*	Symas Corporation
/*--*/

/* System library. */

#include <sys_defs.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* Utility library. */

#include <msg.h>
#include <mymalloc.h>
#include <stringops.h>
#include <dict.h>
#include <dict_mdb.h>
#include <myflock.h>
#include <warn_stat.h>

/* Global library. */

#include <mail_conf.h>
#include <mail_params.h>

/* Application-specific. */

#include "mkmap.h"

#ifdef HAS_MDB
#ifdef PATH_MDB_H
#include PATH_MDB_H
#else
#include <mdb.h>
#endif

int var_proc_limit;

/* mkmap_mdb_open */

MKMAP *mkmap_mdb_open(const char *path)
{
    MKMAP *mkmap = (MKMAP *) mymalloc(sizeof(*mkmap));
    static const CONFIG_INT_TABLE int_table[] = {
	VAR_PROC_LIMIT, DEF_PROC_LIMIT, &var_proc_limit, 1, 0,
	0,
    };

    get_mail_conf_int_table(int_table);

    /*
     * Override the default per-table map size for map (re)builds.
     *
     * mdb_map_size is defined in util/dict_mdb.c and defaults to 10MB.
     * It needs to be large enough to contain the largest tables in use.
     *
     * XXX This should be specified via the DICT interface so that the buffer
     * size becomes an object property, instead of being specified by poking
     * a global variable so that it becomes a class property.
     */
    dict_mdb_map_size = var_mdb_map_size;

	/*
	 * Set the max number of concurrent readers per table. This is the
	 * maximum number of postfix processes, plus some extra for CLI users.
	 */
    dict_mdb_max_readers = var_proc_limit * 2 + 16;

    /*
     * Fill in the generic members.
     */
    mkmap->open = dict_mdb_open;
    mkmap->after_open = 0;
    mkmap->after_close = 0;

    /*
     * MDB uses MVCC so it needs no special lock management here.
     */

    return (mkmap);
}
#endif
