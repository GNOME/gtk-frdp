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

#include <errno.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <math.h>

#include "frdp-session.h"

#define SELECT_TIMEOUT 50

struct _FrdpSessionPrivate
{
  freerdp      *freerdp_session;

  GtkWidget    *display;
  cairo_surface_t *surface;
  gboolean scaling;
  double scale;

  guint update_id;

  gboolean is_connected;
};

G_DEFINE_TYPE_WITH_PRIVATE (FrdpSession, frdp_session, G_TYPE_OBJECT)

enum
{
  PROP_0 = 0,
  PROP_HOSTNAME,
  PROP_PORT,
  PROP_USERNAME,
  PROP_PASSWORD,
  PROP_DISPLAY,
  PROP_SCALING
};

enum
{
  RDP_CONNECTED,
  RDP_DISCONNECTED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

struct frdp_context
{
  rdpContext context;
  FrdpSession *self;
};
typedef struct frdp_context frdpContext;

static void
frdp_session_configure_event (GtkWidget *widget,
                              GdkEvent  *event,
                              gpointer   user_data)
{
  FrdpSession *self = (FrdpSession*) user_data;
  rdpSettings *settings = self->priv->freerdp_session->settings;
  gint width, height;

  if (self->priv->scaling) {
    width = gtk_widget_get_allocated_width (widget);
    height = gtk_widget_get_allocated_height (widget);

    if (width < height)
      self->priv->scale = (double) width / settings->DesktopWidth;
    else
      self->priv->scale = (double) height / settings->DesktopHeight;

    settings->DesktopScaleFactor = self->priv->scale;
  }
}

static void
frdp_session_set_scaling (FrdpSession *self,
                          gboolean     scaling)
{
  self->priv->scaling = scaling;

  frdp_session_configure_event (self->priv->display, NULL, self);
}

static gboolean
frdp_session_draw (GtkWidget *widget,
                   cairo_t   *cr,
                   gpointer   user_data)
{
  FrdpSession *self = (FrdpSession*) user_data;

  if (self->priv->scaling) {
      cairo_scale (cr, self->priv->scale, self->priv->scale);
  }
  cairo_set_source_surface (cr, self->priv->surface, 0, 0);
  cairo_paint (cr);

  return TRUE;
}

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

static guint
frdp_certificate_verify (freerdp     *freerdp_session,
                         const gchar *common_name,
                         const gchar* subject,
                         const gchar* issuer,
                         const gchar* fingerprint,
                         gboolean     host_mismatch)
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
  FrdpSession *self = ((frdpContext *) freerdp_session->context)->self;
  gboolean result;

  return frdp_display_authenticate (FRDP_DISPLAY (self->priv->display),
                                    username,
                                    password,
                                    domain);
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

static gboolean
frdp_begin_paint (rdpContext *context)
{
  rdpGdi *gdi = context->gdi;

  gdi->primary->hdc->hwnd->invalid->null = 1;
  gdi->primary->hdc->hwnd->ninvalid = 0;

  return TRUE;
}

static gboolean
frdp_end_paint (rdpContext *context)
{
  FrdpSessionPrivate *priv;
  FrdpSession *self = ((frdpContext *) context)->self;
  rdpGdi *gdi = context->gdi;
  gint x, y, w, h;
  gint pos_x, pos_y;

  if (gdi->primary->hdc->hwnd->invalid->null)
    return FALSE;

  x = gdi->primary->hdc->hwnd->invalid->x;
  y = gdi->primary->hdc->hwnd->invalid->y;
  w = gdi->primary->hdc->hwnd->invalid->w;
  h = gdi->primary->hdc->hwnd->invalid->h;

  priv = self->priv;

  if (priv->scaling) {
      pos_x = x * priv->scale;
      pos_y = y * priv->scale;
      gtk_widget_queue_draw_area (priv->display,
                                  floor (pos_x),
                                  floor (pos_y),
                                  ceil (pos_x + w * priv->scale) - floor (pos_x),
                                  ceil (pos_y + h * priv->scale) - floor (pos_y));
  } else {
    gtk_widget_queue_draw_area (priv->display, x, y, w, h);
  }

  return TRUE;
}

static gboolean
frdp_post_connect (freerdp *freerdp_session)
{
  FrdpSession *self = ((frdpContext *) freerdp_session->context)->self;
  rdpGdi *gdi;
  gint stride;

  gdi_init (freerdp_session, PIXEL_FORMAT_BGRA32);
  gdi = freerdp_session->context->gdi;

  freerdp_session->update->BeginPaint = frdp_begin_paint;
  freerdp_session->update->EndPaint = frdp_end_paint;

  stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, gdi->width);
  self->priv->surface =
      cairo_image_surface_create_for_data ((unsigned char*) gdi->primary_buffer,
                                           CAIRO_FORMAT_RGB24,
                                           gdi->width,
                                           gdi->height,
                                           stride);

