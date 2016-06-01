
#define __STDC_CONSTANT_MACROS

#include <sys/types.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <math.h>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/avutil.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/pixelutils.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavformat/version.h>

#include <libswscale/swscale.h>
#include <libswscale/version.h>

#include <libswresample/swresample.h>
#include <libswresample/version.h>
}

using namespace std;

#include <string>
#include <vector>

string		input_file;
string		output_file;

/* return a floating point value specifying what to scale the sample
 * value by to reduce it from full volume to dB decibels */
double dBFS(double dB)
{
	/* 10 ^ (dB / 20),
	   based on reversing the formula for converting samples to decibels:
	   dB = 20.0 * log10(sample);
	   where "sample" is -1.0 <= x <= 1.0 */
	return pow(10.0,dB / 20.0);
}

/* attenuate a sample value by this many dBFS */
/* so if you want to reduce it by 20dBFS you pass -20 as dB */
double attenuate_dBFS(double sample,double dB)
{
	return sample * dBFS(dB);
}

/* opposite: convert sample to decibels */
double dBFS_measure(double sample) {
	return 20.0 * log10(sample);
}

// lowpass filter
// you can make it a highpass filter by applying a lowpass then subtracting from source.
class LowpassFilter {
public:
	LowpassFilter() : timeInterval(0), cutoff(0), alpha(0), prev(0), tau(0) {
	}
	void setFilter(const double rate/*sample rate of audio*/,const double hz/*cutoff*/) {
#ifndef M_PI
#error your math.h does not include M_PI constant
#endif
		timeInterval = 1.0 / rate;
		tau = 1 / (hz * 2 * M_PI);
		cutoff = hz;
		alpha = timeInterval / (tau + timeInterval);
	}
	void resetFilter() {
		prev = 0;
	}
	double lowpass(const double sample) {
		const double stage1 = sample * alpha;
		const double stage2 = prev - (prev * alpha); /* NTS: Instead of prev * (1.0 - alpha) */
		return (prev = (stage1 + stage2)); /* prev = stage1+stage2 then return prev */
	}
	double highpass(const double sample) {
		const double stage1 = sample * alpha;
		const double stage2 = prev - (prev * alpha); /* NTS: Instead of prev * (1.0 - alpha) */
		return sample - (prev = (stage1 + stage2)); /* prev = stage1+stage2 then return (sample - prev) */
	}
public:
	double			timeInterval;
	double			cutoff;
	double			alpha; /* timeInterval / (tau + timeInterval) */
	double			prev;
	double			tau;
};

class HiLoPair {
public:
	LowpassFilter		hi,lo;	// highpass, lowpass
public:
	void setFilter(const double rate/*sample rate of audio*/,const double low_hz,const double high_hz) {
		lo.setFilter(rate,low_hz);
		hi.setFilter(rate,high_hz);
	}
	double filter(const double sample) {
		return hi.highpass(lo.lowpass(sample)); /* first lowpass, then highpass */
	}
};

class HiLoPass : public vector<HiLoPair> { // all passes, one sample of one channel
public:
	HiLoPass() : vector() { }
public:
	void setFilter(const double rate/*sample rate of audio*/,const double low_hz,const double high_hz) {
		for (size_t i=0;i < size();i++) (*this)[i].setFilter(rate,low_hz,high_hz);
	}
	double filter(double sample) {
		for (size_t i=0;i < size();i++) sample = (*this)[i].lo.lowpass(sample);
		for (size_t i=0;i < size();i++) sample = (*this)[i].hi.highpass(sample);
		return sample;
	}
	void init(const unsigned int passes) {
		clear();
		resize(passes);
		assert(size() >= passes);
	}
};

class HiLoSample : public vector<HiLoPass> { // all passes, all channels of one sample period
public:
	HiLoSample() : vector() { }
public:
	void init(const unsigned int channels,const unsigned int passes) {
		clear();
		resize(channels);
		assert(size() >= channels);
		for (size_t i=0;i < size();i++) (*this)[i].init(passes);
	}
	void setFilter(const double rate/*sample rate of audio*/,const double low_hz,const double high_hz) {
		for (size_t i=0;i < size();i++) (*this)[i].setFilter(rate,low_hz,high_hz);
	}
};

