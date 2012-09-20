#ifndef _DICT_MDB_H_INCLUDED_
#define _DICT_MDB_H_INCLUDED_

/*++
/* NAME
/*	dict_mdb 3h
/* SUMMARY
/*	dictionary manager interface to OpenLDAP MDB files
/* SYNOPSIS
/*	#include <dict_mdb.h>
/* DESCRIPTION
/* .nf

 /*
  * Utility library.
  */
#include <dict.h>

 /*
  * External interface.
  */
#define DICT_TYPE_MDB	"mdb"

extern DICT *dict_mdb_open(const char *, int, int);

 /*
  * XXX Should be part of the DICT interface.
  */
extern size_t dict_mdb_map_size;
extern unsigned int dict_mdb_max_readers;

/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	Howard Chu
/*	Symas Corporation
/*--*/

#endif
