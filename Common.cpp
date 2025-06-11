#include "Common.h"

namespace WebServer
{
    SocketDataStream::SocketDataStream()
    {
        IncreaseBufferAllocated(mStreamChunkSize);
    }

    SocketDataStream::~SocketDataStream()
    {
        if(mData)
        {
            free(mData);
        }
    }

    SocketDataStream::SocketDataStream(const SocketDataStream& Other)
        : mDataBufferSize(Other.mDataBufferSize), mCurrentDataIndex(Other.mCurrentDataIndex)
    {
        if(mCurrentDataIndex > 0)
        {
            mData = (char*) calloc(mDataBufferSize, sizeof(char));
            memcpy(mData, Other.mData, mCurrentDataIndex);
        }
    }

    SocketDataStream::SocketDataStream(SocketDataStream&& Other) noexcept
        : mData(Other.mData), mDataBufferSize(Other.mDataBufferSize), mCurrentDataIndex(Other.mCurrentDataIndex)
    {
        Other.mData = nullptr;
        Other.mDataBufferSize = 0;
        Other.mCurrentDataIndex = 0;
    }

    SocketDataStream& SocketDataStream::operator=(SocketDataStream&& Other) noexcept
    {
        mDataBufferSize = std::move(Other.mDataBufferSize);
        mCurrentDataIndex = std::move(Other.mCurrentDataIndex);
        mData = Other.mData;

        Other.mData = nullptr;
        Other.mDataBufferSize = 0;
        Other.mCurrentDataIndex = 0;
        return *this;
    }

    void SocketDataStream::Reset()
    {
        if(mData)
        {
            memset(mData, '\0', mDataBufferSize);
        }
        mCurrentDataIndex = 0;
    }

    void SocketDataStream::StreamRequestData(const char* InData, int InDataLen)
    {
        std::cout << OutputServerTime_GetTime() << "Streaming request data" << "\n";

        int InDataIndex = 0;
        int ClampedStreamingChunkSize = std::min(InDataLen, mStreamChunkSize);
        int NumStreamChunks = std::ceil((float) InDataLen / mStreamChunkSize);

        for(int i = 0; i < NumStreamChunks; i++)
        {
            if(mCurrentDataIndex + ClampedStreamingChunkSize > mDataBufferSize)
            {
                IncreaseBufferAllocated(mStreamChunkSize);
            }

            int BytesToStream = (InDataIndex + ClampedStreamingChunkSize > InDataLen) ? InDataLen - InDataIndex : ClampedStreamingChunkSize;
            memcpy(&mData[mCurrentDataIndex], &InData[InDataIndex], BytesToStream);

            InDataIndex += BytesToStream;
            mCurrentDataIndex += BytesToStream;
        }
    }

    void SocketDataStream::IncreaseBufferAllocated(int IncreaseSize)
    {
        mDataBufferSize = mDataBufferSize + mStreamChunkSize;
        char* NewRequestDataBuffer = (char*) calloc(mDataBufferSize, sizeof(char));

        if(mCurrentDataIndex > 0)
        {
            memcpy(NewRequestDataBuffer, mData, mCurrentDataIndex);
            free(mData);
        }

        mData = NewRequestDataBuffer;
    }

    MilliSecStopwatch::MilliSecStopwatch(const std::chrono::system_clock::time_point& TimeNow, double InDurationThresholdMs)
    {
        ResetClock(TimeNow);
        DurationThreshold = InDurationThresholdMs;
    }

    void MilliSecStopwatch::ResetClock(const std::chrono::system_clock::time_point& TimeNow) const
    {
        PreviousCheckTime = TimeNow;
    }

    bool MilliSecStopwatch::DurationReached(const std::chrono::system_clock::time_point& TimeNow) const
    {
        bool DurationReached = std::chrono::duration_cast<std::chrono::milliseconds>(TimeNow - PreviousCheckTime).count() > DurationThreshold;
        if(DurationReached)
        {
            ResetClock(TimeNow);
        }
        return DurationReached;
    }

