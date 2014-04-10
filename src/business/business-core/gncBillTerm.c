/********************************************************************\
 * gncBillTerm.c -- the Gnucash Billing Terms interface             *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 59 Temple Place - Suite 330        Fax:    +1-617-542-2652       *
 * Boston, MA  02111-1307,  USA       gnu@gnu.org                   *
 *                                                                  *
\********************************************************************/

/*
 * Copyright (C) 2002 Derek Atkins
 * Copyright (C) 2003 Linas Vepstas <linas@linas.org>
 * Author: Derek Atkins <warlord@MIT.EDU>
 */

#include "config.h"

#include <glib.h>

#include "messages.h"
#include "gnc-numeric.h"
#include "gnc-engine.h"
#include "gnc-engine-util.h"
#include "qofquerycore.h"
#include "gnc-event-p.h"
#include "gnc-be-utils.h"
#include "kvp_frame.h"

#include "qofbook.h"
#include "qofclass.h"
#include "qofid.h"
#include "qofid-p.h"
#include "qofinstance.h"
#include "qofinstance-p.h"
#include "qofobject.h"
#include "qofquery.h"

#include "gncBusiness.h"
#include "gncBillTermP.h"


struct _gncBillTerm 
{
  QofInstance     inst;
  char *          name;
  char *          desc;
  GncBillTermType type;
  gint            due_days;
  gint            disc_days;
  gnc_numeric     discount;
  gint            cutoff;

  /* See src/doc/business.txt for an explanation of the following */
  /* Code that handles this is *identical* to that in gncTaxTable */
  gint64          refcount;
  GncBillTerm *   parent;      /* if non-null, we are an immutable child */
  GncBillTerm *   child;       /* if non-null, we have not changed */
  gboolean        invisible;
  GList *         children;    /* list of children for disconnection */
};

struct _book_info 
{
  GList *         terms;        /* visible terms */
};

static short        module = MOD_BUSINESS;

#define _GNC_MOD_NAME        GNC_ID_BILLTERM

#define CACHE_INSERT(str) g_cache_insert(gnc_engine_get_string_cache(), (gpointer)(str));
#define CACHE_REMOVE(str) g_cache_remove(gnc_engine_get_string_cache(), (str));

#define SET_STR(obj, member, str) { \
        char * tmp; \
        \
        if (!safe_strcmp (member, str)) return; \
        gncBillTermBeginEdit (obj); \
        tmp = CACHE_INSERT (str); \
        CACHE_REMOVE (member); \
        member = tmp; \
        }

static void add_or_rem_object (GncBillTerm *term, gboolean add);
static void maybe_resort_list (GncBillTerm *term);

/* ============================================================== */
/* Misc inline utilities */

static inline void
mark_term (GncBillTerm *term)
{
  term->inst.dirty = TRUE;
  qof_collection_mark_dirty (term->inst.entity.collection);
  gnc_engine_gen_event (&term->inst.entity, GNC_EVENT_MODIFY);
}

static inline void maybe_resort_list (GncBillTerm *term)
{
  struct _book_info *bi;

  if (term->parent || term->invisible) return;
  bi = qof_book_get_data (term->inst.book, _GNC_MOD_NAME);
  bi->terms = g_list_sort (bi->terms, (GCompareFunc)gncBillTermCompare);
}

static inline void add_or_rem_object (GncBillTerm *term, gboolean add)
{
  struct _book_info *bi;

  if (!term) return;
  bi = qof_book_get_data (term->inst.book, _GNC_MOD_NAME);

  if (add)
    bi->terms = g_list_insert_sorted (bi->terms, term,
                                       (GCompareFunc)gncBillTermCompare);
  else
    bi->terms = g_list_remove (bi->terms, term);
}

static inline void addObj (GncBillTerm *term)
{
  add_or_rem_object (term, TRUE);
}

static inline void remObj (GncBillTerm *term)
{
  add_or_rem_object (term, FALSE);
}

