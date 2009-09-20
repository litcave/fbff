/*
 * fbff - A small ffmpeg-based framebuffer/alsa media player
 *
 * Copyright (C) 2009 Ali Gholami Rudi
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, as published by the
 * Free Software Foundation.
 */
#include <fcntl.h>
#include <pty.h>
#include <stdint.h>
#include <termios.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include "draw.h"

static AVFormatContext *fc;
static AVFrame *frame;
static struct SwsContext *swsc;

static int vsi = -1;		/* video stream index */
static AVCodecContext *vcc;	/* video codec context */
static AVCodec *vc;		/* video codec */

static int asi = -1;		/* audio stream index */
static AVCodecContext *acc;	/* audio codec context */
static AVCodec *ac;		/* audio codec */

static snd_pcm_t *alsa;
static int bps; 		/* bytes per sample */
static int arg;
static struct termios termios;
static uint64_t pts;

static int zoom = 1;
static int magnify = 0;

static void init_streams(void)
{
	int i;
	for (i = 0; i < fc->nb_streams; i++) {
		if (fc->streams[i]->codec->codec_type == CODEC_TYPE_VIDEO)
			vsi = i;
		if (fc->streams[i]->codec->codec_type == CODEC_TYPE_AUDIO)
			asi = i;
	}
	if (vsi != -1) {
		vcc = fc->streams[vsi]->codec;
		vc = avcodec_find_decoder(vcc->codec_id);
		avcodec_open(vcc, vc);
	}
	if (asi != -1) {
		acc = fc->streams[asi]->codec;
		ac = avcodec_find_decoder(acc->codec_id);
		avcodec_open(acc, ac);
	}
}

static void draw_frame(void)
{
	fbval_t buf[1 << 14];
	int r, c;
	int nr = MIN(vcc->height * zoom, fb_rows() / magnify);
	int nc = MIN(vcc->width * zoom, fb_cols() / magnify);
	int i;
	for (r = 0; r < nr; r++) {
		unsigned char *row = frame->data[0] + r * frame->linesize[0];
		for (c = 0; c < nc; c++) {
			fbval_t v = fb_color(row[c * 3],
					row[c * 3 + 1],
					row[c * 3 + 2]);
			for (i = 0; i < magnify; i++)
				buf[c * magnify + i] = v;
		}
		for (i = 0; i < magnify; i++)
			fb_set(r * magnify + i, 0, buf, nc * magnify);
	}
}

static void decode_video_frame(AVFrame *main_frame, AVPacket *packet)
{
	int fine = 0;
	avcodec_decode_video2(vcc, main_frame, &fine, packet);
	if (fine) {
		sws_scale(swsc, main_frame->data, main_frame->linesize,
			  0, vcc->height, frame->data, frame->linesize);
		draw_frame();
	}
}

#define AUDIOBUFSIZE		(1 << 20)

static void decode_audio_frame(AVPacket *pkt)
{
	char buf[AUDIOBUFSIZE];
	AVPacket tmppkt;
	tmppkt.size = pkt->size;
	tmppkt.data = pkt->data;
	while (tmppkt.size > 0) {
		int size = sizeof(buf);
		int len = avcodec_decode_audio3(acc, (int16_t *) buf,
						&size, &tmppkt);
		if (len < 0)
			break;
		if (size <= 0)
			continue;
		if (snd_pcm_writei(alsa, buf, size / bps) < 0)
			snd_pcm_prepare(alsa);
		tmppkt.size -= len;
		tmppkt.data += len;
	}
}
static int readkey(void)
{
	char b;
	if (read(STDIN_FILENO, &b, 1) <= 0)
		return -1;
	return b;
}

static void waitkey(void)
{
	struct pollfd ufds[1];
	ufds[0].fd = STDIN_FILENO;
	ufds[0].events = POLLIN;
	poll(ufds, 1, -1);
}

static int ffarg(void)
{
	int n = arg;
	arg = 0;
	return n ? n : 1;
}

static int ffpos(void)
{
	int idx = vsi != -1 ? vsi : asi;
	double base = av_q2d(fc->streams[idx]->time_base);
	return pts * 1000.0 * base / 1000.0;
}

static int fflen(void)
{
	return fc->duration / AV_TIME_BASE;
}

static void ffjmp(int n, int rel)
{
	int t = MAX(0, MIN(fflen(), rel ? ffpos() + n : n));
	av_seek_frame(fc, -1, t * AV_TIME_BASE, AVSEEK_FLAG_ANY);
}

static void printinfo(void)
{
	int loc = (int64_t) ffpos() * 1000 / fflen();
	int pos = ffpos();
	printf("fbff:   %3d.%d%%   %8ds\r", loc / 10, loc % 10, pos);
	fflush(stdout);
}

#define SHORTJMP	(1 << 3)
#define NORMJMP		(SHORTJMP << 4)
#define LONGJMP		(NORMJMP << 4)