class HiLoComboPass {
public:
	HiLoComboPass() : passes(0), channels(0), rate(0), low_cutoff(0), high_cutoff(0) {
	}
	~HiLoComboPass() {
		clear();
	}
	void setChannels(const size_t _channels) {
		if (channels != _channels) {
			clear();
			channels = _channels;
		}
	}
	void setCutoff(const double _low_cutoff,const double _high_cutoff) {
		if (low_cutoff != _low_cutoff || high_cutoff != _high_cutoff) {
			clear();
			low_cutoff = _low_cutoff;
			high_cutoff = _high_cutoff;
		}
	}
	void setRate(const double _rate) {
		if (rate != _rate) {
			clear();
			rate = _rate;
		}
	}
	void setPasses(const size_t _passes) {
		if (passes != _passes) {
			clear();
			passes = _passes;
		}
	}
	void clear() {
		audiostate.clear();
	}
	void init() {
		clear();
		if (channels == 0 || passes == 0 || rate == 0 || low_cutoff == 0 || high_cutoff == 0) return;
		audiostate.init(channels,passes);
		audiostate.setFilter(rate,low_cutoff,high_cutoff);
	}
public:
	double		rate;
	size_t		passes;
	size_t		channels;
	double		low_cutoff;
	double		high_cutoff;
	HiLoSample	audiostate;
};

HiLoComboPass		audio_hilopass;

// preemphsis emuluation
LowpassFilter		audio_linear_preemphasis_pre[2];
LowpassFilter		audio_linear_preemphasis_post[2];

AVFormatContext*	input_avfmt = NULL;
AVStream*		input_avstream_audio = NULL;	// do not free
AVCodecContext*		input_avstream_audio_codec_context = NULL; // do not free
AVStream*		input_avstream_video = NULL;	// do not free
AVCodecContext*		input_avstream_video_codec_context = NULL; // do not free
AVFrame*		input_avstream_audio_frame = NULL;
AVFrame*		input_avstream_video_frame = NULL;

struct SwrContext*	input_avstream_audio_resampler = NULL;
struct SwsContext*	input_avstream_video_resampler = NULL;

AVFormatContext*	output_avfmt = NULL;
AVStream*		output_avstream_audio = NULL;	// do not free
AVCodecContext*		output_avstream_audio_codec_context = NULL; // do not free
AVStream*		output_avstream_video = NULL;	// do not free
AVCodecContext*		output_avstream_video_codec_context = NULL; // do not free

AVRational	output_field_rate = { 60000, 1001 };	// NTSC 60Hz default
int		output_width = 720;
int		output_height = 480;
bool		output_ntsc = true;	// NTSC color subcarrier emulation
bool		output_pal = false;	// PAL color subcarrier emulation
int		output_audio_channels = 2;	// VHS stereo (set to 1 for mono)
int		output_audio_rate = 44100;	// VHS Hi-Fi goes up to 20KHz
double		output_audio_linear_buzz = -42;	// how loud the "buzz" is audible in dBFS (S/N). Ever notice on old VHS tapes (prior to Hi-Fi) you can almost hear the video signal sync pulses in the audio?
double		output_audio_highpass = 20; // highpass to filter out below 20Hz
double		output_audio_lowpass = 20000; // lowpass to filter out above 20KHz
// NTS:
//   VHS Hi-Fi: 20Hz - 20KHz                  (70dBFS S/N)
//   VHS SP:    100Hz - 10KHz                 (42dBFS S/N)
//   VHS LP:    100Hz - 7KHz (right??)        (42dBFS S/N)
//   VHS EP:    100Hz - 4KHz                  (42dBFS S/N)
bool		output_vhs_hifi = true;
bool		output_vhs_linear_stereo = false; // not common
bool		output_vhs_linear_audio = false; // if true (non Hi-Fi) then we emulate hiss and noise of linear VHS tracks including the video sync pulses audible in the audio.

enum {
	VHS_SP=0,
	VHS_LP,
	VHS_EP
};

int		output_vhs_tape_speed = VHS_SP;

static inline int clips16(const int x) {
	if (x < -32768)
		return -32768;
	else if (x > 32767)
		return 32767;

	return x;
}

