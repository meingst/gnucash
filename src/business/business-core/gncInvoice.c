/*
 * gncInvoice.c -- the Core Business Invoice
 * Copyright (C) 2001 Derek Atkins
 * Author: Derek Atkins <warlord@MIT.EDU>
 */

#include "config.h"

#include <glib.h>

#include "Transaction.h"
#include "Account.h"
#include "messages.h"
#include "gnc-numeric.h"
#include "kvp_frame.h"
#include "gnc-engine-util.h"
#include "gnc-book-p.h"
#include "GNCIdP.h"

#include "gncBusiness.h"
#include "gncEntry.h"
#include "gncEntryP.h"
#include "gncInvoice.h"
#include "gncInvoiceP.h"
#include "gncOwner.h"

struct _gncInvoice {
  GNCBook *book;
  
  GUID		guid;
  char *	id;
  char *	notes;
  GList * 	entries;
  GncOwner	owner;
  Timespec 	date_opened;
  Timespec 	date_due;
  Timespec 	date_closed;
  gint 		terms;

  Account * 	posted_acc;
  Transaction * posted_txn;

  gboolean 	active;

  gboolean	dirty;
};

#define _GNC_MOD_NAME	GNC_ENTRY_MODULE_NAME

#define GNC_INVOICE_ID		"gncInvoice"
#define GNC_INVOICE_GUID	"invoice-guid"

#define CACHE_INSERT(str) g_cache_insert(gnc_engine_get_string_cache(), (gpointer)(str));
#define CACHE_REMOVE(str) g_cache_remove(gnc_engine_get_string_cache(), (str));

#define SET_STR(member, str) { \
	char * tmp; \
	\
	if (!safe_strcmp (member, str)) return; \
	tmp = CACHE_INSERT (str); \
	CACHE_REMOVE (member); \
	member = tmp; \
	}

static void addObj (GncInvoice *invoice);
static void remObj (GncInvoice *invoice);

/* Create/Destroy Functions */

GncInvoice *gncInvoiceCreate (GNCBook *book)
{
  GncInvoice *invoice;

  if (!book) return NULL;

  invoice = g_new0 (GncInvoice, 1);
  invoice->book = book;

  invoice->id = CACHE_INSERT ("");
  invoice->notes = CACHE_INSERT ("");

  invoice->active = TRUE;

  xaccGUIDNew (&invoice->guid, book);
  addObj (invoice);

  return invoice;
}

void gncInvoiceDestroy (GncInvoice *invoice)
{
  if (!invoice) return;

  CACHE_REMOVE (invoice->id);
  CACHE_REMOVE (invoice->notes);
  g_list_free (invoice->entries);
  remObj (invoice);

  g_free (invoice);
}

/* Set Functions */

void gncInvoiceSetGUID (GncInvoice *invoice, const GUID *guid)
{
  if (!invoice || !guid) return;
  if (guid_equal (guid, &invoice->guid)) return;

  remObj (invoice);
  invoice->guid = *guid;
  addObj (invoice);
}

void gncInvoiceSetID (GncInvoice *invoice, const char *id)
{
  if (!invoice || !id) return;
  SET_STR (invoice->id, id);
  invoice->dirty = TRUE;
}

void gncInvoiceSetOwner (GncInvoice *invoice, GncOwner *owner)
{
  if (!invoice || !owner) return;
  gncOwnerCopy (owner, &invoice->owner);
  invoice->dirty = TRUE;
}

void gncInvoiceSetDateOpened (GncInvoice *invoice, const Timespec *date)
{
  if (!invoice || !date) return;
  invoice->date_opened = *date;
  invoice->dirty = TRUE;
}

void gncInvoiceSetDateDue (GncInvoice *invoice, const Timespec *date)
{
  if (!invoice || !date) return;
  invoice->date_due = *date;
  invoice->dirty = TRUE;
}

void gncInvoiceSetDateClosed (GncInvoice *invoice, const Timespec *date)
{
  if (!invoice || !date) return;
  invoice->date_closed = *date;
  invoice->dirty = TRUE;
}

