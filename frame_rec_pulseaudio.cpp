#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>

#include <cstdio>
#include <string>

#include "frame_rec_pulseaudio.hpp"

#ifndef AV_CODEC_ID_MPEG4
#define AV_CODEC_ID_MPEG4 CODEC_ID_MPEG4
#endif

#define TIMEBASE 600.0


extern double get_current_time();

static FILE* pa_dbg_samples_out = nullptr;
static pa_mainloop_api* pa_global_api = nullptr;

static constexpr pa_sample_spec ss = {
	.format = PA_SAMPLE_S16LE,
	.rate = 44100,
	.channels = 2
};



// PulseAudio callbacks
static void pa_get_server_info_callback(pa_context* c, const pa_server_info* i, void* userdata) {
	frame_recorder* fr = reinterpret_cast<frame_recorder*>(userdata);

	printf("[%s] PA default sink name=\"%s\"\n", __func__, i->default_sink_name);
	fr->default_sink = i->default_sink_name;
	pa_global_api->quit(pa_global_api, 1);
}

static void pa_get_sink_info_callback(pa_context* c, const pa_sink_info* i, int is_last, void* userdata) {
	if (is_last) {
		pa_operation_unref(pa_context_get_server_info(c, pa_get_server_info_callback, userdata));
		return;
	}

	printf("[%s] PA sink name=\"%s\" descr=\"%s\"\n", __func__, i->name, i->description);
}

static void pa_get_source_info_callback(pa_context* c, const pa_source_info* i, int is_last, void* userdata) {
	frame_recorder* fr = reinterpret_cast<frame_recorder*>(userdata);

	if (is_last != 0) {
		pa_operation_unref(pa_context_get_sink_info_list(c, pa_get_sink_info_callback, userdata));
		return;
	}

	printf("[%s] PA source name=\"%s\" descr=\"%s\"\n", __func__, i->name, i->description);

	if (i->monitor_of_sink == PA_INVALID_INDEX)
		return;

	printf("\tsink monitor name=\"%s\"\n", i->monitor_of_sink_name);
	fr->monitor_sources.insert(std::pair<std::string, std::string>(i->monitor_of_sink_name, i->name));
}

static void pa_context_state_callback(pa_context* c, void* userdata) {
	if (pa_context_get_state(c) != PA_CONTEXT_READY)
		return;

	pa_operation_unref(pa_context_get_source_info_list(c, pa_get_source_info_callback, userdata));
}



static AVFrame* alloc_picture(int pix_fmt, int width, int height) {
	AVFrame* video_frame = avcodec_alloc_frame();
	uint8_t* picture_buf = nullptr;

	if (video_frame == nullptr)
		return nullptr;

    if ((picture_buf = av_malloc(avpicture_get_size(pix_fmt, width, height))) == nullptr) {
		av_free(video_frame);
		return nullptr;
	}

	avpicture_fill((AVPicture*) video_frame, picture_buf, pix_fmt, width, height);
	video_frame->width = width;
	video_frame->height = height;
	return video_frame;
}



