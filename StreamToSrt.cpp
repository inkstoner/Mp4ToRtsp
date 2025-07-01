#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <fstream>
#include <chrono>

#include <srt/srt.h>
// 使用 extern "C" 来包含 C 库头文件
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
}

const int SRT_LATENCY_MS = 120;
const int SRT_PAYLOAD_SIZE = 1316; // 7 * 188-byte TS packets

// --- 线程安全的TS包队列 ---
class TSPacketQueue {
public:
    // 推入一个AVPacket到队列
    void push(AVPacket* pkt) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push(pkt);
        }
        m_cond.notify_one();
    }

    // 从队列弹出一个AVPacket，如果队列为空则阻塞
    // 返回 nullptr 表示队列已停止且为空
    AVPacket* pop() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cond.wait(lock, [this] { return !m_queue.empty() || m_stop; });

        if (m_stop && m_queue.empty()) {
            return nullptr;
        }

        AVPacket* pkt = m_queue.front();
        m_queue.pop();
        return pkt;
    }

    // 停止队列，唤醒所有等待的线程
    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cond.notify_all();
    }

    // 清理队列中所有剩余的包
    ~TSPacketQueue() {
        while (!m_queue.empty()) {
            AVPacket* pkt = m_queue.front();
            m_queue.pop();
            av_packet_free(&pkt);
        }
    }

private:
    std::queue<AVPacket*> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cond;
    bool m_stop = false;
};

// --- 用于FFmpeg和线程间共享的状态 ---
struct StreamContext {
    AVFormatContext* ofmt_ctx = nullptr;
    int video_stream_index = -1;
    int64_t next_pts = 0;
    AVRational time_base = {1, 90000};
    TSPacketQueue* ts_queue = nullptr;
};


// --- FFmpeg 回调函数 (C风格) ---
// 当FFmpeg muxer生成TS包时，会调用此函数
// opaque指针将指向我们的StreamContext
int write_ts_callback(void* opaque, uint8_t* buf, int buf_size) {
    StreamContext* context = static_cast<StreamContext*>(opaque);

    AVPacket* ts_pkt = av_packet_alloc();
    if (!ts_pkt) return AVERROR(ENOMEM);

    if (av_new_packet(ts_pkt, buf_size) < 0) {
        av_packet_free(&ts_pkt);
        return AVERROR(ENOMEM);
    }
    memcpy(ts_pkt->data, buf, buf_size);

    context->ts_queue->push(ts_pkt);

    return buf_size;
}

// --- FFmpeg 初始化 ---
bool init_ffmpeg_muxer(StreamContext* context) {
    const size_t avio_buffer_size = 1316; // 8KB 缓冲区 //缓冲区改小1316也可以

    avformat_alloc_output_context2(&context->ofmt_ctx, NULL, "mpegts", NULL);
    if (!context->ofmt_ctx) {
        std::cerr << "Could not create output context" << std::endl;
        return false;
    }

    unsigned char* avio_buffer = (unsigned char*)av_malloc(avio_buffer_size);
    AVIOContext* avio_ctx = avio_alloc_context(avio_buffer, avio_buffer_size, 1, context, NULL, write_ts_callback, NULL);
    if (!avio_ctx) {
        std::cerr << "Could not create avio context" << std::endl;
        avformat_free_context(context->ofmt_ctx);
        context->ofmt_ctx = nullptr;
        return false;
    }
    context->ofmt_ctx->pb = avio_ctx;
    context->ofmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

    AVStream* out_stream = avformat_new_stream(context->ofmt_ctx, NULL);
    if (!out_stream) {
        std::cerr << "Failed allocating output stream" << std::endl;
        // 清理 avio_ctx
        av_free(context->ofmt_ctx->pb->buffer);
        avio_context_free(&context->ofmt_ctx->pb);
        avformat_free_context(context->ofmt_ctx);
        context->ofmt_ctx = nullptr;
        return false;
    }
    context->video_stream_index = out_stream->index;

    AVCodecParameters* codecpar = out_stream->codecpar;
    codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    codecpar->codec_id = AV_CODEC_ID_H264;
    codecpar->width = 1920;  // <-- 修改为你的视频宽度
    codecpar->height = 1080; // <-- 修改为你的视频高度
    out_stream->time_base = context->time_base;

    if (avformat_write_header(context->ofmt_ctx, NULL) < 0) {
        std::cerr << "Error occurred when opening output" << std::endl;
        // ... 清理同上 ...
        return false;
    }
    std::cout << "FFmpeg TS Muxer initialized." << std::endl;
    return true;
}