static inline void
gncBillTermAddChild (GncBillTerm *table, GncBillTerm *child)
{
  g_return_if_fail(table);
  g_return_if_fail(child);
  g_return_if_fail(table->inst.do_free == FALSE);

  table->children = g_list_prepend(table->children, child);
}

static inline void
gncBillTermRemoveChild (GncBillTerm *table, GncBillTerm *child)
{
  g_return_if_fail(table);
  g_return_if_fail(child);

  if (table->inst.do_free)
    return;

  table->children = g_list_remove(table->children, child);
}

/* ============================================================== */

/* Create/Destroy Functions */
GncBillTerm * gncBillTermCreate (QofBook *book)
{
  GncBillTerm *term;
  if (!book) return NULL;

  term = g_new0 (GncBillTerm, 1);
  qof_instance_init(&term->inst, _GNC_MOD_NAME, book);
  term->name = CACHE_INSERT ("");
  term->desc = CACHE_INSERT ("");
  term->discount = gnc_numeric_zero ();
  addObj (term);
  gnc_engine_gen_event (&term->inst.entity,  GNC_EVENT_CREATE);
  return term;
}

void gncBillTermDestroy (GncBillTerm *term)
{
  if (!term) return;
  term->inst.do_free = TRUE;
  qof_collection_mark_dirty (term->inst.entity.collection);
  gncBillTermCommitEdit (term);
}

static void gncBillTermFree (GncBillTerm *term)
{
  GncBillTerm *child;
  GList *list;

  if (!term) return;

  gnc_engine_gen_event (&term->inst.entity,  GNC_EVENT_DESTROY);
  CACHE_REMOVE (term->name);
  CACHE_REMOVE (term->desc);
  remObj (term);

  if (!term->inst.do_free)
    PERR("free a billterm without do_free set!");

  /* disconnect from parent */
  if (term->parent)
    gncBillTermRemoveChild(term->parent, term);

  /* disconnect from the children */
  for (list = term->children; list; list=list->next) {
    child = list->data;
    gncBillTermSetParent(child, NULL);
  }
  g_list_free(term->children);

  qof_instance_release(&term->inst);
  g_free (term);
}

GncBillTerm *
gncCloneBillTerm (GncBillTerm *from, QofBook *book)
{
  GList *node;
  GncBillTerm *term;

  if (!book) return NULL;

  term = g_new0 (GncBillTerm, 1);
  qof_instance_init(&term->inst, _GNC_MOD_NAME, book);
  qof_instance_gemini (&term->inst, &from->inst);

  term->name = CACHE_INSERT (from->name);
  term->desc = CACHE_INSERT (from->desc);
  term->type = from->type;
  term->due_days = from->due_days;
  term->disc_days = from->disc_days;
  term->discount = from->discount;
  term->cutoff = from->cutoff;
  term->invisible = from->invisible;

  term->refcount = 0;

  /* Make copies of parents and children. Note that this can be
   * a recursive copy ... treat as doubly-linked list. */
  if (from->child)
  {
    term->child = gncBillTermObtainTwin (from->child, book);
    term->child->parent = term;
  }
  if (from->parent)
  {
    term->parent = gncBillTermObtainTwin (from->parent, book);
    term->parent->child = term;
  }
  for (node=g_list_last(from->children); node; node=node->next)
  {
    GncBillTerm *btrm = node->data;
    btrm = gncBillTermObtainTwin (btrm, book);
    btrm->parent = term;
    term->children = g_list_prepend(term->children, btrm);
  }

  addObj (term);
  gnc_engine_gen_event (&term->inst.entity, GNC_EVENT_CREATE);
  return term;
}

GncBillTerm *
gncBillTermObtainTwin (GncBillTerm *from, QofBook *book)
{
  GncBillTerm *term;
  if (!from) return NULL;

  term = (GncBillTerm *) qof_instance_lookup_twin (QOF_INSTANCE(from), book);
  if (!term)
  {
    term = gncCloneBillTerm (from, book);
  }
  return term;
}

/* ============================================================== */
/* Set Functions */

