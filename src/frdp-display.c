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

struct _FrdpDisplayPrivate
{
  FrdpSession *session;
};

G_DEFINE_TYPE_WITH_PRIVATE (FrdpDisplay, frdp_display, GTK_TYPE_DRAWING_AREA)

enum
{
  PROP_O = 0,
  PROP_USERNAME,
  PROP_PASSWORD,
  PROP_SCALING
};

enum
{
  RDP_INITIALIZED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

static gboolean
frdp_display_is_initialized (FrdpDisplay *self)
{
  return self->priv->session != NULL;
}

static gboolean
frdp_display_key_press_event (GtkWidget   *widget,
                              GdkEventKey *key)
{
  FrdpDisplay *self = FRDP_DISPLAY (widget);
  guint16 keycode = key->hardware_keycode;
  FrdpKeyEvent event;

  if (!frdp_display_is_initialized (self))
    return TRUE;

  switch (key->type) {
    case GDK_KEY_PRESS:
      event = FRDP_KEY_EVENT_PRESS;
      break;
    case GDK_KEY_RELEASE:
      event = FRDP_KEY_EVENT_RELEASE;
      break;
    default:
      g_warn_if_reached ();
      break;
  }

  frdp_session_send_key (self->priv->session, event, keycode);

  return TRUE;
}

static gboolean
frdp_display_motion_notify_event (GtkWidget      *widget,
                                  GdkEventMotion *event)
{
  FrdpDisplay *self = FRDP_DISPLAY (widget);

  if (!frdp_display_is_initialized (self))
    return TRUE;

  frdp_session_mouse_event (self->priv->session,
                            FRDP_MOUSE_EVENT_MOVE,
                            event->x,
                            event->y);

  return TRUE;
}

static gboolean
frdp_display_button_press_event (GtkWidget      *widget,
                                 GdkEventButton *event)
{
  FrdpDisplay *self = FRDP_DISPLAY (widget);
  guint16 flags = 0;

  if (!frdp_display_is_initialized (self))
    return TRUE;

  if ((event->button < 1) || (event->button > 3))
    return FALSE;

  if ((event->type != GDK_BUTTON_PRESS) &&
      (event->type != GDK_BUTTON_RELEASE))
    return FALSE;

  if (event->type == GDK_BUTTON_PRESS)
    flags |= FRDP_MOUSE_EVENT_DOWN;
  if (event->button == 1)
    flags |= FRDP_MOUSE_EVENT_BUTTON1;
  if (event->button == 2)
    flags |= FRDP_MOUSE_EVENT_BUTTON3;
  if (event->button == 3)
    flags |= FRDP_MOUSE_EVENT_BUTTON2;

  frdp_session_mouse_event (self->priv->session,
                            flags,
                            event->x,
                            event->y);

  return TRUE;
}

static gboolean
frdp_display_scroll_event (GtkWidget      *widget,
                           GdkEventScroll *event)
{
  FrdpDisplay *self = FRDP_DISPLAY (widget);
  guint16 flags = FRDP_MOUSE_EVENT_WHEEL;

  if (!frdp_display_is_initialized (self))
    return TRUE;

  switch (event->direction) {
    case GDK_SCROLL_UP:
      break;
    case GDK_SCROLL_DOWN:
      flags |= FRDP_MOUSE_EVENT_WHEEL_NEGATIVE;
      break;
    case GDK_SCROLL_SMOOTH:
      g_debug ("scroll smooth unhandled");
    default:
      return FALSE;
  }

  frdp_session_mouse_event (self->priv->session,
                            flags,
                            event->x,
                            event->y);

  return TRUE;
}

static void
frdp_display_open_host_cb (GObject      *source_object,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  FrdpSession *session = (FrdpSession*) source_object;
  gboolean success;
  GError  *error = NULL;

  success = frdp_session_connect_finish (session,
                                         result,
                                         &error);

  g_print ("OPEN HOST CB! %d\n", success);
}

static void
frdp_display_get_property (GObject      *object,
                           guint         property_id,
                           GValue       *value,
                           GParamSpec   *pspec)
{
  FrdpDisplay *self = FRDP_DISPLAY (object);
  FrdpSession *session = self->priv->session;
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
  FrdpSession *session = self->priv->session;

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
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
frdp_display_class_init (FrdpDisplayClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gobject_class->get_property = frdp_display_get_property;
  gobject_class->set_property = frdp_display_set_property;

  widget_class->key_press_event = frdp_display_key_press_event;
  widget_class->key_release_event = frdp_display_key_press_event;
  widget_class->motion_notify_event = frdp_display_motion_notify_event;
  widget_class->button_press_event = frdp_display_button_press_event;
  widget_class->button_release_event = frdp_display_button_press_event;
  widget_class->scroll_event = frdp_display_scroll_event;

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

  signals[RDP_INITIALIZED] = g_signal_new ("rdp-initialized",
                                           G_TYPE_FROM_CLASS (klass),
                                           G_SIGNAL_RUN_LAST,
                                           0, NULL, NULL, NULL,
                                           G_TYPE_NONE, 0);
}

static void
frdp_display_init (FrdpDisplay *self)
{
  self->priv = frdp_display_get_instance_private (self);

  gtk_widget_add_events (GTK_WIDGET (self),
                         GDK_POINTER_MOTION_MASK |
                         GDK_BUTTON_PRESS_MASK |
                         GDK_BUTTON_RELEASE_MASK |
                         GDK_SCROLL_MASK |
                         GDK_SMOOTH_SCROLL_MASK |
                         GDK_KEY_PRESS_MASK);

  gtk_widget_set_can_focus (GTK_WIDGET (self), TRUE);

  self->priv->session = NULL;
}

void
frdp_display_open_host (FrdpDisplay  *self,
                        const gchar  *host,
                        guint         port)
{
  g_return_if_fail (host != NULL);

  self->priv->session = frdp_session_new (self);

  frdp_session_connect (self->priv->session,
                        host,
                        port,
                        NULL, // TODO: Cancellable
                        frdp_display_open_host_cb,
                        self);

  g_signal_emit (self, signals[RDP_INITIALIZED], 0);
}

void
frdp_display_set_scaling (FrdpDisplay *self,
                          gboolean     scaling)
{
  g_object_set (self->priv->session, "scaling", scaling, NULL);

  if (scaling) {
    gtk_widget_set_size_request (GTK_WIDGET (self), -1, -1);

    gtk_widget_set_halign (GTK_WIDGET (self), GTK_ALIGN_FILL);
    gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_FILL);
  } else {
    gtk_widget_set_halign (GTK_WIDGET (self), GTK_ALIGN_CENTER);
    gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_CENTER);
  }

  gtk_widget_queue_draw_area (GTK_WIDGET (self), 0, 0,
                              gtk_widget_get_allocated_width (GTK_WIDGET (self)),
                              gtk_widget_get_allocated_height (GTK_WIDGET (self)));
}

GtkWidget *frdp_display_new (void)
{
  return GTK_WIDGET (g_object_new (FRDP_TYPE_DISPLAY, NULL));
}
