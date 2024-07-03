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

#define FUSE_USE_VERSION 35
#include <fuse_lowlevel.h>

#ifdef HAVE_FREERDP3
#define COMMON(x) common.x
#else
#define COMMON(x) x
#endif

#define FRDP_CLIPBOARD_FORMAT_PNG          0xD011
#define FRDP_CLIPBOARD_FORMAT_JPEG         0xD012
#define FRDP_CLIPBOARD_FORMAT_TEXT_URILIST 0xD014

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

typedef enum
{
  FUSE_GETATTR_OP,
  FUSE_LOOKUP_OP,
  FUSE_READ_OP
} FrdpFuseOp;

typedef struct
{
  gssize     index;
  fuse_req_t request;
  FrdpFuseOp op;
} FrdpRemoteFileRequest;

typedef struct
{
  gchar           *uri;
  FILEDESCRIPTORW *descriptor;
} FrdpLocalFileInfo;

typedef struct
{
  guint              clip_data_id;
  gsize              local_files_count;
  FrdpLocalFileInfo *local_files_infos;
} FrdpLocalLockData;

typedef struct _FrdpRemoteFileInfo FrdpRemoteFileInfo;

struct _FrdpRemoteFileInfo
{
  gchar              *uri;
  gchar              *path;
  gchar              *filename;

  guint               stream_id;

  gboolean            is_directory;
  gboolean            is_readonly;

  fuse_ino_t          inode;
  gssize              parent_index; /* -1 means root directory */
  GList              *children;

  gboolean            has_size;
  uint64_t            size;
};

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
  GHashTable                  *remote_files_requests;

  gsize                        local_files_count;
  FrdpLocalFileInfo           *local_files_infos;

  guint                        next_stream_id;
  guint                        fgdw_id;

  struct fuse_session         *fuse_session;
  GThread                     *fuse_session_thread;
  gchar                       *fuse_directory;
  GMutex                       fuse_mutex;

  fuse_ino_t                   current_inode;

  GList                       *locked_data;           /* List of locked arrays of files - list of (FrdpLocalLockData *) */
  GMutex                       lock_mutex;
  gboolean                     pending_lock;          /* Lock was requested right after format list has been sent */
  guint                        pending_lock_id;       /* Id for the pending lock */
  gboolean                     awaiting_data_request; /* Format list has been send but data were not requested yet */

  guint                        remote_clip_data_id;   /* clipDataId for copying from remote side */
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

static void  frdp_local_lock_data_free                 (FrdpLocalLockData    *lock_data);
static void  lock_current_local_files                  (FrdpChannelClipboard *self,
                                                        guint                 clip_data_id);

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

  g_signal_handler_disconnect (priv->gtk_clipboard,
                               priv->clipboard_owner_changed_id);

  g_hash_table_unref (priv->remote_files_requests);
  fuse_session_unmount (priv->fuse_session);
  fuse_session_exit (priv->fuse_session);

  if (priv->remote_data_in_clipboard)
    gtk_clipboard_clear (priv->gtk_clipboard);

  g_clear_pointer (&priv->fuse_directory, g_free);

  g_mutex_lock (&priv->lock_mutex);

  g_list_free_full (priv->locked_data, (GDestroyNotify) frdp_local_lock_data_free);
  priv->locked_data = NULL;

  g_mutex_unlock (&priv->lock_mutex);

  g_thread_join (priv->fuse_session_thread);
  g_mutex_clear (&priv->fuse_mutex);
  g_mutex_clear (&priv->lock_mutex);

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

static gssize
get_remote_file_info_index (FrdpChannelClipboard *self,
                            fuse_ino_t            inode)
{
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  gssize                       result = -1, i;

  for (i = 0; i < priv->remote_files_count; i++) {
    if (priv->remote_files_infos[i].inode == inode) {
      result = i;
      break;
    }
  }

  return result;
}

static void
get_root_attributes (struct stat *attr)
{
  memset (attr, 0, sizeof (struct stat));

  attr->st_ino = FUSE_ROOT_ID;
  attr->st_mode = S_IFDIR | 0755;
  attr->st_nlink = 2;
  attr->st_uid = getuid ();
  attr->st_gid = getgid ();
  attr->st_atime = attr->st_mtime = attr->st_ctime = time (NULL);
}

static void
get_file_attributes (FrdpRemoteFileInfo  info,
                     struct stat        *attr)
{
  memset (attr, 0, sizeof (struct stat));