void composite_audio_process(int16_t *audio,unsigned int samples) { // number of channels = output_audio_channels, sample rate = output_audio_rate. audio is interleaved.
	assert(audio_hilopass.audiostate.size() >= output_audio_channels);

	for (unsigned int s=0;s < samples;s++,audio += output_audio_channels) {
		for (unsigned int c=0;c < output_audio_channels;c++) {
			double s;

			s = (double)audio[c] / 32768;
			s = audio_hilopass.audiostate[c].filter(s);

			if (true/*TODO: Whether to emulate preemphasis*/) {
				for (unsigned int i=0;i < output_audio_channels;i++) {
					s = s + audio_linear_preemphasis_pre[i].highpass(s);
				}
			}

			/* analog limiting (when the signal is too loud) */
			if (s > 1.0)
				s = 1.0;
			else if (s < -1.0)
				s = -1.0;

			if (true/*TODO: Whether to emulate preemphasis. Personal experience also says some VCRs do not do this stage if they auto-sense the audio is too muffled*/) {
				for (unsigned int i=0;i < output_audio_channels;i++) {
					s = audio_linear_preemphasis_post[i].lowpass(s);
				}
			}

			audio[c] = clips16(s * 32768);
		}
	}
}

void preset_PAL() {
	output_field_rate.num = 50;
	output_field_rate.den = 1;
	output_height = 576;
	output_width = 720;
	output_pal = true;
	output_ntsc = false;
}

void preset_NTSC() {
	output_field_rate.num = 60000;
	output_field_rate.den = 1001;
	output_height = 480;
	output_width = 720;
	output_pal = false;
	output_ntsc = true;
}
	
static void help(const char *arg0) {
	fprintf(stderr,"%s [options]\n",arg0);
	fprintf(stderr," -i <input file>\n");
	fprintf(stderr," -o <output file>\n");
	fprintf(stderr," -tvstd <pal|ntsc>\n");
	fprintf(stderr," -vhs-hifi <0|1>    (default on)\n");
	fprintf(stderr," -vhs-speed <ep|lp|sp>     (default sp)\n");
	fprintf(stderr,"\n");
	fprintf(stderr," Output file will be up/down converted to 720x480 (NTSC 29.97fps) or 720x576 (PAL 25fps).\n");
	fprintf(stderr," Output will be rendered as interlaced video.\n");
}

static int parse_argv(int argc,char **argv) {
	const char *a;
	int i;

	for (i=1;i < argc;) {
		a = argv[i++];

		if (*a == '-') {
			do { a++; } while (*a == '-');

			if (!strcmp(a,"h") || !strcmp(a,"help")) {
				help(argv[0]);
				return 1;
			}
			else if (!strcmp(a,"i")) {
				input_file = argv[i++];
			}
			else if (!strcmp(a,"o")) {
				output_file = argv[i++];
			}
			else if (!strcmp(a,"vhs-speed")) {
				a = argv[i++];

				if (!strcmp(a,"ep")) {
					output_vhs_tape_speed = VHS_EP;
				}
				else if (!strcmp(a,"lp")) {
					output_vhs_tape_speed = VHS_LP;
				}
				else if (!strcmp(a,"sp")) {
					output_vhs_tape_speed = VHS_SP;
				}
				else {
					fprintf(stderr,"Unknown vhs tape speed '%s'\n",a);
					return 1;
				}
			}
			else if (!strcmp(a,"vhs-hifi")) {
				int x = atoi(argv[i++]);
				output_vhs_hifi = (x > 0);
				output_vhs_linear_audio = !output_vhs_hifi;
			}
			else if (!strcmp(a,"tvstd")) {
				a = argv[i++];

				if (!strcmp(a,"pal")) {
					preset_PAL();
				}
				else if (!strcmp(a,"ntsc")) {
					preset_NTSC();
				}
				else {
					fprintf(stderr,"Unknown tv std '%s'\n",a);
					return 1;
				}
			}
			else {
				fprintf(stderr,"Unknown switch '%s'\n",a);
				return 1;
			}
		}
		else {
			fprintf(stderr,"Unhandled arg '%s'\n",a);
			return 1;
		}
	}

	if (output_vhs_hifi) {
		output_audio_highpass = 20; // highpass to filter out below 20Hz
		output_audio_lowpass = 20000; // lowpass to filter out above 20KHz
		output_audio_channels = 2;
	}
	else if (output_vhs_linear_audio) {
		switch (output_vhs_tape_speed) {
			case VHS_SP:
				output_audio_highpass = 100; // highpass to filter out below 100Hz
				output_audio_lowpass = 10000; // lowpass to filter out above 10KHz
				break;
			case VHS_LP:
				output_audio_highpass = 100; // highpass to filter out below 100Hz
				output_audio_lowpass = 7000; // lowpass to filter out above 7KHz
				break;
			case VHS_EP:
				output_audio_highpass = 100; // highpass to filter out below 100Hz
				output_audio_lowpass = 4000; // lowpass to filter out above 4KHz
				break;
		}

		if (!output_vhs_linear_stereo)
			output_audio_channels = 1;
		else
			output_audio_channels = 2;
	}

	if (input_file.empty() || output_file.empty()) {
		fprintf(stderr,"You must specify an input and output file (-i and -o).\n");
		return 1;
	}

	return 0;
}

