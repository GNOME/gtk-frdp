/* frdp-display.c
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

#include "frdp-display.h"

#include "frdp-session.h"

#include <freerdp/channels/disp.h>

struct _FrdpDisplayPrivate
{
  FrdpSession *session;

  gboolean     allow_resize;
  gboolean     resize_supported;
};

G_DEFINE_TYPE_WITH_PRIVATE (FrdpDisplay, frdp_display, GTK_TYPE_DRAWING_AREA)

enum
{
  PROP_O = 0,
  PROP_USERNAME,
  PROP_PASSWORD,
  PROP_SCALING,
  PROP_ALLOW_RESIZE,
  PROP_RESIZE_SUPPORTED
};

enum
{
  RDP_ERROR,
  RDP_CONNECTED,
  RDP_DISCONNECTED,
  RDP_NEEDS_AUTHENTICATION,
  RDP_AUTH_FAILURE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

static void frdp_display_set_allow_resize (FrdpDisplay *display,
                                           gboolean     allow_resize);

static gboolean
frdp_display_is_initialized (FrdpDisplay *self)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  frdp_display_init (self);

  return priv->session != NULL && frdp_display_is_open (self);
}

static gboolean
frdp_key_pressed_cb (GtkEventControllerKey *controller,
                     guint                  keyval,
                     guint                  keycode,
                     guint                  modifiers,
                     GtkWidget             *widget)
{
  FrdpDisplay *self = FRDP_DISPLAY (widget);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  if (!frdp_display_is_initialized (self))
    return TRUE;

  frdp_session_send_key (priv->session, keyval, keycode, modifiers, true);

  return TRUE;
}

static gboolean
frdp_key_released_cb (GtkEventControllerKey *controller,
                      guint                  keyval,
                      guint                  keycode,
                      guint                  modifiers,
                      GtkWidget             *widget)
{
  FrdpDisplay *self = FRDP_DISPLAY (widget);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  if (!frdp_display_is_initialized (self))
    return TRUE;

  frdp_session_send_key (priv->session, keyval, keycode, modifiers, false);

  return TRUE;
}

static gboolean
frdp_display_motion_event (GtkEventControllerMotion *controller,
                           gdouble                   x,
                           gdouble                   y,
                           GtkWidget                *widget)
{
  FrdpDisplay *self = FRDP_DISPLAY (widget);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  if (!frdp_display_is_initialized (self))
    return TRUE;

  frdp_session_mouse_event (priv->session,
                            FRDP_MOUSE_EVENT_MOVE,
                            x,
                            y);

  return TRUE;
}

static gboolean
frdp_display_button_event (GtkGestureClick *controller,
                           gint             n_press,
                           gdouble          x,
                           gdouble          y,
                           GdkEventType     type,
                           GtkWidget       *widget)
{
  FrdpDisplay *self = FRDP_DISPLAY (widget);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);
  guint16 flags = 0;
  guint button = gtk_gesture_single_get_current_button (controller);

  if (!frdp_display_is_initialized (self))
    return TRUE;

  if ((button < 1) || (button > 3))
    return FALSE;

  if ((type != GDK_BUTTON_PRESS) &&
      (type != GDK_BUTTON_RELEASE))
    return FALSE;

  if (type == GDK_BUTTON_PRESS)
    flags |= FRDP_MOUSE_EVENT_DOWN;
  switch (button) {
  case GDK_BUTTON_PRIMARY:
    flags |= FRDP_MOUSE_EVENT_BUTTON1;
    break;
  case GDK_BUTTON_MIDDLE:
    flags |= FRDP_MOUSE_EVENT_BUTTON3;
    break;
  case GDK_BUTTON_SECONDARY:
    flags |= FRDP_MOUSE_EVENT_BUTTON2;
    break;
  case 8:
    flags |= FRDP_MOUSE_EVENT_BUTTON4;
    break;
  case 9:
    flags |= FRDP_MOUSE_EVENT_BUTTON5;
    break;
  default:
    return FALSE;
  }

  frdp_session_mouse_event (priv->session,
                            flags,
                            x,
                            y);

  return TRUE;
}

static gboolean
frdp_display_button_press_event (GtkGestureClick *controller,
                                 gint             n_press,
                                 gdouble          x,
                                 gdouble          y,
                                 GtkWidget       *widget)
{
  return frdp_display_button_event (controller, n_press, x, y, GDK_BUTTON_PRESS, widget);
}

static gboolean
frdp_display_button_release_event (GtkGestureClick *controller,
                                   gint             n_press,
                                   gdouble          x,
                                   gdouble          y,
                                   GtkWidget       *widget)
{
  return frdp_display_button_event(controller, n_press, x, y, GDK_BUTTON_RELEASE, widget);
}

static gboolean
frdp_display_scroll_event (GtkEventControllerScroll* controller,
                           gdouble                   dx,
                           gdouble                   dy,
                           GtkWidget                *widget)
{
  FrdpDisplay *self = FRDP_DISPLAY (widget);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);
  guint16 flags = FRDP_MOUSE_EVENT_WHEEL;

  if (!frdp_display_is_initialized (self))
    return TRUE;

/*  switch (event->direction) {
    case GDK_SCROLL_UP:
      flags = FRDP_MOUSE_EVENT_WHEEL;
      break;
    case GDK_SCROLL_DOWN:
      flags = FRDP_MOUSE_EVENT_WHEEL | FRDP_MOUSE_EVENT_WHEEL_NEGATIVE;
      break;
    case GDK_SCROLL_LEFT:
      flags = FRDP_MOUSE_EVENT_HWHEEL | FRDP_MOUSE_EVENT_WHEEL_NEGATIVE;
      break;
    case GDK_SCROLL_RIGHT:
      flags = FRDP_MOUSE_EVENT_HWHEEL;
      break;
    case GDK_SCROLL_SMOOTH:*/
    /* Calculate delta and decide which event we have
     * a delta X means horizontal, a delta Y means vertical scroll.
     * Fixes https://bugzilla.gnome.org/show_bug.cgi?id=675959
     */