#define FF_PLAY			0
#define FF_PAUSE		1
#define FF_EXIT			2

static int execkey(void)
{
	int c;
	while ((c = readkey()) != -1) {
		switch (c) {
		case 'q':
			return FF_EXIT;
		case 'l':
			ffjmp(ffarg() * SHORTJMP, 1);
			break;
		case 'h':
			ffjmp(-ffarg() * SHORTJMP, 1);
			break;
		case 'j':
			ffjmp(ffarg() * NORMJMP, 1);
			break;
		case 'k':
			ffjmp(-ffarg() * NORMJMP, 1);
			break;
		case 'J':
			ffjmp(ffarg() * LONGJMP, 1);
			break;
		case 'K':
			ffjmp(-ffarg() * LONGJMP, 1);
			break;
		case '%':
			if (arg)
				ffjmp(ffarg() * fflen() / 100, 0);
			break;
		case 'i':
			printinfo();
			break;
		case ' ':
		case 'p':
			return FF_PAUSE;
			break;
		case 27:
			arg = 0;
			break;
		default:
			if (isdigit(c))
				arg = arg * 10 + c - '0';
		}
	}
	return FF_PLAY;
}

static void read_frames(void)
{
	AVFrame *main_frame = avcodec_alloc_frame();
	AVPacket pkt;
	uint8_t *buf;
	int n = AUDIOBUFSIZE;
	if (vcc)
		n = avpicture_get_size(PIX_FMT_RGB24, vcc->width * zoom,
					   vcc->height * zoom);
	buf = av_malloc(n * sizeof(uint8_t));
	if (vcc)
		avpicture_fill((AVPicture *) frame, buf, PIX_FMT_RGB24,
				vcc->width * zoom, vcc->height * zoom);
	while (av_read_frame(fc, &pkt) >= 0) {
		if (pts < pkt.pts && pkt.pts < (1ull << 60))
			pts = pkt.pts;
		if (vcc && pkt.stream_index == vsi)
			decode_video_frame(main_frame, &pkt);
		if (acc && pkt.stream_index == asi)
			decode_audio_frame(&pkt);
		av_free_packet(&pkt);
		switch (execkey()) {
		case FF_PLAY:
			continue;
		case FF_PAUSE:
			while (readkey() != 'p')
				waitkey();
			continue;
		}
		break;
	}
	av_free(buf);
	av_free(main_frame);
}

#define ALSADEV			"default"

static void alsa_init(void)
{
	int format = SND_PCM_FORMAT_S16_LE;
	if (snd_pcm_open(&alsa, ALSADEV, SND_PCM_STREAM_PLAYBACK, 0) < 0)
		return;
	snd_pcm_set_params(alsa, format, SND_PCM_ACCESS_RW_INTERLEAVED,
			acc->channels, acc->sample_rate, 1, 500000);
	bps = acc->channels * snd_pcm_format_physical_width(format) / 8;
	snd_pcm_prepare(alsa);
}

static void alsa_close(void)
{
	snd_pcm_close(alsa);
}

static void term_setup(void)
{
	struct termios newtermios;
	tcgetattr(STDIN_FILENO, &termios);
	newtermios = termios;
	cfmakeraw(&newtermios);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &newtermios);
	fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
}

static void term_cleanup(void)
{
	tcsetattr(STDIN_FILENO, 0, &termios);
}

static void read_args(int argc, char *argv[])
{
	int i = 1;
	while (i < argc) {
		if (!strcmp(argv[i], "-m"))
			magnify = atoi(argv[++i]);
		if (!strcmp(argv[i], "-z"))
			zoom = atoi(argv[++i]);
		i++;
	}
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		printf("usage: %s [-z zoom] [-m magnify] filename\n", argv[0]);
		return 1;
	}
	read_args(argc, argv);
	av_register_all();
	if (av_open_input_file(&fc, argv[argc - 1], NULL, 0, NULL))
		return 1;
	if (av_find_stream_info(fc) < 0)
		return 1;
	init_streams();
	frame = avcodec_alloc_frame();
	if (acc)
		alsa_init();
	if (vcc) {
		swsc = sws_getContext(vcc->width, vcc->height, vcc->pix_fmt,
			vcc->width * zoom, vcc->height * zoom,
			PIX_FMT_RGB24, SWS_FAST_BILINEAR | SWS_CPU_CAPS_MMX2,
			NULL, NULL, NULL);
		fb_init();
		if (!magnify)
			magnify = fb_rows() / vcc->height / zoom;
	}

	term_setup();
	read_frames();
	term_cleanup();

	if (vcc) {
		fb_free();
		sws_freeContext(swsc);
		avcodec_close(vcc);
	}
	if (acc) {
		alsa_close();
		avcodec_close(acc);
	}
	av_free(frame);
	av_close_input_file(fc);
	return 0;
}
