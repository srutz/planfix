/*
 * Written by Stepan Rutz <stepan.rutz@gmx.de>.
 * Inspired by plantuner, written by Teodor Sigaev.
 *
 * Copyright (c) 2016 Stepan Rutz.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of any co-contributors
 *        may be used to endorse or promote products derived from this software
 *        without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY CONTRIBUTORS ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


/*

Planfix is an extension that allows the user to explicitly force the 
use of a specific index in PostgresQL.

THe particular usecase were Querries that could have (and should have)
used a full-text-index (GIN) but the planner thought it would be a 
better choice to use another index. Reason was (afaics) the multitude
of columns querried and thus the supposed high selectively combined 
with a limit clause, leading to use another index.

With rather high limits the planner would use the text index and be
3 order of magnitude faster, but using this approach here was a bit
more controlled as in the given usecase using the fulltext-index is
definately the right thing to do. In the application right before
the start of the query via a set directive the index-usage is forced
for a given index.

Technically this extension works by parsing out a list of 

PlanfixDirectives

which are kept in a globally allocated list of directives with the 
quite boring name "directives".

Upon planing a query the list is checked and the index is forced.

Plantuner by Teodor Sigaev does similar things, but it maintains
a blacklist of indices, rather than a whitelist like planfix.
Also plantuner would not work with the table-names i have, which
are mixed case and need to be quoted.

*/

#include <postgres.h>

#include <utils/guc.h>
#include <optimizer/plancat.h>
#include <access/heapam.h>

#include <utils/rel.h>
#include <utils/lsyscache.h>
#include <utils/builtins.h>
#include <utils/memutils.h>
#include <nodes/primnodes.h>
#include <nodes/print.h>
#include <catalog/namespace.h>

#include <stdio.h>

PG_MODULE_MAGIC;

/* Declarations */
void SimpleStringSplit(char *s, char separator, List **tokenList);


/*
 * Global variables for planfix
 */

/* the hook pointer */
static get_relation_info_hook_type oldHook = NULL;

/* our memory-context */
static MemoryContext mc;


/* Structure and storage for the directives */
#define PLANFIX_MAX_DIRECTIVES 200

typedef enum PlanfixOp_ {
  PLANFIX_OP_FORCEINDEX
} PlanfixOp;


typedef struct PlanfixDirectives_ {
  PlanfixOp op;
  Oid relation;
  List *indices;
} PlanfixDirective;;

static List *directives = NULL;


/* current values for configuration guc-variables */
static char *varForcedIndex = "";

/* planfix utils */

static void directive_free(PlanfixDirective* d) 
{
  list_free(d->indices);
  free(d);
}

#ifdef PLANFIX_DEBUG
static void directive_print(PlanfixDirective* d) 
{
  ListCell *c;
  printf(">> PlanfixDirective op=%d, relation=%u\n", d->op, d->relation);
  foreach (c, d->indices) {
    Oid index = lfirst_oid(c);
    printf(">>   index=%d\n", index);
  }
}
#endif /* PLANFIX_DEBUG */



/* dealing with set,check,show of forced index */
static bool varForcedIndexCheck(char **newval, void **extra, GucSource source)
{
  return true;
}


static void varForcedIndexAssign(const char *newval, void *extra)
{
  MemoryContext oldmc;
  char *rawname = pstrdup(newval);
  List *sections = NULL;
  List *section = NULL;
  List *tmpdirectives = NULL;
  ListCell *c;

  oldmc = MemoryContextSwitchTo(mc);

  {
    List *fordelete = NULL;
    foreach(c, directives) {
      PlanfixDirective *d = (PlanfixDirective*) lfirst(c);
      if (d->op == PLANFIX_OP_FORCEINDEX) 
	fordelete = lappend(fordelete, d);
    }
    if (fordelete != NULL) {
      foreach(c, fordelete) {
	PlanfixDirective *d = (PlanfixDirective*) lfirst(c);
	directive_free(d);
	directives = list_delete_ptr(directives, d);
      }
    }
  }
  
  SimpleStringSplit(rawname, ';', &sections);
  foreach(c, sections) {
    ListCell *c2;
    char *s = (char *) lfirst(c);
    PlanfixDirective *d = palloc(sizeof(PlanfixDirective));
    section = NULL;
    SimpleStringSplit(s, ',', &section);
    d->op = PLANFIX_OP_FORCEINDEX;
    d->relation = InvalidOid;
    d->indices = NULL;

    foreach (c2, section) {
      Oid oid;
      RangeVar *nameRange;
      List *qualifiedNameList;
      char *name = (char *) lfirst(c2);
      qualifiedNameList = stringToQualifiedNameList(name);
      nameRange = makeRangeVarFromNameList(qualifiedNameList);
      oid = RangeVarGetRelid(nameRange, NoLock, true);

      if (oid == InvalidOid) {
	elog(ERROR, "planfix: oid invalid for name %s", name);
	goto error;
      }  else if (get_rel_relkind(oid) == RELKIND_RELATION) {
	if (d->relation != InvalidOid) {
	  elog(ERROR, "planfix: only one relation must be defined %s", name);
	  goto error;
	}
	d->relation = oid;
      } else if (get_rel_relkind(oid) == RELKIND_INDEX) {
	if (d->relation == InvalidOid) {
	  elog(ERROR, "planfix: one relation must be defined first: %s", name);
	  goto error;
	}
	d->indices = lappend_oid(d->indices, oid);
      } else {
	  elog(ERROR, "planfix: unhandled relkind for %s", name);
	  goto error;
      }
    }
    tmpdirectives = lappend(tmpdirectives, d);
  }

  foreach(c,tmpdirectives) {
    directives = lappend(directives, lfirst(c));
  }

  goto cleanup;

 error:
  foreach (c,tmpdirectives) {
    directive_free((PlanfixDirective*) lfirst(c));
  }
 cleanup:
  list_free(tmpdirectives);
  list_free(sections);
  list_free(section);
  pfree(rawname);
#ifdef PLANFIX_DEBUG
  foreach(c,directives) {
    PlanfixDirective *d = (PlanfixDirective*) lfirst(c);
    directive_print(d);
  }
#endif /* PLANFIX_DEBUG */
  MemoryContextSwitchTo(oldmc);
}


