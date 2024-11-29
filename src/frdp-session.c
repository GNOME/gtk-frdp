/* frdp-session.c
 *
 * Copyright (C) 2018 Felipe Borges <felipeborges@gnome.org>
 * Copyright (C) 2019 Armin Novak <akallabeth@posteo.net>
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

#include <errno.h>
#include <freerdp/input.h>
#include <freerdp/locale/keyboard.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/video.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/rdpei.h>
#include <freerdp/client/tsmf.h>
#include <freerdp/client/rail.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/client/encomsp.h>
#include <freerdp/client/disp.h>
#include <freerdp/client/geometry.h>
#include <freerdp/client/video.h>
#include <freerdp/channels/disp.h>
#include <gdk/gdk.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <math.h>

#include "frdp-session.h"
#include "frdp-context.h"
#include "frdp-channel-display-control.h"
#include "frdp-channel-clipboard.h"

#define SELECT_TIMEOUT 50
#define FRDP_CONNECTION_THREAD_MAX_ERRORS 10

#ifdef HAVE_FREERDP3
#define CONST_QUALIFIER const
#else
#define CONST_QUALIFIER
#endif

struct frdp_pointer
{
	rdpPointer pointer;
	cairo_surface_t *data;
};
typedef struct frdp_pointer frdpPointer;

struct _FrdpSessionPrivate
{
  freerdp      *freerdp_session;

  GtkWidget    *display;
  cairo_surface_t *surface;
  cairo_format_t cairo_format;
  gboolean scaling;
  double scale;
  double offset_x;
  double offset_y;

  guint update_id;

  gboolean is_connected;

  gchar *hostname;
  gchar *username;
  gchar *password;
  gchar *domain;
  guint  port;

  gboolean show_cursor;
  gboolean cursor_null;
  frdpPointer *cursor;

  /* Channels */
  FrdpChannelDisplayControl *display_control_channel;
  FrdpChannelClipboard      *clipboard_channel;
  gboolean                   monitor_layout_supported;

  GQueue *area_draw_queue;  /* elem: GdkRectangle */
  GMutex  area_draw_mutex;
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
  PROP_SCALING,
  PROP_MONITOR_LAYOUT_SUPPORTED,
  PROP_DOMAIN
};

enum
{
  RDP_ERROR,
  RDP_CONNECTED,
  RDP_DISCONNECTED,
  RDP_AUTH_FAILURE,
  RDP_CHANNEL_CONNECTED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

static void queue_draw_area (FrdpSession *self,
                             gint         x,
                             gint         y,
                             gint         width,
                             gint         height);

static void
frdp_session_update_mouse_pointer (FrdpSession  *self)
{
  FrdpSessionPrivate *priv = self->priv;
  GdkCursor *cursor;
  GdkDisplay *display;
  GdkWindow  *window;

  window = gtk_widget_get_window (priv->display);
  if (window == NULL)
    return;

  display = gtk_widget_get_display(priv->display);
  if (priv->show_cursor && priv->cursor_null) {
    cairo_surface_t *surface;
    cairo_t *cairo;

    /* Create a 1x1 image with transparent color */
    surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 1, 1);
    cairo = cairo_create (surface);
    cairo_set_source_rgba (cairo, 0.0, 0.0, 0.0, 0.0);
    cairo_set_line_width(cairo, 1);
    cairo_rectangle(cairo, 0, 0, 1, 1);
    cairo_fill (cairo);

    cursor =  gdk_cursor_new_from_surface (display, surface, 0, 0);
    cairo_surface_destroy (surface);
    cairo_destroy (cairo);
  } else if (!priv->show_cursor || !priv->cursor)
      /* No cursor set or none to show */
    cursor = gdk_cursor_new_from_name (display, "default");
  else {
    rdpPointer *pointer = &priv->cursor->pointer;
    double x = priv->cursor->pointer.xPos * priv->scale;
    double y = priv->cursor->pointer.yPos * priv->scale;
    double w = pointer->width * priv->scale;
    double h = pointer->height * priv->scale;
    cairo_surface_t *surface;
    cairo_t *cairo;

    if (!self->priv->scaling) {
      self->priv->scale = 1.0;
    }

    /* Scale the source image according to current settings. */
    surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
    cairo = cairo_create (surface);

    cairo_scale(cairo, self->priv->scale, self->priv->scale);
    cairo_set_source_surface (cairo, priv->cursor->data, 0, 0);
    cairo_paint (cairo);

    cairo_fill (cairo);
    cursor =  gdk_cursor_new_from_surface (display, surface, x, y);
    cairo_surface_destroy (surface);
    cairo_destroy (cairo);
  }

  gdk_window_set_cursor (window, cursor);
}

static guint32
frdp_session_get_best_color_depth (FrdpSession *self)
{
  GdkScreen *display;
  GdkVisual *visual;

  display = gdk_screen_get_default ();
  visual = gdk_screen_get_rgba_visual (display);

  return gdk_visual_get_depth (visual);
}

static void
create_cairo_surface (FrdpSession *self)
{
  FrdpSessionPrivate *priv = self->priv;
  rdpGdi             *gdi;
  gint                stride;

  if (priv->surface != NULL) {
    cairo_surface_mark_dirty (priv->surface);
    cairo_surface_destroy (priv->surface);
    self->priv->surface = NULL;
  }

  gdi = priv->freerdp_session->context->gdi;

  gtk_widget_set_size_request (priv->display,
                               gdi->width,
                               gdi->height);
  stride = cairo_format_stride_for_width (priv->cairo_format, gdi->width);
  self->priv->surface =
      cairo_image_surface_create_for_data ((unsigned char*) gdi->primary_buffer,
                                           priv->cairo_format,
                                           gdi->width,
                                           gdi->height,
                                           stride);
  cairo_surface_flush (priv->surface);
}

static gboolean
frdp_desktop_resize (rdpContext *context)
{
  FrdpSession *self = ((frdpContext *) context)->self;
  rdpGdi      *gdi = context->gdi;

  if (gdi_resize (gdi,
                  context->settings->DesktopWidth,
                  context->settings->DesktopHeight)) {
    create_cairo_surface (self);
    return TRUE;
  } else {
    return FALSE;
  }
}

