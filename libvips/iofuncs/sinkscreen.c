/* aynchronous screen sink
 *
 * 1/1/10
 * 	- from im_render.c
 * 25/11/10
 * 	- in synchronous mode, use a single region for input and save huge 
 * 	  mem use
 * 20/1/14
 * 	- bg render thread quits on shutdown
 * 1/12/15
 * 	- don't do anything to out or mask after they have closed
 * 	- only run the bg render thread when there's work to do
 */

/*

    This file is part of VIPS.
    
    VIPS is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA

 */

/*

    These files are distributed with VIPS - http://www.vips.ecs.soton.ac.uk

 */

/* Verbose debugging output.
#define VIPS_DEBUG
 */

/* Trace allocate/free.
#define VIPS_DEBUG_AMBER
 */

/* Trace reschedule
#define VIPS_DEBUG_GREEN
 */

/* Trace serious problems.
#define VIPS_DEBUG_RED
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <glib/gi18n-lib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /*HAVE_UNISTD_H*/

#include <vips/vips.h>
#include <vips/thread.h>
#include <vips/internal.h>
#include <vips/debug.h>

#ifdef VIPS_DEBUG_AMBER
static int render_num_renders = 0;
#endif /*VIPS_DEBUG_AMBER*/

/* A tile in our cache. 
 */
typedef struct {
	struct _Render *render;

	VipsRect area;		/* Place here (unclipped) */
	VipsRegion *region;	/* VipsRegion with the pixels */

	/* The tile contains calculated pixels. Though the region may have been
	 * invalidated behind our backs: we have to check that too.
	 */
	gboolean painted;

	/* Time of last use, for LRU flush 
	 */
	int ticks;
} Tile;

/* Per-call state.
 */
typedef struct _Render {
	/* Reference count this, since we use these things from several
	 * threads. We can't easily use the gobject ref count system since we
	 * need a lock around operations.
	 */
#if GLIB_CHECK_VERSION( 2, 58, 0 )
	gatomicrefcount ref_count;
#else
	int ref_count;
	GMutex *ref_count_lock;
#endif

	/* Parameters.
	 */
	VipsImage *in;		/* Image we render */
	VipsImage *out;		/* Write tiles here on demand */
	VipsImage *mask;	/* Set valid pixels here */
	int tile_width;		/* Tile size */
	int tile_height;
	int max_tiles;		/* Maximum number of tiles */
	int priority;		/* Larger numbers done sooner */

	/* Lock here before reading or modifying the tile structure. 
	 */
	GMutex *lock;	

	/* Tile cache.
	 */
	GSList *all;		/* All our tiles */
	int ntiles;		/* Number of tiles */
	int ticks;		/* Inc. on each access ... used for LRU */

	/* Hash of tiles with positions. Tiles can be painted.
	 */
	GHashTable *tiles;
} Render;

static void *
tile_free( Tile *tile, void *a, void *b )
{
	VIPS_DEBUG_MSG_AMBER( "tile_free\n" );

	VIPS_UNREF( tile->region );
	g_free( tile );

	return( NULL );
}

static int
render_free( Render *render )
{
	VIPS_DEBUG_MSG_AMBER( "render_free: %p\n", render );

#if GLIB_CHECK_VERSION( 2, 58, 0 )
	g_assert ( g_atomic_ref_count_compare( &render->ref_count, 0 ) );
#else
	g_assert( render->ref_count == 0 );
#endif

#if !GLIB_CHECK_VERSION( 2, 58, 0 )
	vips_g_mutex_free( render->ref_count_lock );
#endif
	vips_g_mutex_free( render->lock );

	vips_slist_map2( render->all, (VipsSListMap2Fn) tile_free, NULL, NULL );
	VIPS_FREEF( g_slist_free, render->all );
	render->ntiles = 0;
	VIPS_FREEF( g_hash_table_destroy, render->tiles );

	VIPS_UNREF( render->in ); 

	g_free( render );

#ifdef VIPS_DEBUG_AMBER
	render_num_renders -= 1;
#endif /*VIPS_DEBUG_AMBER*/

	return( 0 );
}

/* Ref and unref a Render ... free on last unref.
 */