frame_recorder::frame_recorder(const char* out_file, int width, int height) {
	// pa_dbg_samples_out = fopen("audiosamples.s16", "wb");
	audio_samples_written = 0;

	{
		pa_mainloop* pa_loop = pa_mainloop_new();
		pa_context* pa_ctx = pa_context_new((pa_global_api = pa_mainloop_get_api(pa_loop)), "Rec1");

		if (pa_context_connect(pa_ctx, nullptr, (pa_context_flags_t) 0, nullptr) < 0)
			printf("[%s] could not connect to PA-server\n", __func__);

		int ret = 0;

		pa_context_set_state_callback(pa_ctx, pa_context_state_callback, this);
		pa_mainloop_run(pa_loop, &ret);
		printf("[%s] using PA source \"%s\"\n", __func__, monitor_sources[default_sink].c_str());
		pa_context_disconnect(pa_ctx);
	}

	int error = 0;
	audio_stream = pa_simple_new(nullptr, "SnapShot Record", PA_STREAM_RECORD, monitor_sources[default_sink].c_str(), "record", &ss, nullptr, nullptr , &error);

	if (audio_stream == nullptr)
		printf("[%s] could not create audio-stream (error %d)\n", __func__, error);


	this->frame_width = width;
	this->frame_height = height;

	pthread_mutex_init(&encode_mutex, nullptr);
	pthread_mutex_init(&sound_buffer_lock, nullptr);
	pthread_cond_init(&encode_cond, nullptr);
	pthread_create(&encode_video_thread, nullptr, (void*(*)(void*)) &frame_recorder::encoding_thread_func, this);


	av_log_set_level(AV_LOG_DEBUG);
	format_ctx = avformat_alloc_context();

	format_ctx->oformat = av_guess_format(nullptr, out_file, nullptr);
	snprintf(format_ctx->filename, sizeof(format_ctx->filename), "%s", out_file);

	video_codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
	audio_codec = avcodec_find_encoder(AV_CODEC_ID_MP2);
	video_ctx = avcodec_alloc_context3(video_codec);
	audio_ctx = avcodec_alloc_context3(audio_codec);

	avcodec_get_context_defaults3(audio_ctx, audio_codec);
	avcodec_get_context_defaults3(video_ctx, video_codec);

	video_ctx->width = width;
	video_ctx->height = height;
	video_ctx->bit_rate = 6000 * 1000;
	video_ctx->time_base.den = TIMEBASE;
	video_ctx->time_base.num = 1;
	video_ctx->thread_count = 4;
	video_ctx->qmin = 2;
	video_ctx->qmax = 31;
	video_ctx->b_sensitivity = 100;
	video_ctx->gop_size = 1;
	video_ctx->me_method = 1;
	video_ctx->global_quality = 100;
	video_ctx->lowres = 0;
	video_ctx->bit_rate_tolerance = 200000;

	audio_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
	audio_ctx->sample_rate = 44100;
	audio_ctx->channels = 2;
	audio_ctx->time_base.den = 44100;
	audio_ctx->time_base.num = 1;
	audio_ctx->bit_rate = 128000;

	audio_ctx->frame_size = 8192;
	audio_ctx->channel_layout = 3;

    #if 0
	video_ctx->compression_level = 0;
	video_ctx->trellis = 0;
	video_ctx->gop_size = 1; // emit one intra-frame every ten frames

	video_ctx->me_pre_cmp = 0;
	video_ctx->me_cmp = 0;
	video_ctx->me_sub_cmp = 0;
	video_ctx->mb_cmp = 2;
	video_ctx->pre_dia_size = 0;
	video_ctx->dia_size = 1;

	video_ctx->quantizer_noise_shaping = 0; // qns=0
	video_ctx->noise_reduction = 0; // nr=0
	video_ctx->mb_decision = 0; // mbd=0 ("realtime" encoding)

	video_ctx->flags &= ~CODEC_FLAG_QPEL;
	video_ctx->flags &= ~CODEC_FLAG_4MV;
	video_ctx->trellis = 0;
	video_ctx->flags &= ~CODEC_FLAG_CBP_RD;
	video_ctx->flags &= ~CODEC_FLAG_QP_RD;
	video_ctx->flags &= ~CODEC_FLAG_MV0;
	#endif
	video_ctx->pix_fmt = PIX_FMT_YUV420P;


	if (avcodec_open2(video_ctx, video_codec, nullptr) < 0)
		fprintf(stderr, "[%s] could not open video codec\n", __func__);

	if ((audio_failed = (avcodec_open2(audio_ctx, audio_codec, nullptr) < 0)))
		fprintf(stderr, "[%s] could not open audio codec\n", __func__);

	pthread_create(&record_sound_thread, nullptr, (void*(*)(void*)) &frame_recorder::recording_thread_func, this);


	{
		// create output video&audio streams
		AVStream* vs =                  av_new_stream(format_ctx, 0);
		AVStream* as = (!audio_failed)? av_new_stream(format_ctx, 1): nullptr;

		vs->codec = video_ctx;
		vs->r_frame_rate.den = TIMEBASE;
		vs->r_frame_rate.num = 1;

		if (as != nullptr) {
			as->codec = audio_ctx;
			as->r_frame_rate.den = 44100;
			as->r_frame_rate.num = 1;
		}
	}


	if ((yuv_picture = alloc_picture(PIX_FMT_YUV420P, video_ctx->width, video_ctx->height)) == nullptr) {
		fprintf(stderr, "[%s] could not allocate yuv_picture\n", __func__);
		exit(1);
	}

	if ((rgb_picture = alloc_picture(PIX_FMT_RGBA, video_ctx->width, video_ctx->height)) == nullptr) {
		fprintf(stderr, "[%s] could not allocate rgb_picture\n", __func__);
		exit(1);
	}

	// turn RGB frames into YUV
	img_convert_ctx = sws_getContext(
		video_ctx->width, video_ctx->height, PIX_FMT_RGBA,
		video_ctx->width, video_ctx->height, PIX_FMT_YUV420P,
		SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
	);

	if (img_convert_ctx == nullptr) {
		fprintf(stderr, "[%s] could not initialize image-conversion context\n", __func__);
		exit(1);
	}

	av_dump_format(format_ctx, 0, out_file, 1);
	avio_open2(&format_ctx->pb, out_file, AVIO_FLAG_WRITE, nullptr, nullptr);
	avformat_write_header(format_ctx, nullptr);
}