void gncBillTermSetGUID (GncBillTerm *term, const GUID *guid)
{
  if (!term || !guid) return;
  if (guid_equal (guid, &term->inst.entity.guid)) return;

  remObj (term);
  qof_entity_set_guid (&term->inst.entity, guid);
  addObj (term);
}

void gncBillTermSetName (GncBillTerm *term, const char *name)
{
  if (!term || !name) return;
  SET_STR (term, term->name, name);
  mark_term (term);
  maybe_resort_list (term);
  gncBillTermCommitEdit (term);
}

void gncBillTermSetDescription (GncBillTerm *term, const char *desc)
{
  if (!term || !desc) return;
  SET_STR (term, term->desc, desc);
  mark_term (term);
  maybe_resort_list (term);
  gncBillTermCommitEdit (term);
}

void gncBillTermSetType (GncBillTerm *term, GncBillTermType type)
{
  if (!term) return;
  if (term->type == type) return;
  gncBillTermBeginEdit (term);
  term->type = type;
  mark_term (term);
  gncBillTermCommitEdit (term);
}

void gncBillTermSetDueDays (GncBillTerm *term, gint days)
{
  if (!term) return;
  if (term->due_days == days) return;
  gncBillTermBeginEdit (term);
  term->due_days = days;
  mark_term (term);
  gncBillTermCommitEdit (term);
}

void gncBillTermSetDiscountDays (GncBillTerm *term, gint days)
{
  if (!term) return;
  if (term->disc_days == days) return;
  gncBillTermBeginEdit (term);
  term->disc_days = days;
  mark_term (term);
  gncBillTermCommitEdit (term);
}

void gncBillTermSetDiscount (GncBillTerm *term, gnc_numeric discount)
{
  if (!term) return;
  if (gnc_numeric_eq (term->discount, discount)) return;
  gncBillTermBeginEdit (term);
  term->discount = discount;
  mark_term (term);
  gncBillTermCommitEdit (term);
}

void gncBillTermSetCutoff (GncBillTerm *term, gint cutoff)
{
  if (!term) return;
  if (term->cutoff == cutoff) return;
  gncBillTermBeginEdit (term);
  term->cutoff = cutoff;
  mark_term (term);
  gncBillTermCommitEdit (term);
}

/* XXX this doesn't seem right. If the parent/child relationship
 * is a doubly-linked list, then there shouldn't be separate set-parent,
 * set-child routines, else misuse of the routines will goof up
 * relationships.  These ops should be atomic, I think.
 */
void gncBillTermSetParent (GncBillTerm *term, GncBillTerm *parent)
{
  if (!term) return;
  gncBillTermBeginEdit (term);
  if (term->parent)
    gncBillTermRemoveChild(term->parent, term);
  term->parent = parent;
  if (parent)
    gncBillTermAddChild(parent, term);
  term->refcount = 0;
  gncBillTermMakeInvisible (term);
  gncBillTermCommitEdit (term);
}

void gncBillTermSetChild (GncBillTerm *term, GncBillTerm *child)
{
  if (!term) return;
  gncBillTermBeginEdit (term);
  term->child = child;
  gncBillTermCommitEdit (term);
}

void gncBillTermIncRef (GncBillTerm *term)
{
  if (!term) return;
  if (term->parent || term->invisible) return;        /* children dont need refcounts */
  gncBillTermBeginEdit (term);
  term->refcount++;
  gncBillTermCommitEdit (term);
}

void gncBillTermDecRef (GncBillTerm *term)
{
  if (!term) return;
  if (term->parent || term->invisible) return;        /* children dont need refcounts */
  gncBillTermBeginEdit (term);
  term->refcount--;
  g_return_if_fail (term->refcount >= 0);
  gncBillTermCommitEdit (term);
}

void gncBillTermSetRefcount (GncBillTerm *term, gint64 refcount)
{
  if (!term) return;
  term->refcount = refcount;
}