static int
render_ref( Render *render )
{
#if GLIB_CHECK_VERSION( 2, 58, 0 )
	g_assert( !g_atomic_ref_count_compare( &render->ref_count, 0 ) );
	g_atomic_ref_count_inc( &render->ref_count );
#else
	g_mutex_lock( render->ref_count_lock );
	g_assert( render->ref_count != 0 );
	render->ref_count += 1;
	g_mutex_unlock( render->ref_count_lock );
#endif

	return( 0 );
}

static int
render_unref( Render *render )
{
	int kill;

#if GLIB_CHECK_VERSION( 2, 58, 0 )
	g_assert( !g_atomic_ref_count_compare( &render->ref_count, 0 ) );
	kill = g_atomic_ref_count_dec( &render->ref_count );
#else
	g_mutex_lock( render->ref_count_lock );
	g_assert( render->ref_count > 0 );
	render->ref_count -= 1;
	kill = render->ref_count == 0;
	g_mutex_unlock( render->ref_count_lock );
#endif

	if( kill )
		render_free( render );

	return( 0 );
}

static guint
tile_hash( gconstpointer key )
{
	VipsRect *rect = (VipsRect *) key;

	int x = rect->left / rect->width;
	int y = rect->top / rect->height;

	return( x << 16 ^ y );
}

static gboolean
tile_equal( gconstpointer a, gconstpointer b )
{
	VipsRect *rect1 = (VipsRect *) a;
	VipsRect *rect2 = (VipsRect *) b;

	return( rect1->left == rect2->left &&
		rect1->top == rect2->top );
}

static void
render_close_cb( VipsImage *image, Render *render )
{
	VIPS_DEBUG_MSG_AMBER( "render_close_cb\n" );

	render_unref( render );
}

static Render *
render_new( VipsImage *in, VipsImage *out, VipsImage *mask, 
	int tile_width, int tile_height, 
	int max_tiles, 
	int priority )
{
	Render *render;

	/* Don't use auto-free for render, we do our own lifetime management
	 * with _ref() and _unref().
	 */
	if( !(render = VIPS_NEW( NULL, Render )) )
		return( NULL );

	/* render must hold a ref to in. This is dropped in render_free().
	 */
	g_object_ref( in );

#if GLIB_CHECK_VERSION( 2, 58, 0 )
	g_atomic_ref_count_init( &render->ref_count );
#else
	render->ref_count = 1;
	render->ref_count_lock = vips_g_mutex_new();
#endif

	render->in = in;
	render->out = out;
	render->mask = mask;
	render->tile_width = tile_width;
	render->tile_height = tile_height;
	render->max_tiles = max_tiles;
	render->priority = priority;

	render->lock = vips_g_mutex_new();

	render->all = NULL;
	render->ntiles = 0;
	render->ticks = 0;

	render->tiles = g_hash_table_new( tile_hash, tile_equal );

	/* Both out and mask must close before we can free the render.
	 */
	g_signal_connect( out, "close", 
		G_CALLBACK( render_close_cb ), render );

	if( mask ) {
		g_signal_connect( mask, "close", 
			G_CALLBACK( render_close_cb ), render );
		render_ref( render );
	}

	VIPS_DEBUG_MSG_AMBER( "render_new: %p\n", render );

#ifdef VIPS_DEBUG_AMBER
	render_num_renders += 1;
#endif /*VIPS_DEBUG_AMBER*/

	return( render );
}

/* Make a Tile.
 */
static Tile *
tile_new( Render *render )
{
	Tile *tile;

	VIPS_DEBUG_MSG_AMBER( "tile_new\n" );

	/* Don't use auto-free: we need to make sure we free the tile after
	 * Render.
	 */
	if( !(tile = VIPS_NEW( NULL, Tile )) )
		return( NULL );

	tile->render = render;
	tile->area.left = 0;
	tile->area.top = 0;
	tile->area.width = render->tile_width;
	tile->area.height = render->tile_height;
	tile->region = NULL;
	tile->painted = FALSE;
	tile->ticks = render->ticks;

	if( !(tile->region = vips_region_new( render->in )) ) {
		(void) tile_free( tile, NULL, NULL );
		return( NULL );
	}

	render->all = g_slist_prepend( render->all, tile );
	render->ntiles += 1;

	return( tile );
}