frame_recorder::~frame_recorder() {
	keep_running = false;
	allow_append = false;

	pthread_cond_broadcast(&encode_cond);
	printf("[%s] joining encoder thread\n", __func__);
	pthread_join(encode_video_thread, nullptr);
	printf("[%s] joining recorder thread\n", __func__);
	pthread_join(record_sound_thread, nullptr);

	av_write_trailer(format_ctx);
	av_free(yuv_picture);
	avformat_free_context(format_ctx);
	pa_simple_free(audio_stream);

	// fclose(pa_dbg_samples_out);
}




void frame_recorder::append_frame(float /*time*/, int width, int height, char* data) {
	if (!allow_append)
		return;

	printf("[%s]\n", __func__);
	curr_time = getcurrenttime2();

	if (init_time < 0.0)
		init_time = getcurrenttime2();

	this->frame_width = width;
	this->frame_height = height;

	frame_data = data;
	pthread_cond_broadcast(&encode_cond);

    // memcpy(rgb_picture->data[0], data, width * height * 4);
	allow_append = false;
}



void frame_recorder::recording_thread_func() {
	int error = 0;

	while (keep_running) {
		printf("[%s] reading %d audio-samples\n", __func__, audio_ctx->frame_size * 2);

		short* buf = new short[audio_ctx->frame_size * 2];

		if (pa_simple_read(audio_stream, buf, audio_ctx->frame_size * 4, &error) < 0) {
			printf("[%s] error %d reading audio-stream\n", __func__, error);
			delete[] buf;
			break;
		}

		for (int i = 0; i < audio_ctx->frame_size * 2; i++) {
			buf[i] = short(float(buf[i]) * 0.8f);
		}

		// fwrite(buf, sizeof(short) * audio_ctx->frame_size * 2, 1, pa_dbg_samples_out);

		{
			pthread_mutex_lock(&sound_buffer_lock);
			sound_buffers.push_back(buf);
			pthread_mutex_unlock(&sound_buffer_lock);
		}
    }
}

void frame_recorder::encoding_thread_func() {
	while (keep_running) {
		allow_append = true;
		pthread_cond_wait(&encode_cond, &encode_mutex);

		if (!keep_running) {
			printf("[%s] exiting\n", __func__);
			break;
		}


		for (int y = 0; y < height; y++) {
			const int old_idx = (                    y) * frame_width ;
			const int new_idx = ((frame_height - 1 - y) * frame_width);

	        memcpy(&rgb_picture->data[0][new_idx * 4], &frame_data[old_idx * 4], frame_width * 4);
		}

        sws_scale(img_convert_ctx, rgb_picture->data, rgb_picture->linesize, 0, height, yuv_picture->data, yuv_picture->linesize);

		AVPacket p;
		av_init_packet(&p);
		p.data = nullptr;
		p.size = 0;
		// set time-index
		yuv_picture->pts = int64_t((curr_time - init_time) * TIMEBASE);

		const uint64_t vpts = yuv_picture->pts;
		int encode_status = 0;

		assert(video_ctx != nullptr);
		assert(yuv_picture != nullptr);

		if (avcodec_encode_video2(video_ctx, &p, yuv_picture, &encode_status) < 0)
			return;

		if (encode_status != 0) {
			// container is "mp4"
			p.pts = vpts;
			p.dts = AV_NOPTS_VALUE;

			av_write_frame(format_ctx, &p);
			av_free_packet(&p);
		}

		printf("[%s] video-frame encoded\n", __func__);


		AVFrame* audio_frame = avcodec_alloc_frame();

		{
			pthread_mutex_lock(&sound_buffer_lock);

			for (size_t i = 0, n = sound_buffers.size(); i < n; i++) {
				const uint64_t apts = audio_samples_written;

				if (!audio_failed) {
					avcodec_get_frame_defaults(audio_frame);

					audio_frame->data[0] = reinterpret_cast<char*>(sound_buffers[i]);
					audio_frame->nb_samples = audio_ctx->frame_size;
					audio_frame->sample_rate = 44100;
					audio_frame->channels = 2;
					audio_frame->channel_layout = audio_ctx->channel_layout;
					audio_frame->format = AV_SAMPLE_FMT_S16;

					audio_frame->pkt_pos = -1;
					audio_frame->pts = apts;

					av_init_packet(&p);
					p.data = nullptr;
					p.size = 0;

					audio_samples_written += audio_ctx->frame_size;
					avcodec_encode_audio2(audio_ctx, &p, audio_frame, &encode_status);

					if (encode_status != 0) {
						p.stream_index = 1;
						p.flags |= AV_PKT_FLAG_KEY;

						av_write_frame(format_ctx, &p);
						av_free_packet(&p);
					}

					audio_samples_written += audio_ctx->frame_size;
				}

				delete[] buf;
			}

			sound_buffers.clear();
		}

		pthread_mutex_unlock(&sound_buffer_lock);
		avcodec_free_frame(&audio_frame);
	}
}

