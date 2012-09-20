/*++
/* NAME
/*	dict_mdb 3
/* SUMMARY
/*	dictionary manager interface to OpenLDAP MDB files
/* SYNOPSIS
/*	#include <dict_mdb.h>
/*
/*	DICT	*dict_mdb_open(path, open_flags, dict_flags)
/*	const char *name;
/*	const char *path;
/*	int	open_flags;
/*	int	dict_flags;
/* DESCRIPTION
/*	dict_mdb_open() opens the named MDB database and makes it available
/*	via the generic interface described in dict_open(3).
/*
/*	The dict_mdb_map_size variable specifies a non-default per-table
/*	memory map size.  The map size is 10MB.  The map size is also the
/*	maximum size the table can grow to, so it must be set large enough
/*	to accomodate the largest tables in use.
/* DIAGNOSTICS
/*	Fatal errors: cannot open file, file write error, out of memory.
/* SEE ALSO
/*	dict(3) generic dictionary manager
/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	Howard Chu
/*	Symas Corporation
/*--*/

#include "sys_defs.h"

#ifdef HAS_MDB

/* System library. */

#include <sys/stat.h>
#ifdef PATH_MDB_H
#include PATH_MDB_H
#else
#include <mdb.h>
#endif
#include <string.h>
#include <unistd.h>

/* Utility library. */

#include "msg.h"
#include "mymalloc.h"
#include "htable.h"
#include "iostuff.h"
#include "vstring.h"
#include "myflock.h"
#include "stringops.h"
#include "dict.h"
#include "dict_mdb.h"
#include "warn_stat.h"

/* Application-specific. */

typedef struct {
    DICT    dict;			/* generic members */
    MDB_env *env;			/* MDB environment */
    MDB_dbi dbi;			/* database handle */
    MDB_txn *txn;			/* write transaction for O_TRUNC */
    MDB_cursor *cursor;			/* for sequence ops */
    VSTRING *key_buf;			/* key buffer */
    VSTRING *val_buf;			/* result buffer */
} DICT_MDB;

#define SCOPY(buf, data, size) \
    vstring_str(vstring_strncpy(buf ? buf : (buf = vstring_alloc(10)), data, size))

size_t	dict_mdb_map_size = (10 * 1024 * 1024);	/* 10MB default mmap size */
unsigned int dict_mdb_max_readers = 216;	/* 200 postfix processes, plus some extra */

/* dict_mdb_lookup - find database entry */

static const char *dict_mdb_lookup(DICT *dict, const char *name)
{
    DICT_MDB *dict_mdb = (DICT_MDB *) dict;
    MDB_val   mdb_key;
    MDB_val   mdb_value;
    MDB_txn   *txn;
    const char *result = 0;
    int status, klen;

    dict->error = 0;
    klen = strlen(name);

    /*
     * Sanity check.
     */
    if ((dict->flags & (DICT_FLAG_TRY1NULL | DICT_FLAG_TRY0NULL)) == 0)
	msg_panic("dict_mdb_lookup: no DICT_FLAG_TRY1NULL | DICT_FLAG_TRY0NULL flag");

    /*
     * Optionally fold the key.
     */
    if (dict->flags & DICT_FLAG_FOLD_FIX) {
	if (dict->fold_buf == 0)
	    dict->fold_buf = vstring_alloc(10);
	vstring_strcpy(dict->fold_buf, name);
	name = lowercase(vstring_str(dict->fold_buf));
    }

    /*
     * Start a read transaction if there's no global txn.
     */
    if (dict_mdb->txn)
	txn = dict_mdb->txn;
    else if ((status = mdb_txn_begin(dict_mdb->env, NULL, MDB_RDONLY, &txn)))
	msg_fatal("%s: txn_begin(read) dictionary: %s", dict_mdb->dict.name, mdb_strerror(status));

    /*
     * See if this MDB file was written with one null byte appended to key
     * and value.
     */
    if (dict->flags & DICT_FLAG_TRY1NULL) {
	mdb_key.mv_data = (void *) name;
	mdb_key.mv_size = klen + 1;
	status = mdb_get(txn, dict_mdb->dbi, &mdb_key, &mdb_value);
	if (!status) {
	    dict->flags &= ~DICT_FLAG_TRY0NULL;
	    result = SCOPY(dict_mdb->val_buf, mdb_value.mv_data, mdb_value.mv_size);
	}
    }

    /*
     * See if this MDB file was written with no null byte appended to key and
     * value.
     */
    if (result == 0 && (dict->flags & DICT_FLAG_TRY0NULL)) {
	mdb_key.mv_data = (void *) name;
	mdb_key.mv_size = klen;
	status = mdb_get(txn, dict_mdb->dbi, &mdb_key, &mdb_value);
	if (!status) {
	    dict->flags &= ~DICT_FLAG_TRY1NULL;
	    result = SCOPY(dict_mdb->val_buf, mdb_value.mv_data, mdb_value.mv_size);
	}
    }

    /*
     * Close the read txn if it's not the global txn.
     */
    if (!dict_mdb->txn)
	mdb_txn_abort(txn);

    return (result);
}

