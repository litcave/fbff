#include <stdint.h>
#include <unistd.h>
#include <sys/timeb.h>
#include <alsa/asoundlib.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include "draw.h"

#define ZOOM			1
#define ZOOM2			1

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
static int goalbr;

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
	int nr = MIN(vcc->height * ZOOM, fb_rows() / ZOOM2);
	int nc = MIN(vcc->width * ZOOM, fb_cols() / ZOOM2);
	int i;
	for (r = 0; r < nr; r++) {
		unsigned char *row = frame->data[0] + r * frame->linesize[0];
		for (c = 0; c < nc; c++) {
			fbval_t v = fb_color(row[c * 3],
					row[c * 3 + 1],
					row[c * 3 + 2]);
			for (i = 0; i < ZOOM2; i++)
				buf[c * ZOOM2 + i] = v;
		}
		for (i = 0; i < ZOOM2; i++)
			fb_set(r * ZOOM2 + i, 0, buf, nc * ZOOM2);
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

static void read_frames(void)
{
	AVFrame *main_frame = avcodec_alloc_frame();
	AVPacket pkt;
	uint8_t *buf;
	int n = AUDIOBUFSIZE;
	if (vcc)
		n = avpicture_get_size(PIX_FMT_RGB24, vcc->width * ZOOM,
					   vcc->height * ZOOM);
	buf = av_malloc(n * sizeof(uint8_t));
	if (vcc)
		avpicture_fill((AVPicture *) frame, buf, PIX_FMT_RGB24,
				vcc->width * ZOOM, vcc->height * ZOOM);
	while (av_read_frame(fc, &pkt) >= 0) {
		if (vcc && pkt.stream_index == vsi)
			decode_video_frame(main_frame, &pkt);
		if (acc && pkt.stream_index == asi)
			decode_audio_frame(&pkt);
		av_free_packet(&pkt);
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
	goalbr = acc->sample_rate * bps;
	snd_pcm_prepare(alsa);
}

static void alsa_close(void)
{
	snd_pcm_close(alsa);
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		printf("usage: %s filename\n", argv[0]);
		return 1;
	}
	av_register_all();
	if (av_open_input_file(&fc, argv[1], NULL, 0, NULL))
		return 1;
	if (av_find_stream_info(fc) < 0)
		return 1;
	dump_format(fc, 0, "input", 0);
	init_streams();
	frame = avcodec_alloc_frame();
	if (acc)
		alsa_init();
	if (vcc) {
		swsc = sws_getContext(vcc->width, vcc->height, vcc->pix_fmt,
			vcc->width * ZOOM, vcc->height * ZOOM,
			PIX_FMT_RGB24, SWS_FAST_BILINEAR | SWS_CPU_CAPS_MMX2,
			NULL, NULL, NULL);
		fb_init();
	}

	read_frames();

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
