/* frdp-channel-clipboard.c
 *
 * Copyright (C) 2023 Marek Kasik <mkasik@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "frdp-channel-clipboard.h"

#include <freerdp/freerdp.h>
#include <freerdp/client/cliprdr.h>

typedef struct
{
  guchar   *data;
  guint32   length;
  gboolean  handled;
} FrdpClipboardResponseData;

typedef struct
{
  guint                      count;
  guint                     *requested_ids;
  FrdpClipboardResponseData *responses;
} FrdpClipboardRequest;

typedef struct
{
  gchar           *uri;
  FILEDESCRIPTORW *descriptor;
} FrdpLocalFileInfo;

typedef struct
{
  gchar    *uri;
  guint     stream_id;
  gchar    *filename;
  gboolean  created;
  gboolean  is_directory;
} FrdpRemoteFileInfo;

typedef struct
{
  CliprdrClientContext        *cliprdr_client_context;

  gboolean                     file_streams_supported;

  gboolean                     remote_data_in_clipboard;

  GtkClipboard                *gtk_clipboard;
  guint                        clipboard_owner_changed_id;

  GList                       *requests;

  gsize                        remote_files_count;
  FrdpRemoteFileInfo          *remote_files_infos;
  gchar                       *tmp_directory;

  gsize                        local_files_count;
  FrdpLocalFileInfo           *local_files_infos;

  guint                        next_stream_id;
  guint                        fgdw_id;
} FrdpChannelClipboardPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (FrdpChannelClipboard, frdp_channel_clipboard, FRDP_TYPE_CHANNEL)

enum
{
  PROP_0 = 0,
  PROP_CLIPRDR_CLIENT_CONTEXT,
  LAST_PROP
};

static void  frdp_channel_clipboard_set_client_context (FrdpChannelClipboard *self,
                                                        CliprdrClientContext *context);
static guint send_client_format_list                   (FrdpChannelClipboard *self);

static void  _gtk_clipboard_clear_func                 (GtkClipboard         *clipboard,
                                                        gpointer              user_data);
static void  clipboard_owner_change_cb                 (GtkClipboard         *clipboard,
                                                        GdkEventOwnerChange  *event,
                                                        gpointer              user_data);

static void
frdp_channel_clipboard_get_property (GObject    *object,
                                     guint       property_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  FrdpChannelClipboard        *self = FRDP_CHANNEL_CLIPBOARD (object);
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);

  switch (property_id)
    {
      case PROP_CLIPRDR_CLIENT_CONTEXT:
        g_value_set_pointer (value, priv->cliprdr_client_context);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
frdp_channel_clipboard_set_property (GObject      *object,
                                     guint         property_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
  FrdpChannelClipboard *self = FRDP_CHANNEL_CLIPBOARD (object);

  switch (property_id)
    {
      case PROP_CLIPRDR_CLIENT_CONTEXT:
        frdp_channel_clipboard_set_client_context (self, g_value_get_pointer (value));
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
frdp_channel_clipboard_finalize (GObject *object)
{
  FrdpChannelClipboard        *self = (FrdpChannelClipboard *) object;
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);

  _gtk_clipboard_clear_func (priv->gtk_clipboard, self);

  G_OBJECT_CLASS (frdp_channel_clipboard_parent_class)->finalize (object);
}

static void
frdp_channel_clipboard_class_init (FrdpChannelClipboardClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->get_property = frdp_channel_clipboard_get_property;
  gobject_class->set_property = frdp_channel_clipboard_set_property;
  gobject_class->finalize = frdp_channel_clipboard_finalize;

  g_object_class_install_property (gobject_class,
                                   PROP_CLIPRDR_CLIENT_CONTEXT,
                                   g_param_spec_pointer ("cliprdr-client-context",
                                                         "cliprdr-client-context",
                                                         "Context for clipboard client",
                                                         G_PARAM_READWRITE));
}

static void
frdp_channel_clipboard_init (FrdpChannelClipboard *self)
{
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);

  priv->gtk_clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
  priv->clipboard_owner_changed_id = g_signal_connect (priv->gtk_clipboard, "owner-change", G_CALLBACK (clipboard_owner_change_cb), self);
  priv->fgdw_id = CB_FORMAT_TEXTURILIST;
}

static void
clipboard_owner_change_cb (GtkClipboard        *clipboard,
                           GdkEventOwnerChange *event,
                           gpointer             user_data)
{
  FrdpChannelClipboard        *self = (FrdpChannelClipboard *) user_data;
  FrdpChannelClipboardPrivate *priv;

  if (self != NULL) {
    priv = frdp_channel_clipboard_get_instance_private (self);

    if ((gtk_clipboard_wait_is_text_available (clipboard) ||
         gtk_clipboard_wait_is_image_available (clipboard) ||
         gtk_clipboard_wait_is_uris_available (clipboard)) &&
        !priv->remote_data_in_clipboard) {
      send_client_format_list (self);
    }
  }
}

static guint
send_client_capabilities (FrdpChannelClipboard *self)
{
  FrdpChannelClipboardPrivate    *priv = frdp_channel_clipboard_get_instance_private (self);
  CLIPRDR_GENERAL_CAPABILITY_SET  general_capability_set = { 0 };
  CLIPRDR_CAPABILITIES            capabilities = { 0 };

  capabilities.cCapabilitiesSets = 1;
  capabilities.capabilitySets = (CLIPRDR_CAPABILITY_SET *) &(general_capability_set);

  general_capability_set.capabilitySetType = CB_CAPSTYPE_GENERAL;
  general_capability_set.capabilitySetLength = 12;
  general_capability_set.version = CB_CAPS_VERSION_2;
  general_capability_set.generalFlags = CB_USE_LONG_FORMAT_NAMES |
                                        CB_STREAM_FILECLIP_ENABLED |
                                        CB_FILECLIP_NO_FILE_PATHS |
                                        CB_HUGE_FILE_SUPPORT_ENABLED;

  return priv->cliprdr_client_context->ClientCapabilities (priv->cliprdr_client_context, &capabilities);
}

static guint
send_client_format_list (FrdpChannelClipboard *self)
{
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  CLIPRDR_FORMAT_LIST          format_list = { 0 };
  CLIPRDR_FORMAT              *formats = NULL;
  guint32                      formats_count = 0;
  GdkAtom                     *targets = NULL;
  gchar                       *atom_name;
  guint                        ret = CHANNEL_RC_NOT_INITIALIZED, k;
  gint                         targets_count = 0;
  gint                         i, j = 0;

  /* TODO - change to gtk_clipboard_request_targets() */
  if (gtk_clipboard_wait_for_targets (priv->gtk_clipboard,
                                      &targets,
                                      &targets_count)) {
    formats_count = targets_count;
    formats = g_new0 (CLIPRDR_FORMAT, formats_count);

    for (i = 0; i < targets_count; i++) {
      atom_name = gdk_atom_name (targets[i]);

      if (g_strcmp0 (atom_name, "UTF8_STRING") == 0) {
        formats[j].formatId = CF_UNICODETEXT;
        formats[j++].formatName = NULL;
      } else if (g_strcmp0 (atom_name, "TEXT") == 0) {
        formats[j].formatId = CF_TEXT;
        formats[j++].formatName = NULL;
      } else if (g_strcmp0 (atom_name, "image/png") == 0) {
        formats[j].formatId = CB_FORMAT_PNG;
        formats[j++].formatName = NULL;
      } else if (g_strcmp0 (atom_name, "image/jpeg") == 0) {
        formats[j].formatId = CB_FORMAT_JPEG;
        formats[j++].formatName = NULL;
      } else if (g_strcmp0 (atom_name, "image/bmp") == 0) {
        formats[j].formatId = CF_DIB;
        formats[j++].formatName = NULL;
      } else if (g_strcmp0 (atom_name, "text/uri-list") == 0) {
        formats[j].formatId = priv->fgdw_id;
        formats[j++].formatName = g_strdup ("FileGroupDescriptorW");
      }

      g_free (atom_name);
    }
  }

  format_list.msgType = CB_FORMAT_LIST;
  format_list.msgFlags = CB_RESPONSE_OK;
  format_list.numFormats = j;
  format_list.formats = formats;

  ret = priv->cliprdr_client_context->ClientFormatList (priv->cliprdr_client_context, &format_list);

  if (formats != NULL) {
    for (k = 0; k < formats_count; k++) {
      g_free (formats[k].formatName);
    }
    g_free (formats);
  }

  return ret;
}

