#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

// SRT a头文件
#include <srt/srt.h>
#include "rtsp_demo.h"
// FFmpeg 头文件 (必须是 C 风格的 include)
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
}

rtsp_demo_handle g_rtsplive = NULL;
rtsp_session_handle session = NULL;

char av_error[AV_ERROR_MAX_STRING_SIZE] = { 0 };
#define av_err2str(errnum) av_make_error_string(av_error, AV_ERROR_MAX_STRING_SIZE, errnum)

const int PORT = 9000;
const int SRT_LATENCY_MS = 120;
const int SRT_PAYLOAD_SIZE = 1316; // 7 * 188-byte TS packets

// 自定义 I/O 上下文，用于桥接 FFmpeg 和 SRT
struct SrtIOContext {
    SRTSOCKET srt_socket;
};

// FFmpeg 写回调函数：当 FFmpeg 生成 TS 包时，此函数被调用
static int srt_write_packet(void *opaque, uint8_t *buf, int buf_size) {
    SrtIOContext *io_ctx = static_cast<SrtIOContext *>(opaque);
    int bytes_sent_total = 0;

    while (bytes_sent_total < buf_size) {
        int chunk_size = buf_size - bytes_sent_total;
        if (chunk_size > SRT_PAYLOAD_SIZE) {
            chunk_size = SRT_PAYLOAD_SIZE;
        }

        int result = srt_sendmsg(io_ctx->srt_socket, (const char*)buf + bytes_sent_total, chunk_size, -1, 0);

        if (result == SRT_ERROR) {
            if (srt_getlasterror(nullptr) == SRT_ECONNLOST) {
                std::cerr << "SRT send error: Connection lost." << std::endl;
            } else {
                std::cerr << "SRT send error: " << srt_getlasterror_str() << std::endl;
            }
            return -1;
        }

        bytes_sent_total += result;
    }

    return bytes_sent_total;
}