  attr->st_ino = info.inode;
  if (info.is_directory) {
    attr->st_mode = S_IFDIR | (info.is_readonly ? 0555 : 0755);
    attr->st_nlink = 2;
  } else {
    attr->st_mode = S_IFREG | (info.is_readonly ? 0444 : 0644);
    attr->st_nlink = 1;
    attr->st_size = info.size;
  }
  attr->st_uid = getuid ();
  attr->st_gid = getgid ();
  attr->st_atime = attr->st_mtime = attr->st_ctime = time (NULL);
}

static void
request_size (FrdpChannelClipboard *self,
              fuse_req_t            request,
              gsize                 index,
              FrdpFuseOp            op)
{
  CLIPRDR_FILE_CONTENTS_REQUEST  file_contents_request = { 0 };
  FrdpChannelClipboardPrivate   *priv = frdp_channel_clipboard_get_instance_private (self);
  FrdpRemoteFileRequest         *size_request;

  file_contents_request.streamId = priv->next_stream_id++;
  file_contents_request.listIndex = index;
  file_contents_request.dwFlags = FILECONTENTS_SIZE;
  file_contents_request.cbRequested = 8;
  file_contents_request.nPositionHigh = 0;
  file_contents_request.nPositionLow = 0;
  file_contents_request.haveClipDataId = TRUE;
  file_contents_request.clipDataId = priv->remote_clip_data_id;

  size_request = g_new0 (FrdpRemoteFileRequest, 1);
  size_request->index = index;
  size_request->request = request;
  size_request->op = op;

  g_hash_table_insert (priv->remote_files_requests, GUINT_TO_POINTER (file_contents_request.streamId), size_request);

  priv->cliprdr_client_context->ClientFileContentsRequest (priv->cliprdr_client_context, &file_contents_request);
}

static void
fuse_lookup (fuse_req_t  request,
             fuse_ino_t  parent_inode,
             const char *name)
{
  FrdpChannelClipboard        *self = fuse_req_userdata (request);
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  struct fuse_entry_param      entry = {0};
  gboolean                     found = FALSE;
  gssize                       parent_index;
  gsize                        i, child_index;
  GList                       *iter;

  g_mutex_lock (&priv->fuse_mutex);

  if (parent_inode == FUSE_ROOT_ID) {
    for (i = 0; i < priv->remote_files_count && !found; i++) {
      if (priv->remote_files_infos[i].parent_index == -1 &&
          g_str_equal (name, priv->remote_files_infos[i].filename)) {
        found = TRUE;
        if (priv->remote_files_infos[i].has_size ||
            priv->remote_files_infos[i].is_directory) {
          entry.ino = priv->remote_files_infos[i].inode;
          get_file_attributes (priv->remote_files_infos[i], &entry.attr);
          entry.attr_timeout = 1.0;
          entry.entry_timeout = 1.0;

          fuse_reply_entry (request, &entry);
        } else {
          request_size (self, request, i, FUSE_LOOKUP_OP);
        }
      }
    }
  } else {
    parent_index = get_remote_file_info_index (self, parent_inode);
    if (parent_index >= 0 && priv->remote_files_infos[parent_index].is_directory) {
      for (iter = priv->remote_files_infos[parent_index].children; iter != NULL && !found; iter = iter->next) {
        child_index = *((gsize *) iter->data);
        if (g_str_equal (name, priv->remote_files_infos[child_index].filename)) {
          found = TRUE;
          if (priv->remote_files_infos[child_index].has_size ||
              priv->remote_files_infos[child_index].is_directory) {
            entry.ino = priv->remote_files_infos[child_index].inode;
            get_file_attributes (priv->remote_files_infos[child_index], &entry.attr);
            entry.attr_timeout = 1.0;
            entry.entry_timeout = 1.0;

            fuse_reply_entry (request, &entry);
          } else {
            request_size (self, request, child_index, FUSE_LOOKUP_OP);
          }
        }
      }
    }
  }

  if (!found)
    fuse_reply_err (request, ENOENT);

  g_mutex_unlock (&priv->fuse_mutex);
}

static void
fuse_getattr (fuse_req_t             request,
              fuse_ino_t             inode,
              struct fuse_file_info *file_info)
{
  FrdpChannelClipboard        *self = fuse_req_userdata (request);
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  struct stat                  attr = {0};
  gssize                       index;

  g_mutex_lock (&priv->fuse_mutex);