/* Search the cache for a tile by position.
 */
static Tile *
render_tile_lookup( Render *render, VipsRect *area )
{
	return( (Tile *) g_hash_table_lookup( render->tiles, area ) );
}

/* Add a new tile to the table.
 */
static void
render_tile_add( Tile *tile, VipsRect *area )
{
	Render *render = tile->render;

	g_assert( !render_tile_lookup( render, area ) );

	tile->area = *area;
	tile->painted = FALSE;

	/* Ignore buffer allocate errors, there's not much we could do with 
	 * them.
	 */
	if( vips_region_buffer( tile->region, &tile->area ) )
		VIPS_DEBUG_MSG_RED( "render_tile_add: "
			"buffer allocate failed\n" ); 

	g_hash_table_insert( render->tiles, &tile->area, tile );
}

/* Move a tile to a new position.
 */
static void
render_tile_move( Tile *tile, VipsRect *area )
{
	Render *render = tile->render;

	g_assert( render_tile_lookup( render, &tile->area ) );

	if( tile->area.left != area->left ||
		tile->area.top != area->top ) {
		g_assert( !render_tile_lookup( render, area ) );

		g_hash_table_remove( render->tiles, &tile->area );
		render_tile_add( tile, area );
	}
}

/* We've looked at a tile ... bump to end of LRU.
 */
static void
tile_touch( Tile *tile )
{
	Render *render = tile->render;

	tile->ticks = render->ticks;
	render->ticks += 1;
}

/* Queue a tile for calculation. 
 */
static void
tile_queue( Tile *tile, VipsRegion *reg )
{
	Render *render = tile->render;

	VIPS_DEBUG_MSG( "tile_queue: queue tile %p %dx%d for calculation\n",
		tile, tile->area.left, tile->area.top );

	tile->painted = FALSE;
	tile_touch( tile );

	/* Paint the tile synchronously. No need to notify the 
	 * client since they'll never see black tiles.
	 */
	VIPS_DEBUG_MSG( "tile_queue: "
		"painting tile %p %dx%d synchronously\n",
		tile, tile->area.left, tile->area.top );

	/* While we're computing, let other threads use the cache.
	 * This tile won't get pulled out from under us since it's not
	 * marked as "painted".
	 */
	g_mutex_unlock( render->lock );

	if( vips_region_prepare_to( reg, tile->region, 
		&tile->area, tile->area.left, tile->area.top ) ) 
		VIPS_DEBUG_MSG_RED( "tile_queue: prepare failed\n" ); 

	g_mutex_lock( render->lock );

	tile->painted = TRUE;
}

static void 
tile_test_clean_ticks( VipsRect *key, Tile *value, Tile **best )
{
	if( value->painted )
		if( !*best || value->ticks < (*best)->ticks )
			*best = value;
}

/* Pick a painted tile to reuse. Search for LRU (slow!).
 */
static Tile *
render_tile_get_painted( Render *render )
{
	Tile *tile;

	tile = NULL;
	g_hash_table_foreach( render->tiles,
		(GHFunc) tile_test_clean_ticks, &tile );

	if( tile ) {
		VIPS_DEBUG_MSG( "render_tile_get_painted: "
			"reusing painted %p\n", tile );
	}

	return( tile );
}

/* Ask for an area of calculated pixels. Get from cache, request calculation,
 * or if we've no threads or no notify, calculate immediately.
 */
