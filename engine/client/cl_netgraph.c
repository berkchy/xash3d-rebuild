/*
cl_netgraph.c - Draw Net statistics (borrowed from Xash3D SDL code)
Copyright (C) 2016 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "common.h"
#include "client.h"
#include "kbutton.h"

#if XASH_LOW_MEMORY == 0
#define NET_TIMINGS			1024
#elif XASH_LOW_MEMORY == 1
#define NET_TIMINGS			256
#elif XASH_LOW_MEMORY == 2
#define NET_TIMINGS			64
#endif
#define NET_TIMINGS_MASK		(NET_TIMINGS - 1)
#define LATENCY_AVG_FRAC		0.5f
#define FRAMERATE_AVG_FRAC		0.5f
#define PACKETLOSS_AVG_FRAC		0.5f
#define PACKETCHOKE_AVG_FRAC		0.5f
#define NETGRAPH_LERP_HEIGHT		24
#define NETGRAPH_NET_COLORS		5
#define NUM_LATENCY_SAMPLES		8

CVAR_DEFINE_AUTO( net_graph, "0", FCVAR_ARCHIVE, "draw network usage graph (mode 4: Source-style panel)" );
static CVAR_DEFINE_AUTO( net_graphpos, "1", FCVAR_ARCHIVE, "network usage graph position" );
static CVAR_DEFINE_AUTO( net_scale, "5", FCVAR_ARCHIVE, "network usage graph scale level" );
static CVAR_DEFINE_AUTO( net_graphwidth, "192", FCVAR_ARCHIVE, "network usage graph width" );
static CVAR_DEFINE_AUTO( net_graphheight, "64", FCVAR_ARCHIVE, "network usage graph height" );
static CVAR_DEFINE_AUTO( net_graphsolid, "1", FCVAR_ARCHIVE, "fill segments in network usage graph" );

static struct packet_latency_t
{
	int	latency;
	int	choked;
} netstat_packet_latency[NET_TIMINGS];

static struct cmdinfo_t
{
	float	cmd_lerp;
	int	size;
	qboolean	sent;
} netstat_cmdinfo[NET_TIMINGS];

static byte netcolors[NETGRAPH_NET_COLORS+NETGRAPH_LERP_HEIGHT][4] =
{
	{ 255, 0,   0,   255 },
	{ 0,   0,   255, 255 },
	{ 240, 127, 63,  255 },
	{ 255, 255, 0,   255 },
	{ 63,  255, 63,  150 }
	// other will be generated through NetGraph_InitColors()
};

static const byte sendcolor[4] = { 88, 29, 130, 255 };
static const byte holdcolor[4] = { 255, 0, 0, 200 };
static const byte extrap_base_color[4] = { 255, 255, 255, 255 };
static netbandwidthgraph_t	netstat_graph[NET_TIMINGS];
static float		packet_loss;
static float		packet_choke;
static float		framerate = 0.0;
static float		netgraph_client_frame_avg;
static float		netgraph_client_frame_var;
static float		netgraph_server_frame_avg;
static float		netgraph_server_frame_var;
static int		maxmsgbytes = 0;
static int		netgraph_lastout;
static qboolean		netgraph_has_live_data;

static void NetGraph_DrawRect( const wrect_t *rect, const byte colors[4] );

static qboolean NetGraph_HasLiveData( void )
{
	return cls.state >= ca_connected && cls.state != ca_cinematic;
}

static void NetGraph_ResetData( void )
{
	int i;

	memset( netstat_packet_latency, 0, sizeof( netstat_packet_latency ));
	memset( netstat_cmdinfo, 0, sizeof( netstat_cmdinfo ));
	memset( netstat_graph, 0, sizeof( netstat_graph ));

	for( i = 0; i < ARRAYSIZE( netstat_cmdinfo ); i++ )
		netstat_cmdinfo[i].sent = true;

	packet_loss = 0.0f;
	packet_choke = 0.0f;
	framerate = 0.0f;
	netgraph_client_frame_avg = 0.0f;
	netgraph_client_frame_var = 0.0f;
	netgraph_server_frame_avg = 0.0f;
	netgraph_server_frame_var = 0.0f;
	maxmsgbytes = 0;
	netgraph_lastout = 0;
	netgraph_has_live_data = false;
}

static float NetGraph_NormalizeLatency( float avg, int count )
{
	float normalized_count = (float)count - ( host.frametime * FRAMERATE_AVG_FRAC );

	if( count <= 0 )
		return 0.0f;

	avg /= Q_max( normalized_count, 1.0f );

	if( cl_updaterate.value > 0.0f )
		avg -= 1000.0f / cl_updaterate.value;

	return Q_max( avg, 0.0f );
}

static float NetGraph_GetServerFrameTime( void )
{
	float server_frame;

	if( !NetGraph_HasLiveData( ) || cl.mtime[0] <= 0.0 || cl.mtime[1] <= 0.0 )
		return 0.0f;

	server_frame = cl_serverframetime();

	if( server_frame <= 0.0f || server_frame > 1.0f )
		return 0.0f;

	return server_frame;
}

static void NetGraph_UpdateTimingStats( void )
{
	float client_frame = host.frametime;
	float server_frame = NetGraph_GetServerFrameTime();

	framerate = FRAMERATE_AVG_FRAC * client_frame + ( 1.0f - FRAMERATE_AVG_FRAC ) * framerate;

	if( netgraph_client_frame_avg == 0.0f )
		netgraph_client_frame_avg = client_frame;
	else netgraph_client_frame_avg = FRAMERATE_AVG_FRAC * client_frame + ( 1.0f - FRAMERATE_AVG_FRAC ) * netgraph_client_frame_avg;

	netgraph_client_frame_var = FRAMERATE_AVG_FRAC * fabs( client_frame - netgraph_client_frame_avg ) +
		( 1.0f - FRAMERATE_AVG_FRAC ) * netgraph_client_frame_var;

	if( server_frame <= 0.0f )
	{
		netgraph_server_frame_avg = 0.0f;
		netgraph_server_frame_var = 0.0f;
		return;
	}

	if( netgraph_server_frame_avg == 0.0f )
		netgraph_server_frame_avg = server_frame;
	else netgraph_server_frame_avg = FRAMERATE_AVG_FRAC * server_frame + ( 1.0f - FRAMERATE_AVG_FRAC ) * netgraph_server_frame_avg;

	netgraph_server_frame_var = FRAMERATE_AVG_FRAC * fabs( server_frame - netgraph_server_frame_avg ) +
		( 1.0f - FRAMERATE_AVG_FRAC ) * netgraph_server_frame_var;
}

static int NetGraph_GetLastOutgoingSize( void )
{
	int i = ( cls.netchan.outgoing_sequence - 1 ) & NET_TIMINGS_MASK;
	int out = netstat_cmdinfo[i].size;

	if( !out )
		return netgraph_lastout;

	netgraph_lastout = out;
	return out;
}

static const rgba_t *NetGraph_GetMetricColor( float value, float warn, float bad )
{
	static const rgba_t good = { 235, 235, 235, 255 };
	static const rgba_t caution = { 255, 196, 92, 255 };
	static const rgba_t danger = { 255, 108, 108, 255 };

	if( bad > warn && value >= bad )
		return &danger;
	if( warn > 0.0f && value >= warn )
		return &caution;
	return &good;
}

static const rgba_t *NetGraph_GetLossColor( int value )
{
	static const rgba_t good = { 235, 235, 235, 255 };
	static const rgba_t caution = { 255, 196, 92, 255 };
	static const rgba_t danger = { 255, 108, 108, 255 };

	if( value >= 5 )
		return &danger;
	if( value > 0 )
		return &caution;
	return &good;
}

static const char *NetGraph_GetConnectionLabel( void )
{
	if( !NetGraph_HasLiveData( ))
		return "offline";
	if( NET_IsLocalAddress( cls.netchan.remote_address ) || Host_IsLocalGame( ))
		return "local";
	return "online";
}

static const rgba_t *NetGraph_GetConnectionColor( const char *connection_label )
{
	static const rgba_t offline = { 176, 176, 176, 255 };
	static const rgba_t local = { 132, 230, 148, 255 };
	static const rgba_t online = { 132, 200, 255, 255 };

	if( !Q_strcmp( connection_label, "local" ))
		return &local;
	if( !Q_strcmp( connection_label, "online" ))
		return &online;
	return &offline;
}

static int NetGraph_DrawStatPair( cl_font_t *font, int x, int y, const char *label, const char *value, const rgba_t value_color )
{
	static const rgba_t label_color = { 150, 150, 150, 255 };
	int label_width = 0;

	CL_DrawString( x, y, label, label_color, font, FONT_DRAW_NORENDERMODE );
	CL_DrawStringLen( font, label, &label_width, NULL, 0 );

	return label_width + CL_DrawString( x + label_width + 4, y, value, value_color, font, FONT_DRAW_NORENDERMODE );
}

static void NetGraph_GetSourceScreenPos( wrect_t *rect, cl_font_t *font, int *x, int *y, int *w, int *h )
{
	int panel_width = Q_max( 320, (int)net_graphwidth.value );
	int font_height = font && font->valid ? font->charHeight : 12;

	rect->left = rect->top = 0;
	rect->right = refState.width;
	rect->bottom = refState.height;

	*w = Q_min( rect->right - 10, panel_width );
	*h = font_height * 3 + 18;

	switch( (int)net_graphpos.value )
	{
	case 1:
		*x = rect->left + rect->right - 5 - *w;
		break;
	case 2:
		*x = ( rect->left + ( rect->right - 10 - *w )) / 2;
		break;
	default:
		*x = rect->left + 5;
		break;
	}

	*y = rect->bottom - *h - 6;
}

static void NetGraph_DrawSourceStyle( float avg_ping )
{
	static const byte bg_outer[4] = { 20, 20, 20, 180 };
	static const byte bg_inner[4] = { 8, 8, 8, 200 };
	cl_font_t *font = Con_GetFont( 0 );
	wrect_t rect, outline, fill;
	char value[32];
	const char *connection_label = NetGraph_GetConnectionLabel();
	float server_frame = NetGraph_GetServerFrameTime();
	float fps = framerate > 0.0f ? 1.0f / framerate : 0.0f;
	float tickrate = server_frame > 0.0f ? 1.0f / server_frame : cl_updaterate.value;
	int panel_x, panel_y, panel_w, panel_h;
	int content_x, content_y, column_w;
	int loss = bound( 0, (int)(( packet_loss + PACKETLOSS_AVG_FRAC ) - 0.01f ), 100 );
	int choke = bound( 0, (int)(( packet_choke + PACKETCHOKE_AVG_FRAC ) - 0.01f ), 100 );
	int in = netstat_graph[cls.netchan.incoming_sequence & NET_TIMINGS_MASK].msgbytes;
	int kb_in = (int)cls.netchan.flow[FLOW_INCOMING].avgkbytespersec;
	int row_step;

	if( !font || !font->valid )
		return;

	NetGraph_GetSourceScreenPos( &rect, font, &panel_x, &panel_y, &panel_w, &panel_h );

	outline.left = panel_x;
	outline.top = panel_y;
	outline.right = panel_w;
	outline.bottom = panel_h;

	fill.left = panel_x + 1;
	fill.top = panel_y + 1;
	fill.right = panel_w - 2;
	fill.bottom = panel_h - 2;

	ref.dllFuncs.GL_SetRenderMode( kRenderTransColor );
	ref.dllFuncs.GL_Bind( XASH_TEXTURE0, R_GetBuiltinTexture( REF_WHITE_TEXTURE ) );
	ref.dllFuncs.Begin( TRI_QUADS );
	NetGraph_DrawRect( &outline, bg_outer );
	NetGraph_DrawRect( &fill, bg_inner );
	ref.dllFuncs.End();
	ref.dllFuncs.Color4ub( 255, 255, 255, 255 );
	ref.dllFuncs.GL_SetRenderMode( kRenderNormal );

	CL_SetFontRendermode( font );

	content_x = panel_x + 8;
	content_y = panel_y + 6;
	column_w = ( panel_w - 16 ) / 4;
	row_step = font->charHeight + 3;

	Q_snprintf( value, sizeof( value ), "%.0f", fps );
	NetGraph_DrawStatPair( font, content_x, content_y, "fps:", value, *NetGraph_GetMetricColor( fps > 0.0f ? 300.0f / fps : 999.0f, 16.0f, 33.0f ));

	Q_snprintf( value, sizeof( value ), "%.2f ms", netgraph_client_frame_var * 1000.0f );
	NetGraph_DrawStatPair( font, content_x + column_w, content_y, "var:", value, *NetGraph_GetMetricColor( netgraph_client_frame_var * 1000.0f, 1.0f, 2.0f ));

	Q_snprintf( value, sizeof( value ), "%.0f ms", avg_ping );
	NetGraph_DrawStatPair( font, content_x + column_w * 2, content_y, "ping:", value, *NetGraph_GetMetricColor( avg_ping, 60.0f, 100.0f ));

	Q_snprintf( value, sizeof( value ), "%d%%", loss );
	NetGraph_DrawStatPair( font, content_x + column_w * 3, content_y, "loss:", value, *NetGraph_GetLossColor( loss ));

	Q_snprintf( value, sizeof( value ), "%d%%", choke );
	NetGraph_DrawStatPair( font, content_x, content_y + row_step, "choke:", value, *NetGraph_GetLossColor( choke ));

	Q_snprintf( value, sizeof( value ), "%.1f", tickrate );
	NetGraph_DrawStatPair( font, content_x + column_w, content_y + row_step, "tick:", value, *NetGraph_GetMetricColor( tickrate > 0.0f ? 128.0f / tickrate : 999.0f, 2.0f, 8.0f ));

	Q_snprintf( value, sizeof( value ), "%.2f ms", server_frame * 1000.0f );
	NetGraph_DrawStatPair( font, content_x + column_w * 2, content_y + row_step, "sv:", value, *NetGraph_GetMetricColor( server_frame * 1000.0f, 20.0f, 40.0f ));

	Q_snprintf( value, sizeof( value ), "%.2f ms", netgraph_server_frame_var * 1000.0f );
	NetGraph_DrawStatPair( font, content_x + column_w * 3, content_y + row_step, "svar:", value, *NetGraph_GetMetricColor( netgraph_server_frame_var * 1000.0f, 1.0f, 2.0f ));

	Q_snprintf( value, sizeof( value ), "%d/s", (int)cl_updaterate.value );
	NetGraph_DrawStatPair( font, content_x, content_y + row_step * 2, "up:", value, *NetGraph_GetMetricColor( cl_updaterate.value > 0.0f ? 100.0f / cl_updaterate.value : 999.0f, 0.0f, 0.0f ));

	Q_snprintf( value, sizeof( value ), "%d/s", (int)cl_cmdrate.value );
	NetGraph_DrawStatPair( font, content_x + column_w, content_y + row_step * 2, "cmd:", value, *NetGraph_GetMetricColor( cl_cmdrate.value > 0.0f ? 100.0f / cl_cmdrate.value : 999.0f, 0.0f, 0.0f ));

	Q_snprintf( value, sizeof( value ), "%d / %d", in, kb_in );
	NetGraph_DrawStatPair( font, content_x + column_w * 2, content_y + row_step * 2, "in:", value, *NetGraph_GetMetricColor( kb_in, 0.0f, 0.0f ));

	Q_snprintf( value, sizeof( value ), "%s", connection_label );
	NetGraph_DrawStatPair( font, content_x + column_w * 3, content_y + row_step * 2, "net:", value, *NetGraph_GetConnectionColor( connection_label ));
}

/*
==========
NetGraph_DrawRect

NetGraph_FillRGBA shortcut
==========
*/
static void NetGraph_DrawRect( const wrect_t *rect, const byte colors[4] )
{
	ref.dllFuncs.Color4ub( colors[0], colors[1], colors[2], colors[3] );	// color for this quad

	ref.dllFuncs.Vertex3f( rect->left, rect->top, 0 );
	ref.dllFuncs.Vertex3f( rect->left + rect->right, rect->top, 0 );
	ref.dllFuncs.Vertex3f( rect->left + rect->right, rect->top + rect->bottom, 0 );
	ref.dllFuncs.Vertex3f( rect->left, rect->top + rect->bottom, 0 );
}