static guint
server_capabilities (CliprdrClientContext       *context,
                     const CLIPRDR_CAPABILITIES *capabilities)
{
  FrdpChannelClipboard        *self;
  FrdpChannelClipboardPrivate *priv;
  CLIPRDR_CAPABILITY_SET      *capability;
  guint                        i;

  if (context != NULL) {
    self = (FrdpChannelClipboard *) context->custom;
    priv = frdp_channel_clipboard_get_instance_private (self);

    for (i = 0; i < capabilities->cCapabilitiesSets; i++) {
      capability = &(capabilities->capabilitySets[i]);

      if (capability->capabilitySetType == CB_CAPSTYPE_GENERAL) {
        CLIPRDR_GENERAL_CAPABILITY_SET *general_capability = (CLIPRDR_GENERAL_CAPABILITY_SET *) capability;

        /* Windows 7 does not send files if these flags are disabled. */
        if (general_capability->generalFlags & CB_USE_LONG_FORMAT_NAMES &&
            general_capability->generalFlags & CB_STREAM_FILECLIP_ENABLED &&
            general_capability->generalFlags & CB_FILECLIP_NO_FILE_PATHS) {
          priv->file_streams_supported = TRUE;
        }
      }
    }
  }

  return CHANNEL_RC_OK;
}

static guint
send_data_request (FrdpChannelClipboard *self,
                   guint32               format_id)
{
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  CLIPRDR_FORMAT_DATA_REQUEST *new_request;

  new_request = g_new0 (CLIPRDR_FORMAT_DATA_REQUEST, 1);
  new_request->requestedFormatId = format_id;

  return priv->cliprdr_client_context->ClientFormatDataRequest (priv->cliprdr_client_context, new_request);
}