static const char* varForcedIndexShow()
{
  char *v;
  v = palloc(strlen(varForcedIndex) + 1);
  strcpy(v, varForcedIndex);
  return v;
}



/* 
 * Planner hook, loop through the list of directives.
 * The list if expected to be short and we check for the main table
 * relation first, so unless that one is matched we will not incur
 * any overhead.
 */
static void planfixHook(PlannerInfo *root, Oid relationObjectId, bool inhparent,
                        RelOptInfo *rel) 
{
  ListCell *c;
  foreach (c, directives) {
    PlanfixDirective *d = (PlanfixDirective*) lfirst(c);
    if (d->op == PLANFIX_OP_FORCEINDEX && d->relation == relationObjectId && d->indices != NULL) {
      Relation relation;
      relation = heap_open(relationObjectId, NoLock);
#ifdef PLANFIX_DEBUG
      printf(">> checking rel %s\n", get_rel_name(relationObjectId));
#endif
      if (relation->rd_rel->relkind == RELKIND_RELATION) {
	ListCell *c2;
	List *fordelete = NULL;
	foreach (c2, rel->indexlist) {
	  IndexOptInfo *info = (IndexOptInfo *)lfirst(c2);
	  bool allowed = list_member_oid(d->indices, info->indexoid);
#ifdef PLANFIX_DEBUG
	  printf(">>  allowed=%d for indexoid=%d\n", allowed, info->indexoid);
#endif
	  if (!allowed) {
	    fordelete = lappend(fordelete, info);
	  }
	}
	foreach (c2, fordelete) {
	  IndexOptInfo *info = (IndexOptInfo *)lfirst(c2);
	  rel->indexlist = list_delete_ptr(rel->indexlist, info);
	}
      }
      heap_close(relation, NoLock);
    }
  }
  if (oldHook)
    oldHook(root, relationObjectId, inhparent, rel);
}



/*
 * Customer split a string into a tokenlist
 */
void SimpleStringSplit(char *s, char separator, List **tokenList)
{
  char *nextp = s;
  *tokenList = NULL;
  for(;;) {
    int len;
    char *curr = nextp;
    char *endp;
    while (*nextp && *nextp != separator)
      nextp++;
    endp = nextp;
    len = endp - curr;
    if (len >0 ) {
      char *token = palloc(len + 1);
      strncpy(token, curr, len);
      token[len] = '\0';
      *tokenList = lappend(*tokenList, token);
    }
    if (*nextp == '\0')
      break;
    else
      ++nextp;
  }
}



/* 
 * Initialize this extension...
 */
void _PG_init(void);
void _PG_init(void)
{
  mc = AllocSetContextCreate(NULL,
			     "planfix global",
			     ALLOCSET_DEFAULT_MINSIZE,
			     ALLOCSET_DEFAULT_INITSIZE,
			     ALLOCSET_DEFAULT_MAXSIZE);


  DefineCustomStringVariable(
      "planfix.forcedindex",
      "planfix.forcedindex short description",
      "planfix.forcedindex long description",
      &varForcedIndex,
      "", 
      PGC_USERSET,
      0,
      varForcedIndexCheck,
      varForcedIndexAssign,
      varForcedIndexShow);

  if (get_relation_info_hook != planfixHook) {
    oldHook = get_relation_info_hook;
    get_relation_info_hook = planfixHook;
  }

}