/*
==========
NetGraph_AtEdge

edge detect
==========
*/
static qboolean NetGraph_AtEdge( int x, int width )
{
	if( x > 3 )
	{
		if( x >= width - 4 )
			return true;
		return false;
	}
	return true;
}

/*
==========
NetGraph_InitColors

init netgraph colors
==========
*/
static void NetGraph_InitColors( void )
{
	byte	mincolor[2][3];
	byte	maxcolor[2][3];
	float	dc[2][3];
	int	i, hfrac;
	float	f;

	mincolor[0][0] = 63;
	mincolor[0][1] = 0;
	mincolor[0][2] = 100;

	maxcolor[0][0] = 0;
	maxcolor[0][1] = 63;
	maxcolor[0][2] = 255;

	mincolor[1][0] = 255;
	mincolor[1][1] = 127;
	mincolor[1][2] = 0;

	maxcolor[1][0] = 250;
	maxcolor[1][1] = 0;
	maxcolor[1][2] = 0;

	for( i = 0; i < 3; i++ )
	{
		dc[0][i] = (float)(maxcolor[0][i] - mincolor[0][i]);
		dc[1][i] = (float)(maxcolor[1][i] - mincolor[1][i]);
	}

	hfrac = NETGRAPH_LERP_HEIGHT / 3;

	for( i = 0; i < NETGRAPH_LERP_HEIGHT; i++ )
	{
		if( i < hfrac )
		{
			f = (float)i / (float)hfrac;
			VectorMA( mincolor[0], f, dc[0], netcolors[NETGRAPH_NET_COLORS + i] );
		}
		else
		{
			f = (float)(i - hfrac) / (float)(NETGRAPH_LERP_HEIGHT - hfrac );
			VectorMA( mincolor[1], f, dc[1], netcolors[NETGRAPH_NET_COLORS + i] );
		}
		netcolors[NETGRAPH_NET_COLORS + i][3] = 255;
	}
}

