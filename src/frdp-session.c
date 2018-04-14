/* frdp-session.c
 *
 * Copyright (C) 2018 Felipe Borges <felipeborges@gnome.org>
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

#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

#include "frdp-session.h"

struct _FrdpSessionPrivate
{
  freerdp      *freerdp_session;

  GtkWidget    *display;
};

G_DEFINE_TYPE_WITH_PRIVATE (FrdpSession, frdp_session, G_TYPE_OBJECT)

enum
{
  PROP_0 = 0,
  PROP_HOSTNAME,
  PROP_PORT,
  PROP_USERNAME,
  PROP_PASSWORD,
  PROP_DISPLAY
};

struct frdp_context
{
  rdpContext context;
  FrdpSession *self;
};
typedef struct frdp_context frdpContext;

static void
frdp_session_set_hostname (FrdpSession *self,
                           const gchar *hostname)
{
  rdpSettings *settings = self->priv->freerdp_session->settings;

  g_free (settings->ServerHostname);
  settings->ServerHostname = g_strdup (hostname);
}

static void
frdp_session_set_port (FrdpSession *self,
                       guint        port)
{
  rdpSettings *settings = self->priv->freerdp_session->settings;

  settings->ServerPort = port;
}

static void
frdp_session_set_username (FrdpSession *self,
                           const gchar *username)
{
  rdpSettings *settings = self->priv->freerdp_session->settings;

  g_free (settings->Username);
  settings->Username = g_strdup (username);
}

static void
frdp_session_set_password (FrdpSession *self,
                           const gchar *password)
{
  rdpSettings *settings = self->priv->freerdp_session->settings;

  g_free (settings->Password);
  settings->Password = g_strdup (password);
}

static gboolean
frdp_certificate_verify (freerdp  *freerdp_session,
                         gchar    *subject,
                         gchar    *issuer,
                         gchar    *fingerprint)
{
  /* TODO */
  return FALSE;
}

static gboolean
frdp_authenticate (freerdp  *freerdp_session,
                   gchar   **username,
                   gchar   **password,
                   gchar   **domain)
{
  /* TODO: ask for credentials */

  return TRUE;
}

static gboolean
frdp_pre_connect (freerdp *freerdp_session)
{
  rdpSettings *settings = freerdp_session->settings;

  settings->OrderSupport[NEG_DSTBLT_INDEX] = TRUE;
  settings->OrderSupport[NEG_PATBLT_INDEX] = TRUE;
  settings->OrderSupport[NEG_SCRBLT_INDEX] = TRUE;
  settings->OrderSupport[NEG_OPAQUE_RECT_INDEX] = TRUE;
  settings->OrderSupport[NEG_DRAWNINEGRID_INDEX] = FALSE;
  settings->OrderSupport[NEG_MULTIDSTBLT_INDEX] = FALSE;
  settings->OrderSupport[NEG_MULTIPATBLT_INDEX] = FALSE;
  settings->OrderSupport[NEG_MULTISCRBLT_INDEX] = FALSE;
  settings->OrderSupport[NEG_MULTIOPAQUERECT_INDEX] = TRUE;
  settings->OrderSupport[NEG_MULTI_DRAWNINEGRID_INDEX] = FALSE;
  settings->OrderSupport[NEG_LINETO_INDEX] = TRUE;
  settings->OrderSupport[NEG_POLYLINE_INDEX] = TRUE;
  settings->OrderSupport[NEG_MEMBLT_INDEX] = TRUE;
  settings->OrderSupport[NEG_MEM3BLT_INDEX] = FALSE;
  settings->OrderSupport[NEG_MEMBLT_V2_INDEX] = TRUE;
  settings->OrderSupport[NEG_MEM3BLT_V2_INDEX] = FALSE;
  settings->OrderSupport[NEG_SAVEBITMAP_INDEX] = FALSE;
  settings->OrderSupport[NEG_GLYPH_INDEX_INDEX] = TRUE;
  settings->OrderSupport[NEG_FAST_INDEX_INDEX] = TRUE;
  settings->OrderSupport[NEG_FAST_GLYPH_INDEX] = FALSE;
  settings->OrderSupport[NEG_POLYGON_SC_INDEX] = FALSE;
  settings->OrderSupport[NEG_POLYGON_CB_INDEX] = FALSE;
  settings->OrderSupport[NEG_ELLIPSE_SC_INDEX] = FALSE;
  settings->OrderSupport[NEG_ELLIPSE_CB_INDEX] = FALSE;

  return TRUE;
}

static void
frdp_begin_paint (rdpContext *context)
{
  rdpGdi *gdi = context->gdi;

  gdi->primary->hdc->hwnd->invalid->null = 1;
  gdi->primary->hdc->hwnd->ninvalid = 0;
}

