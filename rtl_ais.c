/*
 * Copyright (C) 2012 by Kyle Keen <keenerd@gmail.com>
 *
 * This program is free software: you can redistribsetute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
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


/* todo
 * support left > right
 * thread left/right channels
 * more array sharing
 * something to correct for clock drift (look at demod's dc bias?)
 * 4x oversampling (with cic up/down)
 * droop correction
 * alsa integration
 * better upsampler (libsamplerate?)
 * windows support
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef WIN32
	#include <fcntl.h>
#endif

#include <pthread.h>

#include <rtl-sdr.h>

#include "rtl_ais.h"
#include "convenience.h"
#include "aisdecoder/aisdecoder.h"

#define DEFAULT_ASYNC_BUF_NUMBER	12
#define DEFAULT_BUF_LENGTH		(16 * 16384)
#define AUTO_GAIN			-100

/* signals are not threadsafe by default */
#define safe_cond_signal(n, m) pthread_mutex_lock(m); pthread_cond_signal(n); pthread_mutex_unlock(m)
#define safe_cond_wait(n, m) pthread_mutex_lock(m); pthread_cond_wait(n, m); pthread_mutex_unlock(m)

struct downsample_state
{
	int16_t  *buf;
	int      len_in;
	int      len_out;
	int      rate_in;
	int      rate_out;
	int      downsample;
	int      downsample_passes;
	int16_t  lp_i_hist[10][6];
	int16_t  lp_q_hist[10][6];
	pthread_rwlock_t rw;
	//droop compensation
	int16_t  droop_i_hist[9];
	int16_t  droop_q_hist[9];

};

struct demod_state
{
	int16_t  *buf;
	int      buf_len;
	int16_t  *result;
	int      result_len;
	int      now_r, now_j;
	int      pre_r, pre_j;
	int      dc_avg;  // really should get its own struct

};

struct upsample_stereo
{
	int16_t *buf_left;
	int16_t *buf_right;
	int16_t *result;
	int     bl_len;
	int     br_len;
	int     result_len;
	int     rate;
};

int cic_9_tables[][10] = {
	{0,},
	{9, -156,  -97, 2798, -15489, 61019, -15489, 2798,  -97, -156},
	{9, -128, -568, 5593, -24125, 74126, -24125, 5593, -568, -128},
	{9, -129, -639, 6187, -26281, 77511, -26281, 6187, -639, -129},
	{9, -122, -612, 6082, -26353, 77818, -26353, 6082, -612, -122},
	{9, -120, -602, 6015, -26269, 77757, -26269, 6015, -602, -120},
	{9, -120, -582, 5951, -26128, 77542, -26128, 5951, -582, -120},
	{9, -119, -580, 5931, -26094, 77505, -26094, 5931, -580, -119},
	{9, -119, -578, 5921, -26077, 77484, -26077, 5921, -578, -119},
	{9, -119, -577, 5917, -26067, 77473, -26067, 5917, -577, -119},
	{9, -199, -362, 5303, -25505, 77489, -25505, 5303, -362, -199},
};


static void rotate_90(int16_t *buf, int len)
/* 90 rotation is 1+0j, 0+1j, -1+0j, 0-1j
   or [0, 1, -3, 2, -4, -5, 7, -6] */
{
	int i;
	int16_t tmp;
	for (i=0; i<len; i+=8) {
		tmp = buf[i+2];
		buf[i+2] = -buf[i+3];
		buf[i+3] = tmp;

		buf[i+4] = -buf[i+4];
		buf[i+5] = -buf[i+5];

		tmp = buf[i+6];
		buf[i+6] = buf[i+7];
		buf[i+7] = -tmp;
	}
}