/*
==========
NetGraph_GetFrameData

get frame data info, like chokes, packet losses, also update graph, packet and cmdinfo
==========
*/
static void NetGraph_GetFrameData( float *latency, int *latency_count )
{
	int		i, choke_count = 0, loss_count = 0;
	double		newtime = Sys_DoubleTime();
	static double	nexttime = 0;
	float		loss, choke;

	*latency_count = 0;
	*latency = 0.0f;

	if( !NetGraph_HasLiveData( ))
	{
		if( netgraph_has_live_data )
			NetGraph_ResetData();
		return;
	}

	netgraph_has_live_data = true;

	if( newtime >= nexttime )
	{
		// soft fading of net peak usage
		maxmsgbytes = Q_max( 0, maxmsgbytes - 50 );
		nexttime = newtime + 0.05;
	}

	for( i = cls.netchan.incoming_sequence - CL_UPDATE_BACKUP + 1; i <= cls.netchan.incoming_sequence; i++ )
	{
		frame_t *f = cl.frames + ( i & CL_UPDATE_MASK );
		struct packet_latency_t *p = netstat_packet_latency + ( i & NET_TIMINGS_MASK );
		netbandwidthgraph_t *g = netstat_graph + ( i & NET_TIMINGS_MASK );

		p->choked = f->choked;
		if( p->choked ) choke_count++;

		if( !f->valid )
		{
			p->latency = 9998; // broken delta
		}
		else if( f->receivedtime == -1.0 )
		{
			p->latency = 9999; // dropped
			loss_count++;
		}
		else if( f->receivedtime == -3.0 )
		{
			p->latency = 9997; // skipped
		}
		else
		{
			int frame_latency = Q_min( 1.0f, f->latency );
			p->latency = (( frame_latency + 0.1f ) / 1.1f ) * ( net_graphheight.value - NETGRAPH_LERP_HEIGHT - 2 );

			if( i > cls.netchan.incoming_sequence - NUM_LATENCY_SAMPLES )
			{
				(*latency) += 1000.0f * f->latency;
				(*latency_count)++;
			}
		}

		memcpy( g, &f->graphdata, sizeof( netbandwidthgraph_t ));

		if( g->msgbytes > maxmsgbytes )
			maxmsgbytes = g->msgbytes;
	}

	if( maxmsgbytes > 1000 )
		maxmsgbytes = 1000;

	for( i = cls.netchan.outgoing_sequence - CL_UPDATE_BACKUP + 1; i <= cls.netchan.outgoing_sequence; i++ )
	{
		netstat_cmdinfo[i & NET_TIMINGS_MASK].cmd_lerp = cl.commands[i & CL_UPDATE_MASK].frame_lerp;
		netstat_cmdinfo[i & NET_TIMINGS_MASK].sent = cl.commands[i & CL_UPDATE_MASK].heldback ? false : true;
		netstat_cmdinfo[i & NET_TIMINGS_MASK].size = cl.commands[i & CL_UPDATE_MASK].sendsize;
	}

	// packet loss
	loss = 100.0f * (float)loss_count / CL_UPDATE_BACKUP;
	packet_loss = PACKETLOSS_AVG_FRAC * packet_loss + ( 1.0f - PACKETLOSS_AVG_FRAC ) * loss;

	// packet choke
	choke = 100.0f * (float)choke_count / CL_UPDATE_BACKUP;
	packet_choke = PACKETCHOKE_AVG_FRAC * packet_choke + ( 1.0f - PACKETCHOKE_AVG_FRAC ) * choke;
}