  gtk_widget_queue_draw_area (self->priv->display,
                              0,
                              0,
                              gdi->width,
                              gdi->height);

  return TRUE;
}

static gboolean
idle_close (gpointer user_data)
{
  FrdpSession *self = (FrdpSession*) user_data;

  g_signal_emit (self, signals[RDP_DISCONNECTED], 0);

  return FALSE;
}

static gboolean
update (gpointer user_data)
{
  FrdpSessionPrivate *priv;
  struct timeval timeout;
  FrdpSession *self = (FrdpSession*) user_data;
  fd_set rfds_set, wfds_set;
  void *rfds[32], *wfds[32];
  gint rcount = 0, wcount = 0;
  gint fds, max_fds = 0;
  gint result;
  gint idx;

  memset (rfds, 0, sizeof (rfds));
  memset (wfds, 0, sizeof (wfds));

  priv = self->priv;

  if (!freerdp_get_fds (priv->freerdp_session, rfds, &rcount, wfds, &wcount)) {
      g_warning ("Failed to get FreeRDP file descriptor");
      return FALSE;
  }

  FD_ZERO (&rfds_set);
  FD_ZERO (&wfds_set);

  for (idx = 0; idx < rcount; idx++) {
    fds = (int)(long) (rfds[idx]);

    if (fds > max_fds)
      max_fds = fds;

    FD_SET (fds, &rfds_set);
  }

  if (max_fds == 0)
    return FALSE;

  timeout.tv_sec = 0;
  timeout.tv_usec = SELECT_TIMEOUT;

  result = select (max_fds + 1, &rfds_set, NULL, NULL, &timeout);
  if (result == -1) {
    if (!((errno == EAGAIN) || (errno == EWOULDBLOCK) ||
          (errno == EINPROGRESS) || (errno == EINTR))) {
      g_warning ("update: select failed");
      return FALSE;
    }
  }

  if (!freerdp_check_fds (priv->freerdp_session)) {
      g_warning ("Failed to check FreeRDP file descriptor");
      return FALSE;
  }

  if (freerdp_shall_disconnect (priv->freerdp_session)) {
      g_idle_add ((GSourceFunc) idle_close, self);

      return FALSE;
  }

  return TRUE;
}

