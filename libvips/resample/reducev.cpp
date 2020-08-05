/* vertical reduce by a float factor with a kernel
 *
 * 29/1/16
 * 	- from shrinkv.c
 * 10/3/16
 * 	- add other kernels
 * 21/3/16
 * 	- add vector path
 * 2/4/16
 * 	- better int mask creation ... we now adjust the scale to keep the sum
 * 	  equal to the target scale
 * 15/6/16
 * 	- better accuracy with smarter multiplication
 * 15/8/16
 * 	- rename yshrink as vshrink for consistency
 * 9/9/16
 * 	- add @centre option
 * 7/3/17
 * 	- add a seq line cache
 * 6/6/20 kleisauke
 * 	- deprecate @centre option, it's now always on
 * 	- fix pixel shift
 * 	- speed up the mask construction for uchar/ushort images
 * 22/4/22 kleisauke
 * 	- add @gap option
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

/*
#define DEBUG
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <glib/gi18n-lib.h>

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <vips/vips.h>
#include <vips/debug.h>
#include <vips/internal.h>

#include "presample.h"
#include "templates.h"

typedef struct _VipsReducev {
	VipsResample parent_instance;

	double vshrink;		/* Reduce factor */
	double gap;		/* Reduce gap */

	/* The thing we use to make the kernel.
	 */
	VipsKernel kernel;

	/* Number of points in kernel.
	 */
	int n_point;

	/* Vertical displacement.
	 */
	double voffset;

	/* Precalculated interpolation matrices. int (used for pel
	 * sizes up to short), and double (for all others). We go to
	 * scale + 1 so we can round-to-nearest safely.
	 */
	int *matrixi[VIPS_TRANSFORM_SCALE + 1];
	double *matrixf[VIPS_TRANSFORM_SCALE + 1];

	/* Deprecated.
	 */
	gboolean centre;

} VipsReducev;

typedef VipsResampleClass VipsReducevClass;

/* We need C linkage for this.
 */
extern "C" {
G_DEFINE_TYPE( VipsReducev, vips_reducev, VIPS_TYPE_RESAMPLE );
}

/* You'd think this would vectorise, but gcc hates mixed types in nested loops
 * :-(
 */
template <typename T, int max_value>
static void inline
reducev_unsigned_int_tab( VipsReducev *reducev,
	VipsPel *pout, const VipsPel *pin,
	const int ne, const int lskip, const int * restrict cy )
{
	T* restrict out = (T *) pout;
	const T* restrict in = (T *) pin;
	const int n = reducev->n_point;
	const int l1 = lskip / sizeof( T );

	for( int z = 0; z < ne; z++ ) {
		int sum;

		sum = reduce_sum<T, int>( in + z, l1, cy, n );
		sum = unsigned_fixed_round( sum ); 
		sum = VIPS_CLIP( 0, sum, max_value ); 

		out[z] = sum;
	}
}

template <typename T, int min_value, int max_value>
static void inline
reducev_signed_int_tab( VipsReducev *reducev,
	VipsPel *pout, const VipsPel *pin,
	const int ne, const int lskip, const int * restrict cy )
{
	T* restrict out = (T *) pout;
	const T* restrict in = (T *) pin;
	const int n = reducev->n_point;
	const int l1 = lskip / sizeof( T );

	for( int z = 0; z < ne; z++ ) {
		int sum;

		sum = reduce_sum<T, int>( in + z, l1, cy, n );
		sum = signed_fixed_round( sum ); 
		sum = VIPS_CLIP( min_value, sum, max_value ); 

		out[z] = sum;
	}
}

/* Floating-point version.
 */
template <typename T>
static void inline
reducev_float_tab( VipsReducev *reducev,
	VipsPel *pout, const VipsPel *pin,
	const int ne, const int lskip, const double * restrict cy )
{
	T* restrict out = (T *) pout;
	const T* restrict in = (T *) pin;
	const int n = reducev->n_point;
	const int l1 = lskip / sizeof( T );

	for( int z = 0; z < ne; z++ ) 
		out[z] = reduce_sum<T, double>( in + z, l1, cy, n );
}

/* 32-bit int output needs a double intermediate.
 */

template <typename T, int max_value>
static void inline
reducev_unsigned_int32_tab( VipsReducev *reducev,
	VipsPel *pout, const VipsPel *pin,
	const int ne, const int lskip, const double * restrict cy )
{
	T* restrict out = (T *) pout;
	const T* restrict in = (T *) pin;
	const int n = reducev->n_point;
	const int l1 = lskip / sizeof( T );

	for( int z = 0; z < ne; z++ ) {
		double sum;

		sum = reduce_sum<T, double>( in + z, l1, cy, n );
		out[z] = VIPS_CLIP( 0, sum, max_value ); 
	}
}