    std::vector<char> GenerateHtmlPage(std::string Message)
    {
        std::string htmlHead =
            "<head><title>Dan's hosted site</title>\
            <style>body{background-color: #e6f2ff }h1{font-size:32; text-align: center; color: black; }</style></head>";

        std::string HtmlBody = "<body><h1>" + Message + "</h1></body>";

        std::string HtmlDoc = "<!DOCTYPE html><html>" + htmlHead + HtmlBody + "</html>";

        return std::vector<char>(HtmlDoc.begin(), HtmlDoc.end());
    }

    std::tm GetLocalTime()
    {
        std::time_t CurrentTimeStamp; time(&CurrentTimeStamp);
        std::tm LocalTime; localtime_s(&LocalTime, &CurrentTimeStamp);
        return LocalTime;
    }

    std::string ConvertToWebServerTimeFormat(std::tm LocalTime)
    {
        // Format: day-name, day-date month year hour:mim:sec Timezone
        char buffer[64];
        std::strftime(buffer, 64, "%a, %d %b %Y %H:%M:%S %Z", &LocalTime);
        return std::string(buffer);
    }

    bool GetStrLine(const char* InData, char* OutDataBuffer, int BufferSize, const char*& OutLineEndPtr)
    {
        assert(InData);

        OutLineEndPtr = (strchr(InData, '\n') != NULL) ? strchr(InData, '\n') : strchr(InData, '\0');
        int LineLength = (OutLineEndPtr - InData);

        if(OutLineEndPtr == NULL || *OutLineEndPtr == '\0' || LineLength > BufferSize)
        {
            assert(LineLength < BufferSize);
            return false;
        }

        memcpy(OutDataBuffer, InData, LineLength);
        OutDataBuffer[LineLength] = '\0';
        OutLineEndPtr++;
        return true;
    }

    int FindCharIndex(const char* InCharArray, const char c)
    {
        const char* CharLocation;
        CharLocation = strchr(InCharArray, c);
        return (CharLocation != nullptr) ? (int) (CharLocation - InCharArray) : -1;
    }
}

namespace WSHelpers
{
    static std::string RequestKeyMagicString = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    static const std::string base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string GetWebSocketAcceptValue(std::string InAcceptKey)
    {
        InAcceptKey = InAcceptKey + RequestKeyMagicString;

        sha1::SHA1 s;
        s.processBytes(InAcceptKey.c_str(), InAcceptKey.size());
        uint32_t digest[5];
        s.getDigest(digest);

        unsigned char sha1_bytes[20];
        for(int i = 0; i < 5; ++i)
        {
            sha1_bytes[i * 4 + 0] = (digest[i] >> 24) & 0xFF;
            sha1_bytes[i * 4 + 1] = (digest[i] >> 16) & 0xFF;
            sha1_bytes[i * 4 + 2] = (digest[i] >> 8) & 0xFF;
            sha1_bytes[i * 4 + 3] = (digest[i] >> 0) & 0xFF;
        }

        return base64_encode((unsigned char*) sha1_bytes, 20);
    }

    std::string base64_encode(unsigned char const* buf, unsigned int bufLen)
    {
        std::string ret;
        int i = 0;
        int j = 0;
        unsigned char char_array_3[3];
        unsigned char char_array_4[4];

        while(bufLen--)
        {
            char_array_3[i++] = *(buf++);
            if(i == 3)
            {
                char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                char_array_4[3] = char_array_3[2] & 0x3f;

                for(i = 0; (i < 4); i++)
                    ret += base64_chars[char_array_4[i]];
                i = 0;
            }
        }

        if(i)
        {
            for(j = i; j < 3; j++)
                char_array_3[j] = '\0';

            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for(j = 0; (j < i + 1); j++)
                ret += base64_chars[char_array_4[j]];

            while((i++ < 3))
                ret += '=';
        }

        return ret;
    }

}