#include <iostream>
#include <stdio.h>
#include <chrono>
#include <thread>
#include <fstream>

#include "FFMpegWrapper.h"
#include "rtsp_demo.h"

using namespace std;

rtsp_demo_handle g_rtsplive = NULL;
rtsp_session_handle session = NULL;

void RecvData(unsigned char *data, int dataLen,int64_t pts) {
    int64_t addpts = pts+ 1000000000000;//补位
    printf("get data %d pts %lld\n", dataLen,addpts);
    if (g_rtsplive && session) {
        rtsp_sever_tx_video(g_rtsplive, session, data ,
                            dataLen, addpts);
    }
}

int main(int argc,char *argv[]) {

    if (!(argc == 2 || argc == 3))
    {
        printf("Para error\n");
        printf("Program FilePath [Port]\n");
        getchar();
        return -1;
    }

    int rtspPort = 554;
    std::string filePath = "F://1880.mp4";
    try
    {
        filePath = argv[1];
        if(argc == 3)
        {
            std::string rtspPortStr = argv[2];
            rtspPort = std::stoi(rtspPortStr);
        }
    }
    catch (...)
    {
        printf("Para error\n");
        printf("Program FilePath [Port]\n");
        getchar();
        return -1;
    }

    if(g_rtsplive==NULL) {
        g_rtsplive = create_rtsp_demo(rtspPort);
        session = create_rtsp_session(g_rtsplive, "/live");
    }

    FFMpegWrapper *ffMpegWrapper = new FFMpegWrapper;

    ffMpegWrapper->Init(filePath, 0);
    ffMpegWrapper->SetCallBack(RecvData, nullptr);
    ffMpegWrapper->Start();

    printf("ffMpegWrapper->Stop()\n");
    ffMpegWrapper->Join();
    while (true) {
        printf("enddl\n");
        this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    return 0;
}
