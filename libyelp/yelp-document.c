/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * Copyright (C) 2003-2009 Shaun McCance  <shaunm@gnome.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Shaun McCance  <shaunm@gnome.org>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>
#include <glib/gi18n.h>

#include "yelp-debug.h"
#include "yelp-document.h"
#include "yelp-error.h"
#include "yelp-docbook-document.h"
#include "yelp-mallard-document.h"
#include "yelp-simple-document.h"

typedef struct _Request Request;
struct _Request {
    YelpDocument         *document;
    gchar                *page_id;
    GCancellable         *cancellable;
    YelpDocumentCallback  callback;
    gpointer              user_data;
    GError               *error;

    gint                  idle_funcs;
};

typedef struct _Hash Hash;
struct _Hash {
    gpointer        null;
    GHashTable     *hash;
    GDestroyNotify  destroy;
};

struct _YelpDocumentPriv {
    GMutex *mutex;

    GSList *reqs_all;         /* Holds canonical refs, only free from here */
    Hash   *reqs_by_page_id;  /* Indexed by page ID, contains GSList */
    GSList *reqs_pending;     /* List of requests that need a page */

    /* Real page IDs map to themselves, so this list doubles
     * as a list of all valid page IDs.
     */
    Hash   *page_ids;      /* Mapping of fragment IDs to real page IDs */
    Hash   *titles;        /* Mapping of page IDs to titles */
    Hash   *mime_types;    /* Mapping of page IDs to mime types */
    Hash   *contents;      /* Mapping of page IDs to string content */

    Hash   *root_ids;      /* Mapping of page IDs to "root page" IDs */
    Hash   *prev_ids;      /* Mapping of page IDs to "previous page" IDs */
    Hash   *next_ids;      /* Mapping of page IDs to "next page" IDs */
    Hash   *up_ids;        /* Mapping of page IDs to "up page" IDs */
};

G_DEFINE_TYPE (YelpDocument, yelp_document, G_TYPE_OBJECT);

#define GET_PRIV(object) (G_TYPE_INSTANCE_GET_PRIVATE ((object), YELP_TYPE_DOCUMENT, YelpDocumentPriv))

static void           yelp_document_class_init  (YelpDocumentClass    *klass);
static void           yelp_document_init        (YelpDocument         *document);
static void           yelp_document_dispose     (GObject              *object);
static void           yelp_document_finalize    (GObject              *object);

static gboolean       document_request_page     (YelpDocument         *document,
                                                 const gchar          *page_id,
                                                 GCancellable         *cancellable,
                                                 YelpDocumentCallback  callback,
                                                 gpointer              user_data);
static const gchar *  document_read_contents    (YelpDocument         *document,
                                                 const gchar          *page_id);
static void           document_finish_read      (YelpDocument         *document,
                                                 const gchar          *contents);
static gchar *        document_get_mime_type    (YelpDocument         *document,
                                                 const gchar          *mime_type);

static Hash *         hash_new                  (GDestroyNotify        destroy);
static void           hash_free                 (Hash                 *hash);
static gpointer       hash_lookup               (Hash                 *hash,
                                                 const gchar          *key);
static void           hash_replace              (Hash                 *hash,
                                                 const gchar          *key,
                                                 gpointer              value);
static void           hash_remove               (Hash                 *hash,
                                                 const gchar          *key);
static void           hash_slist_insert         (Hash                 *hash,
                                                 const gchar          *key,
                                                 gpointer              value);
static void           hash_slist_remove         (Hash                 *hash,
                                                 const gchar          *key,
                                                 gpointer              value);

static void           request_cancel            (GCancellable         *cancellable,
                                                 Request              *request);
static gboolean       request_idle_contents     (Request              *request);
static gboolean       request_idle_info         (Request              *request);
static gboolean       request_idle_error        (Request              *request);
static void           request_try_free          (Request              *request);
static void           request_free              (Request              *request);

static const gchar *  str_ref                   (const gchar          *str);
static void           str_unref                 (const gchar          *str);

GStaticMutex str_mutex = G_STATIC_MUTEX_INIT;
GHashTable  *str_refs  = NULL;

/******************************************************************************/