template <typename T, int min_value, int max_value>
static void inline
reducev_signed_int32_tab( VipsReducev *reducev,
	VipsPel *pout, const VipsPel *pin,
	const int ne, const int lskip, const double * restrict cy )
{
	T* restrict out = (T *) pout;
	const T* restrict in = (T *) pin;
	const int n = reducev->n_point;
	const int l1 = lskip / sizeof( T );

	for( int z = 0; z < ne; z++ ) {
		double sum;

		sum = reduce_sum<T, double>( in + z, l1, cy, n );
		out[z] = VIPS_CLIP( min_value, sum, max_value ); 
	}
}

/* Ultra-high-quality version for double images.
 */
template <typename T>
static void inline
reducev_notab( VipsReducev *reducev,
	VipsPel *pout, const VipsPel *pin,
	const int ne, const int lskip, double y )
{
	T* restrict out = (T *) pout;
	const T* restrict in = (T *) pin;
	const int n = reducev->n_point;
	const int l1 = lskip / sizeof( T );

	double cy[MAX_POINT];

	vips_reduce_make_mask( cy, reducev->kernel, reducev->vshrink, y ); 

	for( int z = 0; z < ne; z++ ) {
		double sum;
		sum = reduce_sum<T, double>( in + z, l1, cy, n );

		out[z] = VIPS_ROUND_UINT( sum );
	}
}

static int
vips_reducev_gen( VipsRegion *out_region, void *seq, 
	void *a, void *b, gboolean *stop )
{
	VipsImage *in = (VipsImage *) a;
	VipsReducev *reducev = (VipsReducev *) b;
	VipsRegion *ir = (VipsRegion *) seq;
	VipsRect *r = &out_region->valid;

	/* Double bands for complex.
	 */
	const int bands = in->Bands * 
		(vips_band_format_iscomplex( in->BandFmt ) ?  2 : 1);
	int ne = r->width * bands;

	VipsRect s;

#ifdef DEBUG
	printf( "vips_reducev_gen: generating %d x %d at %d x %d\n",
		r->width, r->height, r->left, r->top ); 
#endif /*DEBUG*/

	s.left = r->left;
	s.top = r->top * reducev->vshrink - reducev->voffset;
	s.width = r->width;
	s.height = r->height * reducev->vshrink + reducev->n_point;
	if( vips_region_prepare( ir, &s ) )
		return( -1 );

	VIPS_GATE_START( "vips_reducev_gen: work" ); 

	double Y = (r->top + 0.5) * reducev->vshrink - 0.5 - 
		reducev->voffset;

	for( int y = 0; y < r->height; y++ ) { 
		VipsPel *q = 
			VIPS_REGION_ADDR( out_region, r->left, r->top + y );
		const int py = (int) Y;
		VipsPel *p = VIPS_REGION_ADDR( ir, r->left, py );
		const int sy = Y * VIPS_TRANSFORM_SCALE * 2;
		const int siy = sy & (VIPS_TRANSFORM_SCALE * 2 - 1);
		const int ty = (siy + 1) >> 1;
		const int *cyi = reducev->matrixi[ty];
		const double *cyf = reducev->matrixf[ty];
		const int lskip = VIPS_REGION_LSKIP( ir );

		switch( in->BandFmt ) {
		case VIPS_FORMAT_UCHAR:
			reducev_unsigned_int_tab
				<unsigned char, UCHAR_MAX>(
				reducev,
				q, p, ne, lskip, cyi );
			break;

		case VIPS_FORMAT_CHAR:
			reducev_signed_int_tab
				<signed char, SCHAR_MIN, SCHAR_MAX>(
				reducev,
				q, p, ne, lskip, cyi );
			break;

		case VIPS_FORMAT_USHORT:
			reducev_unsigned_int_tab
				<unsigned short, USHRT_MAX>(
				reducev,
				q, p, ne, lskip, cyi );
			break;

		case VIPS_FORMAT_SHORT:
			reducev_signed_int_tab
				<signed short, SHRT_MIN, SHRT_MAX>(
				reducev,
				q, p, ne, lskip, cyi );
			break;

		case VIPS_FORMAT_UINT:
			reducev_unsigned_int32_tab
				<unsigned int, INT_MAX>(
				reducev,
				q, p, ne, lskip, cyf );
			break;

		case VIPS_FORMAT_INT:
			reducev_signed_int32_tab
				<signed int, INT_MIN, INT_MAX>(
				reducev,
				q, p, ne, lskip, cyf );
			break;

		case VIPS_FORMAT_FLOAT:
		case VIPS_FORMAT_COMPLEX:
			reducev_float_tab<float>( reducev,
				q, p, ne, lskip, cyf );
			break;

		case VIPS_FORMAT_DPCOMPLEX:
		case VIPS_FORMAT_DOUBLE:
			reducev_notab<double>( reducev,
				q, p, ne, lskip, Y - py );
			break;

		default:
			g_assert_not_reached();
			break;
		}

		Y += reducev->vshrink;
	}

	VIPS_GATE_STOP( "vips_reducev_gen: work" ); 

	VIPS_COUNT_PIXELS( out_region, "vips_reducev_gen" ); 

	return( 0 );
}