/*    if (event->delta_x > 0.5)
      flags = FRDP_MOUSE_EVENT_HWHEEL;
    else if (event->delta_x < -0.5)
      flags = FRDP_MOUSE_EVENT_HWHEEL | FRDP_MOUSE_EVENT_WHEEL_NEGATIVE;
    else if (event->delta_y > 0.5)
      flags = FRDP_MOUSE_EVENT_WHEEL;
    else if (event->delta_y < -0.5)
      flags = FRDP_MOUSE_EVENT_WHEEL | FRDP_MOUSE_EVENT_WHEEL_NEGATIVE;
    else {
      g_debug ("scroll smooth unhandled");
      return FALSE;
    }
    break;
    default:
      return FALSE;
  }

  frdp_session_mouse_event (priv->session,
                            flags,
                            event->x,
                            event->y);*/

  return TRUE;
}

static gboolean
frdp_enter_notify_event (GtkEventControllerMotion *controller,
                         gdouble                   x,
                         gdouble                   y,
                         GtkWidget                *widget)
{
  FrdpDisplay *self = FRDP_DISPLAY (widget);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);
  frdp_session_mouse_pointer(priv->session, TRUE);
  return TRUE;
}

static gboolean
frdp_leave_notify_event (GtkEventControllerMotion *controller,
                         GtkWidget                *widget)
{
  FrdpDisplay *self = FRDP_DISPLAY (widget);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);
  frdp_session_mouse_pointer(priv->session, FALSE);
  return TRUE;
}

static void
frdp_display_error (GObject     *source_object,
                    const gchar *message,
                    gpointer     user_data)
{
  g_signal_emit (user_data, signals[RDP_ERROR], 0, message);
}

static void
frdp_display_auth_failure (GObject     *source_object,
                           const gchar *message,
                           gpointer     user_data)
{
  g_signal_emit (user_data, signals[RDP_AUTH_FAILURE], 0, message);
}

