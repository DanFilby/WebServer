#pragma once
#define NOMINMAX

#include "External-Headers/date.h"
#include "External-Headers/TinySHA1.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>

#include <iostream>
#include <thread>
#include <future>
#include <map>
#include <string>
#include <array>
#include <queue>
#include <vector>
#include <fstream>

#ifdef MATHLIBRARY_EXPORTS
#define WEBSERVERLIBRARY_API __declspec(dllexport)
#else
#define WEBSERVERLIBRARY_API __declspec(dllimport)
#endif

#pragma comment(lib, "Ws2_32.lib")

#define DEFAULT_PORT "27015"
#define DEFAULT_BUFLEN 512

//#define DECLARE_NO_COPY(ClassName)\
//        ClassName(const ClassName&) = delete;\
//        ClassName & operator=(const ClassName&) = delete;

namespace WebServer
{
#define OutputServerTime(ServerTime) "| " << date::format("%H:%M:%S", ServerTime) << " | "
#define OutputServerTime_GetTime() OutputServerTime(std::chrono::system_clock::now())
#define OutputServerStatus() OutputServerTime(std::chrono::system_clock::now()) << "Error-code: " <<  WSAGetLastError() << " | "

    class SocketDataStream
    {
        const int mStreamChunkSize = 1024;

    public:
        SocketDataStream();
        ~SocketDataStream();

        //copy and move
        SocketDataStream(const SocketDataStream& Other);
        SocketDataStream(SocketDataStream&& Other) noexcept;
        SocketDataStream& operator=(SocketDataStream&& Other) noexcept;

        void Reset();

        void StreamRequestData(const char* InRequestData, int InDataLen);
        int GetDataLen() const { return mCurrentDataIndex; }

        char* mData = nullptr;

    private:
        void IncreaseBufferAllocated(int IncreaseSize);

        int mDataBufferSize = 0;
        int mCurrentDataIndex = 0;
    };

    struct MilliSecStopwatch
    {
        MilliSecStopwatch() = default;

        MilliSecStopwatch(const std::chrono::system_clock::time_point& TimeNow, double InDurationThresholdMs);
        void ResetClock(const std::chrono::system_clock::time_point& TimeNow) const;
        bool DurationReached(const std::chrono::system_clock::time_point& TimeNow) const;

        mutable std::chrono::system_clock::time_point PreviousCheckTime;
        double DurationThreshold;
    };

    template<typename T>
    class ThreadQueue
    {
    public:
        void SyncPush(T& Val);
        T SyncPop();
        bool IsEmpty();

        std::queue<T>& GetQueueExclusive();
        void ReturnQueue();

    private:
        std::queue<T> mQueue;
        std::mutex mQueueMutex;
    };

    // html
    std::vector<char> GenerateHtmlPage(std::string Message);

    // time and date
    std::tm GetLocalTime();
    std::string ConvertToWebServerTimeFormat(std::tm LocalTime);

    // string helpers
    bool GetStrLine(const char* InData, char* OutDataBuffer, int BufferSize, const char*& OutLineEndPtr);
    int FindCharIndex(const char* InCharArray, const char c);

}

namespace WSHelpers
{
    std::string GetWebSocketAcceptValue(std::string InAcceptKey);
    std::string base64_encode(unsigned char const* buf, unsigned int bufLen);
}

namespace WebServer
{
    // Template definitions
    template<typename T>
    void ThreadQueue<T>::SyncPush(T& Val)
    {
        mQueueMutex.lock();
        mQueue.push(Val);
        mQueueMutex.unlock();
    }

    template<typename T>
    T ThreadQueue<T>::SyncPop()
    {
        mQueueMutex.lock();
        T Val = mQueue.front();
        mQueue.pop();
        mQueueMutex.unlock();

        return Val;
    }

    template<typename T>
    inline bool ThreadQueue<T>::IsEmpty()
    {
        mQueueMutex.lock();
        bool empty = mQueue.empty();
        mQueueMutex.unlock();
        return empty;
    }

    template<typename T>
    std::queue<T>& ThreadQueue<T>::GetQueueExclusive()
    {
        mQueueMutex.lock();
        return mQueue;
    }

    template<typename T>
    void ThreadQueue<T>::ReturnQueue()
    {
        mQueueMutex.unlock();
    }
}