YelpDocument *
yelp_document_get_for_uri (YelpUri *uri)
{
    static GHashTable *documents = NULL;
    gchar *docuri;
    YelpDocument *document = NULL;

    if (documents == NULL)
	documents = g_hash_table_new_full (g_str_hash, g_str_equal,
					   g_free, g_object_unref);

    g_return_val_if_fail (yelp_uri_is_resolved (uri), NULL);

    docuri = yelp_uri_get_document_uri (uri);
    if (docuri == NULL)
	return NULL;

    document = g_hash_table_lookup (documents, docuri);

    if (document != NULL) {
	g_free (docuri);
	return g_object_ref (document);
    }

    switch (yelp_uri_get_document_type (uri)) {
    case YELP_URI_DOCUMENT_TYPE_TEXT:
    case YELP_URI_DOCUMENT_TYPE_HTML:
    case YELP_URI_DOCUMENT_TYPE_XHTML:
	document = yelp_simple_document_new (uri);
	break;
    case YELP_URI_DOCUMENT_TYPE_DOCBOOK:
	document = yelp_docbook_document_new (uri);
	break;
    case YELP_URI_DOCUMENT_TYPE_MALLARD:
	document = yelp_mallard_document_new (uri);
	break;
    case YELP_URI_DOCUMENT_TYPE_MAN:
	/* FIXME */
	break;
    case YELP_URI_DOCUMENT_TYPE_INFO:
	/* FIXME */
	break;
    case YELP_URI_DOCUMENT_TYPE_TOC:
	/* FIXME */
	break;
    case YELP_URI_DOCUMENT_TYPE_SEARCH:
	/* FIXME */
	break;
    case YELP_URI_DOCUMENT_TYPE_NOT_FOUND:
    case YELP_URI_DOCUMENT_TYPE_EXTERNAL:
    case YELP_URI_DOCUMENT_TYPE_ERROR:
	break;
    }

    if (document != NULL) {
	g_hash_table_insert (documents, docuri, document);
	return g_object_ref (document);
    }

    g_free (docuri);
    return NULL;
}

/******************************************************************************/

static void
yelp_document_class_init (YelpDocumentClass *klass)
{
    GObjectClass   *object_class = G_OBJECT_CLASS (klass);

    object_class->dispose  = yelp_document_dispose;
    object_class->finalize = yelp_document_finalize;

    klass->request_page =   document_request_page;
    klass->read_contents =  document_read_contents;
    klass->finish_read =    document_finish_read;
    klass->get_mime_type =  document_get_mime_type;

    g_type_class_add_private (klass, sizeof (YelpDocumentPriv));
}

static void
yelp_document_init (YelpDocument *document)
{
    YelpDocumentPriv *priv;

    document->priv = priv = GET_PRIV (document);

    priv->mutex = g_mutex_new ();

    priv->reqs_by_page_id = hash_new ((GDestroyNotify) g_slist_free);
    priv->reqs_all = NULL;
    priv->reqs_pending = NULL;

    priv->page_ids = hash_new (g_free );
    priv->titles = hash_new (g_free);
    priv->mime_types = hash_new (g_free);
    priv->contents = hash_new ((GDestroyNotify) str_unref);

    priv->root_ids = hash_new (g_free);
    priv->prev_ids = hash_new (g_free);
    priv->next_ids = hash_new (g_free);
    priv->up_ids = hash_new (g_free);
}

static void
yelp_document_dispose (GObject *object)
{
    YelpDocument *document = YELP_DOCUMENT (object);

    if (document->priv->reqs_all) {
	g_slist_foreach (document->priv->reqs_all,
			 (GFunc) request_try_free,
			 NULL);
	g_slist_free (document->priv->reqs_all);
	document->priv->reqs_all = NULL;
    }

    G_OBJECT_CLASS (yelp_document_parent_class)->dispose (object);
}