/*
===========
NetGraph_DrawTimes

===========
*/
static void NetGraph_DrawTimes( wrect_t rect, int x, int w )
{
	int	i, j, extrap_point = NETGRAPH_LERP_HEIGHT / 3, a, h;
	rgba_t	colors = { 0.9 * 255, 0.9 * 255, 0.7 * 255, 255 };
	wrect_t	fill;

	for( a = 0; a < w; a++ )
	{
		i = ( cls.netchan.outgoing_sequence - a ) & NET_TIMINGS_MASK;
		h = Q_min(( netstat_cmdinfo[i].cmd_lerp / 3.0f ) * NETGRAPH_LERP_HEIGHT, net_graphheight.value * 0.7f);

		fill.left = x + w - a - 1;
		fill.right = fill.bottom = 1;
		fill.top = rect.top + rect.bottom - 4;

		if( h >= extrap_point )
		{
			int	start = 0;

			h -= extrap_point;
			fill.top -= extrap_point;

			if( !net_graphsolid.value )
			{
				fill.top -= (h - 1);
				start = (h - 1);
			}

			for( j = start; j < h; j++ )
			{
				int color = NETGRAPH_NET_COLORS + j + extrap_point;
				color = Q_min( color, ARRAYSIZE( netcolors ) - 1 );

				NetGraph_DrawRect( &fill, netcolors[color] );
				fill.top--;
			}
		}
		else
		{
			int	oldh = h;

			fill.top -= h;
			h = extrap_point - h;

			if( !net_graphsolid.value )
				h = 1;

			for( j = 0; j < h; j++ )
			{
				int color = NETGRAPH_NET_COLORS + j + oldh;
				color = Q_min( color, ARRAYSIZE( netcolors ) - 1 );

				NetGraph_DrawRect( &fill, netcolors[color] );
				fill.top--;
			}
		}

		fill.top = rect.top + rect.bottom - 4 - extrap_point;

		if( NetGraph_AtEdge( a, w ))
			NetGraph_DrawRect( &fill, extrap_base_color );

		fill.top = rect.top + rect.bottom - 4;

		if( netstat_cmdinfo[i].sent )
			NetGraph_DrawRect( &fill, sendcolor );
		else NetGraph_DrawRect( &fill, holdcolor );
	}
}

