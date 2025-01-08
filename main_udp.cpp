#include <iostream>
#include<WinSock2.h>
#include<iostream>
#include <stdio.h>
#include <chrono>
#include <thread>
#include <fstream>

#include "FFMpegWrapper.h"

using namespace std;
SOCKET clientSocket;
sockaddr_in dstAddr;

void RecvData(unsigned char *data, int dataLen,int64_t pts) {
    int sendsize = 500;

    if (dataLen <= sendsize) {
//            printf("send udp %d \n", len);
        int ret = sendto(clientSocket, (const char *) data, dataLen, 0, (sockaddr *) &dstAddr, sizeof(dstAddr));
        if (ret == -1) {
            printf("send msg failed! error message: %s\n", strerror(errno));
            printf("datelen: %d\n", dataLen);
            return ;
        }
        return ;
    }
    for (int i = 0; i <= dataLen / sendsize; ++i) {
        int sendlen = (i == (dataLen / sendsize)) ? (dataLen % sendsize) : sendsize;
//            printf("send udp %d \n", sendlen);
        int ret = sendto(clientSocket, (const char *)data + i * sendsize, sendlen, 0,(sockaddr *) &dstAddr, sizeof(dstAddr));
        if (ret == -1) {
            printf("send msg failed! error message: %s\n", strerror(errno));
            printf("datelen: %d\n", dataLen);
            return ;
        }
    }

//    int len = sendto(clientSocket, (const char *) data, dataLen, 0, (sockaddr *) &dstAddr, sizeof(dstAddr));
    printf("udp send %d\n", dataLen);
}

void initUdp(std::string ip,int port)
{
    WSADATA WSAData;
    WORD sockVersion = MAKEWORD(2, 2);
    if (WSAStartup(sockVersion, &WSAData) != 0)
        return ;

    clientSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (INVALID_SOCKET == clientSocket) {
        cout << "socket error!";
        return ;
    }

    dstAddr.sin_family = AF_INET;
    dstAddr.sin_port = htons(port);
    dstAddr.sin_addr.S_un.S_addr = inet_addr(ip.c_str());
}

int main(int argc,char *argv[]) {

    if (!(argc == 4))
    {
        printf("Para error\n");
        printf("Program FilePath udpip port\n");
        getchar();
        return -1;
    }

    std::string udpIp = "224.0.0.200";
    int udpPort = 5001;
    std::string filePath = "F://1880.mp4";
    try
    {
        filePath = argv[1];
        udpIp = argv[2];
        std::string udpPortStr = argv[3];
        udpPort = std::stoi(udpPortStr);
    }
    catch (...)
    {
        printf("Para error\n");
        printf("Program FilePath udpip port\n");
        getchar();
        return -1;
    }

    initUdp(udpIp,udpPort);

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

//    closesocket(clientSocket);
//    WSACleanup();

    return 0;
}
