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

G_DECLARE_DERIVABLE_TYPE (FrdpDisplay, frdp_display, FRDP, DISPLAY, GtkDrawingArea)

typedef struct _FrdpDisplayPrivate FrdpDisplayPrivate;

struct _FrdpDisplayClass
{
  GtkDrawingAreaClass parent_parent;

  gboolean (*authenticate) (FrdpDisplay *self, gchar **username, gchar **password, gchar **domain);
};

GtkWidget *frdp_display_new       (void);

void       frdp_display_open_host (FrdpDisplay *display,
                                   const gchar *host,
                                   guint        port);

gboolean   frdp_display_is_open   (FrdpDisplay *display);

void       frdp_display_close     (FrdpDisplay *display);

void       frdp_display_set_scaling (FrdpDisplay *display,
                                     gboolean     scaling);

gboolean   frdp_display_authenticate (FrdpDisplay *self,
                                      gchar **username,
                                      gchar **password,
                                      gchar **domain);

guint      frdp_display_certificate_verify_ex (FrdpDisplay *self,
                                               const gchar *host,
                                               guint16      port,
                                               const gchar *common_name,
                                               const gchar *subject,
                                               const gchar *issuer,
                                               const gchar *fingerprint,
                                               guint32      flags);

guint      frdp_display_certificate_change_verify_ex (FrdpDisplay *self,
                                                      const gchar *host,
                                                      guint16      port,
                                                      const gchar *common_name,
                                                      const gchar *subject,
                                                      const gchar *issuer,
                                                      const gchar *fingerprint,
                                                      const gchar *old_subject,
                                                      const gchar *old_issuer,
                                                      const gchar *old_fingerprint,
                                                      guint32      flags);

void       frdp_display_certificate_verify_ex_finish (FrdpDisplay *self,
                                                      guint        verification);

void       frdp_display_certificate_change_verify_ex_finish (FrdpDisplay *self,
                                                             guint        verification);

void       frdp_display_authenticate_finish (FrdpDisplay *self,
                                             gchar       *username,
                                             gchar       *password,
                                             gchar       *domain);

GdkPixbuf *frdp_display_get_pixbuf (FrdpDisplay *display);

G_END_DECLS