static FrdpClipboardRequest *
frdp_clipboard_request_new (guint count)
{
  FrdpClipboardRequest *request;

  request = g_new0 (FrdpClipboardRequest, 1);
  request->count = count;
  request->requested_ids = g_new0 (guint, count);
  request->responses = g_new0 (FrdpClipboardResponseData, count);

  return request;
}

static FrdpClipboardRequest *
frdp_clipboard_request_send (FrdpChannelClipboard *self,
                             guint                 format_id)
{
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  FrdpClipboardRequest        *request = NULL;
  guint                        i;

  if (format_id == priv->fgdw_id) {
    request = frdp_clipboard_request_new (1);
    request->requested_ids[0] = priv->fgdw_id;
  } else if (format_id == CF_UNICODETEXT) {
    request = frdp_clipboard_request_new (1);
    request->requested_ids[0] = CF_UNICODETEXT;
  } else if (format_id == CF_DIB) {
    request = frdp_clipboard_request_new (1);
    request->requested_ids[0] = CF_DIB;
  }

  if (request != NULL) {
    priv->requests = g_list_append (priv->requests, request);
    for (i = 0; i < request->count; i++) {
      send_data_request (self, request->requested_ids[i]);
    }
  }

  return request;
}

static gboolean
frdp_clipboard_request_done (FrdpClipboardRequest *request)
{
  guint i;

  for (i = 0; i < request->count; i++) {
    if (!request->responses[i].handled) {
      return FALSE;
    }
  }

  return TRUE;
}

static void
frdp_clipboard_request_free (FrdpClipboardRequest *request)
{
  guint i;

  g_free (request->requested_ids);
  for (i = 0; i < request->count; i++)
    g_free (request->responses[i].data);
  g_free (request->responses);
  g_free (request);
}

static guint
send_client_format_list_response (FrdpChannelClipboard *self,
                                  gboolean              status)
{
  CLIPRDR_FORMAT_LIST_RESPONSE  response = { 0 };
  FrdpChannelClipboardPrivate  *priv = frdp_channel_clipboard_get_instance_private (self);

  response.msgType = CB_FORMAT_LIST_RESPONSE;
  response.msgFlags = status ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
  response.dataLen = 0;

  return priv->cliprdr_client_context->ClientFormatListResponse (priv->cliprdr_client_context, &response);
}

static gboolean
files_created (FrdpChannelClipboard *self)
{
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  guint                        i;

  for (i = 0; i < priv->remote_files_count; i++) {
    if (!priv->remote_files_infos[i].created) {
      return FALSE;
    }
  }

  return TRUE;
}

static void
replace_ascii_character (gchar *text,
                         gchar  old_character,
                         gchar  new_character)
{
  guint i;

  for (i = 0; text[i] != '\0'; i++) {
    if (text[i] == old_character)
      text[i] = new_character;
  }
}