static void
yelp_document_finalize (GObject *object)
{
    YelpDocument *document = YELP_DOCUMENT (object);

    g_slist_free (document->priv->reqs_pending);
    hash_free (document->priv->reqs_by_page_id);

    hash_free (document->priv->page_ids);
    hash_free (document->priv->titles);
    hash_free (document->priv->mime_types);

    hash_free (document->priv->contents);

    hash_free (document->priv->root_ids);
    hash_free (document->priv->prev_ids);
    hash_free (document->priv->next_ids);
    hash_free (document->priv->up_ids);

    g_mutex_free (document->priv->mutex);

    G_OBJECT_CLASS (yelp_document_parent_class)->finalize (object);
}

/******************************************************************************/

gchar *
yelp_document_get_page_id (YelpDocument *document,
			   const gchar  *id)
{
    gchar *ret = NULL;

    g_assert (document != NULL && YELP_IS_DOCUMENT (document));

    g_mutex_lock (document->priv->mutex);
    ret = hash_lookup (document->priv->page_ids, id);
    if (ret)
	ret = g_strdup (ret);

    g_mutex_unlock (document->priv->mutex);

    return ret;
}

void
yelp_document_set_page_id (YelpDocument *document,
			   const gchar  *id,
			   const gchar  *page_id)
{
    g_assert (document != NULL && YELP_IS_DOCUMENT (document));

    g_mutex_lock (document->priv->mutex);

    hash_replace (document->priv->page_ids, id, g_strdup (page_id));

    if (id != NULL && !g_str_equal (id, page_id)) {
	GSList *reqs, *cur;
	reqs = hash_lookup (document->priv->reqs_by_page_id, id);
	for (cur = reqs; cur != NULL; cur = cur->next) {
	    Request *request = (Request *) cur->data;
            if (request == NULL)
                continue;
	    g_free (request->page_id);
	    request->page_id = g_strdup (page_id);
	    hash_slist_insert (document->priv->reqs_by_page_id, page_id, request);
	}
	if (reqs)
	    hash_remove (document->priv->reqs_by_page_id, id);
    }

    g_mutex_unlock (document->priv->mutex);
}

gchar *
yelp_document_get_root_id (YelpDocument *document,
			   const gchar  *page_id)
{
    gchar *real, *ret = NULL;

    g_assert (document != NULL && YELP_IS_DOCUMENT (document));

    g_mutex_lock (document->priv->mutex);
    real = hash_lookup (document->priv->page_ids, page_id);
    if (real) {
	ret = hash_lookup (document->priv->root_ids, real);
	if (ret)
	    ret = g_strdup (ret);
    }
    g_mutex_unlock (document->priv->mutex);

    return ret;
}

void
yelp_document_set_root_id (YelpDocument *document,
			   const gchar  *page_id,
			   const gchar  *root_id)
{
    g_assert (document != NULL && YELP_IS_DOCUMENT (document));

    g_mutex_lock (document->priv->mutex);
    hash_replace (document->priv->root_ids, page_id, g_strdup (root_id));
    g_mutex_unlock (document->priv->mutex);
}

gchar *
yelp_document_get_prev_id (YelpDocument *document,
			   const gchar  *page_id)
{
    gchar *real, *ret = NULL;

    g_assert (document != NULL && YELP_IS_DOCUMENT (document));

    g_mutex_lock (document->priv->mutex);
    real = hash_lookup (document->priv->page_ids, page_id);
    if (real) {
	ret = hash_lookup (document->priv->prev_ids, real);
	if (ret)
	    ret = g_strdup (ret);
    }
    g_mutex_unlock (document->priv->mutex);

    return ret;
}

void
yelp_document_set_prev_id (YelpDocument *document,
			   const gchar  *page_id,
			   const gchar  *prev_id)
{
    g_assert (document != NULL && YELP_IS_DOCUMENT (document));

    g_mutex_lock (document->priv->mutex);
    hash_replace (document->priv->prev_ids, page_id, g_strdup (prev_id));
    g_mutex_unlock (document->priv->mutex);
}

gchar *
yelp_document_get_next_id (YelpDocument *document,
			   const gchar  *page_id)
{
    gchar *real, *ret = NULL;

    g_assert (document != NULL && YELP_IS_DOCUMENT (document));

    g_mutex_lock (document->priv->mutex);
    real = hash_lookup (document->priv->page_ids, page_id);
    if (real) {
	ret = hash_lookup (document->priv->next_ids, real);
	if (ret)
	    ret = g_strdup (ret);
    }
    g_mutex_unlock (document->priv->mutex);

    return ret;
}