/* dict_mdb_update - add or update database entry */

static int dict_mdb_update(DICT *dict, const char *name, const char *value)
{
    DICT_MDB *dict_mdb = (DICT_MDB *) dict;
    MDB_val mdb_key;
    MDB_val mdb_value;
    MDB_txn *txn;
    int     status;

    dict->error = 0;

    /*
     * Sanity check.
     */
    if ((dict->flags & (DICT_FLAG_TRY1NULL | DICT_FLAG_TRY0NULL)) == 0)
	msg_panic("dict_mdb_update: no DICT_FLAG_TRY1NULL | DICT_FLAG_TRY0NULL flag");

    /*
     * Optionally fold the key.
     */
    if (dict->flags & DICT_FLAG_FOLD_FIX) {
	if (dict->fold_buf == 0)
	    dict->fold_buf = vstring_alloc(10);
	vstring_strcpy(dict->fold_buf, name);
	name = lowercase(vstring_str(dict->fold_buf));
    }
    mdb_key.mv_data = (void *) name;
    mdb_value.mv_data = (void *) value;
    mdb_key.mv_size = strlen(name);
    mdb_value.mv_size = strlen(value);

    /*
     * If undecided about appending a null byte to key and value, choose a
     * default depending on the platform.
     */
    if ((dict->flags & DICT_FLAG_TRY1NULL)
	&& (dict->flags & DICT_FLAG_TRY0NULL)) {
#ifdef MDB_NO_TRAILING_NULL
	dict->flags &= ~DICT_FLAG_TRY1NULL;
#else
	dict->flags &= ~DICT_FLAG_TRY0NULL;
#endif
    }

    /*
     * Optionally append a null byte to key and value.
     */
    if (dict->flags & DICT_FLAG_TRY1NULL) {
	mdb_key.mv_size++;
	mdb_value.mv_size++;
    }

    /*
     * Start a write transaction if there's no global txn.
     */
    if (dict_mdb->txn)
	txn = dict_mdb->txn;
    else if ((status = mdb_txn_begin(dict_mdb->env, NULL, 0, &txn)))
	msg_fatal("%s: txn_begin(write) dictionary: %s", dict_mdb->dict.name, mdb_strerror(status));

    /*
     * Do the update.
     */
    status = mdb_put(txn, dict_mdb->dbi, &mdb_key, &mdb_value,
     (dict->flags & DICT_FLAG_DUP_REPLACE) ? 0 : MDB_NOOVERWRITE);
    if (status) {
    	if (status == MDB_KEYEXIST) {
	    if (dict->flags & DICT_FLAG_DUP_IGNORE)
		 /* void */ ;
	    else if (dict->flags & DICT_FLAG_DUP_WARN)
		msg_warn("%s: duplicate entry: \"%s\"", dict_mdb->dict.name, name);
	    else
		msg_fatal("%s: duplicate entry: \"%s\"", dict_mdb->dict.name, name);
	} else {
	    msg_fatal("error writing MDB database %s: %s", dict_mdb->dict.name, mdb_strerror(status));
	}
    }

    /*
     * Commit the transaction if it's not the global txn.
     */
    if (!dict_mdb->txn && ((status = mdb_txn_commit(txn))))
	msg_fatal("error committing MDB database %s: %s", dict_mdb->dict.name, mdb_strerror(status));

    return (status);
}