void gncBillTermMakeInvisible (GncBillTerm *term)
{
  if (!term) return;
  gncBillTermBeginEdit (term);
  term->invisible = TRUE;
  add_or_rem_object (term, FALSE);
  gncBillTermCommitEdit (term);
}

void gncBillTermChanged (GncBillTerm *term)
{
  if (!term) return;
  term->child = NULL;
}

void gncBillTermBeginEdit (GncBillTerm *term)
{
  GNC_BEGIN_EDIT (&term->inst);
}

static void gncBillTermOnError (QofInstance *inst, QofBackendError errcode)
{
  PERR("BillTerm QofBackend Failure: %d", errcode);
}

static inline void bill_free (QofInstance *inst)
{
  GncBillTerm *term = (GncBillTerm *) inst;
  gncBillTermFree(term);
}

static inline void on_done (QofInstance *inst) {}

void gncBillTermCommitEdit (GncBillTerm *term)
{
  GNC_COMMIT_EDIT_PART1 (&term->inst);
  GNC_COMMIT_EDIT_PART2 (&term->inst, gncBillTermOnError,
                         on_done, bill_free);
}

/* Get Functions */

GncBillTerm *gncBillTermLookupByName (QofBook *book, const char *name)
{
  GList *list = gncBillTermGetTerms (book);

  for ( ; list; list = list->next) {
    GncBillTerm *term = list->data;
    if (!safe_strcmp (term->name, name))
      return list->data;
  }
  return NULL;
}

GList * gncBillTermGetTerms (QofBook *book)
{
  struct _book_info *bi;
  if (!book) return NULL;

  bi = qof_book_get_data (book, _GNC_MOD_NAME);
  return bi->terms;
}

const char *gncBillTermGetName (GncBillTerm *term)
{
  if (!term) return NULL;
  return term->name;
}

const char *gncBillTermGetDescription (GncBillTerm *term)
{
  if (!term) return NULL;
  return term->desc;
}

GncBillTermType gncBillTermGetType (GncBillTerm *term)
{
  if (!term) return 0;
  return term->type;
}

gint gncBillTermGetDueDays (GncBillTerm *term)
{
  if (!term) return 0;
  return term->due_days;
}

gint gncBillTermGetDiscountDays (GncBillTerm *term)
{
  if (!term) return 0;
  return term->disc_days;
}

gnc_numeric gncBillTermGetDiscount (GncBillTerm *term)
{
  if (!term) return gnc_numeric_zero ();
  return term->discount;
}

gint gncBillTermGetCutoff (GncBillTerm *term)
{
  if (!term) return 0;
  return term->cutoff;
}

static GncBillTerm *gncBillTermCopy (GncBillTerm *term)
{
  GncBillTerm *t;

  if (!term) return NULL;
  t = gncBillTermCreate (term->inst.book);

  gncBillTermBeginEdit(t);

  gncBillTermSetName (t, term->name);
  gncBillTermSetDescription (t, term->desc);

  t->type = term->type;
  t->due_days = term->due_days;
  t->disc_days = term->disc_days;
  t->discount = term->discount;
  t->cutoff = term->cutoff;

  gncBillTermCommitEdit(t);

  return t;
}

GncBillTerm *gncBillTermReturnChild (GncBillTerm *term, gboolean make_new)
{
  GncBillTerm *child = NULL;

  if (!term) return NULL;
  if (term->child) return term->child;
  if (term->parent || term->invisible) return term;
  if (make_new) {
    child = gncBillTermCopy (term);
    gncBillTermSetChild (term, child);
    gncBillTermSetParent (child, term);
  }
  return child;
}

GncBillTerm *gncBillTermGetParent (GncBillTerm *term)
{
  if (!term) return NULL;
  return term->parent;
}

gint64 gncBillTermGetRefcount (GncBillTerm *term)
{
  if (!term) return 0;
  return term->refcount;
}

gboolean gncBillTermGetInvisible (GncBillTerm *term)
{
  if (!term) return FALSE;
  return term->invisible;
}

