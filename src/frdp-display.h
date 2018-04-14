/* frdp-display.h
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

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define FRDP_TYPE_DISPLAY (frdp_display_get_type())
#define FRDP_DISPLAY(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), FRDP_TYPE_DISPLAY, FrdpDisplay))

G_DECLARE_FINAL_TYPE (FrdpDisplay, frdp_display, FRDP_DISPLAY, DISPLAY, GtkDrawingArea)

typedef struct _FrdpDisplayPrivate FrdpDisplayPrivate;

struct _FrdpDisplay
{
  GtkDrawingArea parent;

  FrdpDisplayPrivate *priv;

  /* Do not add fields to this struct */
};

GtkWidget *frdp_display_new       (void);

void       frdp_display_open_host (FrdpDisplay *display,
                                   const gchar *host,
                                   guint        port);

G_END_DECLS