//left = x
//right = width
//top = y
//bottom = height

/*
===========
NetGraph_DrawHatches

===========
*/
static void NetGraph_DrawHatches( int x, int y )
{
	int	ystep = (int)( 10.0f / net_scale.value );
	byte	colorminor[4] = { 0, 63, 63, 200 };
	byte	color[4] = { 0, 200, 0, 255 };
	wrect_t	hatch = { x, 4, y, 1 };
	int	starty;

	ystep = Q_max( ystep, 1 );

	for( starty = hatch.top; hatch.top > 0 && ((starty - hatch.top) * net_scale.value < (maxmsgbytes + 50)); hatch.top -= ystep )
	{
		if(!((int)((starty - hatch.top) * net_scale.value ) % 50 ))
		{
			NetGraph_DrawRect( &hatch, color );
		}
		else if( ystep > 5 )
		{
			NetGraph_DrawRect( &hatch, colorminor );
		}
	}
}

/*
===========
NetGraph_DrawTextFields

===========
*/
static void NetGraph_DrawTextFields( int x, int y, int w, wrect_t rect, int count, float avg, int packet_loss, int packet_choke, int graphtype )
{
	cl_font_t *font = Con_GetFont( 0 );
	rgba_t		colors = { 0.9 * 255, 0.9 * 255, 0.7 * 255, 255 };
	int		ptx = Q_max( x + w - NETGRAPH_LERP_HEIGHT - 1, 1 );
	int		pty = Q_max( rect.top + rect.bottom - NETGRAPH_LERP_HEIGHT - 3, 1 );
	int		out;
	int		j = cls.netchan.incoming_sequence & NET_TIMINGS_MASK;
	int		last_y = y - net_graphheight.value;

	(void)count;

	CL_SetFontRendermode( font );

	if( framerate > 0.0f )
	{
		y -= net_graphheight.value;

		CL_DrawStringf( font, x, y, colors, FONT_DRAW_NORENDERMODE, "%.1f fps" , 1.0f / framerate);

		if( avg > 1.0f )
			CL_DrawStringf( font, x + 75, y, colors, FONT_DRAW_NORENDERMODE, "%i ms" , (int)avg );

		y += 15;

		out = NetGraph_GetLastOutgoingSize();

		CL_DrawStringf( font, x, y, colors, FONT_DRAW_NORENDERMODE,
			"in :  %i %.2f kb/s", netstat_graph[j].msgbytes, cls.netchan.flow[FLOW_INCOMING].avgkbytespersec );
		y += 15;

		CL_DrawStringf( font, x, y, colors, FONT_DRAW_NORENDERMODE,
			"out:  %i %.2f kb/s", out, cls.netchan.flow[FLOW_OUTGOING].avgkbytespersec );
		y += 15;

		if( graphtype > 2 )
		{
			int	loss = (int)(( packet_loss + PACKETLOSS_AVG_FRAC ) - 0.01f );
			int	choke = (int)(( packet_choke + PACKETCHOKE_AVG_FRAC ) - 0.01f );

			CL_DrawStringf( font, x, y, colors, FONT_DRAW_NORENDERMODE, "loss: %i choke: %i", loss, choke );
		}
	}

	if( graphtype < 3 )
		CL_DrawStringf( font, ptx, pty, colors, FONT_DRAW_NORENDERMODE, "%i/s", (int)cl_cmdrate.value );

	CL_DrawStringf( font, ptx, last_y, colors, FONT_DRAW_NORENDERMODE, "%i/s" , (int)cl_updaterate.value );
}