// --- 核心处理函数 ---
void process_h264_data(StreamContext* context, const uint8_t* h264_data, int size) {
    AVPacket pkt;
    av_init_packet(&pkt);

    pkt.data = const_cast<uint8_t*>(h264_data);
    pkt.size = size;
    pkt.stream_index = context->video_stream_index;

    int frame_rate = 25;
    pkt.pts = context->next_pts;
    pkt.dts = context->next_pts;
    pkt.duration = context->time_base.den / frame_rate;
    context->next_pts += pkt.duration;

    int ret = av_interleaved_write_frame(context->ofmt_ctx, &pkt);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, err_buf, sizeof(err_buf));
        std::cerr << "Error muxing packet: " << err_buf << std::endl;
    }
}

// --- 模拟硬件回调的线程函数 ---
void hardware_simulation_thread_func(const std::string& filename, StreamContext* context) {
    std::ifstream h264_file(filename, std::ios::binary);
    if (!h264_file) {
        std::cerr << "Failed to open h264 file: " << filename << std::endl;
        context->ts_queue->stop(); // 确保消费者线程不会永久等待
        return;
    }

    std::vector<uint8_t> buffer(1024 * 1024);
    char read_buf[4096];
    int start_code_pos = -1;
    int buffer_len = 0;
    int frame_count = 0;

    auto find_start_code = [](const uint8_t* p, int len) -> int {
        for (int i = 0; i < len - 3; ++i) {
            if (p[i] == 0 && p[i+1] == 0) {
                if (p[i+2] == 1) return i; // 00 00 01
                if (p[i+2] == 0 && i + 3 < len && p[i+3] == 1) return i; // 00 00 00 01
            }
        }
        return -1;
    };

    while (h264_file.read(read_buf, sizeof(read_buf)) || h264_file.gcount() > 0) {
        int bytes_read = h264_file.gcount();
        if (buffer_len + bytes_read > buffer.size()) {
            std::cerr << "Buffer overflow in hardware simulation." << std::endl;
            break;
        }
        memcpy(buffer.data() + buffer_len, read_buf, bytes_read);
        buffer_len += bytes_read;

        uint8_t* p = buffer.data();
        int search_len = buffer_len;

        if (start_code_pos == -1) {
            start_code_pos = find_start_code(p, search_len);
            if (start_code_pos == -1) continue;
        }

        while (true) {
            int next_start_code_pos = find_start_code(p + start_code_pos + 3, search_len - (start_code_pos + 3));
            if (next_start_code_pos != -1) {
                next_start_code_pos += (start_code_pos + 3);
                int nalu_size = next_start_code_pos - start_code_pos;
                std::cout << "Simulated HW: Got H264 frame " << ++frame_count << ", size " << nalu_size << std::endl;

                // 【核心调用】
                process_h264_data(context, p + start_code_pos, nalu_size);

                std::this_thread::sleep_for(std::chrono::milliseconds(40)); // 25fps

                start_code_pos = next_start_code_pos;
            } else {
                break;
            }
        }

        memmove(p, p + start_code_pos, buffer_len - start_code_pos);
        buffer_len -= start_code_pos;
        start_code_pos = 0;
    }

    // 处理最后一个NALU
    if (buffer_len > 0) {
        std::cout << "Simulated HW: Got last H264 frame, size " << buffer_len << std::endl;
        process_h264_data(context, buffer.data(), buffer_len);
    }

    std::cout << "Hardware simulation finished." << std::endl;
    context->ts_queue->stop();
}