static void
frdp_display_disconnected (GObject  *source_object,
                           gpointer  user_data)
{
  FrdpDisplay *self = FRDP_DISPLAY (user_data);

  g_signal_emit (self, signals[RDP_DISCONNECTED], 0);

  g_debug ("rdp disconnected");
}

static void
frdp_display_open_host_cb (GObject      *source_object,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  FrdpDisplay *self = FRDP_DISPLAY (user_data);
  FrdpSession *session = (FrdpSession*) source_object;
  gboolean success;
  GError  *error = NULL;

  success = frdp_session_connect_finish (session,
                                         result,
                                         &error);

  if (success) {
    g_signal_emit (self, signals[RDP_CONNECTED], 0);

    g_debug ("Connection established");
  } else {
    g_signal_emit (self, signals[RDP_DISCONNECTED], 0);

    g_debug ("Connection failed");
  }
}

static void
frdp_display_get_property (GObject      *object,
                           guint         property_id,
                           GValue       *value,
                           GParamSpec   *pspec)
{
  FrdpDisplay *self = FRDP_DISPLAY (object);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);
  FrdpSession *session = priv->session;
  gpointer str_property;

  switch (property_id)
    {
      case PROP_USERNAME:
        g_object_get (session, "username", &str_property, NULL);
        g_value_set_string (value, str_property);
        break;
      case PROP_PASSWORD:
        g_object_get (session, "password", &str_property, NULL);
        g_value_set_string (value, str_property);
        break;
      case PROP_SCALING:
        g_object_get (session, "scaling", &str_property, NULL);
        g_value_set_boolean (value, (gboolean)GPOINTER_TO_INT (str_property));
        break;
      case PROP_ALLOW_RESIZE:
        g_value_set_boolean (value, priv->allow_resize);
        break;
      case PROP_RESIZE_SUPPORTED:
        g_value_set_boolean (value, priv->resize_supported);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
frdp_display_set_property (GObject      *object,
                           guint         property_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  FrdpDisplay *self = FRDP_DISPLAY (object);
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);
  FrdpSession *session = priv->session;

  switch (property_id)
    {
      case PROP_USERNAME:
        g_object_set (session, "username", g_value_get_string (value), NULL);
        break;
      case PROP_PASSWORD:
        g_object_set (session, "password", g_value_get_string (value), NULL);
        break;
      case PROP_SCALING:
        frdp_display_set_scaling (self, g_value_get_boolean (value));
        break;
      case PROP_ALLOW_RESIZE:
        frdp_display_set_allow_resize (self, g_value_get_boolean (value));
        break;
      case PROP_RESIZE_SUPPORTED:
        priv->resize_supported = g_value_get_boolean (value);
        g_object_notify (G_OBJECT (self), "resize-supported");
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
frdp_display_snapshot (GtkWidget   *widget,
                       GtkSnapshot *snapshot)
{
  FrdpDisplay *self = (FrdpDisplay*) widget;
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  frdp_session_draw (priv->session, widget, snapshot);
}

static void
frdp_display_class_init (FrdpDisplayClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gobject_class->get_property = frdp_display_get_property;
  gobject_class->set_property = frdp_display_set_property;

  widget_class->snapshot = frdp_display_snapshot;

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
                                   PROP_SCALING,
                                   g_param_spec_boolean ("scaling",
                                                         "scaling",
                                                         "scaling",
                                                         TRUE,
                                                         G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_ALLOW_RESIZE,
                                   g_param_spec_boolean ("allow-resize",
                                                         "allow-resize",
                                                         "allow-resize",
                                                         FALSE,
                                                         G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_RESIZE_SUPPORTED,
                                   g_param_spec_boolean ("resize-supported",
                                                         "resize-supported",
                                                         "resize-supported",
                                                         FALSE,
                                                         G_PARAM_READWRITE));

  signals[RDP_ERROR] = g_signal_new ("rdp-error",
                                     G_TYPE_FROM_CLASS (klass),
                                     G_SIGNAL_RUN_LAST,
                                     0, NULL, NULL, NULL,
                                     G_TYPE_NONE, 1,
                                     G_TYPE_STRING);

  signals[RDP_CONNECTED] = g_signal_new ("rdp-connected",
                                         G_TYPE_FROM_CLASS (klass),
                                         G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);

  signals[RDP_DISCONNECTED] = g_signal_new ("rdp-disconnected",
                                            G_TYPE_FROM_CLASS (klass),
                                            G_SIGNAL_RUN_LAST,
                                            0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 0);

  signals[RDP_NEEDS_AUTHENTICATION] = g_signal_new ("rdp-needs-authentication",
                                                    G_TYPE_FROM_CLASS (klass),
                                                    G_SIGNAL_RUN_LAST,
                                                    0, NULL, NULL, NULL,
                                                    G_TYPE_NONE, 0);

  signals[RDP_AUTH_FAILURE] = g_signal_new ("rdp-auth-failure",
                                            G_TYPE_FROM_CLASS (klass),
                                            G_SIGNAL_RUN_LAST,
                                            0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 1,
                                            G_TYPE_STRING);
}

static void
frdp_display_init (FrdpDisplay *self)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (self);

  GtkEventController *key_controller = gtk_event_controller_key_new ();
  g_signal_connect (key_controller, "key-pressed", G_CALLBACK (frdp_key_pressed_cb), self);
  g_signal_connect (key_controller, "key-released", G_CALLBACK (frdp_key_released_cb), self);
  gtk_widget_add_controller (GTK_WIDGET (self), key_controller);

  GtkEventController *motion_controller = gtk_event_controller_motion_new ();
  g_signal_connect (motion_controller,"motion", G_CALLBACK (frdp_display_motion_event), self);
  g_signal_connect (motion_controller,"enter", G_CALLBACK (frdp_enter_notify_event), self);
  g_signal_connect (motion_controller,"leave", G_CALLBACK (frdp_leave_notify_event), self);
  gtk_widget_add_controller (GTK_WIDGET (self), motion_controller);

  GtkGestureClick *gesture_controller = gtk_gesture_click_new ();
  g_signal_connect (gesture_controller,"pressed", G_CALLBACK (frdp_display_button_press_event), self);
  g_signal_connect (gesture_controller,"released", G_CALLBACK (frdp_display_button_release_event), self);
  gtk_widget_add_controller (GTK_WIDGET (self), gesture_controller);

  GtkEventControllerScroll *scroll_controller = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect (scroll_controller,"scroll", G_CALLBACK (frdp_display_scroll_event), self);
  gtk_widget_add_controller (GTK_WIDGET (self), scroll_controller);

/*  gtk_widget_add_events (GTK_WIDGET (self),
                         GDK_POINTER_MOTION_MASK |
                         GDK_BUTTON_PRESS_MASK |
                         GDK_BUTTON_RELEASE_MASK |
                         GDK_SCROLL_MASK |
                         GDK_SMOOTH_SCROLL_MASK |
                         GDK_KEY_PRESS_MASK |
                         GDK_ENTER_NOTIFY_MASK |
                         GDK_LEAVE_NOTIFY_MASK);*/

  gtk_widget_set_can_focus (GTK_WIDGET (self), TRUE);

  priv->session = frdp_session_new (self);

  g_object_bind_property (priv->session, "monitor-layout-supported", self, "resize-supported", 0);
}

/**
 * frdp_display_open_host:
 * @display: (transfer none): the RDP display widget
 * @host: (transfer none): the hostname or IP address
 * @port: the service name or port number
 *
 * Opens a TCP connection to the given @host litening on
 * @port.
 */
void
frdp_display_open_host (FrdpDisplay  *display,
                        const gchar  *host,
                        guint         port)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (display);

  g_return_if_fail (host != NULL);

  g_signal_connect (priv->session, "rdp-error",
                    G_CALLBACK (frdp_display_error),
                    display);
  g_signal_connect (priv->session, "rdp-disconnected",
                    G_CALLBACK (frdp_display_disconnected),
                    display);
  g_signal_connect (priv->session, "rdp-auth-failure",
                    G_CALLBACK (frdp_display_auth_failure),
                    display);

  frdp_session_connect (priv->session,
                        host,
                        port,
                        NULL, // TODO: Cancellable
                        frdp_display_open_host_cb,
                        g_object_ref (display));

  g_debug ("Connecting to %s…", host);
}

/**
 * frdp_display_is_open:
 * @display: (transfer none): the RDP display widget
 *
 * Check if the connection for the display is currently open
 *
 * Returns: TRUE if open, FALSE if closing/closed
 */
gboolean
frdp_display_is_open (FrdpDisplay *display)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (display);

  return frdp_session_is_open (priv->session);
}

