/*
    libmad - MPEG audio decoder library
    Copyright (C) 2000-2004 Underbit Technologies, Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

    $Id: frame.h,v 1.20 2004/01/23 09:41:32 rob Exp $
*/

# ifndef LIBMAD_FRAME_H
# define LIBMAD_FRAME_H

# include "fixed.h"
# include "timer.h"
# include "stream.h"

enum mad_layer {
    MAD_LAYER_I   = 1,			/* Layer I */
    MAD_LAYER_II  = 2,			/* Layer II */
    MAD_LAYER_III = 3			/* Layer III */
};

enum mad_mode {
    MAD_MODE_SINGLE_CHANNEL = 0,		/* single channel */
    MAD_MODE_DUAL_CHANNEL	  = 1,		/* dual channel */
    MAD_MODE_JOINT_STEREO	  = 2,		/* joint (MS/intensity) stereo */
    MAD_MODE_STEREO	  = 3		/* normal LR stereo */
};

enum mad_emphasis {
    MAD_EMPHASIS_NONE	  = 0,		/* no emphasis */
    MAD_EMPHASIS_50_15_US	  = 1,		/* 50/15 microseconds emphasis */
    MAD_EMPHASIS_CCITT_J_17 = 3,		/* CCITT J.17 emphasis */
    MAD_EMPHASIS_RESERVED   = 2		/* unknown emphasis */
};

struct mad_header {
    enum mad_layer layer;			/* audio layer (1, 2, or 3) */
    enum mad_mode mode;			/* channel mode (see above) */
    int mode_extension;			/* additional mode info */
    enum mad_emphasis emphasis;		/* de-emphasis to use (see above) */

    unsigned long bitrate;		/* stream bitrate (bps) */
    unsigned int samplerate;		/* sampling frequency (Hz) */

    unsigned short crc_check;		/* frame CRC accumulator */
    unsigned short crc_target;		/* final target CRC checksum */

    int flags;				/* flags (see below) */
    int private_bits;			/* private bits (see below) */

    mad_timer_t duration;			/* audio playing time of frame */
};

/* PicoDeck: the frame's big arrays, which the caller binds
   (mad_frame_bind) so that each can live where it is cheapest to reach:
   SRAM or QMI PSRAM through the XIP cache the app's core shares (issue
   #28). struct mad_frame_mem holds all of them. sbsample holds one
   granule: each is synthesised before the next is decoded. */
struct mad_frame_mem {
    mad_fixed_t sbsample[2][18][32];	/* synthesis subband filter samples */
    mad_fixed_t overlap[2][32][18];	/* Layer III block overlap data */
    mad_fixed_t xr_raw[576 * 2];
    mad_fixed_t tmp[576];
};

struct mad_frame {
    struct mad_header header;		/* MPEG audio header */

    int options;				/* decoding options (from stream) */

    mad_fixed_t (*sbsample)[18][32];	/* [2] (mad_frame_bind) */
    mad_fixed_t (*overlap)[32][18];	/* [2] */
    mad_fixed_t *xr_raw;		/* [576 * 2] */
    mad_fixed_t *tmp;			/* [576] */

    /* PicoDeck: a Layer III frame between mad_frame_decode_begin() and
       mad_frame_decode_end() (layer3.c's struct l3_ctx) */
    unsigned char l3[320] __attribute__((aligned(4)));
};

# define MAD_NCHANNELS(header)		((header)->mode ? 2 : 1)
# define MAD_NSBSAMPLES(header)  \
  ((header)->layer == MAD_LAYER_I ? 12 :  \
   (((header)->layer == MAD_LAYER_III &&  \
     ((header)->flags & MAD_FLAG_LSF_EXT)) ? 18 : 36))

enum {
    MAD_FLAG_NPRIVATE_III	= 0x0007,	/* number of Layer III private bits */
    MAD_FLAG_INCOMPLETE	= 0x0008,	/* header but not data is decoded */

    MAD_FLAG_PROTECTION	= 0x0010,	/* frame has CRC protection */
    MAD_FLAG_COPYRIGHT	= 0x0020,	/* frame is copyright */
    MAD_FLAG_ORIGINAL	= 0x0040,	/* frame is original (else copy) */
    MAD_FLAG_PADDING	= 0x0080,	/* frame has additional slot */

    MAD_FLAG_I_STEREO	= 0x0100,	/* uses intensity joint stereo */
    MAD_FLAG_MS_STEREO	= 0x0200,	/* uses middle/side joint stereo */
    MAD_FLAG_FREEFORMAT	= 0x0400,	/* uses free format bitrate */

    MAD_FLAG_LSF_EXT	= 0x1000,	/* lower sampling freq. extension */
    MAD_FLAG_MC_EXT	= 0x2000,	/* multichannel audio extension */
    MAD_FLAG_MPEG_2_5_EXT	= 0x4000	/* MPEG 2.5 (unofficial) extension */
};

enum {
    MAD_PRIVATE_HEADER	= 0x0100,	/* header private bit */
    MAD_PRIVATE_III	= 0x001f	/* Layer III private bits (up to 5) */
};

void mad_header_init(struct mad_header *);

# define mad_header_finish(header)  /* nothing */

int mad_header_decode(struct mad_header *, struct mad_stream *);

/* PicoDeck: point the frame's arrays at mem's; before mad_frame_init()
   (which clears them), and again for any array moved elsewhere. */
void mad_frame_bind(struct mad_frame *, struct mad_frame_mem *);
void mad_frame_init(struct mad_frame *);
void mad_frame_finish(struct mad_frame *);

/* PicoDeck: a frame is decoded a granule at a time (mad_frame_decode is
   gone: the frame's sbsample holds one granule). After
   mad_frame_decode_begin() returns 0, decode granules 0 ..
   mad_frame_granules() - 1 in order, synthesising each
   (mad_synth_granule) before decoding the next (stop at the first that
   fails), then mad_frame_decode_end() (its result is the frame's). Between
   the calls the stream's buffer must stay where it is. */
int mad_frame_decode_begin(struct mad_frame *, struct mad_stream *);
int mad_frame_decode_granule(struct mad_frame *, struct mad_stream *,
                             unsigned int);
int mad_frame_decode_end(struct mad_frame *, struct mad_stream *);
unsigned int mad_frame_granules(struct mad_frame const *);

void mad_frame_mute(struct mad_frame *);

# endif