/*
===========
NetGraph_DrawDataSegment

===========
*/
static int NetGraph_DrawDataSegment( wrect_t *fill, int bytes, byte r, byte g, byte b, byte a )
{
	float	h = bytes / net_scale.value;
	byte	colors[4] = { r, g, b, a };

	fill->top -= (int)h;

	if( net_graphsolid.value )
		fill->bottom = (int)h;
	else fill->bottom = 1;

	if( fill->top > 1 )
	{
		NetGraph_DrawRect( fill, colors );
		return 1;
	}

	return 0;
}

/*
===========
NetGraph_ColorForHeight

color based on packet latency
===========
*/
static void NetGraph_ColorForHeight( struct packet_latency_t *packet, byte color[4], int *ping )
{
	switch( packet->latency )
	{
	case 9999:
		memcpy( color, netcolors[0], sizeof( byte ) * 4 ); // dropped
		*ping = 0;
		break;
	case 9998:
		memcpy( color, netcolors[1], sizeof( byte ) * 4 ); // invalid
		*ping = 0;
		break;
	case 9997:
		memcpy( color, netcolors[2], sizeof( byte ) * 4 ); // skipped
		*ping = 0;
		break;
	default:
		*ping = 1;
		if( packet->choked )
		{
			memcpy( color, netcolors[3], sizeof( byte ) * 4 );
		}
		else
		{
			memcpy( color, netcolors[4], sizeof( byte ) * 4 );
		}
	}
}

