//
// Created by hidev on 19-12-19.
//

#include "FFMpegWrapper.h"


FFMpegWrapper::FFMpegWrapper() {
    av_register_all();       // Register all codecs and formats so that they can be used.
    avformat_network_init(); // Initialization of network components
}

bool FFMpegWrapper::Init(std::string address, int chn) {
    m_address = address;
    m_chn = chn;
    /*  pthread_mutex_init(&q_lock, NULL);
      pthread_cond_init(&cond, NULL);*/
    return true;
}

SIZE_V FFMpegWrapper::GetSize() {
    //time_t T;
    //struct timespec t;

    //if (m_width == 0) {
    //    time(&T);
    //    t.tv_sec = T + 20;
    //    printf("starting timedwait at %s", ctime(&T));
    //   /* pthread_cond_timedwait(&cond, &q_lock, &t);*/
    //    time(&T);
    //    printf("timedwait over at %s", ctime(&T));
    //}
    SIZE_V size_v;
    size_v.u32Width = m_width;
    size_v.u32Height = m_height;
    return size_v;
}

void FFMpegWrapper::SetCallBack(STREAMCALLBACK callback, VIDEOSTATUSCALLBACK videoStatusCallback) {
    m_streamcallback = callback;
    m_videoStatusCallback = videoStatusCallback;
}

void *FFMpegWrapper::start_thread(void *pArgs) {
    FFMpegWrapper *ptr = (FFMpegWrapper *) pArgs;
    ptr->GetStream();
    return NULL;
}

void FFMpegWrapper::Start() {
    m_isStart = true;
    m_thread = new std::thread(start_thread, this);
}

void FFMpegWrapper::Stop() {
    m_isStart = false;
}