  if (inode == FUSE_ROOT_ID) {
    get_root_attributes (&attr);
    fuse_reply_attr (request, &attr, 1);
  } else {
    index = get_remote_file_info_index (self, inode);
    if (index >= 0) {
      if (priv->remote_files_infos[index].has_size ||
          priv->remote_files_infos[index].is_directory) {
        get_file_attributes (priv->remote_files_infos[index], &attr);
        fuse_reply_attr (request, &attr, 1);
      } else {
        request_size (self, request, index, FUSE_GETATTR_OP);
      }
    } else {
      fuse_reply_err (request, ENOENT);
    }
  }

  g_mutex_unlock (&priv->fuse_mutex);
}

static void
fuse_open (fuse_req_t             request,
           fuse_ino_t             inode,
           struct fuse_file_info *file_info)
{
  FrdpChannelClipboard        *self = fuse_req_userdata (request);
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  gssize                       index;

  g_mutex_lock (&priv->fuse_mutex);

  if (inode == FUSE_ROOT_ID) {
    fuse_reply_err (request, EISDIR);
  } else {
    index = get_remote_file_info_index (self, inode);
    if (index >= 0) {
      if (priv->remote_files_infos[index].is_directory) {
        fuse_reply_err (request, EISDIR);
      } else {
        file_info->direct_io = 1;
        fuse_reply_open (request, file_info);
      }
    } else {
      fuse_reply_err (request, ENOENT);
    }
  }

  g_mutex_unlock (&priv->fuse_mutex);
}

static void
fuse_read (fuse_req_t             request,
           fuse_ino_t             inode,
           size_t                 size,
           off_t                  offset,
           struct fuse_file_info *file_info)
{
  FrdpChannelClipboard        *self = fuse_req_userdata (request);
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  gssize                       index;

  g_mutex_lock (&priv->fuse_mutex);

  index = get_remote_file_info_index (self, inode);
  if (index >= 0) {
    if (priv->remote_files_infos[index].is_directory) {
      fuse_reply_err (request, EISDIR);
    } else {
      CLIPRDR_FILE_CONTENTS_REQUEST  file_contents_request = { 0 };
      FrdpRemoteFileRequest         *data_request;

      size = MIN (size, 8 * 1024 * 1024);
      g_assert (size > 0);

      file_contents_request.streamId = priv->next_stream_id++;
      file_contents_request.listIndex = index;
      file_contents_request.dwFlags = FILECONTENTS_RANGE;
      file_contents_request.cbRequested = size;
      file_contents_request.nPositionHigh = offset >> 32;
      file_contents_request.nPositionLow = offset & 0xffffffff;
      file_contents_request.haveClipDataId = TRUE;
      file_contents_request.clipDataId = priv->remote_clip_data_id;

      data_request = g_new0 (FrdpRemoteFileRequest, 1);
      data_request->index = index;
      data_request->request = request;
      data_request->op = FUSE_READ_OP;

      g_hash_table_insert (priv->remote_files_requests, GUINT_TO_POINTER (file_contents_request.streamId), data_request);

      priv->cliprdr_client_context->ClientFileContentsRequest (priv->cliprdr_client_context, &file_contents_request);
    }
  } else {
    fuse_reply_err (request, ENOENT);
  }

  g_mutex_unlock (&priv->fuse_mutex);
}

static void
fuse_opendir (fuse_req_t             request,
              fuse_ino_t             inode,
              struct fuse_file_info *file_info)
{
  FrdpChannelClipboard        *self = fuse_req_userdata (request);
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  gssize                       index;

  g_mutex_lock (&priv->fuse_mutex);

  if (inode == FUSE_ROOT_ID) {
    fuse_reply_open (request, file_info);
  } else {
    index = get_remote_file_info_index (self, inode);
    if (index >= 0) {
      if (priv->remote_files_infos[index].is_directory) {
        fuse_reply_open (request, file_info);
      } else {
        fuse_reply_err (request, ENOTDIR);
      }
    } else {
      fuse_reply_err (request, ENOENT);
    }
  }

  g_mutex_unlock (&priv->fuse_mutex);
}

static void
fuse_readdir (fuse_req_t             request,
              fuse_ino_t             inode,
              size_t                 size,
              off_t                  offset,
              struct fuse_file_info *file_info)
{
  FrdpChannelClipboard        *self = fuse_req_userdata (request);
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  struct stat                  attr = {0};
  gboolean                     done = FALSE;
  gssize                       index, i, j;
  GList                       *iter;
  gsize                        written = 0, entry_size, child_index;
  char                        *buffer;