void gncInvoiceSetTerms (GncInvoice *invoice, gint terms)
{
  if (!invoice) return;
  invoice->terms = terms;
  invoice->dirty = TRUE;
}

void gncInvoiceSetNotes (GncInvoice *invoice, const char *notes)
{
  if (!invoice || !notes) return;
  SET_STR (invoice->notes, notes);
  invoice->dirty = TRUE;
}

void gncInvoiceSetActive (GncInvoice *invoice, gboolean active)
{
  if (!invoice) return;
  invoice->active = active;
  invoice->dirty = TRUE;
}

void gncInvoiceSetDirty (GncInvoice *invoice, gboolean dirty)
{
  if (!invoice) return;
  invoice->dirty = dirty;
}

void gncInvoiceSetPostedTxn (GncInvoice *invoice, Transaction *txn)
{
  if (!invoice) return;

  invoice->posted_txn = txn;
  invoice->dirty = TRUE;
}

void gncInvoiceSetPostedAcc (GncInvoice *invoice, Account *acc)
{
  if (!invoice) return;

  invoice->posted_acc = acc;
  invoice->dirty = TRUE;
}

void gncInvoiceAddEntry (GncInvoice *invoice, GncEntry *entry)
{
  GncInvoice *old;

  if (!invoice || !entry) return;

  old = gncEntryGetInvoice (entry);
  if (old == invoice) return;	/* I already own this one */
  if (old) gncInvoiceRemoveEntry (old, entry);

  gncEntrySetInvoice (entry, invoice);
  invoice->entries = g_list_append (invoice->entries, entry);
  invoice->dirty = TRUE;
}

void gncInvoiceRemoveEntry (GncInvoice *invoice, GncEntry *entry)
{
  if (!invoice || !entry) return;

  gncEntrySetInvoice (entry, NULL);
  invoice->entries = g_list_remove (invoice->entries, entry);
  invoice->dirty = TRUE;
}

/* Get Functions */

GNCBook * gncInvoiceGetBook (GncInvoice *invoice)
{
  if (!invoice) return NULL;
  return invoice->book;
}

const GUID * gncInvoiceGetGUID (GncInvoice *invoice)
{
  if (!invoice) return NULL;
  return &(invoice->guid);
}

const char * gncInvoiceGetID (GncInvoice *invoice)
{
  if (!invoice) return NULL;
  return invoice->id;
}

GncOwner * gncInvoiceGetOwner (GncInvoice *invoice)
{
  if (!invoice) return NULL;
  return &invoice->owner;
}

Timespec gncInvoiceGetDateOpened (GncInvoice *invoice)
{
  Timespec ts; ts.tv_sec = 0; ts.tv_nsec = 0;
  if (!invoice) return ts;
  return invoice->date_opened;
}

Timespec gncInvoiceGetDateDue (GncInvoice *invoice)
{
  Timespec ts; ts.tv_sec = 0; ts.tv_nsec = 0;
  if (!invoice) return ts;
  return invoice->date_due;
}

Timespec gncInvoiceGetDateClosed (GncInvoice *invoice)
{
  Timespec ts; ts.tv_sec = 0; ts.tv_nsec = 0;
  if (!invoice) return ts;
  return invoice->date_closed;
}

gint gncInvoiceGetTerms (GncInvoice *invoice)
{
  if (!invoice) return 0;
  return invoice->terms;
}

const char * gncInvoiceGetNotes (GncInvoice *invoice)
{
  if (!invoice) return NULL;
  return invoice->notes;
}

Transaction * gncInvoiceGetPostedTxn (GncInvoice *invoice)
{
  if (!invoice) return NULL;
  return invoice->posted_txn;
}

Account * gncInvoiceGetPostedAcc (GncInvoice *invoice)
{
  if (!invoice) return NULL;
  return invoice->posted_acc;
}

gboolean gncInvoiceGetActive (GncInvoice *invoice)
{
  if (!invoice) return FALSE;
  return invoice->active;
}

GList * gncInvoiceGetEntries (GncInvoice *invoice)
{
  if (!invoice) return NULL;
  return invoice->entries;
}