/* TODO: Rewrite this using async methods of GtkCLipboard once we move to Gtk4 */
static void
_gtk_clipboard_get_func (GtkClipboard     *clipboard,
                         GtkSelectionData *selection_data,
                         guint             info,
                         gpointer          user_data)
{
  FrdpChannelClipboard        *self = (FrdpChannelClipboard *) user_data;
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  FrdpClipboardRequest        *current_request;
  gchar                       *data = NULL;
  gint                         length;

  current_request = frdp_clipboard_request_send (self, info);
  if (current_request != NULL) {

    while (!frdp_clipboard_request_done (current_request))
      gtk_main_iteration ();

    if (info == CF_UNICODETEXT) {
      /* TODO - convert CR LF to CR */
      length = ConvertFromUnicode (CP_UTF8, 0, (WCHAR *) current_request->responses[0].data, (int) (current_request->responses[0].length / sizeof (WCHAR)), &data, 0, NULL, NULL);

      gtk_selection_data_set (selection_data,
                              gdk_atom_intern ("UTF8_STRING", FALSE),
                              8,
                              (guchar *) data,
                              length);
    } else if (info == CF_DIB) {
      /* This has been inspired by function transmute_cf_dib_to_image_bmp() from gtk */
      BITMAPINFOHEADER *bi = (BITMAPINFOHEADER *) current_request->responses[0].data;
      BITMAPFILEHEADER *bf;

      length = current_request->responses[0].length + sizeof (BITMAPFILEHEADER);
      data = g_malloc (length);

      bf = (BITMAPFILEHEADER *) data;
      bf->bfType = 0x4d42; /* "BM" */
      bf->bfSize = length;
      bf->bfReserved1 = 0;
      bf->bfReserved2 = 0;
      bf->bfOffBits = (sizeof (BITMAPFILEHEADER) + bi->biSize/* + bi->biClrUsed * sizeof (RGBQUAD)*/);

      memcpy (data + sizeof (BITMAPFILEHEADER),
              current_request->responses[0].data,
              current_request->responses[0].length);

      gtk_selection_data_set (selection_data,
                              gdk_atom_intern ("image/bmp", FALSE),
                              8,
                              (guchar *) data,
                              length);
    } else if (info == priv->fgdw_id) {
      g_free (priv->tmp_directory);
      priv->tmp_directory = g_dir_make_tmp ("clipboard-XXXXXX", NULL);
      for (guint j = 0; j < current_request->count; j++) {
        if (current_request->requested_ids[j] == priv->fgdw_id) {
          FILEDESCRIPTORW  *files = (FILEDESCRIPTORW *) (current_request->responses[j].data + 4);
          GFile            *file, *parent;
          GList            *iter, *uri_list = NULL;
          gchar            *filename, *path, **uri_array;
          guint             i, count = current_request->responses[j].length / sizeof (FILEDESCRIPTORW);

          priv->remote_files_count = count;
          priv->remote_files_infos = g_new0 (FrdpRemoteFileInfo, priv->remote_files_count);

          for (i = 0; i < count; i++) {
            length = ConvertFromUnicode (CP_UTF8, 0, (WCHAR *) files[i].cFileName, (int) (260 / sizeof (WCHAR)), &filename, 0, NULL, NULL);

            replace_ascii_character (filename, '\\', '/');

            CLIPRDR_FILE_CONTENTS_REQUEST file_contents_request = { 0 };
            file_contents_request.streamId = priv->next_stream_id++;
            file_contents_request.listIndex = i;
            file_contents_request.dwFlags = FILECONTENTS_RANGE;

            priv->remote_files_infos[i].stream_id = file_contents_request.streamId;
            priv->remote_files_infos[i].filename = g_strdup (filename);
            priv->remote_files_infos[i].is_directory = (files[i].dwFlags & FD_ATTRIBUTES) && (files[i].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);

            if (priv->remote_files_infos[i].is_directory) {
              path = g_strdup_printf ("%s/%s", priv->tmp_directory, filename);
              file = g_file_new_for_path (path);
              priv->remote_files_infos[i].created = g_file_make_directory_with_parents (file, NULL, NULL);
              priv->remote_files_infos[i].uri = g_file_get_uri (file);
              g_object_unref (file);
              g_free (path);
            } else {
              file_contents_request.cbRequested = files[i].nFileSizeLow;
              file_contents_request.nPositionHigh = 0;
              file_contents_request.nPositionLow = 0;
              file_contents_request.haveClipDataId = FALSE;

              priv->cliprdr_client_context->ClientFileContentsRequest (priv->cliprdr_client_context, &file_contents_request);
            }

            g_free (filename);
          }

          while (!files_created (self))
            gtk_main_iteration ();

          /* Set URIs for topmost items only, the rest will be pasted as part of those. */
          parent = g_file_new_for_path (priv->tmp_directory);
          for (i = 0; i < priv->remote_files_count; i++) {
            file = g_file_new_for_uri (priv->remote_files_infos[i].uri);
            if (g_file_has_parent (file, parent))
              uri_list = g_list_prepend (uri_list, priv->remote_files_infos[i].uri);
            g_object_unref (file);
          }
          g_object_unref (parent);

          uri_array = g_new0 (gchar *, g_list_length (uri_list) + 1);
          for (iter = uri_list, i = 0; iter != NULL; iter = iter->next, i++)
            uri_array[i] = iter->data;

          gtk_selection_data_set_uris (selection_data, uri_array);

          g_free (uri_array);
          g_list_free (uri_list);
        }
      }
    }

    priv->requests = g_list_remove (priv->requests, current_request);
    frdp_clipboard_request_free (current_request);
  }
}

static gint
sort_file_uris (gconstpointer a,
                gconstpointer b)
{
  const gchar *uri_a = a, *uri_b = b;
  guint        count_a = 0, count_b = 0;
  gint         i;

  for (i = 0; uri_a[i] != '\0'; i++)
    if (uri_a[i] == '/') count_a++;

  for (i = 0; uri_b[i] != '\0'; i++)
    if (uri_b[i] == '/') count_b++;

  if (count_a < count_b)
    return -1;
  else if (count_a > count_b)
    return 1;
  else
    return 0;
}

static void
clear_local_files_infos (FrdpChannelClipboard *self)
{
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  guint                        i;

  if (priv->local_files_infos != NULL) {
    for (i = 0; i < priv->local_files_count; i++) {
      g_free (priv->local_files_infos[i].descriptor);
      g_free (priv->local_files_infos[i].uri);
    }
    g_clear_pointer (&priv->local_files_infos, g_free);
  }

  priv->local_files_count = 0;
}

static void
_gtk_clipboard_clear_func (GtkClipboard *clipboard,
                           gpointer      user_data)
{
  FrdpChannelClipboard        *self = (FrdpChannelClipboard *) user_data;
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  GError                      *error = NULL;
  GFile                       *file, *directory;
  GList                       *sorted_uris = NULL, *iter;
  guint                        i;
  gchar                       *uri;

  for (i = 0; i < priv->remote_files_count; i++) {
    if (priv->remote_files_infos != NULL && priv->remote_files_infos[i].created)
      sorted_uris = g_list_prepend (sorted_uris, priv->remote_files_infos[i].uri);
  }

  sorted_uris = g_list_sort (sorted_uris, sort_file_uris);
  sorted_uris = g_list_reverse (sorted_uris);

  for (iter = sorted_uris; iter != NULL; iter = iter->next) {
    uri = iter->data;
    file = g_file_new_for_uri (uri);

    if (!g_file_delete (file, NULL, &error))
      g_warning ("Temporary file \"%s\" could not be deleted: %s", uri, error->message);

    g_object_unref (file);
  }

  if (priv->tmp_directory != NULL) {
    directory = g_file_new_for_path (priv->tmp_directory);
    if (!g_file_delete (directory, NULL, &error))
      g_warning ("Temporary directory \"%s\" could not be deleted: %s", priv->tmp_directory, error->message);
  }

  if (priv->remote_files_infos != NULL) {
    for (i = 0; i < priv->remote_files_count; i++) {
      g_free (priv->remote_files_infos[i].filename);
      g_free (priv->remote_files_infos[i].uri);
    }
    g_clear_pointer (&priv->remote_files_infos, g_free);
  }

  clear_local_files_infos (self);

  priv->remote_data_in_clipboard = FALSE;
}

