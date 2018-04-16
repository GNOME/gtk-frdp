/* frdp-session.h
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

#pragma once

#include <glib-object.h>
#include <frdp-display.h>

G_BEGIN_DECLS

#define FRDP_TYPE_SESSION (frdp_session_get_type())

G_DECLARE_FINAL_TYPE (FrdpSession, frdp_session, FRDP_SESSION, SESSION, GObject)

typedef struct _FrdpSessionPrivate FrdpSessionPrivate;

struct _FrdpSession
{
  GObject parent;

  FrdpSessionPrivate *priv;

  /* Do not add fields to this struct */
};

typedef enum
{
  FRDP_MOUSE_EVENT_MOVE           = 1 << 0,
  FRDP_MOUSE_EVENT_DOWN           = 1 << 1,
  FRDP_MOUSE_EVENT_WHEEL          = 1 << 2,
  FRDP_MOUSE_EVENT_WHEEL_NEGATIVE = 1 << 3,
  FRDP_MOUSE_EVENT_BUTTON1        = 1 << 4,
  FRDP_MOUSE_EVENT_BUTTON2        = 1 << 5,
  FRDP_MOUSE_EVENT_BUTTON3        = 1 << 6,
} FrdpMouseEvent;

FrdpSession *frdp_session_new            (FrdpDisplay          *display);

void         frdp_session_connect        (FrdpSession          *self,
                                          const gchar          *hostname,
                                          guint                 port,
                                          GCancellable         *cancellable,
                                          GAsyncReadyCallback   callback,
                                          gpointer              user_data);

gboolean     frdp_session_connect_finish (FrdpSession          *self,
                                          GAsyncResult         *result,
                                          GError              **error);

void         frdp_session_mouse_event    (FrdpSession          *self,
                                          FrdpMouseEvent        event,
                                          guint16               x,
                                          guint16               y);

G_END_DECLS