static void
frdp_session_configure_event (GtkWidget *widget,
                              GdkEvent  *event,
                              gpointer   user_data)
{
  FrdpSession *self = (FrdpSession*) user_data;
  FrdpSessionPrivate *priv = self->priv;
  GtkWidget *scrolled;
  rdpSettings *settings;
  rdpGdi *gdi;
  double width, height, widget_ratio, server_ratio;
  gboolean allow_resize;

  if (priv->freerdp_session == NULL)
    return;

  gdi = priv->freerdp_session->context->gdi;
  if (priv->surface == NULL) {
    create_cairo_surface (self);
  }

  scrolled = gtk_widget_get_ancestor (widget, GTK_TYPE_SCROLLED_WINDOW);
  width = (double)gtk_widget_get_allocated_width (scrolled);
  height = (double)gtk_widget_get_allocated_height (scrolled);

  if (priv->freerdp_session->context->settings == NULL)
    return;

  settings = priv->freerdp_session->context->settings;

  g_object_get (G_OBJECT (widget), "allow-resize", &allow_resize, NULL);

  if (allow_resize) {
    if ((settings->DesktopWidth != gtk_widget_get_allocated_width (scrolled) ||
         settings->DesktopHeight != gtk_widget_get_allocated_height (scrolled)) &&
        priv->display_control_channel != NULL) {
      frdp_channel_display_control_resize_display (priv->display_control_channel,
                                                   width,
                                                   height);
    }
  } else {
    if (priv->scaling) {
        widget_ratio = height > 0 ? width / height : 1.0;
        server_ratio = settings->DesktopHeight > 0 ? (double) settings->DesktopWidth / settings->DesktopHeight : 1.0;

        if (widget_ratio > server_ratio)
          self->priv->scale = height / settings->DesktopHeight;
        else
          self->priv->scale = width / settings->DesktopWidth;

        self->priv->offset_x = (width - settings->DesktopWidth * self->priv->scale) / 2.0;
        self->priv->offset_y = (height - settings->DesktopHeight * self->priv->scale) / 2.0;
    } else {
      gtk_widget_set_size_request (priv->display, gdi->width, gdi->height);
    }
  }
}

static void
frdp_session_resize_supported_changed (FrdpDisplay *display,
                                       GParamSpec  *pspec,
                                       gpointer     user_data)
{
  FrdpSession        *self = user_data;
  FrdpSessionPrivate *priv = frdp_session_get_instance_private (self);
  GtkWidget          *scrolled;
  gboolean            resize_supported, allow_resize;
  gint                width, height;

  g_object_get (G_OBJECT (display),
                "resize-supported", &resize_supported,
                "allow-resize", &allow_resize,
                NULL);

  if (resize_supported && allow_resize)
    {
      scrolled = gtk_widget_get_ancestor (GTK_WIDGET (display), GTK_TYPE_SCROLLED_WINDOW);
      width = gtk_widget_get_allocated_width (scrolled);
      height = gtk_widget_get_allocated_height (scrolled);

      frdp_channel_display_control_resize_display (priv->display_control_channel,
                                                   width,
                                                   height);
    }
}

static void
frdp_session_set_scaling (FrdpSession *self,
                          gboolean     scaling)
{
  self->priv->scaling = scaling;
}

static gboolean
frdp_session_draw (GtkWidget *widget,
                   cairo_t   *cr,
                   gpointer   user_data)
{
  FrdpSession *self = (FrdpSession*) user_data;

  // Nothing to draw if disconnected
  if (!self->priv->is_connected)
    return FALSE;

  if (self->priv->surface == NULL ||
      (self->priv->freerdp_session->context->gdi->width != cairo_image_surface_get_width (self->priv->surface) ||
       self->priv->freerdp_session->context->gdi->height != cairo_image_surface_get_height (self->priv->surface))) {
    create_cairo_surface (self);
  }

  if (self->priv->scaling) {
      cairo_translate (cr, self->priv->offset_x, self->priv->offset_y);
      cairo_scale (cr, self->priv->scale, self->priv->scale);
  }

  cairo_set_source_surface (cr, self->priv->surface, 0, 0);
  cairo_paint (cr);

  frdp_display_set_scaling (FRDP_DISPLAY (self->priv->display), self->priv->scaling);

  return TRUE;
}

/*
 * Return 1 to accept and store a certificate, 2 to accept
 * a certificate only for this session, 0 otherwise.
 */
static guint
frdp_certificate_verify_ex (freerdp     *freerdp_session,
                            const gchar *host,
                            guint16      port,
                            const gchar *common_name,
                            const gchar *subject,
                            const gchar *issuer,
                            const gchar *fingerprint,
                            guint32      flags)
{
  FrdpSession *self = ((frdpContext *) freerdp_session->context)->self;

  return frdp_display_certificate_verify_ex (FRDP_DISPLAY (self->priv->display),
                                             host,
                                             port,
                                             common_name,
                                             subject,
                                             issuer,
                                             fingerprint,
                                             flags);
}

static guint
frdp_changed_certificate_verify_ex (freerdp     *freerdp_session,
                                    const gchar *host,
                                    guint16      port,
                                    const gchar *common_name,
                                    const gchar *subject,
                                    const gchar *issuer,
                                    const gchar *fingerprint,
                                    const gchar *old_subject,
                                    const gchar *old_issuer,
                                    const gchar *old_fingerprint,
                                    guint32      flags)
{
  FrdpSession *self = ((frdpContext *) freerdp_session->context)->self;

  return frdp_display_certificate_change_verify_ex (FRDP_DISPLAY (self->priv->display),
                                                    host,
                                                    port,
                                                    common_name,
                                                    subject,
                                                    issuer,
                                                    fingerprint,
                                                    old_subject,
                                                    old_issuer,
                                                    old_fingerprint,
                                                    flags);
}

static gboolean
frdp_authenticate (freerdp  *freerdp_session,
                   gchar   **username,
                   gchar   **password,
                   gchar   **domain)
{
  FrdpSession *self = ((frdpContext *) freerdp_session->context)->self;

  return frdp_display_authenticate (FRDP_DISPLAY (self->priv->display),
                                    username,
                                    password,
                                    domain);
}