void FFMpegWrapper::GetStream() {
    //pthread_mutex_lock(&q_lock);
    int ret;
    char errbuf[64];
    int AVERROR_EOF_FLAG = 0;

    while (m_isStart) {
        AVPacket pkt;
        ERR_RESERT:
        while (m_isStart)//循环开流
        {
            if (AVERROR_EOF_FLAG == 1) {
                avformat_close_input(&m_ifmt_ctx);
                AVERROR_EOF_FLAG = 0;
            }
            AVDictionary *options = NULL;
            av_dict_set(&options, "buffer_size", "4096000", 0);
            av_dict_set(&options, "max_delay", "500000", 0);
            av_dict_set(&options, "stimeout", "20000000", 0);  //设置超时断开连接时间
            //av_dict_set(&options, "rtsp_transport", "tcp", 0); //以udp方式打开，如果以tcp方式打开将udp替换为tcp
            //av_dict_set(&options, "rtsp_transport", "udp", 0); //以udp方式打开，如果以tcp方式打开将udp替换为tcp
            if ((ret = avformat_open_input(&m_ifmt_ctx, m_address.c_str(), 0, &options)) <
                0) { // Open the input file for reading.
                printf("Could not open input adress '%s' (error '%s')\n", m_address.c_str(),
                       av_make_error_string(errbuf, sizeof(errbuf), ret));
                //av_packet_unref(&pkt);
                avformat_close_input(&m_ifmt_ctx);
                std::this_thread::sleep_for(std::chrono::milliseconds(3000));
                continue;
            }

            m_ifmt_ctx->probesize = 1024 * 1024;
            m_ifmt_ctx->max_analyze_duration = 1 * AV_TIME_BASE;//AV_TIME_BASE是定义的时间标准，代表1秒
            if ((ret = avformat_find_stream_info(m_ifmt_ctx, NULL)) <
                0) { // Get information on the input file (number of streams etc.).
                printf("Could not open find stream info (error '%s')\n",
                       av_make_error_string(errbuf, sizeof(errbuf), ret));
                //av_packet_unref(&pkt);
                avformat_close_input(&m_ifmt_ctx);
                std::this_thread::sleep_for(std::chrono::milliseconds(3000));
                continue;
            }
            break;
        }
        if (m_ifmt_ctx == NULL)
            continue;

        for (int i = 0; i < m_ifmt_ctx->nb_streams; i++) { // dump information
            AVStream *in_stream = m_ifmt_ctx->streams[i];

            if (in_stream->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
                m_video_st_index = i;
                m_width = in_stream->codec->width;
                m_height = in_stream->codec->height;
                if (in_stream->avg_frame_rate.den != 0 && in_stream->avg_frame_rate.num != 0) {
                    m_frame_rate = in_stream->avg_frame_rate.num / in_stream->avg_frame_rate.den;//每秒多少帧
                }
                m_vcodec_name = in_stream->codec->codec_id;
            } else if (in_stream->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
                m_audio_st_index = i;
            }
        }

        printf("Video info---stream index: %d, codec id: %d,  width: %d, height: %d, FrameRate: %d\n",
               m_video_st_index, m_vcodec_name.c_str(), m_width, m_height, m_frame_rate);

        av_init_packet(&pkt);
        pkt.data = NULL;
        pkt.size = 0;
        bool hasSendWidth = false;

        AVBSFContext * h264bsfc;
        const AVBitStreamFilter * filter = av_bsf_get_by_name("h264_mp4toannexb");
        ret = av_bsf_alloc(filter, &h264bsfc);
        avcodec_parameters_copy(h264bsfc->par_in, m_ifmt_ctx->streams[m_video_st_index]->codecpar);
        av_bsf_init(h264bsfc);

        int64_t start_time=0;
        start_time=av_gettime();

        while (m_isStart)//recive Freame
        {
            do {
                ret = av_read_frame(m_ifmt_ctx, &pkt);
            } while (ret == AVERROR(EAGAIN));

            if (ret < 0) {
                if (ret == (int) AVERROR_EOF) {
                    printf("0 Could not read frame %d (error '%s')\n", ret,
                           av_make_error_string(errbuf, sizeof(errbuf), ret));
                    if (m_videoStatusCallback != NULL)//发送在线状态
                        m_videoStatusCallback(VideoStatus_E::VIDEO_OFFLINE, m_chn);
                    AVERROR_EOF_FLAG = 1;
                    av_packet_unref(&pkt);
                    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
                    goto ERR_RESERT;
//                    exit(0);
                    //break;
                }
                av_packet_unref(&pkt);
                //printf("1 Could not read frame %d (error '%s')\n",ret, av_make_error_string(errbuf, sizeof(errbuf), ret));
                continue;
            }

            if (pkt.stream_index == m_video_st_index) {
                if (m_streamcallback != NULL) {

                    //拉的流就直接从这里出了
//                    printf("pts %lld %d %d %d %d 0x%x\n", pkt.pts, pkt.data[0], pkt.data[1], pkt.data[2], pkt.data[3],
//                           pkt.data[4]);
//                    if (pkt.size > 1024 * 1024)
//                        printf("%d \n", pkt.size);

//                    m_streamcallback(pkt.data, pkt.size);

                    ret = av_bsf_send_packet(h264bsfc, &pkt);
                    if(ret < 0) printf("av_bsf_send_packet error");

                    while ((ret = av_bsf_receive_packet(h264bsfc, &pkt)) == 0) {

                        m_streamcallback(pkt.data, pkt.size,pkt.pts);
//                        printf("pts %" PRId64 "\n", pkt.pts);

                        //计算等待时间，这样视频出的流才会很流畅
                        AVRational time_base=m_ifmt_ctx->streams[m_video_st_index]->time_base;
                        AVRational time_base_q={1,AV_TIME_BASE};
                        int64_t pts_time = av_rescale_q(pkt.dts, time_base, time_base_q);
                        int64_t now_time = av_gettime() - start_time;
                        if (pts_time > now_time)
                            av_usleep(pts_time - now_time);

                    }
                }
                if (!hasSendWidth) {
                    if (m_width != 0) {
                        hasSendWidth = true;
                        /*           pthread_cond_signal(&cond);
                                   pthread_mutex_unlock(&q_lock);*/
                        if (m_videoStatusCallback != NULL)//发送在线状态
                            m_videoStatusCallback(VideoStatus_E::VIDEO_ONLINE, m_chn);
                    }
                }

            } else if (pkt.stream_index == m_audio_st_index) {// audio frame
            } else {
            }
            av_packet_unref(&pkt);
            //std::this_thread::sleep_for(std::chrono::milliseconds(40));//如果有音频流 就不能从这里sleep ，不然会卡一卡的
        }

        if (AVERROR_EOF_FLAG != 1) {
            if (pkt.buf != NULL)
                av_free(&pkt);
            avformat_close_input(&m_ifmt_ctx);
        }
    }
    printf("GetStream endl\n");
}

void FFMpegWrapper::Join() {
    m_thread->join();
}