static Tile *
render_tile_request( Render *render, VipsRegion *reg, VipsRect *area )
{
	Tile *tile;

	VIPS_DEBUG_MSG( "render_tile_request: asking for %dx%d\n",
		area->left, area->top );

	if( (tile = render_tile_lookup( render, area )) ) {
		/* We already have a tile at this position. If it's invalid,
		 * ask for a repaint.
		 */
		if( tile->region->invalid ) 
			tile_queue( tile, reg );
		else
			tile_touch( tile );
	}
	else if( render->ntiles < render->max_tiles || 
		render->max_tiles == -1 ) {
		/* We have fewer tiles than teh max. We can just make a new 
		 * tile.
		 */
		if( !(tile = tile_new( render )) ) 
			return( NULL );

		render_tile_add( tile, area );

		tile_queue( tile, reg );
	}
	else {
		/* Need to reuse a tile. Try an old painted tile.
		 */
		if( !(tile = render_tile_get_painted( render )) ) {
			VIPS_DEBUG_MSG( "render_tile_request: "
				"no tiles to reuse\n" );
			return( NULL );
		}

		render_tile_move( tile, area );

		tile_queue( tile, reg );
	}

	return( tile );
}

/* Copy what we can from the tile into the region.
 */
static void
tile_copy( Tile *tile, VipsRegion *to )
{
	VipsRect ovlap;

	/* Find common pixels.
	 */
	vips_rect_intersectrect( &tile->area, &to->valid, &ovlap );
	g_assert( !vips_rect_isempty( &ovlap ) );

	/* If the tile is painted, copy over the pixels. Otherwise, fill with
	 * zero. 
	 */
	if( tile->painted && !tile->region->invalid ) {
		int len = VIPS_IMAGE_SIZEOF_PEL( to->im ) * ovlap.width;

		int y;

		VIPS_DEBUG_MSG( "tile_copy: "
			"copying calculated pixels for %p %dx%d\n",
			tile, tile->area.left, tile->area.top ); 

		for( y = ovlap.top; y < VIPS_RECT_BOTTOM( &ovlap ); y++ ) {
			VipsPel *p = VIPS_REGION_ADDR( tile->region, 
				ovlap.left, y );
			VipsPel *q = VIPS_REGION_ADDR( to, ovlap.left, y );

			memcpy( q, p, len );
		}
	}
	else {
		VIPS_DEBUG_MSG( "tile_copy: zero filling for %p %dx%d\n",
			tile, tile->area.left, tile->area.top ); 
		vips_region_paint( to, &ovlap, 0 );
	}
}

/* Loop over the output region, filling with data from cache.
 */
static int
image_fill( VipsRegion *out, void *seq, void *a, void *b, gboolean *stop )
{
	Render *render = (Render *) b;
	int tile_width = render->tile_width;
	int tile_height = render->tile_height;
	VipsRegion *reg = (VipsRegion *) seq;
	VipsRect *r = &out->valid;

	int x, y;

	/* Find top left of tiles we need.
	 */
	int xs = (r->left / tile_width) * tile_width;
	int ys = (r->top / tile_height) * tile_height;

	VIPS_DEBUG_MSG( "image_fill: left = %d, top = %d, "
		"width = %d, height = %d\n",
                r->left, r->top, r->width, r->height );

	g_mutex_lock( render->lock );

	/* 

		FIXME ... if r fits inside a single tile, we could skip the 
		copy.

	 */

	for( y = ys; y < VIPS_RECT_BOTTOM( r ); y += tile_height )
		for( x = xs; x < VIPS_RECT_RIGHT( r ); x += tile_width ) {
			VipsRect area;
			Tile *tile;

			area.left = x;
			area.top = y;
			area.width = tile_width;
			area.height = tile_height;

			tile = render_tile_request( render, reg, &area );
			if( tile )
				tile_copy( tile, out );
			else
				VIPS_DEBUG_MSG_RED( "image_fill: argh!\n" );
		}

	g_mutex_unlock( render->lock );

	return( 0 );
}

/* The mask image is 255 / 0 for the state of painted for each tile.
 */