static void
caps_set (FrdpChannelDisplayControl *channel,
          gpointer                   user_data)
{
  FrdpSession *session = user_data;

  g_object_set (G_OBJECT (session), "monitor-layout-supported", TRUE, NULL);
}

static void
frdp_on_channel_connected_event_handler (void                                      *context,
                                         CONST_QUALIFIER ChannelConnectedEventArgs *e)
{
  frdpContext        *ctx = (frdpContext *) context;
  FrdpSession        *session = ctx->self;
  FrdpSessionPrivate *priv = frdp_session_get_instance_private (session);

  if (strcmp (e->name, RDPEI_DVC_CHANNEL_NAME) == 0) {
    // TODO Touch input redirection
  } else if (strcmp (e->name, DISP_DVC_CHANNEL_NAME) == 0) {
    g_clear_object (&priv->display_control_channel);

    priv->display_control_channel = g_object_new (FRDP_TYPE_CHANNEL_DISPLAY_CONTROL,
                                                  "session", session,
                                                  "display-client-context", (DispClientContext *) e->pInterface,
                                                  NULL);
    g_signal_connect (priv->display_control_channel, "caps-set", G_CALLBACK (caps_set), session);
  } else if (strcmp (e->name, TSMF_DVC_CHANNEL_NAME) == 0) {
    // TODO Old windows 7 multimedia redirection
  } else if (strcmp (e->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
    gdi_graphics_pipeline_init (ctx->context.gdi, (RdpgfxClientContext *) e->pInterface);
  } else if (strcmp (e->name, RAIL_SVC_CHANNEL_NAME) == 0) {
    // TODO Remote application
  } else if (strcmp (e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
    g_clear_object (&priv->clipboard_channel);

    priv->clipboard_channel = g_object_new (FRDP_TYPE_CHANNEL_CLIPBOARD,
                                            "session", session,
                                            "cliprdr-client-context", (CliprdrClientContext *) e->pInterface,
                                            NULL);
  } else if (strcmp (e->name, ENCOMSP_SVC_CHANNEL_NAME) == 0) {
    // TODO Multiparty channel
  } else if (strcmp (e->name, GEOMETRY_DVC_CHANNEL_NAME) == 0) {
    gdi_video_geometry_init (ctx->context.gdi, (GeometryClientContext *) e->pInterface);
  } else if (strcmp (e->name, VIDEO_CONTROL_DVC_CHANNEL_NAME) == 0) {
    gdi_video_control_init (ctx->context.gdi, (VideoClientContext *) e->pInterface);
  } else if (strcmp (e->name, VIDEO_DATA_DVC_CHANNEL_NAME) == 0) {
    gdi_video_data_init (ctx->context.gdi, (VideoClientContext *) e->pInterface);
  }
}

static void
frdp_on_channel_disconnected_event_handler (void                                         *context,
                                            CONST_QUALIFIER ChannelDisconnectedEventArgs *e)
{
  frdpContext        *ctx = (frdpContext *) context;
  FrdpSession        *session = ctx->self;
  FrdpSessionPrivate *priv = frdp_session_get_instance_private (session);

  if (strcmp (e->name, RDPEI_DVC_CHANNEL_NAME) == 0) {
    // TODO Touch input redirection
  } else if (strcmp (e->name, DISP_DVC_CHANNEL_NAME) == 0) {
    g_clear_object (&priv->display_control_channel);
  } else if (strcmp (e->name, TSMF_DVC_CHANNEL_NAME) == 0) {
    // TODO Old windows 7 multimedia redirection
  } else if (strcmp (e->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
    gdi_graphics_pipeline_uninit (ctx->context.gdi, (RdpgfxClientContext *) e->pInterface);
  } else if (strcmp (e->name, RAIL_SVC_CHANNEL_NAME) == 0) {
    // TODO Remote application
  } else if (strcmp (e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
    g_clear_object (&priv->clipboard_channel);
  } else if (strcmp (e->name, ENCOMSP_SVC_CHANNEL_NAME) == 0) {
    // TODO Multiparty channel
  } else if (strcmp (e->name, GEOMETRY_DVC_CHANNEL_NAME) == 0) {
    gdi_video_geometry_uninit (ctx->context.gdi, (GeometryClientContext *) e->pInterface);
  } else if (strcmp (e->name, VIDEO_CONTROL_DVC_CHANNEL_NAME) == 0) {
    gdi_video_control_uninit (ctx->context.gdi, (VideoClientContext *) e->pInterface);
  } else if (strcmp (e->name, VIDEO_DATA_DVC_CHANNEL_NAME) == 0) {
    gdi_video_data_uninit (ctx->context.gdi, (VideoClientContext *) e->pInterface);
  }
}

static gboolean
frdp_load_channels (freerdp *instance)
{
  if (!freerdp_client_load_addins (instance->context->channels, instance->context->settings))
    return FALSE;

  return TRUE;
}

static gboolean
frdp_pre_connect (freerdp *freerdp_session)
{
  rdpSettings *settings = freerdp_session->context->settings;
  rdpContext *context = freerdp_session->context;

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

  PubSub_SubscribeChannelConnected (context->pubSub,
                                    frdp_on_channel_connected_event_handler);
  PubSub_SubscribeChannelDisconnected (context->pubSub,
                                       frdp_on_channel_disconnected_event_handler);

#ifndef HAVE_FREERDP3
  return frdp_load_channels (freerdp_session);
#else
  return TRUE;
#endif
}

static gboolean
frdp_begin_paint (rdpContext *context)
{
  rdpGdi *gdi = context->gdi;

  gdi->primary->hdc->hwnd->invalid->null = 1;
  gdi->primary->hdc->hwnd->ninvalid = 0;

  return TRUE;
}

static void
queue_draw_area (FrdpSession *self,
                 gint x,
                 gint y,
                 gint width,
                 gint height)
{
  FrdpSessionPrivate *priv = self->priv;
  GdkRectangle       *rectangle;

  rectangle = g_new (GdkRectangle, 1);
  rectangle->x = x;
  rectangle->y = y;
  rectangle->width = width;
  rectangle->height = height;

  g_mutex_lock (&priv->area_draw_mutex);

  if (priv->area_draw_queue != NULL)
    g_queue_push_tail (priv->area_draw_queue, rectangle);

  g_mutex_unlock (&priv->area_draw_mutex);
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
    return TRUE;

  x = gdi->primary->hdc->hwnd->invalid->x;
  y = gdi->primary->hdc->hwnd->invalid->y;
  w = gdi->primary->hdc->hwnd->invalid->w;
  h = gdi->primary->hdc->hwnd->invalid->h;

  priv = self->priv;

  if (priv->scaling) {
    pos_x = self->priv->offset_x + x * priv->scale;
    pos_y = self->priv->offset_y + y * priv->scale;
    queue_draw_area (self,
                     floor (pos_x),
                     floor (pos_y),
                     ceil (pos_x + w * priv->scale) - floor (pos_x),
                     ceil (pos_y + h * priv->scale) - floor (pos_y));
  } else {
    queue_draw_area (self, x, y, w, h);
  }

  return TRUE;
}

static gboolean
frdp_post_connect (freerdp *freerdp_session)
{
  rdpSettings *settings;
  rdpContext *context;
  FrdpSession *self = ((frdpContext *) freerdp_session->context)->self;
  guint32 color_format;
  ResizeWindowEventArgs e;

  context = freerdp_session->context;
  settings = context->settings;
  switch (frdp_session_get_best_color_depth (self)) {
    case 32:
      color_format = PIXEL_FORMAT_BGRX32;
      self->priv->cairo_format = CAIRO_FORMAT_ARGB32;
      break;
    case 24:
      color_format = PIXEL_FORMAT_BGRX32;
      self->priv->cairo_format = CAIRO_FORMAT_RGB24;
      break;
    case 16:
    case 15:
      color_format = PIXEL_FORMAT_BGR16;
      self->priv->cairo_format = CAIRO_FORMAT_RGB16_565;
      break;
    default:
      color_format = PIXEL_FORMAT_BGRX32;
      self->priv->cairo_format = CAIRO_FORMAT_RGB16_565;
      break;
  }

  gdi_init (freerdp_session, color_format);

  freerdp_session->context->update->BeginPaint = frdp_begin_paint;
  freerdp_session->context->update->EndPaint = frdp_end_paint;
  freerdp_session->context->update->DesktopResize = frdp_desktop_resize;

  EventArgsInit(&e, "frdp");
	e.width = settings->DesktopWidth;
	e.height = settings->DesktopHeight;
	PubSub_OnResizeWindow(context->pubSub, freerdp_session->context, &e);

  create_cairo_surface (self);

  return TRUE;
}

static void
frdp_post_disconnect (freerdp *instance)
{
  FrdpSession *self;
  rdpContext  *context;

  if (!instance || !instance->context)
    return;

  self = ((frdpContext *) instance->context)->self;

  g_signal_handlers_disconnect_by_func (self->priv->display, G_CALLBACK (frdp_session_draw), self);
  g_signal_handlers_disconnect_by_func (self->priv->display, G_CALLBACK (frdp_session_configure_event), self);
  g_signal_handlers_disconnect_by_func (self->priv->display, G_CALLBACK (frdp_session_resize_supported_changed), self);

  context = instance->context;
  PubSub_UnsubscribeChannelConnected (context->pubSub,
                                      frdp_on_channel_connected_event_handler);
  PubSub_UnsubscribeChannelDisconnected (context->pubSub,
                                         frdp_on_channel_disconnected_event_handler);
  gdi_free (instance);
}

static gboolean
idle_close (gpointer user_data)
{
  FrdpSession *self = (FrdpSession*) user_data;

  self->priv->is_connected = FALSE;

  if (self->priv->update_id > 0) {
    g_source_remove (self->priv->update_id);
    self->priv->update_id = 0;
  }

  g_mutex_lock (&self->priv->area_draw_mutex);
  g_queue_clear_full (self->priv->area_draw_queue, g_free);
  g_mutex_unlock (&self->priv->area_draw_mutex);
  g_mutex_clear (&self->priv->area_draw_mutex);

  if (self->priv->freerdp_session != NULL) {
    freerdp_disconnect (self->priv->freerdp_session);
    g_clear_pointer (&self->priv->freerdp_session, freerdp_free);
  }

  g_signal_emit (self, signals[RDP_DISCONNECTED], 0);
  g_debug ("RDP client disconnected");

  return FALSE;
}

static gboolean
update (gpointer user_data)
{
  DWORD status;
  HANDLE handles[64];
  DWORD usedHandles;
  FrdpSessionPrivate *priv;
  FrdpSession *self = (FrdpSession*) user_data;
  GdkRectangle *rectangle;

  priv = self->priv;

  g_mutex_lock (&priv->area_draw_mutex);

  while (priv->area_draw_queue != NULL && !g_queue_is_empty (priv->area_draw_queue)) {
      rectangle = g_queue_pop_head (priv->area_draw_queue);
      gtk_widget_queue_draw_area (priv->display, rectangle->x, rectangle->y, rectangle->width, rectangle->height);
      g_free (rectangle);
  }

  g_mutex_unlock (&priv->area_draw_mutex);

  if (freerdp_shall_disconnect (priv->freerdp_session)) {
      priv->update_id = 0;
      g_idle_add ((GSourceFunc) idle_close, self);

      return FALSE;
  }

  usedHandles = freerdp_get_event_handles (priv->freerdp_session->context,
                                           handles, ARRAYSIZE(handles));
  if (usedHandles == 0) {
      g_warning ("Failed to get FreeRDP event handle");
      priv->update_id = 0;
      return FALSE;
  }

  status = WaitForMultipleObjects (usedHandles, handles, FALSE, SELECT_TIMEOUT);
  if (status == WAIT_TIMEOUT)
    return TRUE;
  if (status == WAIT_FAILED) {
    priv->update_id = 0;
    return FALSE;
  }

  if (!freerdp_check_event_handles (priv->freerdp_session->context)) {
    if (freerdp_get_last_error(priv->freerdp_session->context) == FREERDP_ERROR_SUCCESS) {
      g_warning ("Failed to check FreeRDP file descriptor");
    }

    return TRUE;
  }

  return TRUE;
}

/*
 *  The local layout string is taken from org.gnome.desktop.input-sources.mru-sources
 *  after adding it among available sources and setting it active.
 *  Most of the national layout has been assigned according to default keyboard
 *  for given nation so these can be inaccurate.
 *  Non-standard layouts has to be checked directly by comparing them
 *  (e.g. against layouts on https://kbdlayout.info/).
 */
static const struct {
  const char  *local_layout;
  const guint  freerdp_layout;
} keyboard_layouts[] = {
  { "ara", KBD_ARABIC_101 },
  { "bg", KBD_BULGARIAN },
  { "cz", KBD_CZECH },
  { "da", KBD_DANISH },
  { "de", KBD_GERMAN },
  { "gr", KBD_GREEK },
  { "us", KBD_US },
  { "es", KBD_SPANISH },
  { "fi", KBD_FINNISH },
  { "fr", KBD_FRENCH },
  { "il", KBD_HEBREW },
  { "hu", KBD_HUNGARIAN },
  { "is", KBD_ICELANDIC },
  { "it", KBD_ITALIAN },
  { "jp", KBD_JAPANESE },
  { "kr", KBD_KOREAN },
  { "nl", KBD_DUTCH },
  { "no", KBD_NORWEGIAN },
  { "pl+dvp", KBD_POLISH_PROGRAMMERS },
  { "pt", KBD_PORTUGUESE_BRAZILIAN_ABNT },
  { "ro", KBD_ROMANIAN },
  { "ru", KBD_RUSSIAN },
  { "hr", KBD_CROATIAN },
  { "sk", KBD_SLOVAK },
  { "al", KBD_ALBANIAN },
  { "se", KBD_SWEDISH },
  { "tr", KBD_TURKISH_Q },
  { "pk+urd-nla", KBD_URDU },
  { "ua", KBD_UKRAINIAN },
  { "by", KBD_BELARUSIAN },
  { "si", KBD_SLOVENIAN },
  { "ee", KBD_ESTONIAN },
  { "lv", KBD_LATVIAN },
  { "lt+ibm", KBD_LITHUANIAN_IBM },
  { "vn", KBD_VIETNAMESE },
  { "am+eastern", KBD_ARMENIAN_EASTERN },
  { "mk", KBD_FYRO_MACEDONIAN },
  { "ge", KBD_GEORGIAN },
  { "fo", KBD_FAEROESE },
  { "no+smi", KBD_NORWEGIAN_WITH_SAMI },
  { "kz", KBD_KAZAKH },
  { "gb+extd", KBD_UNITED_KINGDOM_EXTENDED },
  { "sy+syc", KBD_SYRIAC },
  { "af+ps-olpc", KBD_PASHTO },
  { "m17n:dv:phonetic", KBD_DIVEHI_PHONETIC },
  { "nz+mao", KBD_MAORI },
  { "ch", KBD_SWISS_GERMAN },
  { "gb", KBD_UNITED_KINGDOM },
  { "latam", KBD_LATIN_AMERICAN },
  { "be", KBD_BELGIAN_FRENCH },
  { "pt", KBD_PORTUGUESE },
  { "ca+fr-legacy", KBD_CANADIAN_FRENCH_LEGACY },
  { "ca", KBD_CANADIAN_FRENCH },
  { "ba", KBD_BOSNIAN },
  { "cz+qwerty", KBD_CZECH_QWERTY },
  { "de+T3", KBD_GERMAN_IBM },
  { "us+dvorak", KBD_UNITED_STATES_DVORAK },
  { "it+ibm", KBD_ITALIAN_142 },
  { "pl+qwertz", KBD_POLISH_214 },
  { "pt", KBD_PORTUGUESE_BRAZILIAN_ABNT2 },
  { "sk+qwerty", KBD_SLOVAK_QWERTY },
  { "th+pat", KBD_THAI_PATTACHOTE },
  { "tr+f", KBD_TURKISH_F },
  { "lt", KBD_LITHUANIAN },
  { "am+western", KBD_ARMENIAN_WESTERN },
  { "be+iso-alternate", KBD_BELGIAN_COMMA },
  { "gb+gla", KBD_GAELIC },
  { "us+intl", KBD_UNITED_STATES_INTERNATIONAL },
  { "us+dvorak-l", KBD_UNITED_STATES_DVORAK_FOR_LEFT_HAND },
  { "us+dvorak-r", KBD_UNITED_STATES_DVORAK_FOR_RIGHT_HAND },
  { "us+dvp", KBD_UNITED_STATES_DVORAK_PROGRAMMER },
  { "gr+polytonic", KBD_GREEK_POLYTONIC },
  { "fr+bepo", KBD_FRENCH_BEPO },
  { "de+neo", KBD_GERMAN_NEO },

  /* These need to be determined yet. */

  { "", KBD_CHINESE_TRADITIONAL_US },
  { "", KBD_THAI_KEDMANEE },
  { "", KBD_FARSI },
  { "", KBD_AZERI_LATIN },
  { "", KBD_DEVANAGARI_INSCRIPT },
  { "", KBD_MALTESE_47_KEY },
  { "", KBD_KYRGYZ_CYRILLIC },
  { "", KBD_TATAR },
  { "", KBD_BENGALI },
  { "", KBD_PUNJABI },
  { "", KBD_GUJARATI },
  { "", KBD_TAMIL },
  { "", KBD_TELUGU },
  { "", KBD_KANNADA },
  { "", KBD_MALAYALAM },
  { "", KBD_MARATHI },
  { "", KBD_MONGOLIAN_CYRILLIC },
  { "", KBD_NEPALI },
  { "", KBD_LUXEMBOURGISH },
  { "", KBD_CHINESE_SIMPLIFIED_US },
  { "", KBD_BELGIAN_PERIOD },
  { "", KBD_SERBIAN_LATIN },
  { "", KBD_AZERI_CYRILLIC },
  { "", KBD_SWEDISH_WITH_SAMI },
  { "", KBD_UZBEK_CYRILLIC },
  { "", KBD_INUKTITUT_LATIN },
  { "", KBD_SERBIAN_CYRILLIC },
  { "", KBD_SWISS_FRENCH },
  { "", KBD_IRISH },
  { "", KBD_BOSNIAN_CYRILLIC },
  { "", KBD_ARABIC_102 },
  { "", KBD_BULGARIAN_LATIN },
  { "", KBD_GREEK_220 },
  { "", KBD_SPANISH_VARIATION },
  { "", KBD_HUNGARIAN_101_KEY },
  { "", KBD_RUSSIAN_TYPEWRITER },
  { "", KBD_LATVIAN_QWERTY },
  { "", KBD_HINDI_TRADITIONAL },
  { "", KBD_MALTESE_48_KEY },
  { "", KBD_SAMI_EXTENDED_NORWAY },
  { "", KBD_BENGALI_INSCRIPT },
  { "", KBD_SYRIAC_PHONETIC },
  { "", KBD_DIVEHI_TYPEWRITER },
  { "", KBD_FINNISH_WITH_SAMI },
  { "", KBD_CANADIAN_MULTILINGUAL_STANDARD },
  { "", KBD_ARABIC_102_AZERTY },
  { "", KBD_CZECH_PROGRAMMERS },
  { "", KBD_GREEK_319 },
  { "", KBD_THAI_KEDMANEE_NON_SHIFTLOCK },
  { "", KBD_SAMI_EXTENDED_FINLAND_SWEDEN },
  { "", KBD_GREEK_220_LATIN },
  { "", KBD_THAI_PATTACHOTE_NON_SHIFTLOCK },
  { "", KBD_GREEK_319_LATIN },
  { "", KBD_GREEK_LATIN },
  { "", KBD_US_ENGLISH_TABLE_FOR_IBM_ARABIC_238_L }

/*
  These are available in FreeRDP 3:
  { "", KBD_ROMANIAN_STANDARD },
  { "", KBD_GEORGIAN_QUERTY },
  { "", KBD_KHMER },
  { "", KBD_RUSSIAN_PHONETIC },
  { "", KBD_BANGLA },
  { "", KBD_BULGARIAN_PHONETIC },
  { "", KBD_PERSIAN },
*/
};

static void
frdp_session_set_current_keyboard_layout (FrdpSession *self) {
  GSettingsSchemaSource *source;
  FrdpSessionPrivate    *priv = self->priv;
  GSettingsSchema       *schema;
  rdpSettings           *settings;
  const gchar           *source_type = NULL, *layout = NULL;
  GSettings             *gsettings;
  GVariant              *sources;
  gboolean               keyboard_layout_set = FALSE;
  guint                  i;

  settings = priv->freerdp_session->context->settings;

  source = g_settings_schema_source_get_default ();
  if (source != NULL) {
    schema = g_settings_schema_source_lookup (source, "org.gnome.desktop.input-sources", TRUE);
    if (schema != NULL) {
      gsettings = g_settings_new (g_settings_schema_get_id (schema));
      sources = g_settings_get_value (gsettings, "mru-sources");

      if (g_variant_n_children (sources) >= 1)
        g_variant_get_child (sources, 0, "(&s&s)", &source_type, &layout);

      if (layout != NULL) {
        for (i = 0; i < G_N_ELEMENTS (keyboard_layouts); i++) {
          if (g_strcmp0 (layout, keyboard_layouts[i].local_layout) == 0) {
#ifdef HAVE_FREERDP_3_11_0
            settings->KeyboardLayout = keyboard_layouts[i].freerdp_layout;
#else
            settings->KeyboardLayout = freerdp_keyboard_init (keyboard_layouts[i].freerdp_layout);
#endif
            keyboard_layout_set = TRUE;
            break;
          }
        }
      }

      g_variant_unref (sources);
      g_object_unref (gsettings);
      g_settings_schema_unref (schema);
    }
  }

#ifndef HAVE_FREERDP_3_11_0
  if (!keyboard_layout_set)
    settings->KeyboardLayout = freerdp_keyboard_init (0);
#endif
}

static void
frdp_session_init_freerdp (FrdpSession *self)
{
  CONST_QUALIFIER gchar *collections[] = { "disp" };
  FrdpSessionPrivate    *priv = self->priv;
  rdpSettings           *settings;
  gchar                 *build_options;
  int                    count = 1;

  /* Setup FreeRDP session */
  priv->freerdp_session = freerdp_new ();
  priv->freerdp_session->PreConnect = frdp_pre_connect;
  priv->freerdp_session->PostConnect = frdp_post_connect;
  priv->freerdp_session->PostDisconnect = frdp_post_disconnect;
  priv->freerdp_session->Authenticate = frdp_authenticate;
  priv->freerdp_session->VerifyCertificateEx = frdp_certificate_verify_ex;
  priv->freerdp_session->VerifyChangedCertificateEx = frdp_changed_certificate_verify_ex;
#ifdef HAVE_FREERDP3
  priv->freerdp_session->LoadChannels = frdp_load_channels;
#endif

  priv->freerdp_session->ContextSize = sizeof (frdpContext);

  freerdp_context_new (priv->freerdp_session);
  ((frdpContext *) priv->freerdp_session->context)->self = self;

  settings = priv->freerdp_session->context->settings;

  settings->ServerHostname = g_strdup (priv->hostname);
  settings->ServerPort = priv->port;
  settings->Username = g_strdup (priv->username);
  settings->Password = g_strdup (priv->password);
  settings->Domain = g_strdup (priv->domain);

  settings->AllowFontSmoothing = TRUE;
  settings->AllowUnanouncedOrdersFromServer = TRUE;

  /* Security settings */
  settings->RdpSecurity = TRUE;
  settings->TlsSecurity = TRUE;
  settings->NlaSecurity = TRUE;
  settings->EncryptionMethods = ENCRYPTION_METHOD_40BIT | ENCRYPTION_METHOD_128BIT | ENCRYPTION_METHOD_FIPS;
  settings->EncryptionLevel = ENCRYPTION_LEVEL_CLIENT_COMPATIBLE;
  settings->UseRdpSecurityLayer = FALSE;

  settings->NegotiateSecurityLayer = TRUE;

  settings->DesktopResize = TRUE;
  settings->DynamicResolutionUpdate = TRUE;
  settings->SupportDisplayControl = TRUE;
  settings->RemoteFxCodec = TRUE;
  settings->ColorDepth = 32;
  settings->RedirectClipboard = TRUE;
  settings->SupportGraphicsPipeline = TRUE;

  freerdp_client_add_dynamic_channel (settings, count, collections);

  build_options = g_ascii_strup (freerdp_get_build_config (), -1);
  if (g_strrstr (build_options, "WITH_GFX_H264=ON") != NULL) {
    settings->GfxH264 = TRUE;
    settings->GfxAVC444 = TRUE;
  } else {
    settings->GfxH264 = FALSE;
    settings->GfxAVC444 = FALSE;
  }
  g_free (build_options);

  frdp_session_set_current_keyboard_layout (self);

  freerdp_register_addin_provider(freerdp_channels_load_static_addin_entry, 0);
}

static void
frdp_session_connect_thread (GTask        *task,
                             gpointer      source_object,
                             gpointer      task_data,
                             GCancellable *cancellable)
{
  FrdpSession *self = (FrdpSession*) source_object;

  frdp_session_init_freerdp (self);

  self->priv->is_connected = freerdp_connect (self->priv->freerdp_session);
  if (!self->priv->is_connected) {
    guint32 error_code;

    error_code = freerdp_get_last_error (self->priv->freerdp_session->context);
    switch (error_code) {
        case FREERDP_ERROR_AUTHENTICATION_FAILED:
        case FREERDP_ERROR_CONNECT_FAILED:
        case FREERDP_ERROR_SERVER_DENIED_CONNECTION:
        case FREERDP_ERROR_CONNECT_NO_OR_MISSING_CREDENTIALS:
        case FREERDP_ERROR_CONNECT_LOGON_FAILURE:
        case STATUS_LOGON_FAILURE:
        case STATUS_PASSWORD_EXPIRED:
        case FREERDP_ERROR_CONNECT_ACCOUNT_EXPIRED:
        case FREERDP_ERROR_CONNECT_TRANSPORT_FAILED:
        case ERRCONNECT_CONNECT_TRANSPORT_FAILED:
        case FREERDP_ERROR_TLS_CONNECT_FAILED:
        case FREERDP_ERROR_DNS_NAME_NOT_FOUND:
            g_signal_emit (self,
                           signals[RDP_AUTH_FAILURE], 0,
                           freerdp_get_last_error_string (error_code));

            g_warning ("Failed to connect RDP host with error '%s'",
                       freerdp_get_last_error_string (error_code));
            break;

        default:
            g_signal_emit (self,
                           signals[RDP_ERROR], 0,
                           freerdp_get_last_error_string (error_code));

            g_warning ("Unexpected RDP error: '%s'",
                       freerdp_get_last_error_string (error_code));
            break;
    }

    g_idle_add ((GSourceFunc) idle_close, self);
    g_task_return_boolean (task, FALSE);

    return;
  }

  gtk_widget_realize (self->priv->display);
  create_cairo_surface (self);
  g_signal_connect (self->priv->display, "draw",
                    G_CALLBACK (frdp_session_draw), self);
  g_signal_connect (self->priv->display, "configure-event",
                    G_CALLBACK (frdp_session_configure_event), self);
  g_signal_connect (self->priv->display, "notify::resize-supported",
                    G_CALLBACK (frdp_session_resize_supported_changed), self);

  self->priv->update_id = g_idle_add ((GSourceFunc) update, self);

  g_task_return_boolean (task, TRUE);
}

static void
frdp_session_get_property (GObject    *object,
                           guint       property_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  FrdpSession *self = (FrdpSession*) object;
  rdpSettings *settings = self->priv->freerdp_session->context->settings;

  switch (property_id)
    {
      case PROP_HOSTNAME:
        g_value_set_string (value, self->priv->hostname);
        break;
      case PROP_PORT:
        g_value_set_uint (value, self->priv->port);
        break;
      case PROP_USERNAME:
        g_value_set_string (value, self->priv->username);
        break;
      case PROP_PASSWORD:
        g_value_set_string (value, self->priv->password);
        break;
      case PROP_DOMAIN:
        g_value_set_string (value, self->priv->domain);
        break;
      case PROP_DISPLAY:
        g_value_set_object (value, self->priv->display);
        break;
      case PROP_SCALING:
        g_value_set_boolean (value, self->priv->scaling);
        break;
      case PROP_MONITOR_LAYOUT_SUPPORTED:
        g_value_set_boolean (value, self->priv->monitor_layout_supported);
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
        g_free (self->priv->hostname);
        self->priv->hostname = g_value_dup_string (value);
        break;
      case PROP_PORT:
        self->priv->port = g_value_get_uint (value);
        break;
      case PROP_USERNAME:
        g_free (self->priv->username);
        self->priv->username = g_value_dup_string (value);
        break;
      case PROP_PASSWORD:
        g_free (self->priv->password);
        self->priv->password = g_value_dup_string (value);
        break;
      case PROP_DOMAIN:
        g_free (self->priv->domain);
        self->priv->domain = g_value_dup_string (value);
        break;
      case PROP_DISPLAY:
        self->priv->display = g_value_get_object (value);
        break;
      case PROP_SCALING:
        frdp_session_set_scaling (self, g_value_get_boolean (value));
        break;
      case PROP_MONITOR_LAYOUT_SUPPORTED:
        self->priv->monitor_layout_supported = g_value_get_boolean (value);
        g_object_notify (G_OBJECT (self), "monitor-layout-supported");
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

  idle_close (self);

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
                                   PROP_DOMAIN,
                                   g_param_spec_string ("domain",
                                                        "domain",
                                                        "domain",
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

  g_object_class_install_property (gobject_class,
                                   PROP_MONITOR_LAYOUT_SUPPORTED,
                                   g_param_spec_boolean ("monitor-layout-supported",
                                                         "monitor-layout-supported",
                                                         "monitor-layout-supported",
                                                         FALSE,
                                                         G_PARAM_READWRITE));

  signals[RDP_ERROR] = g_signal_new ("rdp-error",
                                     FRDP_TYPE_SESSION,
                                     G_SIGNAL_RUN_FIRST,
                                     0, NULL, NULL, NULL,
                                     G_TYPE_NONE, 1,
                                     G_TYPE_STRING);

  signals[RDP_CONNECTED] = g_signal_new ("rdp-connected",
                                         FRDP_TYPE_SESSION,
                                         G_SIGNAL_RUN_FIRST,
                                         0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);

  signals[RDP_DISCONNECTED] = g_signal_new ("rdp-disconnected",
                                            FRDP_TYPE_SESSION,
                                            G_SIGNAL_RUN_FIRST,
                                            0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 0);

  signals[RDP_AUTH_FAILURE] = g_signal_new ("rdp-auth-failure",
                                            FRDP_TYPE_SESSION,
                                            G_SIGNAL_RUN_FIRST,
                                            0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 1,
                                            G_TYPE_STRING);
}

static void
frdp_session_init (FrdpSession *self)
{
  self->priv = frdp_session_get_instance_private (self);

  g_mutex_init (&self->priv->area_draw_mutex);
  self->priv->area_draw_queue = g_queue_new ();

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
  GTask *task;

  self->priv->hostname = g_strdup (hostname);
  self->priv->port = port;

  task = g_task_new (self, cancellable, callback, user_data);
  /* Turn off the asynchronous connection via GTask thread as the FreeRDP
   * process then runs in the new thread which makes it hard to cooperate
   * with the original thread running UI leading to random race conditions.
   * Turn it on again if there will be an async support for connection
   * added to FreeRDP.
   * The disadvantage is that the application freezes during connection
   * for some time.

     g_task_run_in_thread (task, frdp_session_connect_thread);

   */

  frdp_session_connect_thread (task, self, user_data, cancellable);

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
  idle_close (self);

  g_debug ("Closing RDP session");
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
  guint16 xflags = 0;

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
  if (event & FRDP_MOUSE_EVENT_HWHEEL) {
    flags |= PTR_FLAGS_HWHEEL;
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
  if (event & FRDP_MOUSE_EVENT_BUTTON4)
    xflags |=  PTR_XFLAGS_BUTTON1;
  if (event & FRDP_MOUSE_EVENT_BUTTON5)
    xflags |=  PTR_XFLAGS_BUTTON2;

  input = priv->freerdp_session->context->input;

  if (priv->scaling) {
    x = (x - priv->offset_x) / priv->scale;
    y = (y - priv->offset_y) / priv->scale;
  }

  x = x < 0.0 ? 0.0 : x;
  y = y < 0.0 ? 0.0 : y;
  if (xflags != 0) {
    if (event & FRDP_MOUSE_EVENT_DOWN)
        xflags |=  PTR_XFLAGS_DOWN;
    freerdp_input_send_extended_mouse_event(input, xflags, x, y);
  } else if (flags != 0) {
    freerdp_input_send_mouse_event (input, flags, x, y);
  }
}

void
frdp_session_mouse_smooth_scroll_event (FrdpSession          *self,
                                        guint16               x,
                                        guint16               y,
                                        double                delta_x,
                                        double                delta_y)
{
  FrdpSessionPrivate *priv = self->priv;
  rdpInput           *input;
  guint16             flags = 0, value;

  g_return_if_fail (priv->freerdp_session != NULL);

  if (fabs (delta_y) >= fabs (delta_x)) {
    flags |= PTR_FLAGS_WHEEL;
    value = (guint16) round (fabs (delta_y) * 0x78);
    if (value > 0) {
      /* Reversing direction here to reflect the behaviour on local side. */
      if (delta_y < 0.0) {
        if (value > 255)
          value = 255;
        flags |= value & WheelRotationMask;
      } else {
        if (value > 256)
          value = 256;
        flags |= PTR_FLAGS_WHEEL_NEGATIVE;
        flags |= (~value + 1) & WheelRotationMask;
      }
    }
  } else {
    flags |= PTR_FLAGS_HWHEEL;
    value = (guint16) round (fabs (delta_x) * 0x78);
    if (value > 0) {
      if (delta_x < 0.0) {
        if (value > 256)
          value = 256;
        flags |= PTR_FLAGS_WHEEL_NEGATIVE;
        flags |= (~value + 1) & WheelRotationMask;
      } else {
        if (value > 255)
          value = 255;
        flags |= value & WheelRotationMask;
      }
    }
  }

  input = priv->freerdp_session->context->input;

  freerdp_input_send_mouse_event (input, flags, 0, 0);
}

void
frdp_session_mouse_pointer  (FrdpSession          *self,
                             gboolean              enter)
{
  FrdpSessionPrivate *priv = self->priv;

  priv->show_cursor = enter;
  frdp_session_update_mouse_pointer (self);
}

void
frdp_session_send_key (FrdpSession  *self,
                       GdkEventKey  *key)
{
  rdpInput *input = self->priv->freerdp_session->context->input;

#ifdef HAVE_FREERDP_3_11_0
  guint32   virtual_code, virtual_scan_code;

  virtual_code = GetVirtualKeyCodeFromKeycode (key->hardware_keycode, WINPR_KEYCODE_TYPE_XKB);
  virtual_scan_code = GetVirtualScanCodeFromVirtualKeyCode (virtual_code, WINPR_KBD_TYPE_IBM_ENHANCED);

  if (virtual_scan_code != RDP_SCANCODE_UNKNOWN)
    freerdp_input_send_keyboard_event_ex (input, key->type == GDK_KEY_PRESS, FALSE, virtual_scan_code);
#else
  DWORD scancode = 0;
  guint8 keycode;
  guint16 flags;
  gboolean extended = FALSE;

  scancode =
    freerdp_keyboard_get_rdp_scancode_from_x11_keycode (key->hardware_keycode);

  keycode = scancode & 0xFF;
  extended = scancode & 0x100;

  flags = extended ? KBD_FLAGS_EXTENDED : 0;
  flags |= key->type == GDK_KEY_PRESS ? KBD_FLAGS_DOWN : KBD_FLAGS_RELEASE;

  if (keycode)
    input->KeyboardEvent (input, flags, keycode);
#endif
}

GdkPixbuf *
frdp_session_get_pixbuf (FrdpSession *self)
{
  guint width, height;

  width = gtk_widget_get_allocated_width (self->priv->display);
  height = gtk_widget_get_allocated_height (self->priv->display);

  return gdk_pixbuf_get_from_surface (self->priv->surface,
                                      0, 0,
                                      width, height);
}