gboolean gncInvoiceIsDirty (GncInvoice *invoice)
{
  if (!invoice) return FALSE;
  return invoice->dirty;
}

void gncInvoiceAttachInvoiceToTxn (GncInvoice *invoice, Transaction *txn)
{
  kvp_frame *kvp;
  kvp_value *value;
  
  if (!invoice || !txn)
    return;

  if (invoice->posted_txn) return;	/* Cannot reset invoice's txn */

  xaccTransBeginEdit (txn);
  kvp = xaccTransGetSlots (txn);
  value = kvp_value_new_guid (gncInvoiceGetGUID (invoice));
  kvp_frame_set_slot_path (kvp, value, GNC_INVOICE_ID, GNC_INVOICE_GUID, NULL);
  kvp_value_delete (value);
  xaccTransSetTxnType (txn, TXN_TYPE_INVOICE);
  xaccTransCommitEdit (txn);

  gncInvoiceSetPostedTxn (invoice, txn);
}

#define GET_OR_ADD_ACCVAL(list,t_acc,res) { \
	GList *li; \
	res = NULL; \
    	for (li = list; li; li = li->next) { \
		res = li->data; \
      		if (res->acc == t_acc) \
			break; \
		res = NULL; \
    	} \
	if (!res) { \
		res = g_new0 (struct acct_val, 1); \
		res->acc = t_acc; \
		res->val = gnc_numeric_zero (); \
		list = g_list_append (list, res); \
	} \
}

Transaction * gncInvoicePostToAccount (GncInvoice *invoice, Account *acc,
				       Timespec *date)
{
  Transaction *txn;
  GList *iter;
  GList *splitinfo = NULL;
  gnc_numeric total;
  gnc_commodity *commonCommodity = NULL; /* XXX: FIXME */
  struct acct_val {
    Account *	acc;
    gnc_numeric val;
  } *acc_val;

  if (!invoice || !acc) return NULL;

  txn = xaccMallocTransaction (invoice->book);
  xaccTransBeginEdit (txn);

  /* XXX: Figure out the common currency */

  /* Set Transaction Description (customer), Num (invoice ID), Currency */
  xaccTransSetDescription
    (txn, gncOwnerGetName (gncInvoiceGetOwner (invoice)));
			   
  xaccTransSetNum (txn, gncInvoiceGetID (invoice));
  xaccTransSetCurrency (txn, commonCommodity);

  /* Entered and Posted at date */
  if (date) {
    xaccTransSetDateEnteredTS (txn, date);
    xaccTransSetDatePostedTS (txn, date);
  }

  /* Set the txn due date to be equal to the invoice */
  {
    Timespec ddue = gncInvoiceGetDateDue (invoice);
    xaccTransSetDateDueTS (txn, &ddue);
  }

  /* Iterate through the entries; sum up everything for each account.
   * then create the appropriate splits in this txn.
   */
  total = gnc_numeric_zero();
  for (iter = gncInvoiceGetEntries(invoice); iter; iter = iter->next) {
    gnc_numeric value, tax;
    GncEntry * entry = iter->data;
    Account *this_acc;

    /* Obtain the Entry Value and TaxValue */
    gncEntryGetValue (entry, &value, &tax);

    /* add the value for the account split */
    this_acc = gncEntryGetAccount (entry);
    if (this_acc) {
      /* Find the account value for this_acc.  If we haven't seen this
       * account before, create a new total and add to list
       */
      GET_OR_ADD_ACCVAL (splitinfo, this_acc, acc_val);

      acc_val->val = gnc_numeric_add_fixed (acc_val->val, value);
      total = gnc_numeric_add_fixed (total, value);
    }

    /* Repeat for the TaxValue */
    this_acc = gncEntryGetTaxAccount (entry);
    if (this_acc) {

      GET_OR_ADD_ACCVAL (splitinfo, this_acc, acc_val);
      acc_val->val = gnc_numeric_add_fixed (acc_val->val, tax);
      total = gnc_numeric_add_fixed (total, tax);
    }
  } /* for */

  /* Iterate through the splitinfo list and generate the splits */
  for (iter = splitinfo; iter; iter = iter->next) {
    Split *split;
    acc_val = iter->data;

    split = xaccMallocSplit (invoice->book);
    /* set action and memo? */

    xaccSplitSetBaseValue (split, acc_val->val, commonCommodity);
    xaccAccountBeginEdit (acc_val->acc);
    xaccAccountInsertSplit (acc_val->acc, split);
    xaccAccountCommitEdit (acc_val->acc);
    xaccTransAppendSplit (txn, split);
  }

  /* Now create the Posted split (which is negative -- it's a credit) */
  {
    Split *split = xaccMallocSplit (invoice->book);
    /* Set action/memo */
    xaccSplitSetBaseValue (split, gnc_numeric_neg (total), commonCommodity);
    xaccAccountBeginEdit (acc);
    xaccAccountInsertSplit (acc, split);
    xaccAccountCommitEdit (acc);
    xaccTransAppendSplit (txn, split);
  }

  /* Now attach this invoice to the txn and account */
  gncInvoiceAttachInvoiceToTxn (invoice, txn);
  gncInvoiceSetPostedAcc (invoice, acc);

  xaccTransCommitEdit (txn);

  return txn;
}