int gncBillTermCompare (GncBillTerm *a, GncBillTerm *b)
{
  int ret;

  if (!a && !b) return 0;
  if (!a) return -1;
  if (!b) return 1;

  ret = safe_strcmp (a->name, b->name);
  if (ret) return ret;

  return safe_strcmp (a->desc, b->desc);
}

gboolean gncBillTermIsDirty (GncBillTerm *term)
{
  if (!term) return FALSE;
  return term->inst.dirty;
}

/********************************************************/
/* functions to compute dates from Bill Terms           */

#define SECS_PER_DAY 86400

/* Based on the timespec and a proximo type, compute the month and
 * year this is due.  The actual day is filled in below.
 */
static void
compute_monthyear (GncBillTerm *term, Timespec post_date,
                   int *month, int *year)
{
  int iday, imonth, iyear;
  int cutoff = term->cutoff;

  g_return_if_fail (term->type == GNC_TERM_TYPE_PROXIMO);

  gnc_timespec2dmy (post_date, &iday, &imonth, &iyear);

  if (cutoff <= 0)
    cutoff += gnc_timespec_last_mday (post_date);

  if (iday <= cutoff) {
    /* We apply this to next month */
    imonth++;
  } else {
    /* We apply to the following month */
    imonth += 2;
  }

  if (imonth > 12) {
    iyear++;
    imonth -= 12;
  }

  if (month) *month = imonth;
  if (year) *year = iyear;
}

static Timespec
compute_time (GncBillTerm *term, Timespec post_date, int days)
{
  Timespec res = post_date;
  int day, month, year;

  switch (term->type) {
  case GNC_TERM_TYPE_DAYS:
    res.tv_sec += (SECS_PER_DAY * days);
    break;
  case GNC_TERM_TYPE_PROXIMO:
    compute_monthyear (term, post_date, &month, &year);
    day = gnc_date_my_last_mday (month, year);
    if (days < day)
      day = days;
    res = gnc_dmy2timespec (day, month, year);
    break;
  }
  return res;
}

Timespec
gncBillTermComputeDueDate (GncBillTerm *term, Timespec post_date)
{
  Timespec res = post_date;
  if (!term) return res;

  return compute_time (term, post_date, term->due_days);
}

Timespec
gncBillTermComputeDiscountDate (GncBillTerm *term, Timespec post_date)
{
  Timespec res = post_date;
  if (!term) return res;

  return compute_time (term, post_date, term->disc_days);
}

/* Package-Private functions */

static void _gncBillTermCreate (QofBook *book)
{
  struct _book_info *bi;

  if (!book) return;

  bi = g_new0 (struct _book_info, 1);
  qof_book_set_data (book, _GNC_MOD_NAME, bi);
}

static void _gncBillTermDestroy (QofBook *book)
{
  struct _book_info *bi;

  if (!book) return;

  bi = qof_book_get_data (book, _GNC_MOD_NAME);

  g_list_free (bi->terms);
  g_free (bi);
}

static QofObject gncBillTermDesc = 
{
  interface_version:   QOF_OBJECT_VERSION,
  e_type:              _GNC_MOD_NAME,
  type_label:          "Billing Term",
  book_begin:          _gncBillTermCreate,
  book_end:            _gncBillTermDestroy,
  is_dirty:            qof_collection_is_dirty,
  mark_clean:          qof_collection_mark_clean,
  foreach:             qof_collection_foreach,
  printable:           NULL
};

gboolean gncBillTermRegister (void)
{
  static QofParam params[] = {
    { QOF_QUERY_PARAM_BOOK, QOF_ID_BOOK, (QofAccessFunc)qof_instance_get_book, NULL },
    { QOF_QUERY_PARAM_GUID, QOF_TYPE_GUID, (QofAccessFunc)qof_instance_get_guid, NULL },
    { NULL },
  };

  qof_class_register (_GNC_MOD_NAME, (QofSortFunc)gncBillTermCompare, params);

  return qof_object_register (&gncBillTermDesc);
}