static guint
server_format_list (CliprdrClientContext      *context,
                    const CLIPRDR_FORMAT_LIST *format_list)
{
  FrdpChannelClipboard        *self;
  FrdpChannelClipboardPrivate *priv;
  GtkTargetEntry              *entries;
  GtkTargetList               *list;
  gboolean                     contains_file_group_descriptor_w = FALSE;
  GdkAtom                      atom;
  guint                        i;
  gint                         count = 0;

  if (context != NULL) {
    self = (FrdpChannelClipboard *) context->custom;
    priv = frdp_channel_clipboard_get_instance_private (self);

    list = gtk_target_list_new (NULL, 0);

    for (i = 0; i < format_list->numFormats; i++) {
      if (g_strcmp0 (format_list->formats[i].formatName, "FileGroupDescriptorW") == 0) {
        contains_file_group_descriptor_w = TRUE;
        priv->fgdw_id = format_list->formats[i].formatId;
      }
    }

    if (contains_file_group_descriptor_w) {
      atom = gdk_atom_intern ("text/uri-list", FALSE);
      gtk_target_list_add (list, atom, 0, priv->fgdw_id);
    } else {
      atom = GDK_NONE;
      for (i = 0; i < format_list->numFormats; i++) {
        if (format_list->formats[i].formatId == CF_TEXT) {
          atom = gdk_atom_intern ("TEXT", FALSE);
        } else if (format_list->formats[i].formatId == CF_UNICODETEXT) {
          atom = gdk_atom_intern ("UTF8_STRING", FALSE);
        } else if (format_list->formats[i].formatId == CF_DIB) {
          atom = gdk_atom_intern ("image/bmp", FALSE);
        } else if (format_list->formats[i].formatId == CB_FORMAT_PNG) {
          atom = gdk_atom_intern ("image/png", FALSE);
        }

        if (atom != GDK_NONE)
          gtk_target_list_add (list, atom, 0, format_list->formats[i].formatId);
      }
    }

    entries = gtk_target_table_new_from_list (list, &count);
    if (!gtk_clipboard_set_with_data (priv->gtk_clipboard,
                                      entries,
                                      count,
                                      _gtk_clipboard_get_func,
                                      _gtk_clipboard_clear_func,
                                      self)) {
      g_warning ("Setting of clipboard entries failed");
    } else {
      priv->remote_data_in_clipboard = TRUE;
    }

    send_client_format_list_response (self, TRUE);
  }

  return CHANNEL_RC_OK;
}

static guint
monitor_ready (CliprdrClientContext        *context,
               const CLIPRDR_MONITOR_READY *monitor_ready)
{
  FrdpChannelClipboard *clipboard;
  guint                 return_value = CHANNEL_RC_OK;

  if (context != NULL) {
    clipboard = (FrdpChannelClipboard *) context->custom;

    if ((return_value = send_client_capabilities (clipboard)) != CHANNEL_RC_OK)
      return return_value;

    if ((return_value = send_client_format_list (clipboard)) != CHANNEL_RC_OK)
      return return_value;
  }

  return return_value;
}

static guint
server_format_list_response (CliprdrClientContext               *context,
                             const CLIPRDR_FORMAT_LIST_RESPONSE *response)
{
  return CHANNEL_RC_OK;
}

static FrdpLocalFileInfo *
frdp_local_file_info_new (GFile     *file,
                          GFileInfo *file_info,
                          GFile     *root)
{
  FrdpLocalFileInfo *frdp_file_info = NULL;
  GFileType          file_type;
  goffset            file_size;
  WCHAR             *file_name = NULL;
  gchar             *relative_path;

  if (file_info != NULL && file != NULL) {
    frdp_file_info = g_new (FrdpLocalFileInfo, 1);
    frdp_file_info->uri = g_file_get_uri (file);
    frdp_file_info->descriptor = g_new0 (FILEDESCRIPTORW, 1);

    relative_path = g_file_get_relative_path (root, file);
    replace_ascii_character (relative_path, '/', '\\');

    ConvertToUnicode (CP_UTF8, 0, (LPCSTR) relative_path, -1, &file_name, 0);
    memcpy (frdp_file_info->descriptor->cFileName, file_name, strlen (relative_path) * 2);
    g_free (file_name);
    g_free (relative_path);

    file_size = g_file_info_get_size (file_info);
    file_type = g_file_info_get_file_type (file_info);

    frdp_file_info->descriptor->dwFlags = FD_ATTRIBUTES | FD_FILESIZE;
    if (file_type == G_FILE_TYPE_DIRECTORY) {
      frdp_file_info->descriptor->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
      frdp_file_info->descriptor->nFileSizeLow = 0;
      frdp_file_info->descriptor->nFileSizeHigh = 0;
    } else {
      frdp_file_info->descriptor->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
      frdp_file_info->descriptor->nFileSizeLow = file_size & 0xffffffff;
      frdp_file_info->descriptor->nFileSizeHigh = file_size >> 32 & 0xffffffff;
    }
  }

  return frdp_file_info;
}