/*
===========
NetGraph_DrawDataUsage

===========
*/
static void NetGraph_DrawDataUsage( int x, int y, int w, int graphtype )
{
	int	a, i, h, lastvalidh = 0, ping;
	int	pingheight = net_graphheight.value - NETGRAPH_LERP_HEIGHT - 2;
	wrect_t	fill = { 0 };
	byte	color[4];

	for( a = 0; a < w; a++ )
	{
		i = (cls.netchan.incoming_sequence - a) & NET_TIMINGS_MASK;
		h = netstat_packet_latency[i].latency;

		NetGraph_ColorForHeight( &netstat_packet_latency[i], color, &ping );

		if( !ping ) h = lastvalidh;
		else lastvalidh = h;

		if( h > pingheight )
			h = pingheight;

		fill.left = x + w - a - 1;
		fill.top = y - h;
		fill.right = 1;
		fill.bottom = ping ? 1: h;

		if( !ping )
		{
			if( fill.bottom > 3 )
			{
				fill.bottom = 2;
				NetGraph_DrawRect( &fill, color );
				fill.top += fill.bottom - 2;
				NetGraph_DrawRect( &fill, color );
			}
			else
			{
				NetGraph_DrawRect( &fill, color );
			}
		}
		else
		{
			NetGraph_DrawRect( &fill, color );
		}

		fill.top = y;
		fill.bottom = 1;

		color[0] = 0;
		color[1] = 255;
		color[2] = 0;
		color[3] = 160;

		if( NetGraph_AtEdge( a, w ))
			NetGraph_DrawRect( &fill, color );

		if( graphtype < 2 )
			continue;

		color[0] = color[1] = color[2] = color[3] = 255;
		fill.top = y - net_graphheight.value - 1;
		fill.bottom = 1;

		if( NetGraph_AtEdge( a, w ))
			NetGraph_DrawRect( &fill, color );

		fill.top -= 1;

		if( netstat_packet_latency[i].latency > 9995 )
			continue; // skip invalid

		if( !NetGraph_DrawDataSegment( &fill, netstat_graph[i].client, 255, 0, 0, 128 ))
			continue;

		if( !NetGraph_DrawDataSegment( &fill, netstat_graph[i].players, 255, 255, 0, 128 ))
			continue;

		if( !NetGraph_DrawDataSegment( &fill, netstat_graph[i].entities, 255, 0, 255, 128 ))
			continue;

		if( !NetGraph_DrawDataSegment( &fill, netstat_graph[i].tentities, 0, 0, 255, 128 ))
			continue;

		if( !NetGraph_DrawDataSegment( &fill, netstat_graph[i].sound, 0, 255, 0, 128 ))
			continue;

		if( !NetGraph_DrawDataSegment( &fill, netstat_graph[i].event, 0, 255, 255, 128 ))
			continue;

		if( !NetGraph_DrawDataSegment( &fill, netstat_graph[i].usr, 200, 200, 200, 128 ))
			continue;

		if( !NetGraph_DrawDataSegment( &fill, netstat_graph[i].voicebytes, 255, 255, 255, 255 ))
			continue;

		fill.top = y - net_graphheight.value - 1;
		fill.bottom = 1;
		fill.top -= 2;

		if( !NetGraph_DrawDataSegment( &fill, netstat_graph[i].msgbytes, 240, 240, 240, 128 ))
			continue;
	}

	if( graphtype >= 2 )
		NetGraph_DrawHatches( x, y - net_graphheight.value - 1 );
}