static void rotate_m90(int16_t *buf, int len)
/* -90 rotation is 1+0j, 0-1j, -1+0j, 0+1j
   or [0, 1, 3, -2, -4, -5, -7, 6] */
{
	int i;
	int16_t tmp;
	for (i=0; i<len; i+=8) {
		tmp = buf[i+2];
		buf[i+2] = buf[i+3];
		buf[i+3] = -tmp;

		buf[i+4] = -buf[i+4];
		buf[i+5] = -buf[i+5];

		tmp = buf[i+6];
		buf[i+6] = -buf[i+7];
		buf[i+7] = tmp;
	}
}

static void fifth_order(int16_t *data, int length, int16_t *hist)
/* for half of interleaved data */
{
	int i;
	int16_t a, b, c, d, e, f;
	a = hist[2];
	b = hist[3];
	c = hist[4];
	d = hist[5];
	e = data[0];
	f = data[2];
	/* a downsample should improve resolution, so don't fully shift */
	data[0] = (a + (b+e)*5 + (c+d)*10 + f) >> 4;
	for (i=4; i<length; i+=4) {
		a = c;
		b = d;
		c = e;
		d = f;
		e = data[i];
		f = data[i+2];
		data[i/2] = (a + (b+e)*5 + (c+d)*10 + f) >> 4;
	}
	/* archive */
	hist[0] = a;
	hist[1] = b;
	hist[2] = c;
	hist[3] = d;
	hist[4] = e;
	hist[5] = f;
}

static void generic_fir(int16_t *data, int length, const int *fir, int16_t *hist)
/* Okay, not at all generic.  Assumes length 9, fix that eventually. */
{
	int d, temp, sum;
	for (d=0; d<length; d+=2) {
		temp = data[d];
		sum = 0;
		sum += (hist[0] + hist[8]) * fir[1];
		sum += (hist[1] + hist[7]) * fir[2];
		sum += (hist[2] + hist[6]) * fir[3];
		sum += (hist[3] + hist[5]) * fir[4];
		sum +=            hist[4]  * fir[5];
		data[d] = sum >> 15 ;
		hist[0] = hist[1];
		hist[1] = hist[2];
		hist[2] = hist[3];
		hist[3] = hist[4];
		hist[4] = hist[5];
		hist[5] = hist[6];
		hist[6] = hist[7];
		hist[7] = hist[8];
		hist[8] = temp;
	}
}

static void downsample(struct downsample_state *d)
{
	int i, ds_p;
	ds_p = d->downsample_passes;
	for (i=0; i<ds_p; i++) 
	{
		fifth_order(d->buf,   (d->len_in >> i),   d->lp_i_hist[i]);
		fifth_order(d->buf+1, (d->len_in >> i)-1, d->lp_q_hist[i]);
	}
	// droop compensation
	generic_fir(d->buf, d->len_in >> ds_p,cic_9_tables[ds_p], d->droop_i_hist);
	generic_fir(d->buf+1, (d->len_in>> ds_p)-1,cic_9_tables[ds_p], d->droop_q_hist);
}

static void multiply(int ar, int aj, int br, int bj, int *cr, int *cj)
{
	*cr = ar*br - aj*bj;
	*cj = aj*br + ar*bj;
}

#if 0 // not used
static int polar_discriminant(int ar, int aj, int br, int bj)
{
	int cr, cj;
	double angle;
	multiply(ar, aj, br, -bj, &cr, &cj);
	angle = atan2((double)cj, (double)cr);
	return (int)(angle / 3.14159 * (1<<14));
}
#endif

static int fast_atan2(int y, int x)
/* pre scaled for int16 */
{
	int yabs, angle;
	int pi4=(1<<12), pi34=3*(1<<12);  // note pi = 1<<14
	if (x==0 && y==0) {
		return 0;
	}
	yabs = y;
	if (yabs < 0) {
		yabs = -yabs;
	}
	if (x >= 0) {
		angle = pi4  - pi4 * (x-yabs) / (x+yabs);
	} else {
		angle = pi34 - pi4 * (x+yabs) / (yabs-x);
	}
	if (y < 0) {
		return -angle;
	}
	return angle;
}

static int polar_disc_fast(int ar, int aj, int br, int bj)
{
	int cr, cj;
	multiply(ar, aj, br, -bj, &cr, &cj);
	return fast_atan2(cj, cr);
}

