/* frdp-channel-clipboard.h
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

#pragma once

#include "frdp-channel.h"

G_BEGIN_DECLS

#define FRDP_TYPE_CHANNEL_CLIPBOARD (frdp_channel_clipboard_get_type())

G_DECLARE_FINAL_TYPE (FrdpChannelClipboard, frdp_channel_clipboard, FRDP, CHANNEL_CLIPBOARD, GObject)

typedef struct _FrdpChannelClipboard FrdpChannelClipboard;

struct _FrdpChannelClipboard
{
  GObject parent_instance;
};

struct _FrdpChannelClipboardClass
{
  FrdpChannelClass parent_class;
};

G_END_DECLS
