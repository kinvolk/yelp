/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * Copyright (C) 2014 Igalia S.L.
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
 */

#include "yelp-uri-builder.h"

#define BOGUS_PREFIX "bogus-"
#define BOGUS_PREFIX_LEN 6

gchar *
build_network_uri (YelpUri *uri, YelpDocument *document)
{
    SoupURI *soup_uri;
    gchar *bogus_scheme;
    gchar *canonical;
    gchar *path;
    gchar *retval;

    canonical = yelp_uri_get_canonical_uri (uri);
    soup_uri = soup_uri_new (canonical);
    g_free (canonical);

    /* Build the URI that will be passed to WebKit. Relative URIs will be
       automatically reolved by WebKit, so we need to add a leading slash to
       help: and ghelp: URIs to be considered as absolute by WebKit. The last
       component of the URI is considered as a file by WebKit, unless there's
       a trailing slash. For help: URIs this assumption is always correct, because
       the page ID is part of the URI, and when resolved by WebKit, the page ID is
       removed and the relative path is appended to the document URI. For ghelp: URIs
       where the page ID is part of the query, we need to add a slash after the
       document URI so that it's considered a directory and the relative path is appended
       to the document URI.
    */
    if (g_str_equal (soup_uri->scheme, "ghelp") || g_str_equal (soup_uri->scheme, "gnome-help")) {
        /* Pages are part of the query, add leading and trailing slash */
        path = g_strdup_printf ("/%s/", soup_uri->path);
        soup_uri_set_path (soup_uri, path);
        g_free (path);
    }
    else if (g_str_equal (soup_uri->scheme, "help")) {
        /* Page is part of the path, add only leading slash */
        path = g_strdup_printf ("/%s", soup_uri->path);
        soup_uri_set_path (soup_uri, path);
        g_free (path);
    }

    /* We don't have actual page and frag IDs for DocBook. We just map IDs
       of block elements.  The result is that we get xref:someid#someid.
       If someid is really the page ID, we just drop the frag reference.
       Otherwise, normal page views scroll past the link trail.
    */
    if (soup_uri->fragment && YELP_IS_DOCBOOK_DOCUMENT (document)) {
        gchar *page_id = yelp_uri_get_page_id (uri);
        gchar *real_id = yelp_document_get_page_id (document, page_id);

        if (g_str_equal (real_id, soup_uri->fragment))
            soup_uri_set_fragment (soup_uri, NULL);

        g_free (real_id);
        g_free (page_id);
    }

    /* We need to use a different scheme from help or ghelp to be able to deal
       with absolute uris in the HTML. Help uri schemes are help:gnome-help/...
       they dont have a slash after the colon so WebKit resolves them as a relative
       url when they are not. This doesn't happen if the current page URI has a different
       scheme from absolute uri scheme.
    */
    bogus_scheme = build_network_scheme (soup_uri->scheme);
    soup_uri_set_scheme (soup_uri, bogus_scheme);

    retval = soup_uri_to_string (soup_uri, FALSE);
    soup_uri_free (soup_uri);
    g_free (bogus_scheme);

    return retval;
}

gchar *
build_yelp_uri (const gchar *uri_str)
{
  gchar *resource;
  int path_len;
  gchar *uri = g_strdup (uri_str);

  if (!g_str_has_prefix (uri, BOGUS_PREFIX "ghelp:/") &&
      !g_str_has_prefix (uri, BOGUS_PREFIX "gnome-help:/") &&
      !g_str_has_prefix (uri, BOGUS_PREFIX "help:/")) {
    return uri;
  }

  memmove (uri, uri + BOGUS_PREFIX_LEN, strlen (uri) - BOGUS_PREFIX_LEN + 1);

  /* Remove the leading slash */
  resource = strstr (uri, ":");
  resource++;
  memmove (resource, resource + 1, strlen (resource));

  /*Remove the last / if any */
  path_len = strlen (uri);
  if (uri[path_len - 1] == '/')
    uri[path_len - 1] = '\0';

  return uri;
}

gchar *
build_network_scheme (const gchar *scheme)
{
	return g_strdup_printf (BOGUS_PREFIX "%s", scheme);
}