static void
frdp_end_paint (rdpContext *context)
{
  FrdpSessionPrivate *priv;
  FrdpSession *self = ((frdpContext *) context)->self;
  rdpGdi *gdi = context->gdi;
  gint x, y, w, h;

  if (gdi->primary->hdc->hwnd->invalid->null)
    return;

  x = gdi->primary->hdc->hwnd->invalid->x;
  y = gdi->primary->hdc->hwnd->invalid->y;
  w = gdi->primary->hdc->hwnd->invalid->w;
  h = gdi->primary->hdc->hwnd->invalid->h;

  priv = self->priv;

  /* TODO: scaling */
  gtk_widget_queue_draw_area (priv->display, x, y, w, h);
}

static gboolean
frdp_post_connect (freerdp *freerdp_session)
{
  gdi_init (freerdp_session, CLRBUF_24BPP, NULL);

  freerdp_session->update->BeginPaint = frdp_begin_paint;
  freerdp_session->update->EndPaint = frdp_end_paint;

  /* cairo create surface image for data */

  return TRUE;
}

static void
frdp_session_connect_thread (GTask        *task,
                             gpointer      source_object,
                             gpointer      task_data,
                             GCancellable *cancellable)
{
  FrdpSession *self = (FrdpSession*) source_object;
  gboolean result;

  result = freerdp_connect (self->priv->freerdp_session);

  g_task_return_boolean (task, result);
}

static void
frdp_session_get_property (GObject    *object,
                           guint       property_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  FrdpSession *self = (FrdpSession*) object;
  rdpSettings *settings = self->priv->freerdp_session->settings;

  switch (property_id)
    {
      case PROP_HOSTNAME:
        g_value_set_string (value, settings->ServerHostname);
        break;
      case PROP_PORT:
        g_value_set_uint (value, settings->ServerPort);
        break;
      case PROP_USERNAME:
        g_value_set_string (value, settings->Username);
        break;
      case PROP_PASSWORD:
        g_value_set_string (value, settings->Password);
        break;
      case PROP_DISPLAY:
        g_value_set_object (value, self->priv->display);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
frdp_session_set_property (GObject      *object,
                           guint         property_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  FrdpSession *self = (FrdpSession*) object;

  switch (property_id)
    {
      case PROP_HOSTNAME:
        frdp_session_set_hostname (self, g_value_get_string (value));
        break;
      case PROP_PORT:
        frdp_session_set_port (self, g_value_get_uint (value));
        break;
      case PROP_USERNAME:
        frdp_session_set_username (self, g_value_get_string (value));
        break;
      case PROP_PASSWORD:
        frdp_session_set_password (self, g_value_get_string (value));
        break;
      case PROP_DISPLAY:
        self->priv->display = g_value_get_object (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
frdp_session_finalize (GObject *object)
{
  /* TODO: free the world! */

  G_OBJECT_CLASS (frdp_session_parent_class)->finalize (object);
}

static void
frdp_session_class_init (FrdpSessionClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = frdp_session_finalize;
  gobject_class->get_property = frdp_session_get_property;
  gobject_class->set_property = frdp_session_set_property;

  g_object_class_install_property (gobject_class,
                                   PROP_HOSTNAME,
                                   g_param_spec_string ("hostname",
                                                        "hostname",
                                                        "hostname",
                                                        NULL,
                                                        G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_HOSTNAME,
                                   g_param_spec_uint ("port",
                                                      "port",
                                                      "port",
                                                       0, G_MAXUINT16, 3389,
                                                       G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_HOSTNAME,
                                   g_param_spec_string ("username",
                                                        "username",
                                                        "username",
                                                        NULL,
                                                        G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_HOSTNAME,
                                   g_param_spec_string ("password",
                                                        "password",
                                                        "password",
                                                        NULL,
                                                        G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_DISPLAY,
                                   g_param_spec_object ("display",
                                                        "display",
                                                        "display",
                                                        GTK_TYPE_WIDGET,
                                                        G_PARAM_READWRITE));
}

static void
frdp_session_init (FrdpSession *self)
{
  FrdpSessionPrivate *priv;

  self->priv = frdp_session_get_instance_private (self);
  priv = self->priv;

  /* Setup FreeRDP session */
  priv->freerdp_session = freerdp_new ();
  priv->freerdp_session->PreConnect = frdp_pre_connect;
  priv->freerdp_session->PostConnect = frdp_post_connect;
  priv->freerdp_session->Authenticate = frdp_authenticate;
  priv->freerdp_session->VerifyCertificate = frdp_certificate_verify;

  priv->freerdp_session->ContextSize = sizeof (frdpContext);

  freerdp_context_new (priv->freerdp_session);
  ((frdpContext *) priv->freerdp_session->context)->self = self;
}

FrdpSession*
frdp_session_new (FrdpDisplay *display)
{
  return g_object_new (FRDP_TYPE_SESSION,
                       "display", display,
                       NULL);
}

void
frdp_session_connect (FrdpSession         *self,
                      const gchar         *hostname,
                      guint                port,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
  GTask *task;

  frdp_session_set_hostname (self, hostname);
  frdp_session_set_port (self, port);

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_run_in_thread (task, frdp_session_connect_thread);

  g_object_unref (task);
}

gboolean
frdp_session_connect_finish (FrdpSession   *self,
                             GAsyncResult  *result,
                             GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}