  buffer = g_malloc0 (size);

  g_mutex_lock (&priv->fuse_mutex);

  if (inode == FUSE_ROOT_ID) {
    j = -1;
    for (i = 0; i < priv->remote_files_count; i++) {
      if (priv->remote_files_infos[i].parent_index == -1) {
        j++;
        if (j <= offset && offset > 0)
          continue;

        get_file_attributes (priv->remote_files_infos[i], &attr);

        entry_size = fuse_add_direntry (request, buffer + written,
                                        size - written,
                                        priv->remote_files_infos[i].filename, &attr, j + 1);

        if (entry_size > size - written)
          break;

        written += entry_size;

        if (i == priv->remote_files_count - 1)
          done = TRUE;
      }
    }
    fuse_reply_buf (request, buffer, written);
  } else {
    index = get_remote_file_info_index (self, inode);
    if (index >= 0) {
      if (priv->remote_files_infos[index].is_directory) {
        for (iter = priv->remote_files_infos[index].children, i = 0; iter != NULL; iter = iter->next, i++) {
          child_index = *((gsize *) iter->data);

          if (i <= offset && offset > 0)
            continue;

          get_file_attributes (priv->remote_files_infos[child_index], &attr);

          entry_size = fuse_add_direntry (request, buffer + written,
                                          size - written,
                                          priv->remote_files_infos[child_index].filename, &attr, i + 1);

          if (entry_size > size - written)
            break;

          written += entry_size;

          if (iter == NULL)
            done = TRUE;
        }

        fuse_reply_buf (request, buffer, written);
      } else {
        fuse_reply_err (request, ENOTDIR);
      }
    } else {
      fuse_reply_err (request, ENOENT);
    }
  }

  if (done)
    fuse_reply_buf (request, NULL, 0);

  g_mutex_unlock (&priv->fuse_mutex);

  g_free (buffer);
}

static const struct fuse_lowlevel_ops fuse_ops =
{
  .lookup = fuse_lookup,
  .getattr = fuse_getattr,
  .open = fuse_open,
  .read = fuse_read,
  .opendir = fuse_opendir,
  .readdir = fuse_readdir,
};

static gpointer
fuse_session_thread_func (gpointer data)
{
  FrdpChannelClipboard        *self = data;
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  gint                         result;

  g_mutex_lock (&priv->fuse_mutex);
  fuse_session_mount (priv->fuse_session,
                      priv->fuse_directory);

  fuse_daemonize (1);
  g_mutex_unlock (&priv->fuse_mutex);

  result = fuse_session_loop (priv->fuse_session);

  g_mutex_lock (&priv->fuse_mutex);
  fuse_session_unmount (priv->fuse_session);
  g_mutex_unlock (&priv->fuse_mutex);

  return NULL;
}

