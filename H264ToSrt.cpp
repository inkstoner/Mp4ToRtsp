#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

#include <srt/srt.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
}

char av_error[AV_ERROR_MAX_STRING_SIZE] = { 0 };
#define av_err2str(errnum) av_make_error_string(av_error, AV_ERROR_MAX_STRING_SIZE, errnum)

const int PORT = 9000;
const int SRT_LATENCY_MS = 120;
const int SRT_PAYLOAD_SIZE = 1316; // 7 * 188-byte TS packets
const double DEFAULT_FRAME_RATE = 25.0; // 假设 H264 裸流的帧率是 25 fps

struct SrtIOContext {
    SRTSOCKET srt_socket;
};

// 最可靠的 srt_write_packet 版本，带分片功能
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

void stream_file_to_client(SRTSOCKET client_sock, const std::string& filename) {
    AVFormatContext *ifmt_ctx = nullptr;
    AVFormatContext *ofmt_ctx = nullptr;
    AVIOContext *avio_ctx = nullptr;
    SrtIOContext srt_io_ctx = {client_sock};
    uint8_t *avio_buffer = nullptr;
    const size_t avio_buffer_size = 1316;
    int ret = 0;

    // --- START OF H264-SPECIFIC CHANGES ---
    AVDictionary *input_opts = nullptr;
    AVInputFormat *ifmt = av_find_input_format("h264");
    if (!ifmt) {
        std::cerr << "Could not find H.264 input format" << std::endl;
        return;
    }

    // 设置帧率，这样 FFmpeg 才能为裸流生成时间戳
    char framerate_str[16];
    snprintf(framerate_str, sizeof(framerate_str), "%f", DEFAULT_FRAME_RATE);
    av_dict_set(&input_opts, "framerate", framerate_str, 0);
    // 模拟实时读取，否则会一次性读完
    av_dict_set(&input_opts, "re", "1", 0);


    // 1. 打开输入文件 (H264)
    if ((ret = avformat_open_input(&ifmt_ctx, filename.c_str(), ifmt, &input_opts)) < 0) {
        std::cerr << "Could not open input file '" << filename << "'. Error: " << av_err2str(ret) << std::endl;
        av_dict_free(&input_opts);
        return;
    }
    av_dict_free(&input_opts); // 选项已被使用，释放字典
    // --- END OF H264-SPECIFIC CHANGES ---

    if ((ret = avformat_find_stream_info(ifmt_ctx, nullptr)) < 0) {
        std::cerr << "Failed to find stream information. Error: " << av_err2str(ret) << std::endl;
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
    // 对于裸流，只有一个视频流
    AVStream *in_stream = ifmt_ctx->streams[0];
    AVStream *out_stream = avformat_new_stream(ofmt_ctx, nullptr);
    if (!out_stream) {
        std::cerr << "Failed to allocate output stream" << std::endl;
        goto end;
    }
    avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
    out_stream->codecpar->codec_tag = 0;
    // H.264 裸流的时间基可能不标准，MPEG-TS 有一个标准时间基 (90000)
    // 通常 FFmpeg 会在转封装时自动处理，但手动设置输出时间基更稳妥
    out_stream->time_base = {1, 90000};


    // 5. 写入 TS 头
    if ((ret = avformat_write_header(ofmt_ctx, nullptr)) < 0) {
        std::cerr << "Error occurred when opening output. Error: " << av_err2str(ret) << std::endl;
        goto end;
    }

    // 6. 循环读取、转封装和发送数据包
    AVPacket pkt;
    long long start_time = av_gettime();
    std::cout << "Starting to stream H.264 file..." << std::endl;
    int64_t frame_index = 0; // <--- FIX: 初始化帧计数器

    while (av_read_frame(ifmt_ctx, &pkt) >= 0) {
        // 检查从 av_read_frame 获取的包是否没有时间戳
        if (pkt.pts == AV_NOPTS_VALUE) {
            // 根据我们的帧计数器生成 PTS 和 DTS
            pkt.pts = frame_index;
            pkt.dts = frame_index;
            // 对于恒定帧率的视频，可以简单地将 duration 设为 1 个 time_base 单位
            pkt.duration = 1;
        }
        frame_index+=40; // 递增帧计数器，为下一帧做准备

        long long now = av_gettime() - start_time;
        long long pts_time = av_rescale_q(pkt.dts, in_stream->time_base, {1, AV_TIME_BASE});
        if (pts_time > now) {
            av_usleep(pts_time - now);
        }
        av_usleep(40*1000);
        // 转换时间基 (非常重要！)
        av_packet_rescale_ts(&pkt, in_stream->time_base, out_stream->time_base);
        pkt.pos = -1;
        pkt.stream_index = 0;

        // 写入数据包 (这将通过我们的回调函数发送)
        ret = av_interleaved_write_frame(ofmt_ctx, &pkt);
        if (ret < 0) {
            std::cerr << "Error muxing packet. Maybe client disconnected." << std::endl;
            av_packet_unref(&pkt);
            break;
        }
        av_packet_unref(&pkt);
    }
    std::cout << "Streaming finished (end of file or error)." << std::endl;

    // 7. 写入 TS 尾
    av_write_trailer(ofmt_ctx);

    end:
    if (ifmt_ctx) avformat_close_input(&ifmt_ctx);
    if (ofmt_ctx) {
        if (avio_ctx) {
            av_free(avio_ctx->buffer);
            avio_context_free(&avio_ctx);
        }
        avformat_free_context(ofmt_ctx);
    }
    if (ret < 0 && ret != AVERROR_EOF) {
        std::cerr << "Error occurred: " << av_err2str(ret) << std::endl;
    }
}


// main 函数与之前的例子完全相同
int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path_to_video.h264>" << std::endl;
        return 1;
    }
    std::string filename = argv[1];

    av_register_all();       // Register all codecs and formats so that they can be used.
    avformat_network_init(); // Initialization of network components

    srt_startup();

    SRTSOCKET serv_sock = srt_create_socket();
    // ... (main 函数的剩余部分与之前完全相同)
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

    std::cout << "SRT H.264 server is listening on port " << PORT << ", waiting for a client..." << std::endl;

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

        stream_file_to_client(client_sock, filename);

        std::cout << "Client disconnected." << std::endl;
        srt_close(client_sock);
    }

    srt_close(serv_sock);
    srt_cleanup();
    return 0;
}