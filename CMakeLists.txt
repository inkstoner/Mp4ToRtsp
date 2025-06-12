cmake_minimum_required(VERSION 3.10)
project(Mp4ToRtsp)

set(CMAKE_CXX_STANDARD 11)

include_directories(./thirdpart/ffmpeg/include)
link_directories(./thirdpart/ffmpeg/lib)

include_directories(./thirdpart/rtspserver/include)
link_directories(./thirdpart/rtspserver/lib)

include_directories(./thirdpart/srt/include)
link_directories(./thirdpart/srt/lib)

#include_directories(./thirdpart/aarch64-himix100-ffmpeg/include)
#link_directories(./thirdpart/aarch64-himix100-ffmpeg/lib)
add_definitions(-D__WIN32__ -D_CRT_SECURE_NO_WARNINGS)
add_executable(Mp4ToRtsp main.cpp FFMpegWrapper.cpp FFMpegWrapper.h)

target_link_libraries(Mp4ToRtsp
        avcodec.lib
        avformat.lib
        avutil.lib
        avdevice.lib
        avfilter.lib
        postproc.lib
        swresample.lib
        swscale.lib
        ws2_32.lib
        RtspSever)

add_executable(UdpMuticastVideo main_udp.cpp FFMpegWrapper.cpp FFMpegWrapper.h)

target_link_libraries(UdpMuticastVideo avcodec.lib
        avformat.lib
        avutil.lib
        avdevice.lib
        avfilter.lib
        postproc.lib
        swresample.lib
        swscale.lib
        ws2_32.lib)

#target_link_libraries(UdpMuticastVideo
#        libavcodec.a
#        libavformat.a
#        libavutil.a
#        libavdevice.a
#        libavfilter.a
#        libpostproc.a
#        libswresample.a
#        libswscale.a
#        libx264.a
#        rt
##        ao z lzma bz2 va
#        pthread
#        )


add_executable(Mp4ToSrt main_srt.cpp)

target_link_libraries(Mp4ToSrt
        avcodec.lib
        avformat.lib
        avutil.lib
        avdevice.lib
        avfilter.lib
        postproc.lib
        swresample.lib
        swscale.lib
        ws2_32.lib
        srt.lib
)

add_executable(H264ToSrt H264ToSrt.cpp)

target_link_libraries(H264ToSrt
        avcodec.lib
        avformat.lib
        avutil.lib
        avdevice.lib
        avfilter.lib
        postproc.lib
        swresample.lib
        swscale.lib
        ws2_32.lib
        srt.lib
)