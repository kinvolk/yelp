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

#ifndef __YELP_DOCUMENT_H__
#define __YELP_DOCUMENT_H__

#include <glib-object.h>
#include <gio/gio.h>

#include "yelp-uri.h"

G_BEGIN_DECLS

#define YELP_TYPE_DOCUMENT         (yelp_document_get_type ())
#define YELP_DOCUMENT(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), YELP_TYPE_DOCUMENT, YelpDocument))
#define YELP_DOCUMENT_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST ((k), YELP_TYPE_DOCUMENT, YelpDocumentClass))
#define YELP_IS_DOCUMENT(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), YELP_TYPE_DOCUMENT))
#define YELP_IS_DOCUMENT_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), YELP_TYPE_DOCUMENT))
#define YELP_DOCUMENT_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), YELP_TYPE_DOCUMENT, YelpDocumentClass))

typedef struct _YelpDocument      YelpDocument;
typedef struct _YelpDocumentClass YelpDocumentClass;
typedef struct _YelpDocumentPriv  YelpDocumentPriv;

typedef enum {
    YELP_DOCUMENT_SIGNAL_CONTENTS,
    YELP_DOCUMENT_SIGNAL_INFO,
    YELP_DOCUMENT_SIGNAL_ERROR
} YelpDocumentSignal;

typedef void      (*YelpDocumentCallback)      (YelpDocument         *document,
                                                YelpDocumentSignal    signal,
                                                gpointer              user_data,
                                                GError               *error);

struct _YelpDocument {
    GObject           parent;
    YelpDocumentPriv *priv;
};

struct _YelpDocumentClass {
    GObjectClass      parent_class;

    /* Virtual Functions */
    gboolean      (*request_page)              (YelpDocument         *document,
                                                const gchar          *page_id,
                                                GCancellable         *cancellable,
                                                YelpDocumentCallback  callback,
                                                gpointer              user_data);
    const gchar * (*read_contents)             (YelpDocument         *document,
                                                const gchar          *page_id);
    void          (*finish_read)               (YelpDocument         *document,
                                                const gchar          *contents);
    gchar *       (*get_mime_type)             (YelpDocument         *document,
                                                const gchar          *mime_type);

};

GType             yelp_document_get_type       (void);

YelpDocument *    yelp_document_get_for_uri    (YelpUri              *uri);

gboolean          yelp_document_request_page   (YelpDocument         *document,
                                                const gchar          *page_id,
                                                GCancellable         *cancellable,
                                                YelpDocumentCallback  callback,
                                                gpointer              user_data);

void              yelp_document_give_contents  (YelpDocument         *document,
                                                const gchar          *page_id,
                                                gchar                *contents,
                                                const gchar          *mime);
gchar *           yelp_document_get_mime_type  (YelpDocument         *document,
                                                const gchar          *page_id);
const gchar *     yelp_document_read_contents  (YelpDocument         *document,
                                                const gchar          *page_id);
void              yelp_document_finish_read    (YelpDocument         *document,
                                                const gchar          *contents);

gchar *           yelp_document_get_page_id    (YelpDocument         *document,
                                                const gchar          *id);
void              yelp_document_set_page_id    (YelpDocument         *document,
                                                const gchar          *id,
                                                const gchar          *page_id);

gchar *           yelp_document_get_root_id    (YelpDocument         *document,
                                                const gchar          *page_id);
void              yelp_document_set_root_id    (YelpDocument         *document,
                                                const gchar          *page_id,
                                                const gchar          *root_id);

gchar *           yelp_document_get_prev_id    (YelpDocument         *document,
                                                const gchar          *page_id);
void              yelp_document_set_prev_id    (YelpDocument         *document,
                                                const gchar          *page_id,
                                                const gchar          *prev_id);

char *            yelp_document_get_next_id    (YelpDocument         *document,
                                                const gchar          *page_id);
void              yelp_document_set_next_id    (YelpDocument         *document,
                                                const gchar          *page_id,
                                                const gchar          *next_id);

gchar *           yelp_document_get_up_id      (YelpDocument         *document,
                                                const gchar          *page_id);
void              yelp_document_set_up_id      (YelpDocument         *document,
                                                const gchar          *page_id,
                                                const gchar          *up_id);

gchar *           yelp_document_get_title      (YelpDocument         *document,
                                                const gchar          *page_id);
void              yelp_document_set_title      (YelpDocument         *document,
                                                const gchar          *page_id,
                                                const gchar          *title);

gboolean          yelp_document_has_page       (YelpDocument         *document,
                                                const gchar          *page_id);

void              yelp_document_signal         (YelpDocument         *document,
                                                const gchar          *page_id,
                                                YelpDocumentSignal    signal,
                                                const GError         *error);
void              yelp_document_error_pending  (YelpDocument         *document,
                                                const GError         *error);
/* FIXME */
/*
void              yelp_document_error_request  (YelpDocument        *document,
                                                gint                 req_id,
                                                YelpError           *error);
void              yelp_document_error_page     (YelpDocument        *document,
                                                gchar               *page_id,
                                                YelpError           *error);
void              yelp_document_error_pending  (YelpDocument        *document,
                                                YelpError           *error);
void              yelp_document_final_pending  (YelpDocument        *document,
                                                YelpError           *error);
*/

G_END_DECLS

#endif /* __YELP_DOCUMENT_H__ */