/* dict_mdb_delete - delete one entry from the dictionary */

static int dict_mdb_delete(DICT *dict, const char *name)
{
    DICT_MDB *dict_mdb = (DICT_MDB *) dict;
    MDB_val mdb_key;
    MDB_txn *txn;
    int     status = 1, klen, rc;

    dict->error = 0;
    klen = strlen(name);

    /*
     * Sanity check.
     */
    if ((dict->flags & (DICT_FLAG_TRY1NULL | DICT_FLAG_TRY0NULL)) == 0)
	msg_panic("dict_mdb_delete: no DICT_FLAG_TRY1NULL | DICT_FLAG_TRY0NULL flag");

    /*
     * Optionally fold the key.
     */
    if (dict->flags & DICT_FLAG_FOLD_FIX) {
	if (dict->fold_buf == 0)
	    dict->fold_buf = vstring_alloc(10);
	vstring_strcpy(dict->fold_buf, name);
	name = lowercase(vstring_str(dict->fold_buf));
    }

    /*
     * Start a write transaction if there's no global txn.
     */
    if (dict_mdb->txn)
	txn = dict_mdb->txn;
    else if ((status = mdb_txn_begin(dict_mdb->env, NULL, 0, &txn)))
	msg_fatal("%s: txn_begin(write) dictionary: %s", dict_mdb->dict.name, mdb_strerror(status));

    /*
     * See if this MDB file was written with one null byte appended to key
     * and value.
     */
    if (dict->flags & DICT_FLAG_TRY1NULL) {
	mdb_key.mv_data = (void *) name;
	mdb_key.mv_size = klen + 1;
	status = mdb_del(txn, dict_mdb->dbi, &mdb_key, NULL);
	if (status) {
	    if (status == MDB_NOTFOUND)
	    	status = 1;
	    else
		msg_fatal("error deleting from %s: %s", dict_mdb->dict.name, mdb_strerror(status));
	} else {
	    dict->flags &= ~DICT_FLAG_TRY0NULL;	/* found */
	}
    }

    /*
     * See if this MDB file was written with no null byte appended to key and
     * value.
     */
    if (status > 0 && (dict->flags & DICT_FLAG_TRY0NULL)) {
	mdb_key.mv_data = (void *) name;
	mdb_key.mv_size = klen;
	status = mdb_del(txn, dict_mdb->dbi, &mdb_key, NULL);
	if (status) {
	    if (status == MDB_NOTFOUND)
	    	status = 1;
	    else
		msg_fatal("error deleting from %s: %s", dict_mdb->dict.name, mdb_strerror(status));
	} else {
	    dict->flags &= ~DICT_FLAG_TRY1NULL;	/* found */
	}
    }

    /*
     * Commit the transaction if it's not the global txn.
     */
    if (!dict_mdb->txn && ((rc = mdb_txn_commit(txn))))
	msg_fatal("error committing MDB database %s: %s", dict_mdb->dict.name, mdb_strerror(rc));

    return (status);
}

/* traverse the dictionary */

