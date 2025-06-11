#pragma once
#include "Common.h"

//TODO: Investigate UDP and POST

namespace WebServer
{
    class ServerRequestMessage;
    class ServerResponseMessage;
    class WebSocketHandle;
    class WebSocketMessage;

    struct WebSocketInfo;

    enum class ServerRequestType
    {
        ServerRequestType_Invalid,
        ServerRequestType_GET,
        ServerRequestType_POST,
        ServerRequestType_PUT,
        ServerRequestType_PATCH,
        ServerRequestType_DELETE,
    };

    const std::map<ServerRequestType, std::string> ServerRequestTypeStrings =
    {
        { ServerRequestType::ServerRequestType_Invalid, "Invalid-Request" },
        { ServerRequestType::ServerRequestType_GET, "GET" },
        { ServerRequestType::ServerRequestType_POST, "POST" },
        { ServerRequestType::ServerRequestType_PUT, "PUT" },
        { ServerRequestType::ServerRequestType_PATCH, "PATCH" },
        { ServerRequestType::ServerRequestType_DELETE, "DELETE" },
    };

    enum class ServerResponseStatusCode
    {
        ServerResponseStatusCode_Invalid,
        ServerResponseStatusCode_100,
        ServerResponseStatusCode_101,
        ServerResponseStatusCode_200,
        ServerResponseStatusCode_201,
        ServerResponseStatusCode_202,
        ServerResponseStatusCode_400,
        ServerResponseStatusCode_401,
        ServerResponseStatusCode_403,
        ServerResponseStatusCode_404,
        ServerResponseStatusCode_408,
        ServerResponseStatusCode_429,
        ServerResponseStatusCode_500,
        ServerResponseStatusCode_501,
        ServerResponseStatusCode_503,
    };

    const std::map<ServerResponseStatusCode, std::string> ServerResponseStatusStrings =
    {
        { ServerResponseStatusCode::ServerResponseStatusCode_100, "100 Continue" },
        { ServerResponseStatusCode::ServerResponseStatusCode_101, "101 Switching Protocols" },
        { ServerResponseStatusCode::ServerResponseStatusCode_200, "200 Ok" },
        { ServerResponseStatusCode::ServerResponseStatusCode_201, "201 Created" },
        { ServerResponseStatusCode::ServerResponseStatusCode_202, "202 Accepted" },
        { ServerResponseStatusCode::ServerResponseStatusCode_400, "400 Bad Request" },
        { ServerResponseStatusCode::ServerResponseStatusCode_401, "401 Unauthorized" },
        { ServerResponseStatusCode::ServerResponseStatusCode_403, "403 Forbidden" },
        { ServerResponseStatusCode::ServerResponseStatusCode_404, "404 Not Found" },
        { ServerResponseStatusCode::ServerResponseStatusCode_408, "408 Request Timeout" },
        { ServerResponseStatusCode::ServerResponseStatusCode_429, "429 Too Many Requests" },
        { ServerResponseStatusCode::ServerResponseStatusCode_500, "500 Internal Server Error" },
        { ServerResponseStatusCode::ServerResponseStatusCode_501, "501 Not Implemented" },
        { ServerResponseStatusCode::ServerResponseStatusCode_503, "503 Service Unavailable" },
    };

    WEBSERVERLIBRARY_API enum class WebSocketOpCode : uint16_t
    {
        WebSocketOpCode_Invalid = -1,
        WebSocketOpCode_continuation = 0,
        WebSocketOpCode_text = 1,
        WebSocketOpCode_binary = 2,
    };

    typedef std::pair<MilliSecStopwatch, std::function<void()>> TimedFunctionPair;
    
    struct ReceiveDataTickInfo
    {
        std::vector<TimedFunctionPair> TimedFunctions;
        TimedFunctionPair ReceiveTimeout;

        std::function<void(SocketDataStream&&)> MessageRecievedCallback;
        std::function<void()> ErrorCallback;

        SocketDataStream ReceiveDataStream;
    };

    //TODO: add client ID to receive data
    //TODO: Does send data need client id?
    typedef std::function<void(const char*, int, WebSocketOpCode)> WebSocketReceiveDataCallBack;
    typedef std::function<void(const char*, int, WebSocketOpCode)> WebSocketSendDataFunc;
    typedef std::function<void(std::string, uint64_t)> WebSocketClientJoinedCallback; // url, client id add to create web socket

    struct WebSocketInfo
    {
        WebSocketClientJoinedCallback ClientJoinedCallback;
        WebSocketReceiveDataCallBack RecieveDataCallbackFunction;
        std::map<uint64_t, WebSocketSendDataFunc> SendDataFunctions;
    };

    class ListenServer
    {
    public:
        ListenServer() = default;
        ListenServer& operator=(ListenServer&& Other) = delete;
        ListenServer(const ListenServer& Other) = delete;
        ListenServer(ListenServer&& Other) = delete;