static void
enumerate_directory (GFile  *directory,
                     GList **infos,
                     GFile  *root)
{
  FrdpLocalFileInfo *frdp_file_info;
  GFileEnumerator   *enumerator;
  GFileInfo         *file_info;
  GError            *error = NULL;
  GFile             *file;
  GList             *list = NULL;

  enumerator = g_file_enumerate_children (directory,
                                          G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                          G_FILE_QUERY_INFO_NONE,
                                          NULL,
                                          &error);

  while (TRUE) {
    if (!g_file_enumerator_iterate (enumerator, &file_info, &file, NULL, &error)) {
      g_warning ("Enumeration of files failed: %s", error->message);
      g_error_free (error);
      break;
    }

    if (file_info == NULL || file == NULL)
      break;

    frdp_file_info = frdp_local_file_info_new (file, file_info, root);
    list = g_list_append (list, frdp_file_info);

    if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
      enumerate_directory (file, &list, root);
  }

  g_object_unref (enumerator);

  *infos = g_list_concat (*infos, list);
}

static guint
send_data_response (FrdpChannelClipboard *self,
                    const BYTE           *data,
                    size_t                size)
{
  CLIPRDR_FORMAT_DATA_RESPONSE response = { 0 };
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);

  if (size > UINT32_MAX)
    return ERROR_INVALID_PARAMETER;

  response.msgFlags = (data) ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
  response.dataLen = (guint32) size;
  response.requestedFormatData = data;

  return priv->cliprdr_client_context->ClientFormatDataResponse (priv->cliprdr_client_context, &response);
}

static void
clipboard_content_received (GtkClipboard     *clipboard,
                            GtkSelectionData *selection_data,
                            gpointer          user_data)
{
  FrdpChannelClipboard        *self = (FrdpChannelClipboard *) user_data;
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  GdkPixbuf                   *pixbuf;
  GdkAtom                      data_type;
  guchar                      *data, *text;
  GError                      *error = NULL;
  gsize                        text_length, buffer_size = 0;
  guint                        i;
  gint                         length;

  length = gtk_selection_data_get_length (selection_data);
  data_type = gtk_selection_data_get_data_type (selection_data);

  if (length >= 0) {
    if (data_type == gdk_atom_intern ("UTF8_STRING", FALSE)) {
      text = gtk_selection_data_get_text (selection_data);
      text_length = strlen ((gchar *) text);
      if (ConvertToUnicode (CP_UTF8, 0, (LPCSTR) text, text_length, (WCHAR **) &data, 0) > 0) {
        send_data_response (self, data, (text_length + 1) * sizeof (WCHAR));
        g_free (data);
      }
      g_free (text);
    } else if (data_type == gdk_atom_intern ("image/png", FALSE)) {
      pixbuf = gtk_selection_data_get_pixbuf (selection_data);
      if (gdk_pixbuf_save_to_buffer (pixbuf, (gchar **) &data, &buffer_size, "png", NULL, NULL))
        send_data_response (self, data, buffer_size);
      g_object_unref (pixbuf);
    } else if (data_type == gdk_atom_intern ("image/jpeg", FALSE)) {
      pixbuf = gtk_selection_data_get_pixbuf (selection_data);
      if (gdk_pixbuf_save_to_buffer (pixbuf, (gchar **) &data, &buffer_size, "jpeg", NULL, NULL))
        send_data_response (self, data, buffer_size);
      g_object_unref (pixbuf);
    } else if (data_type == gdk_atom_intern ("image/bmp", FALSE)) {
      pixbuf = gtk_selection_data_get_pixbuf (selection_data);
      if (gdk_pixbuf_save_to_buffer (pixbuf, (gchar **) &data, &buffer_size, "bmp", NULL, NULL)) {
        send_data_response (self, data + sizeof (BITMAPFILEHEADER), buffer_size - sizeof (BITMAPFILEHEADER));
      }
      g_object_unref (pixbuf);
    } else if (data_type == gdk_atom_intern ("text/uri-list", FALSE)) {
      FrdpLocalFileInfo *frdp_file_info;
      FILEDESCRIPTORW   *descriptors;
      GFileInfo         *file_info;
      guint32           *size;
      GFile             *file, *root = NULL;
      GList             *list = NULL, *iter;
      gchar            **uris;

      uris = gtk_selection_data_get_uris (selection_data);

      if (uris != NULL && uris[0] != NULL) {
        file = g_file_new_for_uri (uris[0]);
        root = g_file_get_parent (file);
        g_object_unref (file);
      }

      for (i = 0; uris[i] != NULL; i++) {
        file = g_file_new_for_uri (uris[i]);
        file_info = g_file_query_info (file,
                                       G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                       G_FILE_QUERY_INFO_NONE,
                                       NULL,
                                       &error);

        if (file_info != NULL) {
          frdp_file_info = frdp_local_file_info_new (file, file_info, root);
          list = g_list_append (list, frdp_file_info);

          if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
            enumerate_directory (file, &list, root);

          g_object_unref (file_info);
        } else {
          g_warning ("Error getting file info: %s", error->message);
        }

        g_object_unref (file);
      }

      if (root != NULL)
        g_object_unref (root);

      length = g_list_length (list);
      data = g_malloc (length * sizeof (FILEDESCRIPTORW) + 4);

      size = (guint32 *) data;
      size[0] = length;

      /*g_strfreev (priv->uris);*/

      priv->local_files_count = length;
      priv->local_files_infos = g_new0 (FrdpLocalFileInfo, priv->local_files_count);

      descriptors = (FILEDESCRIPTORW *) (data + 4);
      for (iter = list, i = 0; iter != NULL; iter = iter->next, i++) {
        frdp_file_info = iter->data;
        memcpy (&(descriptors[i]), frdp_file_info->descriptor, sizeof (FILEDESCRIPTORW));
        priv->local_files_infos[i].descriptor = frdp_file_info->descriptor;
        priv->local_files_infos[i].uri = frdp_file_info->uri;
      }
      g_list_free_full (list, g_free);

      send_data_response (self, data, priv->local_files_count * sizeof (FILEDESCRIPTORW) + 4);
    }
  } else {
    g_warning ("No data received from local clipboard for sending to remote side!");
  }
}