static int dict_mdb_sequence(DICT *dict, int function,
			             const char **key, const char **value)
{
    const char *myname = "dict_mdb_sequence";
    DICT_MDB *dict_mdb = (DICT_MDB *) dict;
    MDB_val mdb_key;
    MDB_val mdb_value;
    MDB_txn *txn;
    MDB_cursor_op op;
    int     status;

    dict->error = 0;

    /*
     * Determine the seek function.
     */
    switch (function) {
    case DICT_SEQ_FUN_FIRST:
    	op = MDB_FIRST;
	break;
    case DICT_SEQ_FUN_NEXT:
    	op = MDB_NEXT;
	break;
    default:
	msg_panic("%s: invalid function: %d", myname, function);
    }

    /*
     * Open a read transaction and cursor if needed.
     */
    if (dict_mdb->cursor == 0) {
	if ((status = mdb_txn_begin(dict_mdb->env, NULL, MDB_RDONLY, &txn)))
	    msg_fatal("%s: txn_begin(read) dictionary: %s", dict_mdb->dict.name, mdb_strerror(status));
    	if ((status = mdb_cursor_open(txn, dict_mdb->dbi, &dict_mdb->cursor)))
	    msg_fatal("%s: cursor_open dictionary: %s", dict_mdb->dict.name, mdb_strerror(status));
    }

    /*
     * Database lookup.
     */
    status = mdb_cursor_get(dict_mdb->cursor, &mdb_key, &mdb_value, op);
    if (status && status != MDB_NOTFOUND)
	msg_fatal("%s: seeking dictionary: %s", dict_mdb->dict.name, mdb_strerror(status));

    if (status == MDB_NOTFOUND) {
	/*
	 * Caller must read to end, to ensure cursor gets closed.
	 */
	status = 1;
	txn = mdb_cursor_txn(dict_mdb->cursor);
	mdb_cursor_close(dict_mdb->cursor);
	mdb_txn_abort(txn);
	dict_mdb->cursor = 0;
    } else {

	/*
	 * Copy the key so that it is guaranteed null terminated.
	 */
	*key = SCOPY(dict_mdb->key_buf, mdb_key.mv_data, mdb_key.mv_size);

	if (mdb_value.mv_data != 0 && mdb_value.mv_size > 0) {

	    /*
	     * Copy the value so that it is guaranteed null terminated.
	     */
	    *value = SCOPY(dict_mdb->val_buf, mdb_value.mv_data, mdb_value.mv_size);
	    status = 0;
	} 
    }

    return (status);
}

/* dict_mdb_lock - noop lock handler */

static int dict_mdb_lock(DICT *dict, int unused_op)
{
	/* MDB does its own concurrency control */
	return 0;
}

/* dict_mdb_close - disassociate from data base */

static void dict_mdb_close(DICT *dict)
{
    DICT_MDB *dict_mdb = (DICT_MDB *) dict;

    if (dict_mdb->txn) {
	int status = mdb_txn_commit(dict_mdb->txn);
	if (status)
	    msg_fatal("%s: closing dictionary: %s", dict_mdb->dict.name, mdb_strerror(status));
	dict_mdb->cursor = NULL;
    }
    if (dict_mdb->cursor) {
    	MDB_txn *txn = mdb_cursor_txn(dict_mdb->cursor);
	mdb_cursor_close(dict_mdb->cursor);
    	mdb_txn_abort(txn);
    }
    if (dict_mdb->dict.stat_fd >= 0)
    	close(dict_mdb->dict.stat_fd);
    mdb_env_close(dict_mdb->env);
    if (dict_mdb->key_buf)
	vstring_free(dict_mdb->key_buf);
    if (dict_mdb->val_buf)
	vstring_free(dict_mdb->val_buf);
    if (dict->fold_buf)
	vstring_free(dict->fold_buf);
    dict_free(dict);
}

/* dict_mdb_open - open MDB data base */