/**
 * frdp_display_close:
 * @display: (transfer none): the RDP display widget
 *
 * Request the closing of the RDP session.
 */
void
frdp_display_close (FrdpDisplay *display)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (display);

  frdp_session_close (priv->session);
}

/**
 * frdp_display_set_scaling:
 * @display: (transfer none): the RDP display widget
 * @scaling: TRUE to scale the desktop to fit, FALSE otherwise
 *
 * Set whether the remote desktop contents is automatically
 * scaled to fit the available widget size, or whether it will
 * be rendered at 1:1 size
 */
void
frdp_display_set_scaling (FrdpDisplay *display,
                          gboolean     scaling)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (display);

  g_object_set (priv->session, "scaling", scaling, NULL);

  if (scaling) {
    gtk_widget_set_size_request (GTK_WIDGET (display), -1, -1);

    gtk_widget_set_halign (GTK_WIDGET (display), GTK_ALIGN_FILL);
    gtk_widget_set_valign (GTK_WIDGET (display), GTK_ALIGN_FILL);
  }

  gtk_widget_queue_draw (GTK_WIDGET (display));
}

static void
frdp_display_set_allow_resize (FrdpDisplay *display,
                               gboolean     allow_resize)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (display);

  priv->allow_resize = allow_resize;

  if (allow_resize) {
    gtk_widget_set_size_request (GTK_WIDGET (display), -1, -1);

    gtk_widget_set_halign (GTK_WIDGET (display), GTK_ALIGN_FILL);
    gtk_widget_set_valign (GTK_WIDGET (display), GTK_ALIGN_FILL);
  }

  gtk_widget_queue_draw_area (GTK_WIDGET (display), 0, 0,
                              gtk_widget_get_allocated_width (GTK_WIDGET (display)),
                              gtk_widget_get_allocated_height (GTK_WIDGET (display)));
}

/*
 * frdp_display_new:
 *
 * Create a new widget capable of connecting to a RDP server
 * and displaying its contents
 *
 * The widget will be initially be in a disconnected state
 *
 * Returns: (transfer full): the new RDP display widget
 */
GtkWidget *frdp_display_new (void)
{
  return GTK_WIDGET (g_object_new (FRDP_TYPE_DISPLAY, NULL));
}

gboolean
frdp_display_authenticate (FrdpDisplay *self,
                           gchar **username,
                           gchar **password,
                           gchar **domain)
{
  FrdpDisplayClass *klass = FRDP_DISPLAY_GET_CLASS (self);

  g_signal_emit (self, signals[RDP_NEEDS_AUTHENTICATION], 0);

  return klass->authenticate (self, username, password, domain);
}

/**
 * frdp_display_get_pixbuf:
 * @display: (transfer none): the RDP display widget
 *
 * Take a screenshot of the display.
 *
 * Returns: (transfer full): a #GdkPixbuf with the screenshot image buffer
 */
GdkPixbuf *
frdp_display_get_pixbuf (FrdpDisplay *display)
{
  FrdpDisplayPrivate *priv = frdp_display_get_instance_private (display);

  return frdp_session_get_pixbuf (priv->session);
}