static void demodulate(struct demod_state *d)
{
	int i, pcm;
	int16_t *buf = d->buf;
	int16_t *result = d->result;
	pcm = polar_disc_fast(buf[0], buf[1],
		d->pre_r, d->pre_j);
	
	result[0] = (int16_t)pcm;
	for (i = 2; i < (d->buf_len-1); i += 2) {
		// add the other atan types?
		pcm = polar_disc_fast(buf[i], buf[i+1],
			buf[i-2], buf[i-1]);
		result[i/2] = (int16_t)pcm;
	}
	d->pre_r = buf[d->buf_len - 2];
	d->pre_j = buf[d->buf_len - 1];
}

static void dc_block_filter(struct demod_state *d)
{
	int i, avg;
	int64_t sum = 0;
	int16_t *result = d->result;
	for (i=0; i < d->result_len; i++) {
		sum += result[i];
	}
	avg = sum / d->result_len;
	avg = (avg + d->dc_avg * 9) / 10;
	for (i=0; i < d->result_len; i++) {
		result[i] -= avg;
	}
	d->dc_avg = avg;
}

static void arbitrary_upsample(int16_t *buf1, int16_t *buf2, int len1, int len2)
/* linear interpolation, len1 < len2 */
{
	int i = 1;
	int j = 0;
	int tick = 0;
	double frac;  // use integers...
	while (j < len2) {
		frac = (double)tick / (double)len2;
		buf2[j] = (int16_t)((double)buf1[i-1]*(1-frac) + (double)buf1[i]*frac);
		j++;
		tick += len1;
		if (tick > len2) {
			tick -= len2;
			i++;
		}
		if (i >= len1) {
			i = len1 - 1;
			tick = len2;
		}
	}
}

struct rtl_ais_context
{
        int active, dc_filter, use_internal_aisdecoder;

        pthread_t demod_thread;
        pthread_t rtlsdr_thread;

        pthread_cond_t ready;
        pthread_mutex_t ready_m;

        rtlsdr_dev_t *dev;
        FILE *file;

        /* complex iq pairs */
        struct downsample_state both;
        struct downsample_state left;
        struct downsample_state right;
        /* iq pairs and real mono */
        struct demod_state left_demod;
        struct demod_state right_demod;
        /* real stereo pairs (upsampled) */
        struct upsample_stereo stereo;
};

static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *arg)
{
        struct rtl_ais_context *ctx = arg;
	unsigned i;
	if (!ctx->active) {
		return;}
	pthread_rwlock_wrlock(&ctx->both.rw);
	for (i=0; i<len; i++)
		ctx->both.buf[i] = ((int16_t)buf[i]) - 127;

	pthread_rwlock_unlock(&ctx->both.rw);
	safe_cond_signal(&ctx->ready, &ctx->ready_m);
}

static void *rtlsdr_thread_fn(void *arg)
{
        struct rtl_ais_context *ctx = arg;
        rtlsdr_read_async(ctx->dev, rtlsdr_callback, arg,
                          DEFAULT_ASYNC_BUF_NUMBER,
                          DEFAULT_BUF_LENGTH);
        
        ctx->active = 0;
        return 0;
}

static void pre_output(struct rtl_ais_context *ctx)
{
	int i;
	for (i=0; i<ctx->stereo.bl_len; i++) {
		ctx->stereo.result[i*2]   = ctx->stereo.buf_left[i];
		ctx->stereo.result[i*2+1] = ctx->stereo.buf_right[i];
	}
}