static int
mask_fill( VipsRegion *out, void *seq, void *a, void *b, gboolean *stop )
{
	Render *render = (Render *) a;
	int tile_width = render->tile_width;
	int tile_height = render->tile_height;
	VipsRect *r = &out->valid;

	int x, y;

	/* Find top left of tiles we need.
	 */
	int xs = (r->left / tile_width) * tile_width;
	int ys = (r->top / tile_height) * tile_height;

	VIPS_DEBUG_MSG( "mask_fill: left = %d, top = %d, "
		"width = %d, height = %d\n",
                r->left, r->top, r->width, r->height );

	g_mutex_lock( render->lock );

	for( y = ys; y < VIPS_RECT_BOTTOM( r ); y += tile_height )
		for( x = xs; x < VIPS_RECT_RIGHT( r ); x += tile_width ) {
			VipsRect area;
			Tile *tile;
			int value;

			area.left = x;
			area.top = y;
			area.width = tile_width;
			area.height = tile_height;

			tile = render_tile_lookup( render, &area );
			value = (tile &&
                                tile->painted && 
				!tile->region->invalid) ? 255 : 0;

			/* Only mark painted tiles containing valid pixels.
			 */
			vips_region_paint( out, &area, value ); 
		}

	g_mutex_unlock( render->lock );

	return( 0 );
}

/**
 * vips_sink_screen: (method)
 * @in: input image
 * @out: (out): output image
 * @mask: mask image indicating valid pixels
 * @tile_width: tile width
 * @tile_height: tile height
 * @max_tiles: maximum tiles to cache
 * @priority: rendering priority
 *
 * This operation renders @in in the background, making pixels available on
 * @out as they are calculated. Calculated pixels are kept in a cache with
 * tiles sized @tile_width by @tile_height pixels and with at most @max_tiles 
 * tiles.
 * If @max_tiles is -1, the cache is of unlimited size (up to the maximum image
 * size).
 * The @mask image is a one-band uchar image and has 255 for pixels which are 
 * currently in cache and 0 for uncalculated pixels.
 *
 * Only a single sink is calculated at any one time, though many may be
 * alive. Use @priority to indicate which renders are more important:  
 * zero means normal
 * priority, negative numbers are low priority, positive numbers high
 * priority.
 *
 * Calls to vips_region_prepare() on @out return immediately and hold 
 * whatever is
 * currently in cache for that #VipsRect (check @mask to see which parts of the
 * #VipsRect are valid). Any pixels in the #VipsRect which are not in 
 * cache are added to a queue.
 *
 * See also: vips_tilecache(), vips_region_prepare(), 
 * vips_sink_disc(), vips_sink().
 *
 * Returns: 0 on sucess, -1 on error.
 */
int
vips_sink_screen( VipsImage *in, VipsImage *out, VipsImage *mask, 
	int tile_width, int tile_height, 
	int max_tiles, 
	int priority,
	VipsSinkNotify notify_fn, void *a )
{
	Render *render;

	(void) notify_fn;
	(void) a;

	if( tile_width <= 0 || tile_height <= 0 || 
		max_tiles < -1 ) {
		vips_error( "vips_sink_screen", "%s", _( "bad parameters" ) );
		return( -1 );
	}

	if( vips_image_pio_input( in ) ||
		vips_image_pipelinev( out, 
			VIPS_DEMAND_STYLE_SMALLTILE, in, NULL ) )
		return( -1 );

	if( mask ) {
		if( vips_image_pipelinev( mask, 
			VIPS_DEMAND_STYLE_SMALLTILE, in, NULL ) )
			return( -1 );

		mask->Bands = 1;
		mask->BandFmt = VIPS_FORMAT_UCHAR;
		mask->Type = VIPS_INTERPRETATION_B_W;
		mask->Coding = VIPS_CODING_NONE;
	}

	if( !(render = render_new( in, out, mask, 
		tile_width, tile_height, max_tiles, priority )) )
		return( -1 );

	VIPS_DEBUG_MSG( "vips_sink_screen: max = %d, %p\n", max_tiles, render );

	if( vips_image_generate( out, 
		vips_start_one, image_fill, vips_stop_one, in, render ) )
		return( -1 );
	if( mask && 
		vips_image_generate( mask, 
			NULL, mask_fill, NULL, render, NULL ) )
		return( -1 );

	return( 0 );
}

int
vips__print_renders( void )
{
	int n_leaks;

	n_leaks = 0;

#ifdef VIPS_DEBUG_AMBER
	if( render_num_renders > 0 ) {
		printf( "%d active renders\n", render_num_renders );
		n_leaks += render_num_renders;
	}
#endif /*VIPS_DEBUG_AMBER*/

	return( n_leaks );
}