DICT   *dict_mdb_open(const char *path, int open_flags, int dict_flags)
{
    DICT_MDB *dict_mdb;
    struct stat st;
    MDB_env *env;
    MDB_txn *txn;
    MDB_dbi dbi;
    char   *mdb_path;
    int	   env_flags, status;

    mdb_path = concatenate(path, ".mdb", (char *) 0);

    env_flags = MDB_NOSUBDIR;
    if (open_flags == O_RDONLY)
    	env_flags |= MDB_RDONLY;

    if ((status = mdb_env_create(&env)))
	msg_fatal("env_create %s: %s", mdb_path, mdb_strerror(status));

    if ((status = mdb_env_set_mapsize(env, dict_mdb_map_size)))
	msg_fatal("env_set_mapsize %s: %s", mdb_path, mdb_strerror(status));

    if ((status = mdb_env_set_maxreaders(env, dict_mdb_max_readers)))
	msg_fatal("env_set_maxreaders %s: %s", mdb_path, mdb_strerror(status));

    if ((status = mdb_env_open(env, mdb_path, env_flags, 0644)))
	msg_fatal("env_open %s: %s", mdb_path, mdb_strerror(status));

    if ((status = mdb_txn_begin(env, NULL, env_flags & MDB_RDONLY, &txn)))
	msg_fatal("txn_begin %s: %s", mdb_path, mdb_strerror(status));

    /* mdb_open requires a txn, but since the default DB always exists
     * in an MDB environment, we don't need to do anything else with
     * the txn.
     */
    if ((status = mdb_open(txn, NULL, 0, &dbi)))
	msg_fatal("mdb_open %s: %s", mdb_path, mdb_strerror(status));

    /* However, if O_TRUNC was specified, we need to do it now.
     * Also with O_TRUNC we keep this write txn for as long as the
     * database is open, since we'll probably be doing a bulk import
     * immediately after.
     */
    if (open_flags & O_TRUNC) {
	if ((status = mdb_drop(txn, dbi, 0)))
	    msg_fatal("truncate %s: %s", mdb_path, mdb_strerror(status));
    } else {
	mdb_txn_abort(txn);
	txn = NULL;
    }

    dict_mdb = (DICT_MDB *) dict_alloc(DICT_TYPE_MDB, path, sizeof(*dict_mdb));
    dict_mdb->dict.lookup = dict_mdb_lookup;
    dict_mdb->dict.update = dict_mdb_update;
    dict_mdb->dict.delete = dict_mdb_delete;
    dict_mdb->dict.sequence = dict_mdb_sequence;
    dict_mdb->dict.close = dict_mdb_close;
    dict_mdb->dict.lock = dict_mdb_lock;
    dict_mdb->dict.stat_fd = open(mdb_path, O_RDONLY);
    if (fstat(dict_mdb->dict.stat_fd, &st) < 0)
	msg_fatal("dict_mdb_open: fstat: %m");
    dict_mdb->dict.mtime = st.st_mtime;
    dict_mdb->dict.owner.uid = st.st_uid;
    dict_mdb->dict.owner.status = (st.st_uid != 0);

    /*
     * Warn if the source file is newer than the indexed file, except when
     * the source file changed only seconds ago.
     */
    if ((dict_flags & DICT_FLAG_LOCK) != 0
	&& stat(path, &st) == 0
	&& st.st_mtime > dict_mdb->dict.mtime
	&& st.st_mtime < time((time_t *) 0) - 100)
	msg_warn("database %s is older than source file %s", mdb_path, path);

    close_on_exec(dict_mdb->dict.stat_fd, CLOSE_ON_EXEC);
    dict_mdb->dict.flags = dict_flags | DICT_FLAG_FIXED;
    if ((dict_flags & (DICT_FLAG_TRY0NULL | DICT_FLAG_TRY1NULL)) == 0)
	dict_mdb->dict.flags |= (DICT_FLAG_TRY0NULL | DICT_FLAG_TRY1NULL);
    if (dict_flags & DICT_FLAG_FOLD_FIX)
	dict_mdb->dict.fold_buf = vstring_alloc(10);
    dict_mdb->env = env;
    dict_mdb->dbi = dbi;

    /* Save the write txn if we opened with O_TRUNC */
    dict_mdb->txn = txn;

    dict_mdb->cursor = 0;
    dict_mdb->key_buf = 0;
    dict_mdb->val_buf = 0;

    myfree(mdb_path);

    return (DICT_DEBUG (&dict_mdb->dict));
}

#endif