int main(int argc,char **argv) {
	if (parse_argv(argc,argv))
		return 1;

	av_register_all();
	avformat_network_init();
	avcodec_register_all();

	assert(input_avfmt == NULL);
	if (avformat_open_input(&input_avfmt,input_file.c_str(),NULL,NULL) < 0) {
		fprintf(stderr,"Failed to open input file\n");
		return 1;
	}

	if (avformat_find_stream_info(input_avfmt,NULL) < 0)
		fprintf(stderr,"WARNING: Did not find stream info on input\n");

	/* scan streams for one video, one audio */
	{
		size_t i;
		AVStream *is;
		AVCodecContext *isctx;

		fprintf(stderr,"Input format: %u streams found\n",input_avfmt->nb_streams);
		for (i=0;i < (size_t)input_avfmt->nb_streams;i++) {
			is = input_avfmt->streams[i];
			if (is == NULL) continue;

			isctx = is->codec;
			if (isctx == NULL) continue;

			if (isctx->codec_type == AVMEDIA_TYPE_AUDIO) {
				if (input_avstream_audio == NULL) {
					if (avcodec_open2(isctx,avcodec_find_decoder(isctx->codec_id),NULL) >= 0) {
						input_avstream_audio = is;
						input_avstream_audio_codec_context = isctx;
						fprintf(stderr,"Found audio stream idx=%zu\n",i);
					}
					else {
						fprintf(stderr,"Found audio stream but not able to decode\n");
					}
				}
			}
			else if (isctx->codec_type == AVMEDIA_TYPE_VIDEO) {
				if (avcodec_open2(isctx,avcodec_find_decoder(isctx->codec_id),NULL) >= 0) {
					input_avstream_video = is;
					input_avstream_video_codec_context = isctx;
					fprintf(stderr,"Found video stream idx=%zu\n",i);
				}
				else {
					fprintf(stderr,"Found video stream but not able to decode\n");
				}
			}
		}

		if (input_avstream_video == NULL && input_avstream_audio == NULL) {
			fprintf(stderr,"Neither video nor audio found\n");
			return 1;
		}
	}

	assert(output_avfmt == NULL);
	if (avformat_alloc_output_context2(&output_avfmt,NULL,NULL,output_file.c_str()) < 0) {
		fprintf(stderr,"Failed to open output file\n");
		return 1;
	}

	if (input_avstream_audio != NULL) {
		output_avstream_audio = avformat_new_stream(output_avfmt, NULL);
		if (output_avstream_audio == NULL) {
			fprintf(stderr,"Unable to create output audio stream\n");
			return 1;
		}

		output_avstream_audio_codec_context = output_avstream_audio->codec;
		if (output_avstream_audio_codec_context == NULL) {
			fprintf(stderr,"Output stream audio no codec context?\n");
			return 1;
		}

		if (output_audio_channels == 2)
			output_avstream_audio_codec_context->channel_layout = AV_CH_LAYOUT_STEREO;
		else
			output_avstream_audio_codec_context->channel_layout = AV_CH_LAYOUT_MONO;

		output_avstream_audio_codec_context->sample_rate = output_audio_rate;
		output_avstream_audio_codec_context->channels = output_audio_channels;
		output_avstream_audio_codec_context->sample_fmt = AV_SAMPLE_FMT_S16;
		output_avstream_audio_codec_context->time_base = (AVRational){1, output_audio_rate};
		output_avstream_audio->time_base = output_avstream_audio_codec_context->time_base;

		if (output_avfmt->oformat->flags & AVFMT_GLOBALHEADER)
			output_avstream_audio_codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

		if (avcodec_open2(output_avstream_audio_codec_context,avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE),NULL) < 0) {
			fprintf(stderr,"Output stream cannot open codec\n");
			return 1;
		}

		input_avstream_audio_resampler = swr_alloc();
		av_opt_set_int(input_avstream_audio_resampler, "in_channel_layout", input_avstream_audio_codec_context->channel_layout, 0);
		av_opt_set_int(input_avstream_audio_resampler, "out_channel_layout", output_avstream_audio_codec_context->channel_layout, 0);
		av_opt_set_int(input_avstream_audio_resampler, "in_sample_rate", input_avstream_audio_codec_context->sample_rate, 0);
		av_opt_set_int(input_avstream_audio_resampler, "out_sample_rate", output_avstream_audio_codec_context->sample_rate, 0);
		av_opt_set_sample_fmt(input_avstream_audio_resampler, "in_sample_fmt", input_avstream_audio_codec_context->sample_fmt, 0);
		av_opt_set_sample_fmt(input_avstream_audio_resampler, "out_sample_fmt", output_avstream_audio_codec_context->sample_fmt, 0);
		if (swr_init(input_avstream_audio_resampler) < 0) {
			fprintf(stderr,"Failed to init audio resampler\n");
			return 1;
		}

		fprintf(stderr,"Audio resampler init %uHz -> %uHz\n",
			input_avstream_audio_codec_context->sample_rate,
			output_avstream_audio_codec_context->sample_rate);
	}

	if (input_avstream_video != NULL) {
		output_avstream_video = avformat_new_stream(output_avfmt, NULL);
		if (output_avstream_video == NULL) {
			fprintf(stderr,"Unable to create output video stream\n");
			return 1;
		}

		output_avstream_video_codec_context = output_avstream_video->codec;
		if (output_avstream_video_codec_context == NULL) {
			fprintf(stderr,"Output stream video no codec context?\n");
			return 1;
		}

		avcodec_get_context_defaults3(output_avstream_video_codec_context,avcodec_find_encoder(AV_CODEC_ID_H264));
		output_avstream_video_codec_context->width = output_width;
		output_avstream_video_codec_context->height = output_height;
		output_avstream_video_codec_context->sample_aspect_ratio = (AVRational){output_height*4, output_width*3};
		output_avstream_video_codec_context->pix_fmt = AV_PIX_FMT_YUV422P;
		output_avstream_video_codec_context->time_base = (AVRational){output_field_rate.den, (output_field_rate.num/2)}; // NTS: divide by 2 to convert fields -> frames
		output_avstream_video_codec_context->gop_size = 15;
		output_avstream_video_codec_context->max_b_frames = 0;
		output_avstream_video->time_base = output_avstream_video_codec_context->time_base;

		if (output_avfmt->oformat->flags & AVFMT_GLOBALHEADER)
			output_avstream_video_codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

		// our output is interlaced
		output_avstream_video_codec_context->flags |= CODEC_FLAG_INTERLACED_DCT;

		if (avcodec_open2(output_avstream_video_codec_context,avcodec_find_encoder(AV_CODEC_ID_H264),NULL) < 0) {
			fprintf(stderr,"Output stream cannot open codec\n");
			return 1;
		}
	}

	if (!(output_avfmt->oformat->flags & AVFMT_NOFILE)) {
		if (avio_open(&output_avfmt->pb, output_file.c_str(), AVIO_FLAG_WRITE) < 0) {
			fprintf(stderr,"Output file cannot open file\n");
			return 1;
		}
	}

	if (avformat_write_header(output_avfmt,NULL) < 0) {
		fprintf(stderr,"Failed to write header\n");
		return 1;
	}

	/* prepare audio filtering */
	audio_hilopass.setChannels(output_audio_channels);
	audio_hilopass.setRate(output_audio_rate);
	audio_hilopass.setCutoff(output_audio_lowpass,output_audio_highpass); // hey, our filters aren't perfect
	audio_hilopass.setPasses(8);
	audio_hilopass.init();

	if (true/*TODO: whether to emulate preemphasis*/) {
		for (unsigned int i=0;i < output_audio_channels;i++) {
			audio_linear_preemphasis_pre[i].setFilter(output_audio_rate,10000/*FIXME: Guess! Also let user set this.*/);
			audio_linear_preemphasis_post[i].setFilter(output_audio_rate,10000/*FIXME: Guess! Also let user set this.*/);
		}
	}

	/* prepare audio decoding */
	input_avstream_audio_frame = av_frame_alloc();
	if (input_avstream_audio_frame == NULL) {
		fprintf(stderr,"Failed to alloc audio frame\n");
		return 1;
	}

	/* prepare audio decoding */
	input_avstream_video_frame = av_frame_alloc();
	if (input_avstream_video_frame == NULL) {
		fprintf(stderr,"Failed to alloc video frame\n");
		return 1;
	}

	// PARSE
	{
		uint8_t **dst_data = NULL;
		int dst_data_alloc_samples = 0;
		int dst_data_linesize = 0;
		int dst_data_samples = 0;
		int got_frame = 0;
		AVPacket pkt;

		av_init_packet(&pkt);
		while (av_read_frame(input_avfmt,&pkt) >= 0) {
			if (input_avstream_audio != NULL && pkt.stream_index == input_avstream_audio->index) {
				av_packet_rescale_ts(&pkt,input_avstream_audio->time_base,output_avstream_audio->time_base);
				if (avcodec_decode_audio4(input_avstream_audio_codec_context,input_avstream_audio_frame,&got_frame,&pkt) >= 0) {
					if (got_frame != 0 && input_avstream_audio_frame->nb_samples != 0) {
						dst_data_samples = av_rescale_rnd(
							swr_get_delay(input_avstream_audio_resampler, input_avstream_audio_frame->sample_rate) + input_avstream_audio_frame->nb_samples,
							output_avstream_audio_codec_context->sample_rate, input_avstream_audio_frame->sample_rate, AV_ROUND_UP);

						if (dst_data == NULL || dst_data_samples > dst_data_alloc_samples) {
							if (dst_data != NULL) {
								av_freep(&dst_data[0]); // NTS: Why??
								av_freep(&dst_data);
							}

							dst_data_alloc_samples = 0;
							fprintf(stderr,"Allocating audio buffer %u samples\n",(unsigned int)dst_data_samples);
							if (av_samples_alloc_array_and_samples(&dst_data,&dst_data_linesize,
								output_avstream_audio_codec_context->channels,dst_data_samples,
								output_avstream_audio_codec_context->sample_fmt, 0) >= 0) {
								dst_data_alloc_samples = dst_data_samples;
							}
							else {
								fprintf(stderr,"Failure to allocate audio buffer\n");
								dst_data_alloc_samples = 0;
							}
						}

						if (dst_data != NULL) {
							int out_samples;

							if ((out_samples=swr_convert(input_avstream_audio_resampler,dst_data,dst_data_samples,
								(const uint8_t**)input_avstream_audio_frame->data,input_avstream_audio_frame->nb_samples)) > 0) {
								// PROCESS THE AUDIO. At this point by design the code can assume S16LE (16-bit PCM interleaved)
								composite_audio_process((int16_t*)dst_data[0],out_samples);
								// write it out. TODO: At some point, support conversion to whatever the codec needs and then convert to it.
								// that way we can render directly to MP4 our VHS emulation.
								AVPacket dstpkt;
								av_init_packet(&dstpkt);
								if (av_new_packet(&dstpkt,out_samples * 2 * output_audio_channels) >= 0) { // NTS: Will reset fields too!
									assert(dstpkt.data != NULL);
									assert(dstpkt.size >= (out_samples * 2 * output_audio_channels));
									memcpy(dstpkt.data,dst_data[0],out_samples * 2 * output_audio_channels);
								}
								av_packet_copy_props(&dstpkt,&pkt);
								dstpkt.stream_index = output_avstream_audio->index;
								if (av_interleaved_write_frame(output_avfmt,&dstpkt) < 0)
									fprintf(stderr,"Failed to write frame\n");
								av_packet_unref(&dstpkt);
							}
							else if (out_samples < 0) {
								fprintf(stderr,"Failed to resample audio\n");
							}
						}
					}
				}
				else {
					fprintf(stderr,"No audio decoded\n");
				}
			}
			else if (input_avstream_video != NULL && pkt.stream_index == input_avstream_video->index) {
			}
			else {
			}

			av_packet_unref(&pkt);
			av_init_packet(&pkt);
		}

		if (dst_data != NULL) {
			av_freep(&dst_data[0]); // NTS: Why??
			av_freep(&dst_data);
		}
	}

	if (input_avstream_video_frame != NULL)
		av_frame_free(&input_avstream_video_frame);
	if (input_avstream_audio_frame != NULL)
		av_frame_free(&input_avstream_audio_frame);
	audio_hilopass.clear();
	av_write_trailer(output_avfmt);
	if (input_avstream_video_resampler != NULL) {
		sws_freeContext(input_avstream_video_resampler);
		input_avstream_video_resampler = NULL;
	}
	if (input_avstream_audio_resampler != NULL)
		swr_free(&input_avstream_audio_resampler);
	if (output_avfmt != NULL && !(output_avfmt->oformat->flags & AVFMT_NOFILE))
		avio_closep(&output_avfmt->pb);
	avformat_free_context(output_avfmt);
	avformat_close_input(&input_avfmt);
	return 0;
}