static int
vips_reducev_build( VipsObject *object )
{
	VipsObjectClass *object_class = VIPS_OBJECT_GET_CLASS( object );
	VipsResample *resample = VIPS_RESAMPLE( object );
	VipsReducev *reducev = (VipsReducev *) object;
	VipsImage **t = (VipsImage **) 
		vips_object_local_array( object, 5 );

	VipsImage *in;
	int height;
	int int_vshrink;
	double extra_pixels;

	if( VIPS_OBJECT_CLASS( vips_reducev_parent_class )->build( object ) )
		return( -1 );

	in = resample->in; 

	if( reducev->vshrink < 1.0 ) { 
		vips_error( object_class->nickname, 
			"%s", _( "reduce factor should be >= 1.0" ) );
		return( -1 );
	}

	/* Output size. We need to always round to nearest, so round(), not
	 * rint().
	 */
	height = VIPS_ROUND_UINT( 
		(double) in->Ysize / reducev->vshrink );

	/* How many pixels we are inventing in the input, -ve for
	 * discarding.
	 */
	extra_pixels = height * reducev->vshrink - in->Ysize;

	if( reducev->gap > 0.0 &&
		reducev->kernel != VIPS_KERNEL_NEAREST ) {
		if( reducev->gap < 1.0 ) {
			vips_error( object_class->nickname,
				"%s", _( "reduce gap should be >= 1.0" ) );
			return( -1 );
		}

		/* The int part of our reduce.
		 */
		int_vshrink = VIPS_MAX( 1, VIPS_FLOOR( 
			(double) in->Ysize / height / reducev->gap ) );

		if( int_vshrink > 1 ) {
			g_info( "shrinkv by %d", int_vshrink );
			if( vips_shrinkv( in, &t[0], int_vshrink,
				"ceil", TRUE,
				NULL ) )
				return( -1 );
			in = t[0];

			reducev->vshrink /= int_vshrink;
			extra_pixels /= int_vshrink;
		}
	}

	if( reducev->vshrink == 1.0 ) 
		return( vips_image_write( in, resample->out ) );

	reducev->n_point = 
		vips_reduce_get_points( reducev->kernel, reducev->vshrink ); 
	g_info( "reducev: %d point mask", reducev->n_point );
	if( reducev->n_point > MAX_POINT ) {
		vips_error( object_class->nickname, 
			"%s", _( "reduce factor too large" ) );
		return( -1 );
	}

	/* If we are rounding down, we are not using some input
	 * pixels. We need to move the origin *inside* the input image
	 * by half that distance so that we discard pixels equally
	 * from left and right. 
	 */
	reducev->voffset = (1 + extra_pixels) / 2.0 - 1;

	/* Build the tables of pre-computed coefficients.
	 */
	for( int y = 0; y < VIPS_TRANSFORM_SCALE + 1; y++ ) {
		reducev->matrixf[y] = 
			VIPS_ARRAY( object, reducev->n_point, double ); 
		reducev->matrixi[y] = 
			VIPS_ARRAY( object, reducev->n_point, int ); 
		if( !reducev->matrixf[y] ||
			!reducev->matrixi[y] )
			return( -1 ); 

		vips_reduce_make_mask( reducev->matrixf[y],
			reducev->kernel, reducev->vshrink, 
			(float) y / VIPS_TRANSFORM_SCALE ); 

		for( int i = 0; i < reducev->n_point; i++ )
			reducev->matrixi[y][i] = reducev->matrixf[y][i] *
				VIPS_INTERPOLATE_SCALE;

#ifdef DEBUG
		printf( "vips_reducev_build: mask %d\n    ", y );
		for( int i = 0; i < reducev->n_point; i++ ) 
			printf( "%d ", reducev->matrixi[y][i] );
		printf( "\n" ); 
#endif /*DEBUG*/
	}

	/* Unpack for processing.
	 */
	if( vips_image_decode( in, &t[1] ) )
		return( -1 );
	in = t[1];

	/* Add new pixels around the input so we can interpolate at the edges.
	 */
	if( vips_embed( in, &t[2], 
		0, VIPS_CEIL( reducev->n_point / 2.0 ) - 1, 
		in->Xsize, in->Ysize + reducev->n_point, 
		"extend", VIPS_EXTEND_COPY,
		(void *) NULL ) )
		return( -1 );
	in = t[2];

	t[3] = vips_image_new();
	if( vips_image_pipelinev( t[3], 
		VIPS_DEMAND_STYLE_THINSTRIP, in, (void *) NULL ) )
		return( -1 );

	/* Size output. We need to always round to nearest, so round(), not
	 * rint().
	 *
	 * Don't change xres/yres, leave that to the application layer. For
	 * example, vipsthumbnail knows the true reduce factor (including the
	 * fractional part), we just see the integer part here.
	 */
	t[3]->Ysize = height;
	if( t[3]->Ysize <= 0 ) { 
		vips_error( object_class->nickname, 
			"%s", _( "image has shrunk to nothing" ) );
		return( -1 );
	}

#ifdef DEBUG
	printf( "vips_reducev_build: reducing %d x %d image to %d x %d\n", 
		in->Xsize, in->Ysize, 
		t[3]->Xsize, t[3]->Ysize );
#endif /*DEBUG*/

	if( vips_image_generate( t[3],
		vips_start_one, vips_reducev_gen, vips_stop_one, 
		in, reducev ) )
		return( -1 );

	in = t[3];

	vips_reorder_margin_hint( in, reducev->n_point );

	/* Large reducev will throw off sequential mode. Suppose thread1 is
	 * generating tile (0, 0), but stalls. thread2 generates tile
	 * (0, 1), 128 lines further down the output. After it has done,
	 * thread1 tries to generate (0, 0), but by then the pixels it needs
	 * have gone from the input image line cache if the reducev is large.
	 *
	 * To fix this, put another seq on the output of reducev. Now we'll
	 * always have the previous XX lines of the shrunk image, and we won't
	 * fetch out of order. 
	 */
	if( vips_image_is_sequential( in ) ) { 
		g_info( "reducev sequential line cache" ); 

		if( vips_sequential( in, &t[4], 
			"tile_height", 10,
			// "trace", TRUE,
			(void *) NULL ) )
			return( -1 );
		in = t[4];
	}

	if( vips_image_write( in, resample->out ) )
		return( -1 ); 

	return( 0 );
}