void
yelp_document_set_next_id (YelpDocument *document,
			   const gchar  *page_id,
			   const gchar  *next_id)
{
    g_assert (document != NULL && YELP_IS_DOCUMENT (document));

    g_mutex_lock (document->priv->mutex);
    hash_replace (document->priv->next_ids, page_id, g_strdup (next_id));
    g_mutex_unlock (document->priv->mutex);
}

gchar *
yelp_document_get_up_id (YelpDocument *document,
			 const gchar  *page_id)
{
    gchar *real, *ret = NULL;

    g_assert (document != NULL && YELP_IS_DOCUMENT (document));

    g_mutex_lock (document->priv->mutex);
    real = hash_lookup (document->priv->page_ids, page_id);
    if (real) {
	ret = hash_lookup (document->priv->up_ids, real);
	if (ret)
	    ret = g_strdup (ret);
    }
    g_mutex_unlock (document->priv->mutex);

    return ret;
}

void
yelp_document_set_up_id (YelpDocument *document,
			 const gchar  *page_id,
			 const gchar  *up_id)
{
    g_assert (document != NULL && YELP_IS_DOCUMENT (document));

    g_mutex_lock (document->priv->mutex);
    hash_replace (document->priv->up_ids, page_id, g_strdup (up_id));
    g_mutex_unlock (document->priv->mutex);
}

gchar *
yelp_document_get_title (YelpDocument *document,
			 const gchar  *page_id)
{
    gchar *real, *ret = NULL;

    g_assert (document != NULL && YELP_IS_DOCUMENT (document));

    g_mutex_lock (document->priv->mutex);
    real = hash_lookup (document->priv->page_ids, page_id);
    if (real) {
	ret = hash_lookup (document->priv->titles, real);
	if (ret)
	    ret = g_strdup (ret);
    }
    g_mutex_unlock (document->priv->mutex);

    return ret;
}

void
yelp_document_set_title (YelpDocument *document,
			 const gchar  *page_id,
			 const gchar  *title)
{
    g_assert (document != NULL && YELP_IS_DOCUMENT (document));

    g_mutex_lock (document->priv->mutex);
    hash_replace (document->priv->titles, page_id, g_strdup (title));
    g_mutex_unlock (document->priv->mutex);
}

/******************************************************************************/

gboolean
yelp_document_request_page (YelpDocument         *document,
			    const gchar          *page_id,
			    GCancellable         *cancellable,
			    YelpDocumentCallback  callback,
			    gpointer              user_data)
{
    g_return_if_fail (YELP_IS_DOCUMENT (document));
    g_return_if_fail (YELP_DOCUMENT_GET_CLASS (document)->request_page != NULL);

    debug_print (DB_FUNCTION, "entering\n");

    return YELP_DOCUMENT_GET_CLASS (document)->request_page (document,
							     page_id,
							     cancellable,
							     callback,
							     user_data);
}

static gboolean
document_request_page (YelpDocument         *document,
		       const gchar          *page_id,
		       GCancellable         *cancellable,
		       YelpDocumentCallback  callback,
		       gpointer              user_data)
{
    Request *request;
    gchar *real_id;
    gboolean ret = FALSE;

    request = g_slice_new0 (Request);
    request->document = g_object_ref (document);

    real_id = hash_lookup (document->priv->page_ids, page_id);
    if (real_id)
	request->page_id = g_strdup (real_id);
    else
	request->page_id = g_strdup (page_id);

    request->cancellable = g_object_ref (cancellable);
    g_signal_connect (cancellable, "cancelled",
		      G_CALLBACK (request_cancel), request);

    request->callback = callback;
    request->user_data = user_data;
    request->idle_funcs = 0;

    g_mutex_lock (document->priv->mutex);

    hash_slist_insert (document->priv->reqs_by_page_id,
		       request->page_id,
		       request);

    document->priv->reqs_all = g_slist_prepend (document->priv->reqs_all, request);
    document->priv->reqs_pending = g_slist_prepend (document->priv->reqs_pending, request);

    if (hash_lookup (document->priv->titles, request->page_id)) {
	request->idle_funcs++;
	g_idle_add ((GSourceFunc) request_idle_info, request);
    }

    if (hash_lookup (document->priv->contents, request->page_id)) {
	request->idle_funcs++;
	g_idle_add ((GSourceFunc) request_idle_contents, request);
	ret = TRUE;
    }

    g_mutex_unlock (document->priv->mutex);

    return ret;
}