GncInvoice * gncInvoiceGetInvoiceFromTxn (Transaction *txn)
{
  kvp_frame *kvp;
  kvp_value *value;
  GUID *guid;
  GNCBook *book;

  if (!txn) return NULL;

  book = xaccTransGetBook (txn);
  kvp = xaccTransGetSlots (txn);
  value = kvp_frame_get_slot_path (kvp, GNC_INVOICE_ID, GNC_INVOICE_GUID, NULL);
  if (!value) return NULL;

  guid = kvp_value_get_guid (value);

  return xaccLookupEntity (gnc_book_get_entity_table (book),
			   guid, _GNC_MOD_NAME);
}

GncInvoice * gncInvoiceLookup (GNCBook *book, const GUID *guid)
{
  if (!book || !guid) return NULL;
  return xaccLookupEntity (gnc_book_get_entity_table (book),
			   guid, _GNC_MOD_NAME);
}

void gncInvoiceBeginEdit (GncInvoice *invoice)
{
  if (!invoice) return;
}
void gncInvoiceCommitEdit (GncInvoice *invoice)
{
  if (!invoice) return;
}

/* Package-Private functions */

static void addObj (GncInvoice *invoice)
{
  GHashTable *ht;

  xaccStoreEntity (gnc_book_get_entity_table (invoice->book),
		   invoice, &invoice->guid, _GNC_MOD_NAME);

  ht = gnc_book_get_data (invoice->book, _GNC_MOD_NAME);
  g_hash_table_insert (ht, &invoice->guid, invoice);
}

static void remObj (GncInvoice *invoice)
{
  GHashTable *ht;

  xaccRemoveEntity (gnc_book_get_entity_table (invoice->book), &invoice->guid);
  ht = gnc_book_get_data (invoice->book, _GNC_MOD_NAME);
  g_hash_table_remove (ht, &invoice->guid);
}

static void _gncInvoiceCreate (GNCBook *book)
{
  GHashTable *ht;

  if (!book) return;

  ht = guid_hash_table_new ();
  gnc_book_set_data (book, _GNC_MOD_NAME, ht);
}

static void _gncInvoiceDestroy (GNCBook *book)
{
  GHashTable *ht;

  if (!book) return;

  ht = gnc_book_get_data (book, _GNC_MOD_NAME);

  /* XXX : Destroy the objects? */
  g_hash_table_destroy (ht);
}

static GncBusinessObject gncInvoiceDesc = {
  GNC_BUSINESS_VERSION,
  _GNC_MOD_NAME,
  "Purchase/Sales Invoice",
  _gncInvoiceCreate,
  _gncInvoiceDestroy,
  NULL,				/* get list */
  NULL				/* printable */
};

gboolean gncInvoiceRegister (void)
{
  return gncBusinessRegister (&gncInvoiceDesc);
}

static gint lastId = 187;	/* XXX */

gint gncInvoiceNextID (GNCBook *book)
{
  return lastId++;
}
