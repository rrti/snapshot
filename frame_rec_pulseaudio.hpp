#ifndef FRAME_REC_PULSEAUDIO_HDR
#define FRAME_REC_PULSEAUDIO_HDR

extern "C" {
#include <avcodec.h>
#include <avformat.h>
#include <swscale.h>
// #include <avutil.h>
}

#include <pthread.h>

#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

#include <pulse/pulseaudio.h>
#include <pulse/simple.h>


class frame_recorder {
public:
    frame_recorder(const char* out_file, int width, int height);
    ~frame_recorder();

    void append_frame(float time, int width, int height, char* data);
    bool is_ready() const { return allow_append; }

    void encoding_thread_func();
    void recording_thread_func();

private:
	pa_simple* audio_stream = nullptr;

	AVFrame* rgb_picture = nullptr;
	AVFrame* yuv_picture = nullptr;
	AVCodec* video_codec = nullptr;
	AVCodec* audio_codec = nullptr;

	AVCodecContext* video_ctx = nullptr;
	AVCodecContext* audio_ctx = nullptr;
	AVFormatContext* format_ctx = nullptr;

	SwsContext* img_convert_ctx = nullptr;

	pthread_t encode_video_thread;
	pthread_t record_sound_thread;

	pthread_mutex_t encode_mutex;
	pthread_mutex_t sound_buffer_lock;
	pthread_cond_t encode_cond;

public:
	std::vector<short*> sound_buffers;
	std::unordered_map<std::string, std::string> monitor_sources;
	std::string default_sink;

private:
	char* frame_data = nullptr;

	size_t audio_samples_written = 0;

	double init_time = -1.0;
	double curr_time = -1.0;

	int frame_width = 0;
	int frame_height = 0;

	std::atomic<bool> allow_append = {false};
	std::atomic<bool> keep_running = { true};
	std::atomic<bool> audio_failed = {false};
};

#endif