/******************************************************************************/

const gchar *
yelp_document_read_contents (YelpDocument *document,
			     const gchar  *page_id)
{
    g_return_val_if_fail (YELP_IS_DOCUMENT (document), NULL);
    g_return_val_if_fail (YELP_DOCUMENT_GET_CLASS (document)->read_contents != NULL, NULL);

    return YELP_DOCUMENT_GET_CLASS (document)->read_contents (document, page_id);
}

static const gchar *
document_read_contents (YelpDocument *document,
			const gchar  *page_id)
{
    gchar *real, *str;

    g_mutex_lock (document->priv->mutex);

    real = hash_lookup (document->priv->page_ids, page_id);
    str = hash_lookup (document->priv->contents, real);
    if (str)
	str_ref (str);

    g_mutex_unlock (document->priv->mutex);

    return (const gchar *) str;
}

void
yelp_document_finish_read (YelpDocument *document,
			   const gchar  *contents)
{
    g_return_if_fail (YELP_IS_DOCUMENT (document));
    g_return_if_fail (YELP_DOCUMENT_GET_CLASS (document)->finish_read != NULL);

    YELP_DOCUMENT_GET_CLASS (document)->finish_read (document, contents);
}

static void
document_finish_read (YelpDocument *document,
		      const gchar  *contents)
{
    str_unref (contents);
}

void
yelp_document_give_contents (YelpDocument *document,
			     const gchar  *page_id,
			     gchar        *contents,
			     const gchar  *mime)
{
    g_return_if_fail (YELP_IS_DOCUMENT (document));

    debug_print (DB_FUNCTION, "entering\n");
    debug_print (DB_ARG, "    page_id = \"%s\"\n", page_id);

    g_mutex_lock (document->priv->mutex);

    hash_replace (document->priv->contents,
                  page_id,
                  (gpointer) str_ref (contents));

    hash_replace (document->priv->mime_types,
                  page_id,
                  g_strdup (mime));

    g_mutex_unlock (document->priv->mutex);
}

gchar *
yelp_document_get_mime_type (YelpDocument *document,
			     const gchar  *page_id)
{
    g_return_if_fail (YELP_IS_DOCUMENT (document));
    g_return_if_fail (YELP_DOCUMENT_GET_CLASS (document)->get_mime_type != NULL);

    return YELP_DOCUMENT_GET_CLASS (document)->get_mime_type (document, page_id);
}

static gchar *
document_get_mime_type (YelpDocument *document,
			const gchar  *page_id)
{
    gchar *real, *ret = NULL;

    g_mutex_lock (document->priv->mutex);
    real = hash_lookup (document->priv->page_ids, page_id);
    if (real) {
	ret = hash_lookup (document->priv->mime_types, real);
	if (ret)
	    ret = g_strdup (ret);
    }
    g_mutex_unlock (document->priv->mutex);

    return ret;
}

/******************************************************************************/

void
yelp_document_signal (YelpDocument       *document,
		      const gchar        *page_id,
		      YelpDocumentSignal  signal,
		      const GError       *error)
{
    GSList *reqs, *cur;

    g_return_if_fail (YELP_IS_DOCUMENT (document));

    g_mutex_lock (document->priv->mutex);

    reqs = hash_lookup (document->priv->reqs_by_page_id, page_id);
    for (cur = reqs; cur != NULL; cur = cur->next) {
	Request *request = (Request *) cur->data;
	if (!request)
	    continue;
	switch (signal) {
	case YELP_DOCUMENT_SIGNAL_CONTENTS:
	    request->idle_funcs++;
	    g_idle_add ((GSourceFunc) request_idle_contents, request);
	    break;
	case YELP_DOCUMENT_SIGNAL_INFO:
	    request->idle_funcs++;
	    g_idle_add ((GSourceFunc) request_idle_info, request);
	    break;
	case YELP_DOCUMENT_SIGNAL_ERROR:
	    request->idle_funcs++;
	    request->error = yelp_error_copy (error);
	    g_idle_add ((GSourceFunc) request_idle_error, request);
            break;
	default:
	    break;
	}
    }

    g_mutex_unlock (document->priv->mutex);
}

