#ifndef FRAME_REC_HDR
#define FRAME_REC_HDR

extern "C" {
#include <avcodec.h>
#include <avformat.h>
#include <swscale.h>
// #include <avutil.h>
}

#include <pthread.h>

#include <atomic>
#include <string>


class frame_recorder {
public:
    frame_recorder(const char* out_file, int width, int height);
    ~frame_recorder();

    void append_frame(float time, int width, int height, char* data);
    bool is_ready() const { return allow_append; }

    void encoding_thread_func();
    void recording_thread_func() {}

private:
	AVFrame* rgb_picture = nullptr;
	AVFrame* yuv_picture = nullptr;
	AVCodec* video_codec = nullptr;

	AVCodecContext* video_ctx = nullptr;
	AVFormatContext* format_ctx = nullptr;

	SwsContext* img_convert_ctx = nullptr;

	pthread_t encode_video_thread;
	pthread_mutex_t encode_mutex;
	pthread_cond_t encode_cond;

private:
	char* frame_data = nullptr;

	double init_time = -1.0;
	double curr_time = -1.0;

	int frame_width = 0;
	int frame_height = 0;

	std::atomic<bool> allow_append = {false};
	std::atomic<bool> keep_running = {true};
};

#endif