static guint
server_format_data_request (CliprdrClientContext              *context,
                            const CLIPRDR_FORMAT_DATA_REQUEST *format_data_request)
{
  FrdpChannelClipboard        *self = (FrdpChannelClipboard *) context->custom;
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  guint                        format;

  format = format_data_request->requestedFormatId;

  /* TODO: Add more formats (e.g. CF_DIBV5, CF_TEXT, CF_OEMTEXT) */
  switch (format) {
    case CF_UNICODETEXT:
      gtk_clipboard_request_contents (priv->gtk_clipboard,
                                      gdk_atom_intern ("UTF8_STRING", FALSE),
                                      clipboard_content_received,
                                      self);
      break;
    case CB_FORMAT_PNG:
      gtk_clipboard_request_contents (priv->gtk_clipboard,
                                      gdk_atom_intern ("image/png", FALSE),
                                      clipboard_content_received,
                                      self);
      break;
    case CB_FORMAT_JPEG:
      gtk_clipboard_request_contents (priv->gtk_clipboard,
                                      gdk_atom_intern ("image/jpeg", FALSE),
                                      clipboard_content_received,
                                      self);
      break;
    case CF_DIB:
      gtk_clipboard_request_contents (priv->gtk_clipboard,
                                      gdk_atom_intern ("image/bmp", FALSE),
                                      clipboard_content_received,
                                      self);
      break;
    default:
      if (format == priv->fgdw_id) {
        gtk_clipboard_request_contents (priv->gtk_clipboard,
                                        gdk_atom_intern ("text/uri-list", FALSE),
                                        clipboard_content_received,
                                        self);
        break;
      } else {
        g_warning ("Requesting clipboard data of type %d not implemented.", format);
      }
  }

  return CHANNEL_RC_OK;
}

static guint
server_format_data_response (CliprdrClientContext               *context,
                             const CLIPRDR_FORMAT_DATA_RESPONSE *response)
{
  FrdpChannelClipboard        *self;
  FrdpChannelClipboardPrivate *priv;
  FrdpClipboardRequest        *current_request;
  guint                        j;
  gint                         subrequest_index = -1;

  if (context != NULL) {
    self = (FrdpChannelClipboard *) context->custom;
    priv = frdp_channel_clipboard_get_instance_private (self);

    if (response->msgType == CB_FORMAT_DATA_RESPONSE) {
      if (priv->requests != NULL) {
        current_request = priv->requests->data;
        for (j = 0; j < current_request->count; j++) {
          if (!current_request->responses[j].handled) {
            subrequest_index = j;
            break;
          }
        }

        if (subrequest_index >= 0 && subrequest_index < current_request->count) {
          current_request->responses[subrequest_index].handled = TRUE;
          if (response->msgFlags & CB_RESPONSE_OK) {
            current_request->responses[subrequest_index].length = response->dataLen;
            current_request->responses[subrequest_index].data = g_new (guchar, response->dataLen);
            memcpy (current_request->responses[subrequest_index].data, response->requestedFormatData, response->dataLen);
          } else {
            g_warning ("Clipboard data request failed!");
          }
        }
      } else {
        g_warning ("Response without request!");
      }
    }
  }

  return CHANNEL_RC_OK;
}

