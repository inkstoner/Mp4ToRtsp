//
// Created by hidev on 19-12-19.
//

#ifndef NVRVIDEO_FFMPEGWRAPPER_H
#define NVRVIDEO_FFMPEGWRAPPER_H

#include <string>
#include <functional>
#include <thread>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavdevice/avdevice.h"
#include "libavformat/avformat.h"
#include "libavfilter/avfilter.h"
#include "libavutil/avutil.h"
#include "libswscale/swscale.h"
#include "libavutil/pixdesc.h"
#include <libavutil/time.h>
}

typedef enum streamVideoStatus_E {
    VIDEO_OFFLINE,
    VIDEO_ONLINE,
    VIDEO_PAUSE,
} VideoStatus_E;

typedef struct videoSIZE_V {
    int u32Width;
    int u32Height;
} SIZE_V;

typedef std::function<void(unsigned char *data, int dataLen,int64_t pts)> STREAMCALLBACK;
typedef std::function<void(VideoStatus_E videoStatusE, int chn)> VIDEOSTATUSCALLBACK;

class FFMpegWrapper {

public:
    FFMpegWrapper();

    bool Init(std::string address, int chn);

    void SetCallBack(STREAMCALLBACK callback, VIDEOSTATUSCALLBACK videoStatusCallback);

    void Start();

    void Stop();

    void Join();
    SIZE_V GetSize();

private:
    static void *start_thread(void *pArgs);

    void GetStream();

private:
    bool m_isStart = false;
    int m_width = 0;
    int m_height = 0; //视频分辨率
    int m_frame_rate; //视频帧率
    std::string m_vcodec_name;//视频编码格式
    std::string m_acodec_name;
    int m_video_st_index = -1;
    int m_audio_st_index = -1;
    AVFormatContext *m_ifmt_ctx = NULL;
    STREAMCALLBACK m_streamcallback = NULL;
    VIDEOSTATUSCALLBACK m_videoStatusCallback = NULL;
	std::thread* m_thread;
    //pthread_mutex_t q_lock;
    //pthread_cond_t cond;
	std::string m_address;
    int m_chn;
};


#endif //NVRVIDEO_FFMPEGWRAPPER_H
