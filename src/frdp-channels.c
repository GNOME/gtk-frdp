/* frdp-channels.c
 *
 * Copyright (C) 2019 Armin Novak <akallabeth@posteo.net>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <freerdp/gdi/video.h>
#include <freerdp/gdi/gfx.h>

#include "frdp-channels.h"
#include "frdp-context.h"

void frdp_OnChannelConnectedEventHandler(void* context, ChannelConnectedEventArgs* e)
{
	frdpContext* ctx = (frdpContext*) context;

	if (strcmp(e->name, RDPEI_DVC_CHANNEL_NAME) == 0)
	{
		// TODO Touch input redirection
	}
  else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0)
	{
		// TODO Display resize channel
	}
	else if (strcmp(e->name, TSMF_DVC_CHANNEL_NAME) == 0)
	{
		// TODO Old windows 7 multimedia redirection
	}
	else if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0)
	{
		gdi_graphics_pipeline_init(ctx->context.gdi, (RdpgfxClientContext*) e->pInterface);
	}
	else if (strcmp(e->name, RAIL_SVC_CHANNEL_NAME) == 0)
	{
		// TODO Remote application
	}
	else if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0)
	{
		// TODO Clipboard redirection channel
	}
	else if (strcmp(e->name, ENCOMSP_SVC_CHANNEL_NAME) == 0)
	{
		// TODO Multiparty channel
	}
	else if (strcmp(e->name, GEOMETRY_DVC_CHANNEL_NAME) == 0)
	{
		gdi_video_geometry_init(ctx->context.gdi, (GeometryClientContext*)e->pInterface);
	}
	else if (strcmp(e->name, VIDEO_CONTROL_DVC_CHANNEL_NAME) == 0)
	{
		gdi_video_control_init(ctx->context.gdi, (VideoClientContext*)e->pInterface);
	}
	else if (strcmp(e->name, VIDEO_DATA_DVC_CHANNEL_NAME) == 0)
	{
		gdi_video_data_init(ctx->context.gdi, (VideoClientContext*)e->pInterface);
	}
}

void frdp_OnChannelDisconnectedEventHandler(void* context, ChannelDisconnectedEventArgs* e)
{
	frdpContext* ctx = (frdpContext*) context;

	if (strcmp(e->name, RDPEI_DVC_CHANNEL_NAME) == 0)
	{
		// TODO Touch input redirection
	}
	else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0)
	{
		// TODO Display resize channel
	}
	else if (strcmp(e->name, TSMF_DVC_CHANNEL_NAME) == 0)
	{
		// TODO Old windows 7 multimedia redirection
	}
	else if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0)
	{
		gdi_graphics_pipeline_uninit(ctx->context.gdi, (RdpgfxClientContext*) e->pInterface);
	}
	else if (strcmp(e->name, RAIL_SVC_CHANNEL_NAME) == 0)
	{
    // TODO Remote application
	}
	else if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0)
	{
		// TODO Clipboard redirection channel
	}
	else if (strcmp(e->name, ENCOMSP_SVC_CHANNEL_NAME) == 0)
	{
		// TODO Multiparty channel
	}
	else if (strcmp(e->name, GEOMETRY_DVC_CHANNEL_NAME) == 0)
	{
		gdi_video_geometry_uninit(ctx->context.gdi, (GeometryClientContext*)e->pInterface);
	}
	else if (strcmp(e->name, VIDEO_CONTROL_DVC_CHANNEL_NAME) == 0)
	{
		gdi_video_control_uninit(ctx->context.gdi, (VideoClientContext*)e->pInterface);
	}
	else if (strcmp(e->name, VIDEO_DATA_DVC_CHANNEL_NAME) == 0)
	{
		gdi_video_data_uninit(ctx->context.gdi, (VideoClientContext*)e->pInterface);
	}
}