static guint
server_file_contents_request (CliprdrClientContext                *context,
                              const CLIPRDR_FILE_CONTENTS_REQUEST *file_contents_request)
{
  FrdpChannelClipboard           *self = (FrdpChannelClipboard *) context->custom;
  FrdpChannelClipboardPrivate    *priv = frdp_channel_clipboard_get_instance_private (self);
  CLIPRDR_FILE_CONTENTS_RESPONSE  response = { 0 };
  GFileInputStream               *stream;
  GFileInfo                      *file_info;
  GFileType                       file_type;
  guint64                        *size;
  goffset                         offset;
  guchar                         *data = NULL;
  gssize                          bytes_read;
  GFile                          *file;

  response.msgType = CB_FILECONTENTS_RESPONSE;
  response.msgFlags = CB_RESPONSE_FAIL;
  response.streamId = file_contents_request->streamId;

  /* TODO: Make it async. Signal progress if FD_SHOWPROGRESSUI is present. */
  if (file_contents_request->listIndex < priv->local_files_count) {
    file = g_file_new_for_uri (priv->local_files_infos[file_contents_request->listIndex].uri);

    if (file_contents_request->dwFlags & FILECONTENTS_SIZE) {
      file_info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
      size = g_new (guint64, 1);
      *size = g_file_info_get_size (file_info);

      response.requestedData = (guchar *) size;
      response.cbRequested = 8;
      response.dataLen = 8;
      response.msgFlags = CB_RESPONSE_OK;

      g_object_unref (file_info);
    } else if (file_contents_request->dwFlags & FILECONTENTS_RANGE) {
      file_info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
      file_type = g_file_info_get_file_type (file_info);

      if (file_type != G_FILE_TYPE_DIRECTORY) {
        offset = ((guint64) file_contents_request->nPositionHigh << 32) + file_contents_request->nPositionLow;

        stream = g_file_read (file, NULL, NULL);

        if (g_seekable_can_seek (G_SEEKABLE (stream)) && g_seekable_seek (G_SEEKABLE (stream), offset, G_SEEK_SET, NULL, NULL)) {
          data = g_new (guchar, file_contents_request->cbRequested);
          bytes_read = g_input_stream_read (G_INPUT_STREAM (stream), data, file_contents_request->cbRequested, NULL, NULL);

          response.requestedData = data;
          response.cbRequested = bytes_read;
          response.dataLen = bytes_read;
          response.msgFlags = CB_RESPONSE_OK;
        }
      } else {
        g_warning ("Content of a directory was requested!");
      }

      g_object_unref (stream);
      g_object_unref (file_info);
    }

    g_object_unref (file);
  } else {
    g_warning ("Requested index is outside of the file list!");
  }

  return priv->cliprdr_client_context->ClientFileContentsResponse (priv->cliprdr_client_context, &response);
}

static guint
server_file_contents_response (CliprdrClientContext                 *context,
                               const CLIPRDR_FILE_CONTENTS_RESPONSE *file_contents_response)
{
  FrdpChannelClipboard        *self;
  FrdpChannelClipboardPrivate *priv;
  FrdpClipboardRequest        *current_request;
  GFileOutputStream           *stream;
  const guchar                *data;
  guint32                      stream_id;
  gsize                        data_length, written = 0, i, j;
  GFile                       *file;
  gchar                       *path, *filename;
  GList                       *iter;

  if (context != NULL && file_contents_response->msgFlags & CB_RESPONSE_OK) {
    self = (FrdpChannelClipboard *) context->custom;
    priv = frdp_channel_clipboard_get_instance_private (self);

    stream_id = file_contents_response->streamId;
    data_length = file_contents_response->cbRequested;
    data = file_contents_response->requestedData;

    for (iter = priv->requests; iter != NULL; iter = iter->next) {
      current_request = iter->data;
      for (j = 0; j < current_request->count; j++) {
        if (current_request->requested_ids[j] == priv->fgdw_id) {
          for (i = 0; i < priv->remote_files_count; i++) {
            if (!priv->remote_files_infos[i].is_directory && priv->remote_files_infos[i].stream_id == stream_id) {
              filename = priv->remote_files_infos[i].filename;
              path = g_strdup_printf ("%s/%s", priv->tmp_directory, filename);

              file = g_file_new_for_path (path);
              stream = g_file_create (file, G_FILE_CREATE_PRIVATE, NULL, NULL);
              priv->remote_files_infos[i].created = TRUE;

              if (g_output_stream_write_all (G_OUTPUT_STREAM (stream), data, data_length, &written, NULL, NULL)) {
                priv->remote_files_infos[i].uri = g_file_get_uri (file);
              } else {
                g_warning ("Writing of pasted file failed!");
              }

              g_output_stream_close (G_OUTPUT_STREAM (stream), NULL, NULL);

              g_object_unref (file);
              g_free (path);
            }
          }
        }
      }
    }
  }

  return CHANNEL_RC_OK;
}

static void
frdp_channel_clipboard_set_client_context (FrdpChannelClipboard *self,
                                           CliprdrClientContext *context)
{
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);

  priv->cliprdr_client_context = context;

  context->custom = self;
  context->MonitorReady = monitor_ready;
  context->ServerCapabilities = server_capabilities;
  context->ServerFormatList = server_format_list;
  context->ServerFormatListResponse = server_format_list_response;
  context->ServerFormatDataRequest = server_format_data_request;
  context->ServerFormatDataResponse = server_format_data_response;
  context->ServerFileContentsRequest = server_file_contents_request;
  context->ServerFileContentsResponse = server_file_contents_response;

  /* TODO: Implement these:
       pcCliprdrServerLockClipboardData ServerLockClipboardData;
       pcCliprdrServerUnlockClipboardData ServerUnlockClipboardData;
   */
}