// --- SRT 发送线程函数 ---
void srt_sender_thread_func(SRTSOCKET client_sock, TSPacketQueue* queue) {
    while (true) {
        AVPacket* ts_pkt = queue->pop();
        if (!ts_pkt) {
            std::cout << "Sender thread stopping." << std::endl;
            break; // 队列已停止
        }

        std::cout << "send size " << ts_pkt->size << std::endl;

        int bytes_sent_total = 0;
        int buf_size = ts_pkt->size;
        while (bytes_sent_total < buf_size) {
            int chunk_size = buf_size - bytes_sent_total;
            if (chunk_size > SRT_PAYLOAD_SIZE) {
                chunk_size = SRT_PAYLOAD_SIZE;
            }

            int result = srt_sendmsg(client_sock, (const char*)ts_pkt->data + bytes_sent_total, chunk_size, -1, 0);

            if (result == SRT_ERROR) {
                if (srt_getlasterror(nullptr) == SRT_ECONNLOST) {
                    std::cerr << "SRT send error: Connection lost." << std::endl;
                } else {
                    std::cerr << "SRT send error: " << srt_getlasterror_str() << std::endl;
                }
                av_packet_free(&ts_pkt);
                break;
            }

            bytes_sent_total += result;
        }

        av_packet_free(&ts_pkt);

    }
    srt_close(client_sock);
}


int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <input.h264>" << std::endl;
        return -1;
    }
    const std::string h264_file = argv[1];
    const int srt_port = 1234;

    av_register_all();       // Register all codecs and formats so that they can be used.
    avformat_network_init(); // Initialization of network components

    TSPacketQueue ts_queue;
    StreamContext context;
    context.ts_queue = &ts_queue;

    if (!init_ffmpeg_muxer(&context)) {
        return -1;
    }

    std::thread hw_thread(hardware_simulation_thread_func, h264_file, &context);

    srt_startup();
    SRTSOCKET serv_sock = srt_create_socket();
    if (serv_sock == SRT_ERROR) {
        std::cerr << "srt_create_socket: " << srt_getlasterror_str() << std::endl;
        return -1;
    }

    // 设置为监听模式
    bool yes = true;
    srt_setsockopt(serv_sock, 0, SRTO_SENDER, &yes, sizeof(yes));

    int latency = SRT_LATENCY_MS;
    srt_setsockopt(serv_sock, 0, SRTO_PEERLATENCY, &latency, sizeof(latency));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(srt_port);
    sa.sin_addr.s_addr = INADDR_ANY;

    if (srt_bind(serv_sock, (struct sockaddr*)&sa, sizeof(sa)) == SRT_ERROR) {
        std::cerr << "srt_bind: " << srt_getlasterror_str() << std::endl;
        srt_close(serv_sock);
        srt_cleanup();
        return -1;
    }

    if (srt_listen(serv_sock, 5) == SRT_ERROR) {
        std::cerr << "srt_listen: " << srt_getlasterror_str() << std::endl;
        srt_close(serv_sock);
        srt_cleanup();
        return -1;
    }

    std::cout << "SRT server is listening on port " << srt_port << std::endl;
    std::cout << "You can connect with: ffplay srt://127.0.0.1:" << srt_port << std::endl;

    while (true) {
        struct sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);
        SRTSOCKET client_sock = srt_accept(serv_sock, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock == SRT_ERROR) {
            std::cerr << "srt_accept: " << srt_getlasterror_str() << std::endl;
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        std::cout << "New client connected from " << client_ip << ":" << ntohs(client_addr.sin_port) << std::endl;

        // 为每个客户端创建一个新的发送线程
        // 在这个简单示例中，我们一次只处理一个客户端
        // 处理完后，线程会结束，我们可以接受下一个连接
        std::thread srt_sender_thread(srt_sender_thread_func, client_sock, &ts_queue);
        srt_sender_thread.join(); // 等待当前客户端处理完毕
        std::cout << "Client session finished. Ready for new connection." << std::endl;
    }

    // --- 清理 ---
    hw_thread.join();

    if (context.ofmt_ctx) {
//        avformat_write_trailer(context.ofmt_ctx);
        if (context.ofmt_ctx->pb) {
            av_free(context.ofmt_ctx->pb->buffer);
            avio_context_free(&context.ofmt_ctx->pb);
        }
        avformat_free_context(context.ofmt_ctx);
    }

    srt_close(serv_sock);
    srt_cleanup();

    return 0;
}