static void *demod_thread_fn(void *arg)
{
        struct rtl_ais_context *ctx = arg;
        while (ctx->active) {
                safe_cond_wait(&ctx->ready, &ctx->ready_m);
                pthread_rwlock_wrlock(&ctx->both.rw);
		downsample(&ctx->both);
		memcpy(ctx->left.buf,  ctx->both.buf, 2*ctx->both.len_out);
		memcpy(ctx->right.buf, ctx->both.buf, 2*ctx->both.len_out);
		pthread_rwlock_unlock(&ctx->both.rw);
		rotate_90(ctx->left.buf, ctx->left.len_in);
		downsample(&ctx->left);
		memcpy(ctx->left_demod.buf, ctx->left.buf, 2*ctx->left.len_out);
		demodulate(&ctx->left_demod);
		if (ctx->dc_filter) {
			dc_block_filter(&ctx->left_demod);}
		//if (oversample) {
		//	downsample(&left);}
		//fprintf(stderr,"\nUpsample result_len:%d stereo.bl_len:%d :%f\n",left_demod.result_len,stereo.bl_len,(float)stereo.bl_len/(float)left_demod.result_len);
		arbitrary_upsample(ctx->left_demod.result, ctx->stereo.buf_left, ctx->left_demod.result_len, ctx->stereo.bl_len);
		rotate_m90(ctx->right.buf, ctx->right.len_in);
		downsample(&ctx->right);
		memcpy(ctx->right_demod.buf, ctx->right.buf, 2*ctx->right.len_out);
		demodulate(&ctx->right_demod);
		if (ctx->dc_filter) {
			dc_block_filter(&ctx->right_demod);}
		//if (oversample) {
		//	downsample(&right);}
		arbitrary_upsample(ctx->right_demod.result, ctx->stereo.buf_right, ctx->right_demod.result_len, ctx->stereo.br_len);
		pre_output(ctx);
		if(ctx->use_internal_aisdecoder){
			// stereo.result -> int_16
			// stereo.result_len -> number of samples for each channel
			run_rtlais_decoder(ctx->stereo.result,ctx->stereo.result_len);
		}
		else{
                    fwrite(ctx->stereo.result, 2, ctx->stereo.result_len, ctx->file);
		}
	}

        free_ais_decoder();
	return 0;
}

static void downsample_init(struct downsample_state *dss)
/* simple ints should be already set */
{
	int i, j;
	dss->buf = malloc(dss->len_in * sizeof(int16_t));
	dss->rate_out = dss->rate_in / dss->downsample;
	
	//dss->downsample_passes = (int)log2(dss->downsample);
	dss->len_out = dss->len_in / dss->downsample;
	for (i=0; i<10; i++) { for (j=0; j<6; j++) {
		dss->lp_i_hist[i][j] = 0;
		dss->lp_q_hist[i][j] = 0;
	}}
	pthread_rwlock_init(&dss->rw, NULL);
}

static void demod_init(struct demod_state *ds)
{
	ds->buf = malloc(ds->buf_len * sizeof(int16_t));
	ds->result = malloc(ds->result_len * sizeof(int16_t));
	ds->dc_avg=0;
}

static void stereo_init(struct upsample_stereo *us)
{
	us->buf_left  = malloc(us->bl_len * sizeof(int16_t));
	us->buf_right = malloc(us->br_len * sizeof(int16_t));
	us->result    = malloc(us->result_len * sizeof(int16_t));
}

void rtl_ais_default_config(struct rtl_ais_config *config)
{
        config->gain = AUTO_GAIN; /* tenths of a dB */
        config->dev_index = 0;
        config->dev_given = 0;
        config->ppm_error = 0;
        config->rtl_agc=0;
        config->custom_ppm = 0;
        config->left_freq = 161975000;
        config->right_freq = 162025000;
        config->sample_rate = 24000;
        config->output_rate = 48000;
	config->dc_filter=1;
        config->edge = 0;
        config->use_tcp_listener = 0, config->tcp_keep_ais_time = 15;
		config->tcp_stream_forever = 0;
	config->use_internal_aisdecoder=1;
	config->seconds_for_decoder_stats=0;
        /* Aisdecoder */
        config->show_levels=0;
        config->debug_nmea = 0;

        config->host=NULL;
        config->port=NULL;

        config->filename = "-";

        config->add_sample_num = 0;
}