static void
vips_reducev_class_init( VipsReducevClass *reducev_class )
{
	GObjectClass *gobject_class = G_OBJECT_CLASS( reducev_class );
	VipsObjectClass *vobject_class = VIPS_OBJECT_CLASS( reducev_class );
	VipsOperationClass *operation_class = 
		VIPS_OPERATION_CLASS( reducev_class );

	VIPS_DEBUG_MSG( "vips_reducev_class_init\n" );

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	vobject_class->nickname = "reducev";
	vobject_class->description = _( "shrink an image vertically" );
	vobject_class->build = vips_reducev_build;

	operation_class->flags = VIPS_OPERATION_SEQUENTIAL;

	VIPS_ARG_DOUBLE( reducev_class, "vshrink", 3, 
		_( "Vshrink" ), 
		_( "Vertical shrink factor" ),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET( VipsReducev, vshrink ),
		1.0, 1000000.0, 1.0 );

	VIPS_ARG_ENUM( reducev_class, "kernel", 4, 
		_( "Kernel" ), 
		_( "Resampling kernel" ),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET( VipsReducev, kernel ),
		VIPS_TYPE_KERNEL, VIPS_KERNEL_LANCZOS3 );

	VIPS_ARG_DOUBLE( reducev_class, "gap", 5, 
		_( "Gap" ), 
		_( "Reducing gap" ),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET( VipsReducev, gap ),
		0.0, 1000000.0, 0.0 );

	/* Old name.
	 */
	VIPS_ARG_DOUBLE( reducev_class, "yshrink", 3, 
		_( "Yshrink" ), 
		_( "Vertical shrink factor" ),
		VIPS_ARGUMENT_REQUIRED_INPUT | VIPS_ARGUMENT_DEPRECATED,
		G_STRUCT_OFFSET( VipsReducev, vshrink ),
		1.0, 1000000.0, 1.0 );

	/* We used to let people pick centre or corner, but it's automatic now.
	 */
	VIPS_ARG_BOOL( reducev_class, "centre", 7,
		_( "Centre" ),
		_( "Use centre sampling convention" ),
		VIPS_ARGUMENT_OPTIONAL_INPUT | VIPS_ARGUMENT_DEPRECATED,
		G_STRUCT_OFFSET( VipsReducev, centre ),
		FALSE );

}

static void
vips_reducev_init( VipsReducev *reducev )
{
	reducev->gap = 0.0;
	reducev->kernel = VIPS_KERNEL_LANCZOS3;
}

/* See reduce.c for the doc comment.
 */

int
vips_reducev( VipsImage *in, VipsImage **out, double vshrink, ... )
{
	va_list ap;
	int result;

	va_start( ap, vshrink );
	result = vips_call_split( "reducev", ap, in, out, vshrink );
	va_end( ap );

	return( result );
}
