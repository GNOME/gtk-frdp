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
  PROP_PASSWORD
};

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
  gchar *str_property;

  switch (property_id)
    {
      case PROP_USERNAME:
        g_object_get (session, "username", &str_property, NULL);
        g_value_set_string (value, str_property);
        g_free (str_property);
        break;
      case PROP_PASSWORD:
        g_object_get (session, "password", &str_property, NULL);
        g_value_set_string (value, str_property);
        g_free (str_property);
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
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
frdp_display_class_init (FrdpDisplayClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->get_property = frdp_display_get_property;
  gobject_class->set_property = frdp_display_set_property;

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
}

static void
frdp_display_init (FrdpDisplay *self)
{
  FrdpDisplayPrivate *priv;

  self->priv = frdp_display_get_instance_private (self);
  priv = self->priv;

  priv->session = frdp_session_new (FRDP_DISPLAY (self));
}

void
frdp_display_open_host (FrdpDisplay  *self,
                        const gchar  *host,
                        guint         port)
{
  g_return_if_fail (host != NULL);

  frdp_session_connect (self->priv->session,
                        host,
                        port,
                        NULL, // TODO: Cancellable
                        frdp_display_open_host_cb,
                        self);

}

GtkWidget *frdp_display_new (void)
{
  return GTK_WIDGET (g_object_new (FRDP_TYPE_DISPLAY, NULL));
}