static void
frdp_channel_clipboard_init (FrdpChannelClipboard *self)
{
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  struct fuse_args             args = {0};
  gchar                       *argv[2];

  priv->gtk_clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
  priv->clipboard_owner_changed_id = g_signal_connect (priv->gtk_clipboard, "owner-change", G_CALLBACK (clipboard_owner_change_cb), self);
  priv->fgdw_id = FRDP_CLIPBOARD_FORMAT_TEXT_URILIST;
  priv->current_inode = FUSE_ROOT_ID + 1;
  priv->locked_data = NULL;
  priv->pending_lock = FALSE;
  priv->remote_clip_data_id = 0;

  argv[0] = "gnome-connections";
  argv[1] = "-d";
  args.argc = 1; /* Set to 2 to see debug logs of Fuse */
  args.argv = argv;

  priv->remote_files_requests = g_hash_table_new (g_direct_hash, g_direct_equal);

  g_mutex_init (&priv->fuse_mutex);
  g_mutex_init (&priv->lock_mutex);

  priv->fuse_directory = g_mkdtemp (g_strdup_printf ("%s/clipboard-XXXXXX/", g_get_user_runtime_dir ()));

  priv->fuse_session = fuse_session_new (&args, &fuse_ops, sizeof (fuse_ops), self);
  if (priv->fuse_session != NULL) {
    priv->fuse_session_thread = g_thread_new ("RDP FUSE session thread",
                                              fuse_session_thread_func,
                                              self);
  } else {
    g_warning ("Could not initiate FUSE session\n");
  }
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
                                        CB_CAN_LOCK_CLIPDATA |
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
        formats[j].formatId = FRDP_CLIPBOARD_FORMAT_PNG;
        formats[j++].formatName = NULL;
      } else if (g_strcmp0 (atom_name, "image/jpeg") == 0) {
        formats[j].formatId = FRDP_CLIPBOARD_FORMAT_JPEG;
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

  format_list.COMMON(msgType) = CB_FORMAT_LIST;
  format_list.COMMON(msgFlags) = CB_RESPONSE_OK;
  format_list.numFormats = j;
  format_list.formats = formats;

  priv->awaiting_data_request = TRUE;
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

  response.COMMON(msgType) = CB_FORMAT_LIST_RESPONSE;
  response.COMMON(msgFlags) = status ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
  response.COMMON(dataLen) = 0;

  return priv->cliprdr_client_context->ClientFormatListResponse (priv->cliprdr_client_context, &response);
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

static WCHAR *
convert_to_unicode (const gchar *text)
{
  WCHAR *result = NULL;

  if (text != NULL) {
#ifdef HAVE_FREERDP3
    result = ConvertUtf8ToWCharAlloc (text, NULL);
#else
    ConvertToUnicode (CP_UTF8, 0, (LPCSTR) text, -1, &result, 0);
#endif
  }

  return result;
}

static gchar *
convert_from_unicode (const WCHAR *text,
                      gint         text_length)
{
  gchar *result = NULL;

  if (text != NULL) {
#ifdef HAVE_FREERDP3
    result = ConvertWCharNToUtf8Alloc (text, text_length, NULL);
#else
    ConvertFromUnicode (CP_UTF8, 0, text, text_length, &result, 0, NULL, NULL);
#endif
  }

  return result;
}

/* TODO: Rewrite this using async methods of GtkCLipboard once we move to Gtk4 */
static void
_gtk_clipboard_get_func (GtkClipboard     *clipboard,
                         GtkSelectionData *selection_data,
                         guint             info,
                         gpointer          user_data)
{
  CLIPRDR_LOCK_CLIPBOARD_DATA  lock_clipboard_data = { 0 };
  FrdpChannelClipboard        *self = (FrdpChannelClipboard *) user_data;
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  FrdpClipboardRequest        *current_request;
  gchar                       *data = NULL;
  gint                         length;

  lock_clipboard_data.COMMON(msgType) = CB_LOCK_CLIPDATA;
  lock_clipboard_data.COMMON(msgFlags) = 0;
  lock_clipboard_data.COMMON(dataLen) = 4;
  lock_clipboard_data.clipDataId = ++priv->remote_clip_data_id;
  priv->cliprdr_client_context->ClientLockClipboardData (priv->cliprdr_client_context, &lock_clipboard_data);

  current_request = frdp_clipboard_request_send (self, info);
  if (current_request != NULL) {

    while (!frdp_clipboard_request_done (current_request))
      gtk_main_iteration ();

    if (info == CF_UNICODETEXT) {
      /* TODO - convert CR LF to CR */
      data = convert_from_unicode ((WCHAR *) current_request->responses[0].data, current_request->responses[0].length / sizeof (WCHAR));
      if (data != NULL) {
        length = strlen (data);
        gtk_selection_data_set (selection_data,
                                gdk_atom_intern ("UTF8_STRING", FALSE),
                                8,
                                (guchar *) data,
                                length);
      }
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
      for (guint j = 0; j < current_request->count; j++) {
        if (current_request->requested_ids[j] == priv->fgdw_id) {
          FILEDESCRIPTORW  *files = (FILEDESCRIPTORW *) (current_request->responses[j].data + 4);
          GList            *iter, *uri_list = NULL;
          gchar            *path, **uri_array, *tmps, *slash, *dir;
          guint             i, count = current_request->responses[j].length / sizeof (FILEDESCRIPTORW);

          g_mutex_lock (&priv->fuse_mutex);

          priv->remote_files_count = count;
          priv->remote_files_infos = g_new0 (FrdpRemoteFileInfo, priv->remote_files_count);

          for (i = 0; i < count; i++) {
            path = convert_from_unicode ((WCHAR *) files[i].cFileName, 260 / sizeof (WCHAR));

            replace_ascii_character (path, '\\', '/');

            priv->remote_files_infos[i].path = g_strdup (path);
            priv->remote_files_infos[i].is_directory = (files[i].dwFlags & FD_ATTRIBUTES) && (files[i].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
            priv->remote_files_infos[i].is_readonly = (files[i].dwFlags & FD_ATTRIBUTES) && (files[i].dwFileAttributes & FILE_ATTRIBUTE_READONLY);
            priv->remote_files_infos[i].inode = priv->current_inode++;
            priv->remote_files_infos[i].uri = g_strdup_printf ("file://%s/%s%s", priv->fuse_directory, path, priv->remote_files_infos[i].is_directory ? "/" : "");
            if (files[i].dwFlags & FD_FILESIZE) {
              priv->remote_files_infos[i].size = ((guint64) files[i].nFileSizeHigh << 32) + files[i].nFileSizeLow;
              priv->remote_files_infos[i].has_size = TRUE;
            }
            priv->remote_files_infos[i].parent_index = -1;

            g_free (path);
          }

          for (i = 0; i < count; i++) {
            slash = NULL;

            tmps = g_strdup (priv->remote_files_infos[i].uri);
            if (priv->remote_files_infos[i].is_directory) {
              if (g_str_has_suffix (tmps, "/"))
                tmps[strlen (tmps) - 1] = '\0';
            }
            slash = g_strrstr (tmps, "/");

            dir = NULL;
            if (slash != NULL) {

              if (strlen (slash) > 1) {
                priv->remote_files_infos[i].filename = g_strdup (slash + 1);
                slash[1] = '\0';
                dir = g_strdup (tmps);
              }

              if (dir != NULL) {
                if (g_str_equal (dir, priv->fuse_directory)) {
                } else {
                  for (j = 0; j < count; j++) {
                    if (g_str_equal (dir, priv->remote_files_infos[j].uri)) {
                      gsize *child_index;
                      priv->remote_files_infos[i].parent_index = j;

                      child_index = g_new (gsize, 1);
                      *child_index = i;
                      priv->remote_files_infos[j].children = g_list_append (priv->remote_files_infos[j].children, child_index);
                      priv->remote_files_infos[i].parent_index = j;
                      break;
                    }
                  }
                }
                g_free (dir);
              }
            }
            g_free (tmps);
          }

          /* Set URIs for topmost items only, the rest will be pasted as part of those. */
          for (i = 0; i < priv->remote_files_count; i++) {
            if (priv->remote_files_infos[i].parent_index < 0) {
              uri_list = g_list_prepend (uri_list, priv->remote_files_infos[i].uri);
            }
          }

          g_mutex_unlock (&priv->fuse_mutex);

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
  CLIPRDR_UNLOCK_CLIPBOARD_DATA  unlock_clipboard_data = { 0 };
  FrdpChannelClipboard          *self = (FrdpChannelClipboard *) user_data;
  FrdpChannelClipboardPrivate   *priv = frdp_channel_clipboard_get_instance_private (self);
  guint                          i;

  g_mutex_lock (&priv->fuse_mutex);

  if (priv->remote_files_infos != NULL) {
    for (i = 0; i < priv->remote_files_count; i++) {
      g_free (priv->remote_files_infos[i].uri);
      g_free (priv->remote_files_infos[i].path);
      g_free (priv->remote_files_infos[i].filename);
      g_list_free_full (priv->remote_files_infos[i].children, g_free);
    }
    g_clear_pointer (&priv->remote_files_infos, g_free);
  }
  priv->remote_files_count = 0;

  g_mutex_unlock (&priv->fuse_mutex);

  unlock_clipboard_data.COMMON(msgType) = CB_UNLOCK_CLIPDATA;
  unlock_clipboard_data.COMMON(msgFlags) = 0;
  unlock_clipboard_data.COMMON(dataLen) = 4;
  unlock_clipboard_data.clipDataId = priv->remote_clip_data_id;
  priv->cliprdr_client_context->ClientUnlockClipboardData (priv->cliprdr_client_context, &unlock_clipboard_data);

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
        } else if (format_list->formats[i].formatId == FRDP_CLIPBOARD_FORMAT_PNG) {
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

    file_name = convert_to_unicode (relative_path);
    if (file_name != NULL) {
      memcpy (frdp_file_info->descriptor->cFileName, file_name, strlen (relative_path) * 2);
      g_free (file_name);
    }
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

  response.COMMON(msgFlags) = (data) ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
  response.COMMON(dataLen) = (guint32) size;
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

  if (length > 0) {
    if (data_type == gdk_atom_intern ("UTF8_STRING", FALSE)) {
      text = gtk_selection_data_get_text (selection_data);
      text_length = strlen ((gchar *) text);

      data = (guchar *) convert_to_unicode ((gchar *) text);
      if (data != NULL) {
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

      g_strfreev (uris);

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

      if (priv->awaiting_data_request && priv->pending_lock) {
        lock_current_local_files (self, priv->pending_lock_id);

        priv->awaiting_data_request = FALSE;
      }

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
    case FRDP_CLIPBOARD_FORMAT_PNG:
      gtk_clipboard_request_contents (priv->gtk_clipboard,
                                      gdk_atom_intern ("image/png", FALSE),
                                      clipboard_content_received,
                                      self);
      break;
    case FRDP_CLIPBOARD_FORMAT_JPEG:
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

    if (response->COMMON(msgType) == CB_FORMAT_DATA_RESPONSE) {
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
          if (response->COMMON(msgFlags) & CB_RESPONSE_OK) {
            current_request->responses[subrequest_index].length = response->COMMON(dataLen);
            current_request->responses[subrequest_index].data = g_new (guchar, response->COMMON(dataLen));
            memcpy (current_request->responses[subrequest_index].data, response->requestedFormatData, response->COMMON(dataLen));
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
  FrdpLocalFileInfo               local_file_info;
  FrdpLocalLockData              *ldata;
  GFileInputStream               *stream;
  GFileInfo                      *file_info;
  GFileType                       file_type;
  gboolean                        local_file_info_set = FALSE, clip_data_id_found = FALSE;
  guint64                        *size;
  goffset                         offset;
  guchar                         *data = NULL;
  gssize                          bytes_read;
  GList                          *iter;
  GFile                          *file;

  response.COMMON(msgType) = CB_FILECONTENTS_RESPONSE;
  response.COMMON(msgFlags) = CB_RESPONSE_FAIL;
  response.streamId = file_contents_request->streamId;

  g_mutex_lock (&priv->lock_mutex);

  if (file_contents_request->haveClipDataId) {
    for (iter = priv->locked_data; iter != NULL; iter = iter->next) {
      ldata = (FrdpLocalLockData *) iter->data;

      if (ldata->clip_data_id == file_contents_request->clipDataId) {
        clip_data_id_found = TRUE;
        if (file_contents_request->listIndex < ldata->local_files_count) {
          local_file_info = ldata->local_files_infos[file_contents_request->listIndex];
          local_file_info_set = TRUE;
        }
        break;
      }
    }
  }

  if (!local_file_info_set && !clip_data_id_found) {
    if (file_contents_request->listIndex < priv->local_files_count) {
      local_file_info = priv->local_files_infos[file_contents_request->listIndex];
      local_file_info_set = TRUE;
    }
  }

  /* TODO: Make it async. Signal progress if FD_SHOWPROGRESSUI is present. */
  if (local_file_info_set) {
    file = g_file_new_for_uri (local_file_info.uri);

    if (file_contents_request->dwFlags & FILECONTENTS_SIZE) {
      file_info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
      size = g_new (guint64, 1);
      *size = g_file_info_get_size (file_info);

      response.requestedData = (guchar *) size;
      response.cbRequested = 8;
      response.COMMON(dataLen) = 8;
      response.COMMON(msgFlags) = CB_RESPONSE_OK;

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
          response.COMMON(dataLen) = bytes_read;
          response.COMMON(msgFlags) = CB_RESPONSE_OK;
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

  g_mutex_unlock (&priv->lock_mutex);

  return priv->cliprdr_client_context->ClientFileContentsResponse (priv->cliprdr_client_context, &response);
}

static guint
server_file_contents_response (CliprdrClientContext                 *context,
                               const CLIPRDR_FILE_CONTENTS_RESPONSE *file_contents_response)
{
  FrdpChannelClipboard        *self;
  FrdpChannelClipboardPrivate *priv;
  struct fuse_entry_param      entry = {0};
  FrdpRemoteFileRequest       *request;
  struct stat                  attr = {0};

  if (context != NULL && file_contents_response->COMMON(msgFlags) & CB_RESPONSE_OK) {
    self = (FrdpChannelClipboard *) context->custom;
    priv = frdp_channel_clipboard_get_instance_private (self);

    request = g_hash_table_lookup (priv->remote_files_requests,
                                   GUINT_TO_POINTER (file_contents_response->streamId));
    if (request != NULL) {
      g_mutex_lock (&priv->fuse_mutex);
      switch (request->op) {
        case FUSE_LOOKUP_OP:
          priv->remote_files_infos[request->index].size = *((guint64 *) file_contents_response->requestedData);
          priv->remote_files_infos[request->index].has_size = TRUE;

          entry.ino = priv->remote_files_infos[request->index].inode;
          get_file_attributes (priv->remote_files_infos[request->index], &entry.attr);
          entry.attr_timeout = 1.0;
          entry.entry_timeout = 1.0;

          fuse_reply_entry (request->request, &entry);
          break;

        case FUSE_GETATTR_OP:
          priv->remote_files_infos[request->index].size = *((guint64 *) file_contents_response->requestedData);
          priv->remote_files_infos[request->index].has_size = TRUE;

          get_file_attributes (priv->remote_files_infos[request->index], &attr);
          fuse_reply_attr (request->request, &attr, 1);
          break;

        case FUSE_READ_OP:
          fuse_reply_buf (request->request,
                          (const char *) file_contents_response->requestedData,
                          file_contents_response->cbRequested);
          break;

        default:
          g_assert_not_reached ();
      }

      g_hash_table_remove (priv->remote_files_requests,
                           GUINT_TO_POINTER (file_contents_response->streamId));
      g_free (request);
      g_mutex_unlock (&priv->fuse_mutex);
    }
  } else {
    if (file_contents_response->COMMON(msgFlags) & CB_RESPONSE_FAIL) {
      g_warning ("Server file response has failed!");
    }
  }

  return CHANNEL_RC_OK;
}

static void
lock_current_local_files (FrdpChannelClipboard *self,
                          guint                 clip_data_id)
{
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  FrdpLocalLockData           *lock_data;
  guint                        i;

  g_mutex_lock (&priv->lock_mutex);

  /* TODO: Implement flock */
  if (priv->local_files_count > 0) {
    lock_data = g_new (FrdpLocalLockData, 1);
    lock_data->clip_data_id = clip_data_id;
    lock_data->local_files_count = priv->local_files_count;
    lock_data->local_files_infos = g_new (FrdpLocalFileInfo, lock_data->local_files_count);
    for (i = 0; i < lock_data->local_files_count; i++) {
      lock_data->local_files_infos[i].descriptor = priv->local_files_infos[i].descriptor;
      lock_data->local_files_infos[i].uri = g_strdup (priv->local_files_infos[i].uri);
    }

    priv->locked_data = g_list_append (priv->locked_data, lock_data);
    if (priv->pending_lock_id == clip_data_id)
      priv->pending_lock = FALSE;
  }

  g_mutex_unlock (&priv->lock_mutex);
}

static guint
server_lock_clipboard_data (CliprdrClientContext              *context,
                            const CLIPRDR_LOCK_CLIPBOARD_DATA *lock_clipboard_data)
{
  FrdpChannelClipboard        *self = (FrdpChannelClipboard *) context->custom;
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);

  if (priv->awaiting_data_request) {
    priv->pending_lock = TRUE;
    priv->pending_lock_id = lock_clipboard_data->clipDataId;
  } else {
    lock_current_local_files (self, lock_clipboard_data->clipDataId);
  }

  return CHANNEL_RC_OK;
}

static void
frdp_local_lock_data_free (FrdpLocalLockData *lock_data)
{
  guint i;

  for (i = 0; i < lock_data->local_files_count; i++)
    g_free (lock_data->local_files_infos[i].uri);
  g_free (lock_data->local_files_infos);
  g_free (lock_data);
}

static guint
server_unlock_clipboard_data (CliprdrClientContext                *context,
                              const CLIPRDR_UNLOCK_CLIPBOARD_DATA *unlock_clipboard_data)
{
  FrdpChannelClipboard        *self = (FrdpChannelClipboard *) context->custom;
  FrdpChannelClipboardPrivate *priv = frdp_channel_clipboard_get_instance_private (self);
  FrdpLocalLockData           *lock_data;
  GList                       *iter;

  g_mutex_lock (&priv->lock_mutex);

  for (iter = priv->locked_data; iter != NULL; iter = iter->next) {
    lock_data = iter->data;

    if (lock_data->clip_data_id == unlock_clipboard_data->clipDataId) {
      frdp_local_lock_data_free (lock_data);

      priv->locked_data = g_list_delete_link (priv->locked_data, iter);
      break;
    }
  }

  g_mutex_unlock (&priv->lock_mutex);

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

  /* These don't lock/unlock files currently but store lists of files with their clipDataId. */
  context->ServerLockClipboardData = server_lock_clipboard_data;
  context->ServerUnlockClipboardData = server_unlock_clipboard_data;
}