/*
===========
NetGraph_GetScreenPos

===========
*/
static void NetGraph_GetScreenPos( wrect_t *rect, int *w, int *x, int *y )
{
	rect->left = rect->top = 0;
	rect->right = refState.width;
	rect->bottom = refState.height;

	*w = Q_min( NET_TIMINGS, net_graphwidth.value );
	if( rect->right < *w + 10 )
		*w = rect->right - 10;

	// detect x and y position
	switch( (int)net_graphpos.value )
	{
	case 1: // right sided
		*x = rect->left + rect->right - 5 - *w;
		break;
	case 2: // center
		*x = ( rect->left + ( rect->right - 10 - *w )) / 2;
		break;
	default: // left sided
		*x = rect->left + 5;
		break;
	}

	*y = rect->bottom + rect->top - NETGRAPH_LERP_HEIGHT - 5;
}

/*
===========
SCR_DrawNetGraph

===========
*/
void SCR_DrawNetGraph( void )
{
	wrect_t	rect;
	float	avg_ping;
	int	ping_count;
	int	w, x, y;
	kbutton_t *in_graph;
	int   graphtype;

	if( cls.state == ca_cinematic )
		return;

	in_graph = clgame.dllFuncs.KB_Find( "in_graph" );

	if( in_graph && in_graph->state & 1 )
		graphtype = 2;
	else if( net_graph.value != 0.0f )
		graphtype = (int)net_graph.value;
	else return;

	if( net_scale.value <= 0 )
		Cvar_SetValue( "net_scale", 0.1f );

	NetGraph_GetScreenPos( &rect, &w, &x, &y );

	NetGraph_GetFrameData( &avg_ping, &ping_count );
	avg_ping = NetGraph_NormalizeLatency( avg_ping, ping_count );
	NetGraph_UpdateTimingStats();

	if( graphtype == 4 )
	{
		NetGraph_DrawSourceStyle( avg_ping );
		return;
	}

	NetGraph_DrawTextFields( x, y, w, rect, ping_count, avg_ping, packet_loss, packet_choke, graphtype );

	if( graphtype < 3 )
	{
		ref.dllFuncs.GL_SetRenderMode( kRenderTransColor );
		ref.dllFuncs.GL_Bind( XASH_TEXTURE0, R_GetBuiltinTexture( REF_WHITE_TEXTURE ) );
		ref.dllFuncs.Begin( TRI_QUADS ); // draw all the fills as a long solid sequence of quads for speedup reasons

		// NOTE: fill colors without texture at this point
		NetGraph_DrawDataUsage( x, y, w, graphtype );
		NetGraph_DrawTimes( rect, x, w );

		ref.dllFuncs.End();
		ref.dllFuncs.Color4ub( 255, 255, 255, 255 );
		ref.dllFuncs.GL_SetRenderMode( kRenderNormal );
	}
}

void CL_InitNetgraph( void )
{
	Cvar_RegisterVariable( &net_graph );
	Cvar_RegisterVariable( &net_graphpos );
	Cvar_RegisterVariable( &net_scale );
	Cvar_RegisterVariable( &net_graphwidth );
	Cvar_RegisterVariable( &net_graphheight );
	Cvar_RegisterVariable( &net_graphsolid );
	packet_loss = packet_choke = 0.0;

	NetGraph_ResetData();
	NetGraph_InitColors();
}