// 主处理函数，将文件流式传输到客户端
void stream_file_to_client(SRTSOCKET client_sock, const std::string& filename) {
    AVFormatContext *ifmt_ctx = nullptr;
    AVFormatContext *ofmt_ctx = nullptr;
    AVIOContext *avio_ctx = nullptr;
    SrtIOContext srt_io_ctx = {client_sock};
    uint8_t *avio_buffer = nullptr;
    const size_t avio_buffer_size = 8192; // 8KB 缓冲区 //缓冲区改小1316也可以

    int ret = 0;
    int m_video_st_index = -1;

    // 1. 打开输入文件 (MP4)
    if ((ret = avformat_open_input(&ifmt_ctx, filename.c_str(), nullptr, nullptr)) < 0) {
        std::cout << "Could not open input file '" << filename << "'. Error: " << av_err2str(ret) << std::endl;
        return;
    }
    if ((ret = avformat_find_stream_info(ifmt_ctx, nullptr)) < 0) {
//        std::cerr << "Failed to find stream information. Error: " << av_err2str(ret) << std::endl;
        goto end;
    }
    av_dump_format(ifmt_ctx, 0, filename.c_str(), 0);

    // 2. 设置输出格式 (mpegts)
    avformat_alloc_output_context2(&ofmt_ctx, nullptr, "mpegts", nullptr);
    if (!ofmt_ctx) {
        std::cerr << "Could not create output context" << std::endl;
        goto end;
    }

    // 3. 设置自定义 I/O
    avio_buffer = static_cast<uint8_t *>(av_malloc(avio_buffer_size));
    avio_ctx = avio_alloc_context(avio_buffer, avio_buffer_size, 1, &srt_io_ctx, nullptr, srt_write_packet, nullptr);
    if (!avio_ctx) {
        std::cerr << "Could not create AVIO context" << std::endl;
        goto end;
    }
    ofmt_ctx->pb = avio_ctx;

    // 4. 复制流信息
    for (unsigned int i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *in_stream = ifmt_ctx->streams[i];

        if (in_stream->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_video_st_index = i;
        }
        AVStream *out_stream = avformat_new_stream(ofmt_ctx, nullptr);
        if (!out_stream) {
            std::cerr << "Failed to allocate output stream" << std::endl;
            goto end;
        }
        avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        out_stream->codecpar->codec_tag = 0;
    }

    // 5. 写入 TS 头
    AVDictionary *muxer_opts = nullptr;
    // 选项1: 告诉 mpegts 封装器立即刷新，减少延迟
    av_dict_set(&muxer_opts, "flush_packets", "1", 0);

    // 选项2: 这是关键！强制封装器将输出打包成固定大小的块。
    // 我们将其设置为 7 个 TS 包的大小 (7 * 188 = 1316)。
    char pkt_size_str[16];
    snprintf(pkt_size_str, sizeof(pkt_size_str), "%d", SRT_PAYLOAD_SIZE);
    av_dict_set(&muxer_opts, "pkt_size", pkt_size_str, 0);

    if ((ret = avformat_write_header(ofmt_ctx, &muxer_opts)) < 0) {
//        std::cerr << "Error occurred when opening output. Error: " << av_err2str(ret) << std::endl;
        av_dict_free(&muxer_opts); // 即使失败也要释放
        goto end;
    }

    av_dict_free(&muxer_opts); // 成功后也要释放

    // 6. 循环读取、转封装和发送数据包
    AVPacket pkt;


    long long start_time = av_gettime();

    std::cout << "Starting to stream..." << std::endl;
    AVBSFContext * h264bsfc;
    const AVBitStreamFilter * filter = av_bsf_get_by_name("h264_mp4toannexb");
    ret = av_bsf_alloc(filter, &h264bsfc);
    avcodec_parameters_copy(h264bsfc->par_in, ifmt_ctx->streams[m_video_st_index]->codecpar);
    av_bsf_init(h264bsfc);

    while (av_read_frame(ifmt_ctx, &pkt) >= 0) {
        AVStream *in_stream = ifmt_ctx->streams[pkt.stream_index];
        AVStream *out_stream = ofmt_ctx->streams[pkt.stream_index];


        if(m_video_st_index == pkt.stream_index)
        {
            AVPacket dst_pkt;
            av_init_packet(&dst_pkt);
            dst_pkt.data = NULL;
            dst_pkt.size = 0;
            // 深拷贝数据
            dst_pkt.data = (uint8_t*)av_malloc(pkt.size);
            if (!dst_pkt.data) {
                // 内存分配失败
            }
            memcpy(dst_pkt.data, pkt.data, pkt.size);
            dst_pkt.size = pkt.size;

// 拷贝元数据（pts/dts/stream_index等）
            dst_pkt.pts = pkt.pts;
            dst_pkt.dts = pkt.dts;
            dst_pkt.stream_index = pkt.stream_index;
            dst_pkt.flags = pkt.flags;
            dst_pkt.duration = pkt.duration;

// 拷贝 Side Data（可选）
            if (pkt.side_data_elems > 0) {
                av_packet_copy_props(&dst_pkt, &pkt); // FFmpeg API 复制属性
            }

            ret = av_bsf_send_packet(h264bsfc, &dst_pkt);
            if(ret < 0) printf("av_bsf_send_packet error");

            while ((ret = av_bsf_receive_packet(h264bsfc, &dst_pkt)) == 0) {
                if (g_rtsplive && session) {
                    rtsp_sever_tx_video(g_rtsplive, session, dst_pkt.data ,
                                        dst_pkt.size, dst_pkt.pts);
                }

            }
            // 使用后需手动释放 dst_pkt 的数据
            av_freep(&dst_pkt.data);    // 释放数据缓冲区
        }


        // 控制发送速率，模拟直播
        long long now = av_gettime() - start_time;
        long long pts_time = av_rescale_q(pkt.dts, in_stream->time_base, {1, AV_TIME_BASE});
        if (pts_time > now) {
            av_usleep(pts_time - now);
        }

        // 转换时间基
        av_packet_rescale_ts(&pkt, in_stream->time_base, out_stream->time_base);
        pkt.pos = -1;

        // 写入数据包 (这将通过我们的回调函数发送)
        ret = av_interleaved_write_frame(ofmt_ctx, &pkt);
        if (ret < 0) {
            std::cerr << "Error muxing packet" << std::endl;
            av_packet_unref(&pkt);
            break;
        }

        av_packet_unref(&pkt);
    }
    std::cout << "Streaming finished." << std::endl;

    // 7. 写入 TS 尾
    av_write_trailer(ofmt_ctx);

    end:
    // 8. 清理资源
    if (ifmt_ctx) avformat_close_input(&ifmt_ctx);
    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        // 只有在 AVIO 上下文是我们自己创建时才释放
        if (avio_ctx) {
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
        }
    }
    if (ofmt_ctx) avformat_free_context(ofmt_ctx);

    if (ret < 0 && ret != AVERROR_EOF) {
//        std::cerr << "Error occurred: " << av_err2str(ret) << std::endl;
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path_to_video.mp4>" << std::endl;
        return 1;
    }

    av_register_all();       // Register all codecs and formats so that they can be used.
    avformat_network_init(); // Initialization of network components

    std::string filename = argv[1];

    if(g_rtsplive==NULL) {
        g_rtsplive = create_rtsp_demo(7554);
        session = create_rtsp_session(g_rtsplive, "/live");
    }

    // 初始化 SRT
    srt_startup();

    SRTSOCKET serv_sock = srt_create_socket();
    if (serv_sock == SRT_INVALID_SOCK) {
        std::cerr << "srt_create_socket failed: " << srt_getlasterror_str() << std::endl;
        srt_cleanup();
        return -1;
    }

    // 设置为监听模式
    bool yes = true;
    srt_setsockopt(serv_sock, 0, SRTO_SENDER, &yes, sizeof(yes));

    int latency = SRT_LATENCY_MS;
    srt_setsockopt(serv_sock, 0, SRTO_PEERLATENCY, &latency, sizeof(latency));

    sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    serv_addr.sin_addr.s_addr = INADDR_ANY;

    if (srt_bind(serv_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1) {
        std::cerr << "srt_bind failed: " << srt_getlasterror_str() << std::endl;
        srt_close(serv_sock);
        srt_cleanup();
        return -1;
    }

    if (srt_listen(serv_sock, 5) == -1) {
        std::cerr << "srt_listen failed: " << srt_getlasterror_str() << std::endl;
        srt_close(serv_sock);
        srt_cleanup();
        return -1;
    }

    std::cout << "SRT server is listening on port " << PORT << ", waiting for a client..." << std::endl;

    while (true) {
        sockaddr_in client_addr;
        int client_addr_len = sizeof(client_addr);
        SRTSOCKET client_sock = srt_accept(serv_sock, (struct sockaddr*)&client_addr, &client_addr_len);

        if (client_sock == SRT_INVALID_SOCK) {
            std::cerr << "srt_accept failed: " << srt_getlasterror_str() << std::endl;
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
        std::cout << "Client connected from " << client_ip << ":" << ntohs(client_addr.sin_port) << std::endl;

        // 为该客户端启动流式传输
        stream_file_to_client(client_sock, filename);

        // 关闭连接
        std::cout << "Client disconnected." << std::endl;
        srt_close(client_sock);
    }

    srt_close(serv_sock);
    srt_cleanup();
    return 0;
}