        ~ListenServer();

        int Initialise(const char* PortNumber);
        void AsyncStart();
        int CloseServer();

        //TODO: Add synchronous start functionality, will invlove a list of handles which will need checking

        void UploadData(const std::string& Url, std::vector<char> Data, const std::string& ContentType, std::vector<std::pair<std::string, std::string>> MessageHeaders);

        void CreateWebSocket(const std::string& Url, WebSocketReceiveDataCallBack RecieveDataCallback, WebSocketClientJoinedCallback ClientJoinedCallback);
        void SendWebSocketMessage(const std::string& Url, uint64_t ClientId, const char* Content, int ContentLen, WebSocketOpCode OpCode);

    private:
        void ListenServerMainThread();

        void HandleServerRequest(SOCKET ClientSocket, ServerRequestMessage& RequestMessage);
        void HandleWebSocketRequest(SOCKET ClientSocket, const ServerRequestMessage& RequestMessage);

        SOCKET mListenSocket = INVALID_SOCKET;
        std::thread mListenThread;
        std::atomic<bool> bRunListenServer = false;

        std::map<SOCKET, ReceiveDataTickInfo> mSocketsReceivingData;

        std::map<std::string, ServerResponseMessage> mUrlData;

        std::map<SOCKET, WebSocketHandle> mActiveWebSockets;
        std::map<std::string, WebSocketInfo> mWebSocketsInfo;
    };

    class ServerRequestMessage
    {
    public:
        void BuildFromDataStream(const SocketDataStream& DataStream);
        bool CheckHeaderValue(const std::string& InHeader, const std::string& InValue);

        void DebugPrint();

        bool bIsMessageComplete = false;

        ServerRequestType mRequestType = ServerRequestType::ServerRequestType_Invalid;
        std::string mUrl = "";
        std::string mQuery = "";
        std::map<std::string, std::string> mHeaders;
    };

    class ServerResponseMessage
    {
    public:
        ServerResponseMessage(ServerResponseStatusCode MessageStatus);
        ServerResponseMessage(const ServerResponseMessage& Other);
        ServerResponseMessage(ServerResponseMessage&& Other) noexcept;
        ~ServerResponseMessage();

        void AddContent(const std::vector<char>& MessageContent, const std::string& ContentType);
        void AddMessageHeaders(const std::vector<std::pair<std::string, std::string>>& MessageHeaders);
        void BuildMessage();

        void DebugPrint();

        const char* mMessage = nullptr;
        int mMessageLength = 0;

        ServerResponseStatusCode mStatusCode = ServerResponseStatusCode::ServerResponseStatusCode_Invalid;
        std::map<std::string, std::string> mHeaders;

    private:
        const char* mContentData = nullptr;
        int mContentLength = 0;
    };

    class WebSocketMessage
    {
        //TODO: implement masking for send

    public:
        WebSocketMessage() = default;
        ~WebSocketMessage();
        WebSocketMessage& operator=(WebSocketMessage&& Other) noexcept;
        WebSocketMessage(const WebSocketMessage& Other);
        WebSocketMessage(WebSocketMessage&& Other);

        void BuildFromDataStream(const SocketDataStream& InDataStream);
        void ConcatMessage(WebSocketMessage& InContinuationMessage);

        void BuildFromContent(bool InIsCompleteMessage, WebSocketOpCode InOpCode, const char* InContent, uint64_t InContentLen);
        int GetGeneratedMessageLength();
        void GenerateMessage(char* OutMessageBuffer, int InbufferLength);

        void DebugPrint();

        bool bIsComplete = false;
        WebSocketOpCode mOpCode = WebSocketOpCode::WebSocketOpCode_Invalid;

        char* mContent = nullptr;
        uint64_t mContentLen = 0;
    };

    class WebSocketHandle
    {
    public:
        WebSocketHandle() = default;
        WebSocketHandle(const WebSocketHandle& Other) = delete;
        WebSocketHandle& operator=(WebSocketHandle&& Other) noexcept;
        WebSocketHandle(WebSocketHandle&& Other);

        void StartWebSocketThread(SOCKET ClientSocket, WebSocketReceiveDataCallBack RecieveDataCallback);
        void EndWebSocketThread();
        void AddMessageToSendQueue(const char* InContent, int InContentLen, WebSocketOpCode InOpCode);

    private:
        void WebSocketMainThread();
        void SendWebMessage(WebSocketMessage& Message);

        SOCKET mClientSocket{};
        std::thread mMainThread;
        WebSocketReceiveDataCallBack mRecieveDataCallback;
        ThreadQueue<WebSocketMessage> mSendMessageQueue;

        std::atomic<bool> bRunThread = false;
    };
}