struct rtl_ais_context *rtl_ais_start(struct rtl_ais_config *config)
{
	if (config->left_freq > config->right_freq)
		return NULL;

        struct rtl_ais_context *ctx = malloc(sizeof(struct rtl_ais_context));
        ctx->active = 1;

        /* precompute rates */
        int dongle_freq, dongle_rate, delta, i;
        dongle_freq = config->left_freq/2 + config->right_freq/2;
	if (config->edge) {
		dongle_freq -= config->sample_rate/2;}
	delta = config->right_freq - config->left_freq;
        if (delta > 1.2e6) {
		fprintf(stderr, "Frequencies may be at most 1.2MHz apart.");
		exit(1);
	}
	if (delta < 0) {
		fprintf(stderr, "Left channel must be lower than right channel.");
		exit(1);
	}
	i = (int)log2(2.4e6 / delta);
	dongle_rate = delta * (1<<i);
	ctx->both.rate_in = dongle_rate;
	ctx->both.rate_out = delta * 2;
	i = (int)log2(ctx->both.rate_in/ctx->both.rate_out);
	ctx->both.downsample_passes = i;
	ctx->both.downsample = 1 << i;
	ctx->left.rate_in = ctx->both.rate_out;
	i = (int)log2(ctx->left.rate_in / config->sample_rate);
	ctx->left.downsample_passes = i;
	ctx->left.downsample = 1 << i;
	ctx->left.rate_out = ctx->left.rate_in / ctx->left.downsample;
	
	ctx->right.rate_in = ctx->left.rate_in;
	ctx->right.rate_out = ctx->left.rate_out;
	ctx->right.downsample = ctx->left.downsample;
	ctx->right.downsample_passes = ctx->left.downsample_passes;
	ctx->dc_filter=config->dc_filter;
	if (ctx->left.rate_out > config->output_rate) {
		fprintf(stderr, "Channel bandwidth too high or output bandwidth too low.");
		exit(1);
	}

        fprintf(stderr, "Buffer size: %0.2f mS\n", 1000 * (double)DEFAULT_BUF_LENGTH / (double)dongle_rate);
	fprintf(stderr, "Downsample factor: %i\n", ctx->both.downsample * ctx->left.downsample);
	fprintf(stderr, "Low pass: %i Hz\n", ctx->left.rate_out);
	fprintf(stderr, "Output: %i Hz\n", config->output_rate);

	/* precompute lengths */
	ctx->both.len_in  = DEFAULT_BUF_LENGTH;
	ctx->both.len_out = ctx->both.len_in / ctx->both.downsample;
	ctx->left.len_in  = ctx->both.len_out;
	ctx->right.len_in = ctx->both.len_out;
	ctx->left.len_out = ctx->left.len_in / ctx->left.downsample;
	ctx->right.len_out = ctx->right.len_in / ctx->right.downsample;
	ctx->left_demod.buf_len = ctx->left.len_out;
	ctx->left_demod.result_len = ctx->left_demod.buf_len / 2;
	ctx->right_demod.buf_len = ctx->left_demod.buf_len;
	ctx->right_demod.result_len = ctx->left_demod.result_len;
//	stereo.bl_len = (int)((long)(DEFAULT_BUF_LENGTH/2) * (long)output_rate / (long)dongle_rate); -> Doesn't work on Linux
	ctx->stereo.bl_len = (int)((double)(DEFAULT_BUF_LENGTH/2) * (double)config->output_rate / (double)dongle_rate);
	ctx->stereo.br_len = ctx->stereo.bl_len;
	ctx->stereo.result_len = ctx->stereo.br_len * 2;
	ctx->stereo.rate = config->output_rate;

	if (!config->dev_given) {
		config->dev_index = verbose_device_search("0");
        }

	if (config->dev_index < 0) {
		exit(1);
	}

	downsample_init(&ctx->both);
	downsample_init(&ctx->left);
	downsample_init(&ctx->right);
	demod_init(&ctx->left_demod);
	demod_init(&ctx->right_demod);
	stereo_init(&ctx->stereo);