void
yelp_document_error_pending (YelpDocument *document,
			     const GError *error)
{
    YelpDocumentPriv *priv = GET_PRIV (document);
    GSList *cur;
    Request *request;

    g_assert (document != NULL && YELP_IS_DOCUMENT (document));

    g_mutex_lock (priv->mutex);

    if (priv->reqs_pending) {
	for (cur = priv->reqs_pending; cur; cur = cur->next) {
	    request = cur->data;
	    request->error = yelp_error_copy (error);
	    request->idle_funcs++;
	    g_idle_add ((GSourceFunc) request_idle_error, request);
	}

	g_slist_free (priv->reqs_pending);
	priv->reqs_pending = NULL;
    }

    g_mutex_unlock (priv->mutex);
}

/******************************************************************************/

static Hash *
hash_new (GDestroyNotify destroy)
{
    Hash *hash = g_new0 (Hash, 1);
    hash->destroy = destroy;
    hash->hash = g_hash_table_new_full (g_str_hash, g_str_equal,
					g_free, destroy);
    return hash;
}

static void
hash_free (Hash *hash)
{
    if (hash->null)
	hash->destroy (hash->null);
    g_hash_table_destroy (hash->hash);
    g_free (hash);
}

static gpointer
hash_lookup (Hash *hash, const gchar *key)
{
    if (key == NULL)
	return hash->null;
    else
	return g_hash_table_lookup (hash->hash, key);
}

static void
hash_replace (Hash        *hash,
              const gchar *key,
              gpointer     value)
{
    if (key == NULL) {
        if (hash->null)
            hash->destroy (hash->null);
        hash->null = value;
    }
    else
        g_hash_table_replace (hash->hash, g_strdup (key), value);
}

static void
hash_remove (Hash        *hash,
             const gchar *key)
{
    if (key == NULL) {
        if (hash->null) {
            hash->destroy (hash->null);
            hash->null = NULL;
        }
    }
    else
        g_hash_table_remove (hash->hash, key);
}

static void
hash_slist_insert (Hash        *hash,
                   const gchar *key,
                   gpointer     value)
{
    GSList *list;
    list = hash_lookup (hash, key);
    if (list) {
        list->next = g_slist_prepend (list->next, value);
    } else {
        list = g_slist_prepend (NULL, value);
        list = g_slist_prepend (list, NULL);
        if (key == NULL)
            hash->null = list;
        else
            g_hash_table_insert (hash->hash, g_strdup (key), list);
    }
}

static void
hash_slist_remove (Hash        *hash,
                   const gchar *key,
                   gpointer     value)
{
    GSList *list;
    list = hash_lookup (hash, key);
    if (list) {
        list = g_slist_remove (list, value);
        if (list->next == NULL)
            hash_remove (hash, key);
    }
}

/******************************************************************************/

static void
request_cancel (GCancellable *cancellable, Request *request)
{
    GSList *cur;
    YelpDocument *document = request->document;

    g_assert (document != NULL && YELP_IS_DOCUMENT (document));

    g_mutex_lock (document->priv->mutex);

    document->priv->reqs_pending = g_slist_remove (document->priv->reqs_pending,
						   (gconstpointer) request);
    hash_slist_remove (document->priv->reqs_by_page_id,
		       request->page_id,
		       request);
    for (cur = document->priv->reqs_all; cur != NULL; cur = cur->next) {
	if (cur->data == request) {
	    document->priv->reqs_all = g_slist_delete_link (document->priv->reqs_all, cur);
	    break;
	}
    }
    request_try_free (request);

    g_mutex_unlock (document->priv->mutex);
}