static void
frdp_session_connect_thread (GTask        *task,
                             gpointer      source_object,
                             gpointer      task_data,
                             GCancellable *cancellable)
{
  FrdpSession *self = (FrdpSession*) source_object;

  self->priv->is_connected = freerdp_connect (self->priv->freerdp_session);

  if (self->priv->is_connected) {
    g_signal_connect (self->priv->display, "draw",
                      G_CALLBACK (frdp_session_draw), self);
    g_signal_connect (self->priv->display, "configure-event",
                      G_CALLBACK (frdp_session_configure_event), self);
    frdp_session_set_scaling (self, TRUE);

    self->priv->update_id = g_idle_add ((GSourceFunc) update, self);
  } else {
    freerdp_free (self->priv->freerdp_session);
  }

  g_task_return_boolean (task, self->priv->is_connected);
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
      case PROP_SCALING:
        g_value_set_boolean (value, self->priv->scaling);
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
      case PROP_SCALING:
        frdp_session_set_scaling (self, g_value_get_boolean (value));
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
frdp_session_finalize (GObject *object)
{
  FrdpSession *self = (FrdpSession*) object;
  /* TODO: free the world! */

  if (self->priv->freerdp_session) {
    freerdp_disconnect (self->priv->freerdp_session);
    freerdp_context_free (self->priv->freerdp_session);
    g_clear_pointer (&self->priv->freerdp_session, freerdp_free);
  }

  frdp_session_close (self);

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
                                   PROP_PORT,
                                   g_param_spec_uint ("port",
                                                      "port",
                                                      "port",
                                                       0, G_MAXUINT16, 3389,
                                                       G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_USERNAME,
                                   g_param_spec_string ("username",
                                                        "username",
                                                        "username",
                                                        NULL,
                                                        G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_PASSWORD,
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

  g_object_class_install_property (gobject_class,
                                   PROP_SCALING,
                                   g_param_spec_boolean ("scaling",
                                                         "scaling",
                                                         "scaling",
                                                         TRUE,
                                                         G_PARAM_READWRITE));

  signals[RDP_DISCONNECTED] = g_signal_new ("rdp-disconnected",
                                            FRDP_TYPE_SESSION,
                                            G_SIGNAL_RUN_FIRST,
                                            0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 0);
}

static void
frdp_session_init (FrdpSession *self)
{
  self->priv = frdp_session_get_instance_private (self);

  self->priv->is_connected = FALSE;
}

FrdpSession*
frdp_session_new (FrdpDisplay *display)
{
  gtk_widget_show (GTK_WIDGET (display));

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
  FrdpSessionPrivate *priv = self->priv;
  GTask *task;

  if (!self->priv->is_connected) {
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

gboolean
frdp_session_is_open (FrdpSession *self)
{
  return self->priv->is_connected;
}

void
frdp_session_close (FrdpSession *self)
{
  if (self->priv->update_id > 0) {
    g_source_remove (self->priv->update_id);
    self->priv->update_id = 0;
  }

  if (self->priv->freerdp_session != NULL) {
    gdi_free (self->priv->freerdp_session);

    self->priv->is_connected = FALSE;

    g_debug ("Closing RDP session");
  }
}

void
frdp_session_mouse_event (FrdpSession          *self,
                          FrdpMouseEvent        event,
                          guint16               x,
                          guint16               y)
{
  FrdpSessionPrivate *priv = self->priv;
  rdpInput *input;
  guint16 flags = 0;

  g_return_if_fail (priv->freerdp_session != NULL);

  if (event & FRDP_MOUSE_EVENT_MOVE)
    flags |= PTR_FLAGS_MOVE;
  if (event & FRDP_MOUSE_EVENT_DOWN)
    flags |= PTR_FLAGS_DOWN;
  if (event & FRDP_MOUSE_EVENT_WHEEL) {
    flags |= PTR_FLAGS_WHEEL;
    if (event & FRDP_MOUSE_EVENT_WHEEL_NEGATIVE)
      flags |= PTR_FLAGS_WHEEL_NEGATIVE | 0x0088;
    else
      flags |= 0x0078;
  }

  if (event & FRDP_MOUSE_EVENT_BUTTON1)
    flags |= PTR_FLAGS_BUTTON1;
  if (event & FRDP_MOUSE_EVENT_BUTTON2)
    flags |= PTR_FLAGS_BUTTON2;
  if (event & FRDP_MOUSE_EVENT_BUTTON3)
    flags |= PTR_FLAGS_BUTTON3;

  input = priv->freerdp_session->input;

  if (priv->scaling) {
    x = x / priv->scale;
    y = y / priv->scale;
  }

  if (flags != 0) {
    x = x < 0.0 ? 0.0 : x;
    y = y < 0.0 ? 0.0 : y;

    input->MouseEvent (input, flags, x, y);
  }
}

static unsigned char keycode_scancodes[] = {
   0,  0,  0,  0,  0,  0,  0, 28,
  29, 53, 55, 56,  0, 71, 72, 73,
  75, 77, 79, 80, 81, 82, 83,  0,
   0,  0,  0,  0,  0,  0, 69,  0,
   0,  0,  0,  0, 91, 92, 93,
};

static guint16
frdp_session_get_scancode_by_keycode (guint16 keycode)
{
  if (keycode < 8)
    return 0;
  else if (keycode < 97)
    return keycode - 8;
  else if (keycode < 97 + sizeof (keycode_scancodes))
    return keycode_scancodes[keycode - 97];
  else
    return 0;
}

void
frdp_session_send_key (FrdpSession  *self,
                       FrdpKeyEvent  event,
                       guint16       keycode)
{
  rdpInput *input = self->priv->freerdp_session->input;
  guint16 flags = 0;
  guint16 scancode =
      frdp_session_get_scancode_by_keycode (keycode);

  if (event == FRDP_KEY_EVENT_PRESS)
    flags |= KBD_FLAGS_DOWN;
  else
    flags |= KBD_FLAGS_RELEASE;

  input->KeyboardEvent (input, flags, scancode);
}