	int r = rtlsdr_open(&ctx->dev, (uint32_t)config->dev_index);
	if (r < 0) {
		fprintf(stderr, "Failed to open rtlsdr device #%d.\n", config->dev_index);
		exit(1);
	}

        if(!config->use_internal_aisdecoder){
		if (strcmp(config->filename, "-") == 0) { /* Write samples to stdout */
			ctx->file = stdout;
	#ifdef WIN32		
			setmode(fileno(stdout), O_BINARY); // Binary mode, avoid text mode
	#endif		
			setvbuf(stdout, NULL, _IONBF, 0);
		} else {
			ctx->file = fopen(config->filename, "wb");
			if (!ctx->file) {
                            fprintf(stderr, "Failed to open %s\n", config->filename);
    exit(1);
			}
		}
	}
	else{ // Internal AIS decoder
            int ret=init_ais_decoder(config->host,config->port,config->show_levels,config->debug_nmea,ctx->stereo.bl_len,config->seconds_for_decoder_stats, config->use_tcp_listener, config->tcp_keep_ais_time, config->tcp_stream_forever, config->add_sample_num);
		if(ret != 0){
			fprintf(stderr,"Error initializing built-in AIS decoder\n");
			rtlsdr_cancel_async(ctx->dev);
			rtlsdr_close(ctx->dev);
			exit(1);
		}
	}
        ctx->use_internal_aisdecoder = config->use_internal_aisdecoder;

	/* Set the tuner gain */
	if (config->gain == AUTO_GAIN) {
		verbose_auto_gain(ctx->dev);
	} else {
		config->gain = nearest_gain(ctx->dev, config->gain);
		verbose_gain_set(ctx->dev, config->gain);
	}
	if(config->rtl_agc){
		int r = rtlsdr_set_agc_mode(ctx->dev, 1);
		if(r<0)	{
			fprintf(stderr,"Error seting RTL AGC mode ON");
			exit(1);
		}
		else {
			fprintf(stderr,"RTL AGC mode ON\n");
		}
	}
	if (!config->custom_ppm) {
		verbose_ppm_eeprom(ctx->dev, &config->ppm_error);
	}
	
	verbose_ppm_set(ctx->dev, config->ppm_error);
	
	/* Set the tuner frequency */
	verbose_set_frequency(ctx->dev, dongle_freq);

	/* Set the sample rate */
	verbose_set_sample_rate(ctx->dev, dongle_rate);

	/* Reset endpoint before we start reading from it (mandatory) */
	verbose_reset_buffer(ctx->dev);

	pthread_cond_init(&ctx->ready, NULL);
	pthread_mutex_init(&ctx->ready_m, NULL);

        /* create two threads */
	pthread_create(&ctx->demod_thread, NULL, demod_thread_fn, ctx);
        pthread_create(&ctx->rtlsdr_thread, NULL, rtlsdr_thread_fn, ctx);

        return ctx;
}

int rtl_ais_isactive(struct rtl_ais_context *ctx)
{
        return ctx->active;
}

const char *rtl_ais_next_message(struct rtl_ais_context *ctx)
{
        (void)(ctx); //unused for now
        return aisdecoder_next_message();
}

void rtl_ais_cleanup(struct rtl_ais_context *ctx)
{
	rtlsdr_cancel_async(ctx->dev);
        ctx->active = 0;

        pthread_detach(ctx->demod_thread);
        pthread_detach(ctx->rtlsdr_thread);

	if (ctx->file != stdout) {
		if(ctx->file)
			fclose(ctx->file);
	}

	rtlsdr_cancel_async(ctx->dev);
	safe_cond_signal(&ctx->ready, &ctx->ready_m);
	pthread_cond_destroy(&ctx->ready);
	pthread_mutex_destroy(&ctx->ready_m);

        rtlsdr_close(ctx->dev);

        free(ctx);
}


// vim: tabstop=8:softtabstop=8:shiftwidth=8:noexpandtab