static gboolean
request_idle_contents (Request *request)
{
    YelpDocument *document;
    YelpDocumentPriv *priv;
    YelpDocumentCallback callback = NULL;
    gpointer user_data = user_data;

    g_assert (request != NULL && YELP_IS_DOCUMENT (request->document));

    if (g_cancellable_is_cancelled (request->cancellable)) {
	request->idle_funcs--;
	return FALSE;
    }

    document = g_object_ref (request->document);
    priv = GET_PRIV (document);

    g_mutex_lock (document->priv->mutex);

    priv->reqs_pending = g_slist_remove (priv->reqs_pending, request);

    callback = request->callback;
    user_data = request->user_data;
    request->idle_funcs--;

    g_mutex_unlock (document->priv->mutex);

    if (callback)
	callback (document, YELP_DOCUMENT_SIGNAL_CONTENTS, user_data, NULL);

    g_object_unref (document);
    return FALSE;
}

static gboolean
request_idle_info (Request *request)
{
    YelpDocument *document;
    YelpDocumentCallback callback = NULL;
    gpointer user_data;

    g_assert (request != NULL && YELP_IS_DOCUMENT (request->document));

    if (g_cancellable_is_cancelled (request->cancellable)) {
	request->idle_funcs--;
	return FALSE;
    }

    document = g_object_ref (request->document);

    g_mutex_lock (document->priv->mutex);

    callback = request->callback;
    user_data = request->user_data;
    request->idle_funcs--;

    g_mutex_unlock (document->priv->mutex);

    if (callback)
	callback (document, YELP_DOCUMENT_SIGNAL_INFO, user_data, NULL);

    g_object_unref (document);
    return FALSE;
}

static gboolean
request_idle_error (Request *request)
{
    YelpDocument *document;
    YelpDocumentPriv *priv;
    YelpDocumentCallback callback = NULL;
    GError *error = NULL;
    gpointer user_data = user_data;

    g_assert (request != NULL && YELP_IS_DOCUMENT (request->document));

    if (g_cancellable_is_cancelled (request->cancellable)) {
	request->idle_funcs--;
	return FALSE;
    }

    document = g_object_ref (request->document);
    priv = GET_PRIV (document);

    g_mutex_lock (priv->mutex);

    if (request->error) {
	callback = request->callback;
	user_data = request->user_data;
	error = request->error;
	request->error = NULL;
	priv->reqs_pending = g_slist_remove (priv->reqs_pending, request);
    }

    request->idle_funcs--;
    g_mutex_unlock (priv->mutex);

    if (callback)
	callback (document,
		  YELP_DOCUMENT_SIGNAL_ERROR,
		  user_data,
		  error);

    g_object_unref (document);
    return FALSE;
}

static void
request_try_free (Request *request)
{
    if (!g_cancellable_is_cancelled (request->cancellable))
	g_cancellable_cancel (request->cancellable);

    if (request->idle_funcs == 0)
	request_free (request);
    else
	g_idle_add ((GSourceFunc) request_try_free, request);
}

static void
request_free (Request *request)
{
    g_object_unref (request->document);
    g_free (request->page_id);
    g_object_unref (request->cancellable);

    if (request->error)
	g_error_free (request->error);

    g_slice_free (Request, request);
}

/******************************************************************************/

static const gchar *
str_ref (const gchar *str)
{
    gpointer p;
    guint i;

    g_static_mutex_lock (&str_mutex);
    if (str_refs == NULL)
	str_refs = g_hash_table_new (g_direct_hash, g_direct_equal);
    p = g_hash_table_lookup (str_refs, str);

    i = GPOINTER_TO_UINT (p);
    i++;
    p = GUINT_TO_POINTER (i);

    g_hash_table_insert (str_refs, (gpointer) str, p);
    g_static_mutex_unlock (&str_mutex);

    return str;
}

static void
str_unref (const gchar *str)
{
    gpointer p;
    guint i;

    g_static_mutex_lock (&str_mutex);
    p = g_hash_table_lookup (str_refs, str);

    i = GPOINTER_TO_UINT (p);
    i--;
    p = GUINT_TO_POINTER (i);

    if (i > 0) {
	g_hash_table_insert (str_refs, (gpointer) str, p);
    }
    else {
	g_hash_table_remove (str_refs, (gpointer) str);
	g_free ((gchar *) str);
    }

    g_static_mutex_unlock (&str_mutex);
}