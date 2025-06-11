#include "WebServer.h"

#include <iostream>
#include <fstream>
#include <chrono>
#include <memory>
#include <cassert>
#include <bitset>

//helpers
namespace 
{
    using namespace WebServer;
    constexpr int SecondsToMs = 1000;

    enum class StatusLogSeverity : uint16_t
    {
        StatusLogSeverity_Invalid,
        StatusLogSeverity_Log,
        StatusLogSeverity_Warning,
        StatusLogSeverity_Error,
    };

    void StatusLogPost(std::string Message, StatusLogSeverity LogSeverity)
    {
        if(LogSeverity == StatusLogSeverity::StatusLogSeverity_Error)
        {
            std::cout << OutputServerStatus() << Message << "\n";
        }
        else if(LogSeverity == StatusLogSeverity::StatusLogSeverity_Log)
        {
            std::cout << OutputServerTime_GetTime() << Message << "\n";
        }
    }

    addrinfo BuildAddressInfoHints(int Protocol)
    {
        addrinfo AddressInfo;

        ZeroMemory(&AddressInfo, sizeof(AddressInfo));
        AddressInfo.ai_family = AF_INET;
        AddressInfo.ai_socktype = SOCK_STREAM;
        AddressInfo.ai_protocol = Protocol;
        AddressInfo.ai_flags = AI_PASSIVE;

        return AddressInfo;
    }

    int SetupNonBlockingSocket(const char* PortNumber, SOCKET& OutSocket)
    {
        // Resolve the local address and port to be used by the server
        addrinfo AddressInfoHints = BuildAddressInfoHints(IPPROTO_TCP);
        addrinfo* AddressInfo = nullptr;
        int Result_AddressInfo = getaddrinfo(NULL, PortNumber, &AddressInfoHints, &AddressInfo);
        if(Result_AddressInfo != 0)
        {
            printf("getaddrinfo failed: %d\n", Result_AddressInfo);
            return 1;
        }

        //Create socket
        OutSocket = socket(AddressInfo->ai_family, AddressInfo->ai_socktype, AddressInfo->ai_protocol);
        if(OutSocket == INVALID_SOCKET)
        {
            printf("Error at socket(): %ld\n", WSAGetLastError());
            return 1;
        }

        u_long NonBlockingMode = 1;
        int Result_SocketMode = ioctlsocket(OutSocket, FIONBIO, &NonBlockingMode);
        if(Result_SocketMode != NO_ERROR)
        {
            printf("Socket mode change failed: %d\n", WSAGetLastError());
            return 1;
        }

        // Setup the TCP listening socket
        int Result_Bind = bind(OutSocket, AddressInfo->ai_addr, (int) AddressInfo->ai_addrlen);
        if(Result_Bind == SOCKET_ERROR)
        {
            printf("bind failed with error: %d\n", WSAGetLastError());
            return 1;
        }

        freeaddrinfo(AddressInfo);
        return 0;
    }

    ServerResponseMessage BuildBasicStatusResponse(ServerResponseStatusCode StatusCode)
    {
        //TODO: Add functionality to upload custom status responses from dll API

        ServerResponseMessage StatusResponseMessage(StatusCode);
        StatusResponseMessage.AddMessageHeaders({ std::make_pair("Connection", "Close") });

        std::vector<char> PageHtml = GenerateHtmlPage(ServerResponseStatusStrings.at(StatusCode));
        StatusResponseMessage.AddContent(PageHtml, "text/html; charset=utf-8");
        StatusResponseMessage.BuildMessage();

        return StatusResponseMessage;
    }

    void PopulateStatusMessageResponses(std::map<std::string, ServerResponseMessage>& UrlData)
    {
        for(const auto& StatusResponsePair : ServerResponseStatusStrings)
        {
            ServerResponseMessage StatusResponseMessage = BuildBasicStatusResponse(StatusResponsePair.first);
            UrlData.emplace(StatusResponsePair.second, std::move(StatusResponseMessage));
        }

        ServerResponseMessage WebSocketSucessBaseMessage(ServerResponseStatusCode::ServerResponseStatusCode_101);
        WebSocketSucessBaseMessage.AddMessageHeaders({std::make_pair("Connection", "Upgrade")});
        WebSocketSucessBaseMessage.AddMessageHeaders({ std::make_pair("Upgrade", "websocket") });
        UrlData.emplace("websocket-success-base", std::move(WebSocketSucessBaseMessage));
    }

    bool SendServerResponseMessage(SOCKET ClientSocket, const ServerResponseMessage& MessageData)
    {
        int SendResult = send(ClientSocket, MessageData.mMessage, MessageData.mMessageLength, 0);
        if(SendResult == SOCKET_ERROR)
        {
            StatusLogPost("Send message failed", StatusLogSeverity::StatusLogSeverity_Error);
            return false;
        }

        std::cout << "Reply-Send - Success - Bytes sent: " << SendResult;
        return true;
    }

    void SendServerStatusResponse(SOCKET ClientSocket, std::string LogMessage, ServerResponseStatusCode StatusCode, const std::map<std::string, ServerResponseMessage>& UrlData)
    {
        StatusLogPost(LogMessage, StatusLogSeverity::StatusLogSeverity_Error);

        const std::string& StatusResponseUrl = ServerResponseStatusStrings.at(StatusCode);
        const ServerResponseMessage& StatusResponseMessage = UrlData.at(StatusResponseUrl);
        SendServerResponseMessage(ClientSocket, StatusResponseMessage);
    }

    void ReceiveMessageTick(SOCKET ClientSocket, std::function<void(SocketDataStream&&)> MessageRecievedCallback, std::function<void()> ErrorCallback)
    {
        char Receivebuffer[DEFAULT_BUFLEN]; SocketDataStream wsDataStream{};    //TODO: take these as parameters to save allocs

        int ReceiveResult = recv(ClientSocket, Receivebuffer, DEFAULT_BUFLEN, 0);
        if(ReceiveResult > 0)
        {
            wsDataStream.StreamRequestData(Receivebuffer, ReceiveResult);
            MessageRecievedCallback(std::move(wsDataStream));
        }
        else if(WSAGetLastError() != WSAEWOULDBLOCK)
        {
            ErrorCallback();
        }     
    }

    void ReceieveMessageLoop(SOCKET ClientSocket, std::atomic<bool>* LoopCondition, const std::vector<TimedFunctionPair>& TimedFunctions,
        std::function<void(SocketDataStream&&)> MessageRecievedCallback, std::function<void()> ErrorCallback, TimedFunctionPair TimeOutCallBack)
    {
        auto ServerTime = std::chrono::system_clock::now();

        auto RecievedDataCallback = [&] (SocketDataStream&& DataReceived) {TimeOutCallBack.first.ResetClock(ServerTime); MessageRecievedCallback(std::move(DataReceived)); };
        auto ReceiveErrorCallBack = [&] () {*LoopCondition = false; ErrorCallback(); };

        while(*LoopCondition == true)
        {
            ServerTime = std::chrono::system_clock::now();

            for(const auto& TimedFunctionPair : TimedFunctions)
            {
                if(TimedFunctionPair.first.DurationReached(ServerTime))
                {
                    TimedFunctionPair.second();
                }
            }

            ReceiveMessageTick(ClientSocket, RecievedDataCallback, ReceiveErrorCallBack);

            if(TimeOutCallBack.first.DurationReached(ServerTime) && *LoopCondition == true)
            {
                TimeOutCallBack.second();
                *LoopCondition = false;
            }
        }
    }

    void HandleSocketDataReceiveTick(SOCKET ClientSocket, const ReceiveDataTickInfo& ReceiveTickInfo)
    {
        auto ServerTime = std::chrono::system_clock::now();

        ReceiveMessageTick(ClientSocket, ReceiveTickInfo.MessageRecievedCallback, ReceiveTickInfo.ErrorCallback);

        for(const auto& TimedFunctionPair : ReceiveTickInfo.TimedFunctions)
        {
            if(TimedFunctionPair.first.DurationReached(ServerTime))
            {
                TimedFunctionPair.second();
            }
        }

        if(ReceiveTickInfo.ReceiveTimeout.first.DurationReached(ServerTime))
        {
            ReceiveTickInfo.ReceiveTimeout.second();
        }
    }

    void BuildReceiveTickInfo(ReceiveDataTickInfo& ReceiveDataTickInfo, SOCKET ClientSocket, std::function<void(SOCKET, ServerRequestMessage&)> OnReceivedServerRequest, std::function<void(SOCKET, bool)> OnReceiveFinished, const std::map<std::string, ServerResponseMessage>& UrlData)
    {
        auto ServerTime = std::chrono::system_clock::now();

        std::function<void()> ReceiveTimeoutCallBack = [=] () {
            SendServerStatusResponse(ClientSocket, "recv - Timed out", ServerResponseStatusCode::ServerResponseStatusCode_408, UrlData);
            OnReceiveFinished(ClientSocket, false);
            };

        std::function<void()> ReceiveErrorCallBack = [=] () {
            SendServerStatusResponse(ClientSocket, "recv - failed", ServerResponseStatusCode::ServerResponseStatusCode_500, UrlData);
            OnReceiveFinished(ClientSocket, false);
            };

        ReceiveDataTickInfo.ReceiveTimeout = { MilliSecStopwatch{ ServerTime, 5 * SecondsToMs }, ReceiveTimeoutCallBack };
        ReceiveDataTickInfo.TimedFunctions.push_back(std::make_pair(MilliSecStopwatch{ ServerTime, 250 }, std::bind(StatusLogPost, "recv - Awaiting data", StatusLogSeverity::StatusLogSeverity_Log)));
        ReceiveDataTickInfo.ErrorCallback = ReceiveErrorCallBack;

        const auto MessageRecievedCallback = [&, ClientSocket, OnReceivedServerRequest, OnReceiveFinished] (SocketDataStream&& DataStream)
            {
                ServerTime = std::chrono::system_clock::now();
                std::cout << OutputServerTime(ServerTime) << "recv - Bytes received: " << DataStream.GetDataLen() << "\n";

                ReceiveDataTickInfo.ReceiveDataStream.StreamRequestData(DataStream.mData, DataStream.GetDataLen());

                ServerRequestMessage RequestMessage;
                RequestMessage.BuildFromDataStream(ReceiveDataTickInfo.ReceiveDataStream);
                if(RequestMessage.bIsMessageComplete)
                {
                    StatusLogPost("recv - Request Complete - Proceeding to response", StatusLogSeverity::StatusLogSeverity_Log);
                    RequestMessage.DebugPrint();

                    OnReceivedServerRequest(ClientSocket, RequestMessage);
                    if(RequestMessage.CheckHeaderValue("Connection", "keep-alive") == false)
                    {
                        OnReceiveFinished(ClientSocket, RequestMessage.CheckHeaderValue("Connection", "Upgrade"));
                    }
                }
            };

        ReceiveDataTickInfo.MessageRecievedCallback = MessageRecievedCallback;
    }

    bool IsServerRequestComplete(const char* InRequestData, int InRequestDataLen)
    {
        return InRequestDataLen != -1 && InRequestData[InRequestDataLen - 4] == '\r' && InRequestData[InRequestDataLen - 3] == '\n'
            && InRequestData[InRequestDataLen - 2] == '\r' && InRequestData[InRequestDataLen - 1] == '\n';
    }

    bool IsServerRequestStartLine(const char* HttpRequestLine, int Len)
    {
        constexpr char const* HttpRequestStartLineEnding = "HTTP/1.1\r";
        int StartLineEndingLength = std::strlen(HttpRequestStartLineEnding);

        const char* RequestLineOffset = &HttpRequestLine[Len - StartLineEndingLength];
        return std::strncmp(RequestLineOffset, HttpRequestStartLineEnding, StartLineEndingLength) == 0;
    }

    ServerRequestType ResolveServerRequestDetails(char* HttpRequestLine, std::string& OutUrl, std::string& OutQuery)
    {
        int UrlStartIndex = FindCharIndex(HttpRequestLine, '/');
        int QueryStartIndex = FindCharIndex(HttpRequestLine, '?');

        char* UrlStart = &HttpRequestLine[UrlStartIndex];
        char* QueryStart = (QueryStartIndex != -1) ? &HttpRequestLine[QueryStartIndex] : nullptr;

        int UrlLen = (QueryStartIndex != -1) ? QueryStartIndex - UrlStartIndex : FindCharIndex(UrlStart, ' ');
        OutUrl = std::string(UrlStart, UrlLen);

        if(QueryStart != nullptr)
        {
            int QueryLength = FindCharIndex(QueryStart, ' ');
            OutQuery = std::string(&HttpRequestLine[QueryStartIndex + 1], QueryLength - 1);
        }

        for(const auto& RequestTypePair : ServerRequestTypeStrings)
        {
            if(std::strncmp(HttpRequestLine, RequestTypePair.second.c_str(), RequestTypePair.second.size()) == 0)
            {
                return RequestTypePair.first;
            }
        }
    }

    std::pair<std::string, std::string> ResolveRequestHeader(char* RequestLine)
    {
        int MetaTagKeyEndIndex = FindCharIndex(RequestLine, ':');
        std::string MetaTagKey = std::string(RequestLine, MetaTagKeyEndIndex);

        int MetaTagValueOffset = FindCharIndex(&RequestLine[MetaTagKeyEndIndex + 2], '\r');
        std::string MetaTagValue = std::string(&RequestLine[MetaTagKeyEndIndex + 2], MetaTagValueOffset);

        return std::make_pair(MetaTagKey, MetaTagValue);
    }

    std::vector<std::pair<std::string, std::string>> GenerateMetaDataMessageHeaders()
    {
        std::vector<std::pair<std::string, std::string>> MetaDataHeaders;
        MetaDataHeaders.push_back(std::make_pair("Date", ConvertToWebServerTimeFormat(GetLocalTime())));
        return MetaDataHeaders;
    }

    bool ValidResponseMessageData(const ServerResponseMessage& Message, const char* MessageContent, int ContentLength)
    {
        bool Valid = Message.mStatusCode != ServerResponseStatusCode::ServerResponseStatusCode_Invalid;

        auto ContentTypeHeader = Message.mHeaders.find("Content-Type");
        if(ContentTypeHeader != Message.mHeaders.end() && ContentTypeHeader->second == "image/webp")
        {
            Valid &= MessageContent != nullptr && ContentLength > 1;
            //image checks
        }
        else if(ContentTypeHeader != Message.mHeaders.end() && ContentTypeHeader->second == "text/html")
        {
            Valid &= MessageContent != nullptr && ContentLength > 1;
            //html checks
        }

        return Valid;
    }

    int PrintnResponseMessageStatus(char* Buffer, int BufLen, ServerResponseStatusCode StatusCode)
    {
        const std::string& StatusString = ServerResponseStatusStrings.at(StatusCode);
        return snprintf(Buffer, BufLen, "%s %s\r\n", "HTTP/1.1", StatusString.c_str());
    }

    int PrintnResponseMessageHeaders(char* Buffer, int BufLen, std::map<std::string, std::string>& Headers)
    {
        int HeadersLength = 0;
        for(const auto& Header : Headers)
        {
            HeadersLength += snprintf(Buffer + HeadersLength, BufLen, "%s: %s\r\n", Header.first.c_str(), Header.second.c_str());
        }
        HeadersLength += snprintf(Buffer + HeadersLength, BufLen, "\r\n");

        return HeadersLength;
    }

    ServerResponseMessage BuildWSHandshakeAcceptResponse(const ServerRequestMessage& InRequestMessage, const ServerResponseMessage& AcceptMessageBase)
    {
        std::string wsRequestKey = (*InRequestMessage.mHeaders.find("Sec-WebSocket-Key")).second;
        std::string wsAcceptValue = WSHelpers::GetWebSocketAcceptValue(wsRequestKey);

        ServerResponseMessage AcceptResponse = AcceptMessageBase;
        AcceptResponse.AddMessageHeaders({ std::make_pair("Sec-WebSocket-Accept", wsAcceptValue) });
        return AcceptResponse;
    }

    void WSHandleMessageRecieved(const SocketDataStream& DataStream, WebSocketMessage& OutMessage)
    {
        WebSocketMessage MessageRecieved;
        MessageRecieved.BuildFromDataStream(DataStream);

        bool ContinuationMessage = MessageRecieved.mOpCode == WebSocketOpCode::WebSocketOpCode_continuation;
        if(ContinuationMessage)
        {
            // TODO: content getting copied twice, once in BuildFromDataStream then concat, if know continuation prior can refactor to copy once
            OutMessage.ConcatMessage(MessageRecieved);
        }
        else
        {
            OutMessage = std::move(MessageRecieved);
        }
    }

    int GetWSMessageContentLength(const unsigned char* WebSocketMessage, int& OutLengthEndIndex)
    {
        int ContentLength = WebSocketMessage[1] & 0b01111111;
        OutLengthEndIndex = 2;

        if(ContentLength == 126)  // Read the next two bytes for a 16 bit int
        {
            ContentLength = (WebSocketMessage[OutLengthEndIndex] << 8) | WebSocketMessage[OutLengthEndIndex + 1];
            OutLengthEndIndex += 2;
        }
        else if(ContentLength == 127) // Read the next four bytes for a 32 bit int
        {
            assert(false); // TODO: this should be 8 bytes, won't fit in output lol

            ContentLength = WebSocketMessage[OutLengthEndIndex] << 24 | WebSocketMessage[OutLengthEndIndex + 1] << 16
                | WebSocketMessage[OutLengthEndIndex + 2] << 8 | WebSocketMessage[OutLengthEndIndex + 3];
            OutLengthEndIndex += 4;
        }

        return ContentLength;
    }

    void WSApplyDataMask(char* Content, int ContentLen, std::array<unsigned char, 4> Mask)
    {
        int maskIndex = 0;
        for(size_t i = 0; i < ContentLen; i++)
        {
            Content[i] = Content[i] ^ Mask[maskIndex];
            maskIndex = (maskIndex + 1) % 4;
        }
    }

    constexpr uint8_t GetWSEncodedContentLength(int ContentLength)
    {
        int EncodedLength = (ContentLength > 126) ? ((ContentLength > UINT16_MAX) ? 127 : 126) : ContentLength;
        return static_cast<uint8_t>(EncodedLength);
    }

    constexpr uint8_t GetWSEncodedLengthByteCount(int ContentLength)
    {
        return (ContentLength > 126) ? ((ContentLength > UINT16_MAX) ? 8 : 2) : 0;
    }

    void WSPackMessageLength(char* OutMessageBuffer, int ContentLength, int& OutLengthEndIndex)
    {
        constexpr int BufferStartIndex = 2;

        int NumExtendedPayloadLength = GetWSEncodedLengthByteCount(ContentLength);
        OutLengthEndIndex = BufferStartIndex + NumExtendedPayloadLength;

        for(size_t i = BufferStartIndex; i < OutLengthEndIndex; i++)
        {
            int BitShiftAmount = (NumExtendedPayloadLength - ((i - BufferStartIndex) + 1)) * 8;
            OutMessageBuffer[i] = (char) ((ContentLength >> BitShiftAmount) & 0xff);
        }
    }
}

namespace WebServer
{

#pragma region ListenServer

    ListenServer::~ListenServer()
    {
        if(mListenSocket != INVALID_SOCKET) { closesocket(mListenSocket); }
    }

    int ListenServer::Initialise(const char* PortNumber)
    {
        PopulateStatusMessageResponses(mUrlData);
        return SetupNonBlockingSocket(PortNumber, mListenSocket);
    }

    void ListenServer::AsyncStart()
    {
        bRunListenServer = true;
        mListenThread = std::thread(&ListenServer::ListenServerMainThread, this);
    }

    int ListenServer::CloseServer()
    {
        bRunListenServer = false;

        if(mListenThread.joinable())
        {
            mListenThread.join();
        }

        return 0;
    }

    void ListenServer::UploadData(const std::string& Url, std::vector<char> Data, const std::string& ContentType, std::vector<std::pair<std::string, std::string>> MessageHeaders)
    {
        ServerResponseMessage ServerResponseMessage(ServerResponseStatusCode::ServerResponseStatusCode_200);
        ServerResponseMessage.AddContent(Data, ContentType);
        ServerResponseMessage.AddMessageHeaders(MessageHeaders);
        ServerResponseMessage.BuildMessage();

        mUrlData.emplace(Url, std::move(ServerResponseMessage));
    }

    void ListenServer::CreateWebSocket(const std::string& Url, WebSocketReceiveDataCallBack RecieveDataCallback, WebSocketClientJoinedCallback ClientJoinedCallback)
    {
        ServerResponseMessage WebSocketEmptySuccessResponse(ServerResponseStatusCode::ServerResponseStatusCode_100);
        WebSocketInfo WebSocketInfo{ ClientJoinedCallback, RecieveDataCallback };

        mUrlData.emplace(Url, std::move(WebSocketEmptySuccessResponse));
        mWebSocketsInfo.emplace(Url, WebSocketInfo);
    }

    void ListenServer::SendWebSocketMessage(const std::string& Url, uint64_t ClientId, const char* Content, int ContentLen, WebSocketOpCode OpCode)
    {
        auto WSSendFunction = mWebSocketsInfo.at(Url).SendDataFunctions.at(ClientId);
        WSSendFunction(Content, ContentLen, OpCode);
    }

    void ListenServer::ListenServerMainThread()
    {
        using namespace std::placeholders;

        if(listen(mListenSocket, SOMAXCONN) == SOCKET_ERROR)
        {
            StatusLogPost("Serv - Listen failed", StatusLogSeverity::StatusLogSeverity_Error);
            bRunListenServer = false;
        }

        auto ServerTime = std::chrono::system_clock::now();
        MilliSecStopwatch ServerStatusClock{ ServerTime, 500 };

        std::vector<std::pair<SOCKET, bool>> SocketsFinishedReceiving;
        auto OnReceiveFinished = [&] (SOCKET Socket, bool KeepSocketOpen) {SocketsFinishedReceiving.push_back({Socket, KeepSocketOpen}); };

        while(bRunListenServer)
        {
            ServerTime = std::chrono::system_clock::now();

            if(ServerStatusClock.DurationReached(ServerTime))
            {
                StatusLogPost("Serv-Listen - Running listen server", StatusLogSeverity::StatusLogSeverity_Log);
            }

            // Accept connections
            SOCKET ClientSocket = INVALID_SOCKET;
            ClientSocket = accept(mListenSocket, NULL, NULL);
            if(ClientSocket != INVALID_SOCKET && mSocketsReceivingData.find(ClientSocket) == mSocketsReceivingData.end())
            {
                StatusLogPost("Serv-Listen - Accepted message - Proceeding to recieve data", StatusLogSeverity::StatusLogSeverity_Log);

                ReceiveDataTickInfo& ReceiveTickInfo = mSocketsReceivingData[ClientSocket];
                BuildReceiveTickInfo(ReceiveTickInfo, ClientSocket, std::bind(&ListenServer::HandleServerRequest, this, _1, _2), OnReceiveFinished, mUrlData);
            }
            else if(WSAGetLastError() != WSAEWOULDBLOCK)
            {
                StatusLogPost("Serv - Listen - accept failed", StatusLogSeverity::StatusLogSeverity_Error);
                bRunListenServer = false;
            }

            // Receive data
            for(const auto& ReceiveTickPair : mSocketsReceivingData)
            {
                SOCKET Socket = ReceiveTickPair.first;
                const ReceiveDataTickInfo& ReceiveDataInfo = ReceiveTickPair.second;
                HandleSocketDataReceiveTick(Socket, ReceiveDataInfo);
            }

            // Clear sockets
            for(auto SocketCloseDetails : SocketsFinishedReceiving)
            {
                SOCKET Socket = SocketCloseDetails.first;
                bool KeepSocketAlive = SocketCloseDetails.second;

                mSocketsReceivingData.erase(Socket);
                if(KeepSocketAlive == false)
                {
                    shutdown(Socket, SD_SEND);
                    closesocket(Socket);
                }
            }
        }
    }

    void ListenServer::HandleServerRequest(SOCKET ClientSocket, ServerRequestMessage& RequestMessage)
    {
        if(RequestMessage.mRequestType != ServerRequestType::ServerRequestType_GET)
        {
            SendServerStatusResponse(ClientSocket, "Response - failed: 501 Request Not Implemented", ServerResponseStatusCode::ServerResponseStatusCode_501, mUrlData);
            return;
        }

        if(mUrlData.find(RequestMessage.mUrl) == mUrlData.end())
        {
            SendServerStatusResponse(ClientSocket, "Response - failed: 404 Page Not Found\n", ServerResponseStatusCode::ServerResponseStatusCode_404, mUrlData);
            return;
        }

        if(RequestMessage.CheckHeaderValue("Connection", "Upgrade") && RequestMessage.CheckHeaderValue("Upgrade", "websocket"))
        {
            StatusLogPost("Response - Success - Web socket upgrade requested", StatusLogSeverity::StatusLogSeverity_Log);
            HandleWebSocketRequest(ClientSocket, RequestMessage);
            return;
        }

        StatusLogPost("Response - Success - Proceeding to send reply", StatusLogSeverity::StatusLogSeverity_Log);

        const ServerResponseMessage& ResponseMessage = mUrlData.at(RequestMessage.mUrl);
        SendServerResponseMessage(ClientSocket, ResponseMessage);
    }

    void ListenServer::HandleWebSocketRequest(SOCKET ClientSocket, const ServerRequestMessage& RequestMessage)
    {
        using namespace std::placeholders;

        ServerResponseMessage wsAcceptResponse = BuildWSHandshakeAcceptResponse(RequestMessage, mUrlData.find("websocket-success-base")->second);
        SendServerResponseMessage(ClientSocket, wsAcceptResponse);

        mActiveWebSockets.emplace(std::make_pair(ClientSocket, WebSocketHandle{}));
        WebSocketHandle& wsHandle =  mActiveWebSockets.at(ClientSocket);
        WebSocketSendDataFunc wsPushMessageFunction = std::bind(&WebSocketHandle::AddMessageToSendQueue, &wsHandle, _1, _2, _3);
        
        uint64_t wsClientId = (uint64_t) ClientSocket;
        WebSocketInfo& wsInfo = mWebSocketsInfo.at(RequestMessage.mUrl);
        wsInfo.SendDataFunctions.emplace(wsClientId, wsPushMessageFunction);

        wsHandle.StartWebSocketThread(ClientSocket, wsInfo.RecieveDataCallbackFunction);
        wsInfo.ClientJoinedCallback(RequestMessage.mUrl, wsClientId);
    }

#pragma endregion   //ListenServer

#pragma region ServerRequestMessage

    void ServerRequestMessage::BuildFromDataStream(const SocketDataStream& DataStream)
    {
        bIsMessageComplete = IsServerRequestComplete(DataStream.mData, DataStream.GetDataLen());

        std::string LogMessage = "Computing request data (Request " + bIsMessageComplete ? "Complete)" : "Incomplete)";
        StatusLogPost(LogMessage, StatusLogSeverity::StatusLogSeverity_Log);

        char LineBuffer[1024];
        const char* LineIteratorPtr = DataStream.mData;
        while(GetStrLine(LineIteratorPtr, LineBuffer, 1024, LineIteratorPtr))
        {
            int LineLength = strlen(LineBuffer);
            bool IsCompleteRequestLine = LineLength > 2 && LineBuffer[LineLength - 1] == '\r';

            if(IsCompleteRequestLine && IsServerRequestStartLine(LineBuffer, LineLength))
            {
                mRequestType = ResolveServerRequestDetails(LineBuffer, mUrl, mQuery);
            }
            else if(IsCompleteRequestLine)
            {
                mHeaders.emplace(ResolveRequestHeader(LineBuffer));
            }
        }

        // TODO: if message had content parse that (eg POST)
        // Any errors in parsing need the bad request response message
    }

    bool ServerRequestMessage::CheckHeaderValue(const std::string& InHeader, const std::string& InValue)
    {
        auto header = mHeaders.find(InHeader);
        return header != mHeaders.end() && (*header).second.compare(InValue) == 0;
    }

    void ServerRequestMessage::DebugPrint()
    {
        std::cout << OutputServerTime_GetTime() << "Request data:\n\n";
        std::cout << "--------------------REQUEST-START--------------------\n\n";
        std::cout << ServerRequestTypeStrings.at(mRequestType) << " | " << mUrl << " | ?" << mQuery << "\n";
        
        for(const auto& RequestTag : mHeaders)
        {
            std::cout << RequestTag.first << " : " << RequestTag.second << "\n";
        }

        std::cout << "---------------------REQUEST-END---------------------\n\n";
    }

#pragma endregion //ServerRequestMessage

#pragma region ServerResponseMessage

    ServerResponseMessage::ServerResponseMessage(ServerResponseStatusCode MessageStatus)
        : mStatusCode(MessageStatus)
    {
        AddMessageHeaders(GenerateMetaDataMessageHeaders());  
    }

    ServerResponseMessage::ServerResponseMessage(const ServerResponseMessage& Other)
        : mStatusCode(Other.mStatusCode), mMessageLength(Other.mMessageLength), mContentLength(Other.mContentLength)
    {
        mHeaders = Other.mHeaders;

        if(Other.mMessage != nullptr)
        {
            mMessage = (const char*)malloc(mMessageLength);
            memcpy((char*) mMessage, Other.mMessage, mMessageLength);
        }
        
        if(Other.mContentData != nullptr)
        {
            mContentData = (const char*) malloc(mContentLength);
            memcpy((char*) mContentData, Other.mContentData, mContentLength);
        }
    }

    ServerResponseMessage::ServerResponseMessage(ServerResponseMessage&& Other) noexcept
        : mStatusCode(Other.mStatusCode), mMessage(Other.mMessage), mMessageLength(Other.mMessageLength), mContentData(Other.mContentData), mContentLength(Other.mContentLength)
    {
        mHeaders = std::move(Other.mHeaders);
        Other.mMessage = nullptr;
        Other.mContentData = nullptr;
    }

    ServerResponseMessage::~ServerResponseMessage()
    {
        if(mContentData){ free((char*) mContentData); }
        if(mMessage){ free((char*) mMessage); }
    }

    void ServerResponseMessage::AddContent(const std::vector<char>& MessageContent, const std::string& ContentType)
    {
        mContentLength = MessageContent.size();
        mContentData = (const char*) malloc(mContentLength);
        memcpy((char*) mContentData, MessageContent.data(), mContentLength);

        mHeaders.emplace(std::make_pair("Content-Type", ContentType));
        mHeaders.emplace(std::make_pair("Content-Length", std::to_string(mContentLength)));
    }

    void ServerResponseMessage::BuildMessage()
    {
        //TODO: could check if anything has changed since last build

        if(mMessage) { free((char*) mMessage); }
        assert(ValidResponseMessageData(*this, mContentData, mContentLength));
 
        mMessageLength = mContentLength + PrintnResponseMessageStatus(NULL, 0, mStatusCode) + PrintnResponseMessageHeaders(NULL, 0, mHeaders);
        char* MessageBuffer = (char*) malloc(mMessageLength + 1);
        int MessageBufferIndex = 0;

        MessageBufferIndex += PrintnResponseMessageStatus(&MessageBuffer[MessageBufferIndex], mMessageLength + 1, mStatusCode);
        MessageBufferIndex += PrintnResponseMessageHeaders(&MessageBuffer[MessageBufferIndex], mMessageLength + 1, mHeaders);

        if(mContentData != nullptr)
        {
            memcpy(&MessageBuffer[MessageBufferIndex], mContentData, mContentLength);
        }

        mMessage = MessageBuffer;
    }

    void ServerResponseMessage::AddMessageHeaders(const std::vector<std::pair<std::string, std::string>>& MessageHeaders)
    {
        mHeaders.insert(MessageHeaders.begin(), MessageHeaders.end());
        BuildMessage();
    }

    void ServerResponseMessage::DebugPrint()
    {
        std::cout << OutputServerTime_GetTime() << "Response data:\n\n";
        std::cout << "--------------------Response-START--------------------\n\n";
        std::cout << mMessage << "\n\n";
        std::cout << "---------------------Response-END---------------------\n\n";
    }

#pragma endregion //ServerResponseMessage

#pragma region WebSocketHandle

    WebSocketHandle& WebSocketHandle::operator=(WebSocketHandle&& Other) noexcept
    {
        mClientSocket = std::move(Other.mClientSocket);
        mMainThread = std::move(Other.mMainThread);
        return *this;
    }

    WebSocketHandle::WebSocketHandle(WebSocketHandle&& Other)
    {
        *this = std::move(Other);
    }

    void WebSocketHandle::StartWebSocketThread(SOCKET ClientSocket, WebSocketReceiveDataCallBack RecieveDataCallback)
    {
        mClientSocket = ClientSocket;
        mRecieveDataCallback = RecieveDataCallback;
        bRunThread = true;
        mMainThread = std::thread(&WebSocketHandle::WebSocketMainThread, this);
    }

    void WebSocketHandle::EndWebSocketThread()
    {
        bRunThread = false;
    }

    void WebSocketHandle::AddMessageToSendQueue(const char* InContent, int InContentLen, WebSocketOpCode InOpCode)
    {
        WebSocketMessage wsMessage;
        wsMessage.BuildFromContent(true, InOpCode, InContent, InContentLen);
        mSendMessageQueue.SyncPush(wsMessage);
    }

    void WebSocketHandle::SendWebMessage(WebSocketMessage& Message)
    {
        int messageBufferSize = Message.GetGeneratedMessageLength();
        char* messageBuffer = (char*) calloc(messageBufferSize, sizeof(char));

        Message.GenerateMessage(messageBuffer, messageBufferSize);

        int SendResult = send(mClientSocket, messageBuffer, messageBufferSize, 0);
        if(SendResult == SOCKET_ERROR)
        {
            std::cout << "Web-Socket send failed" << WSAGetLastError() << "\n";
        }

        std::cout << "Web-Socket message sent - bytes: " << SendResult << "\n";
    }

    void WebSocketHandle::WebSocketMainThread()
    {
        auto ServerTime = std::chrono::system_clock::now();

        std::vector<TimedFunctionPair> TimedFunctions;
        TimedFunctions.push_back(std::make_pair(MilliSecStopwatch{ServerTime, 250}, std::bind(StatusLogPost, "Web-Socket - Open for data", StatusLogSeverity::StatusLogSeverity_Log)));

        const auto SendWSMessagesFunc = [&] ()
            {
                while(mSendMessageQueue.IsEmpty() == false)
                {
                    WebSocketMessage SendMessageItem = mSendMessageQueue.SyncPop();
                    SendWebMessage(SendMessageItem);
                }
            };
        TimedFunctions.push_back(std::make_pair(MilliSecStopwatch{ServerTime, 1}, SendWSMessagesFunc));

        WebSocketMessage PersistentWSMessage;
        const auto MessageRecievedCallback = [&] (SocketDataStream&& DataStream)
            {
                WSHandleMessageRecieved(DataStream, PersistentWSMessage);
                if(PersistentWSMessage.bIsComplete)
                {
                    std::cout << OutputServerTime_GetTime() << "Web-Socket - Message complete\n";
                    mRecieveDataCallback(PersistentWSMessage.mContent, PersistentWSMessage.mContentLen, PersistentWSMessage.mOpCode);
                }
            };

        TimedFunctionPair TimeOutFunctionPair = std::make_pair(MilliSecStopwatch{ServerTime, 600 * SecondsToMs}, std::bind(StatusLogPost, "Web-Socket - Timed out\n", StatusLogSeverity::StatusLogSeverity_Error));
        const auto ErrorEncounteredCallback = [] () { std::cout << OutputServerTime_GetTime() << "Web-Socket - failed: " << WSAGetLastError() << "\n"; };

        ReceieveMessageLoop(mClientSocket, &bRunThread, TimedFunctions, MessageRecievedCallback, ErrorEncounteredCallback, TimeOutFunctionPair);

        std::cout << "Websocket - Stopped receiving messages\n";
    }

#pragma endregion   //WebSocketHandle

#pragma region WebSocketMessage

    WebSocketMessage::~WebSocketMessage()
    {
        if(mContent != nullptr)
        {
            free(mContent);
            mContent = nullptr; mContentLen = 0;
        }
    }

    WebSocketMessage& WebSocketMessage::operator=(WebSocketMessage&& Other) noexcept
    {
        std::cout << "WebSocketMessage Move called\n";

        if(mContent != nullptr)
        {
            free(mContent);
            mContent = nullptr; mContentLen = 0;
        }

        bIsComplete = std::move(Other.bIsComplete);
        mOpCode = std::move(Other.mOpCode);
        mContent = std::move(Other.mContent);
        mContentLen = std::move(Other.mContentLen);

        Other.mContent = nullptr;
        Other.mContentLen = 0;

        return *this;
    }

    WebSocketMessage::WebSocketMessage(const WebSocketMessage& Other)
        : bIsComplete(Other.bIsComplete), mOpCode(Other.mOpCode), mContentLen(Other.mContentLen)
    {
        std::cout << "WebSocketMessage Copy called\n";
        mContent = (char*) calloc(mContentLen + 1, sizeof(char));
        memcpy(mContent, Other.mContent, mContentLen);
    }

    WebSocketMessage::WebSocketMessage(WebSocketMessage&& Other)
    {
        *this = std::move(Other);
    }

    void WebSocketMessage::BuildFromDataStream(const SocketDataStream& InDataStream)
    {
        unsigned char* UData = (unsigned char*) InDataStream.mData;

        bIsComplete = (UData[0] & 0b10000000) != 0;
        mOpCode = (WebSocketOpCode)(UData[0] & 0b00001111);
        bool IsMasked = (UData[1] & 0b10000000) != 0;
        
        int LengthEndIndex{};
        mContentLen = GetWSMessageContentLength(UData, LengthEndIndex);

        mContent = (char*) calloc(mContentLen + 1, sizeof(char));
        memcpy(mContent, &UData[LengthEndIndex + ((IsMasked) ? 4 : 0)], mContentLen);

        if(IsMasked)
        {
            std::array<unsigned char, 4> Mask = { UData[LengthEndIndex], UData[LengthEndIndex + 1], UData[LengthEndIndex + 2], UData[LengthEndIndex + 3] };
            WSApplyDataMask(mContent, mContentLen, Mask);
        }
    }

    void WebSocketMessage::ConcatMessage(WebSocketMessage& InContinuationMessage)
    {
        assert(bIsComplete == false && InContinuationMessage.mOpCode == WebSocketOpCode::WebSocketOpCode_continuation);

        char* ConcatenatedContent = (char*) calloc(mContentLen + InContinuationMessage.mContentLen + 1, sizeof(char));
        memmove(&ConcatenatedContent[0], mContent, mContentLen);
        memcpy(&ConcatenatedContent[mContentLen], InContinuationMessage.mContent, InContinuationMessage.mContentLen);
        
        free(mContent);

        mContent = ConcatenatedContent;
        mContentLen = mContentLen + InContinuationMessage.mContentLen;
        bIsComplete = InContinuationMessage.bIsComplete;
    }

    void WebSocketMessage::BuildFromContent(bool InIsCompleteMessage, WebSocketOpCode InOpCode, const char* InContent, uint64_t InContentLen)
    {
        bIsComplete = InIsCompleteMessage; mOpCode = InOpCode; mContentLen = InContentLen;

        assert(mContent == nullptr);
        mContent = (char*) calloc(mContentLen, sizeof(char));
        memcpy(mContent, InContent, mContentLen);
    }

    void WebSocketMessage::GenerateMessage(char* OutMessageBuffer, int InbufferLength)
    {
        bool IsMasked = false;

        OutMessageBuffer[0] = (0b10000000 & ((int)bIsComplete << 7));
        OutMessageBuffer[0] = OutMessageBuffer[0] | (uint8_t)mOpCode;
        OutMessageBuffer[1] = (0b10000000 & ((int) IsMasked << 7));
        OutMessageBuffer[1] = OutMessageBuffer[1] | (0b01111111 & GetWSEncodedContentLength(mContentLen));

        int LengthEndIndex{};
        WSPackMessageLength(OutMessageBuffer, mContentLen, LengthEndIndex);
        memcpy(&OutMessageBuffer[LengthEndIndex], mContent, mContentLen);
    }

    int WebSocketMessage::GetGeneratedMessageLength()
    {
        return 2 + mContentLen + GetWSEncodedLengthByteCount(mContentLen);
    }

    void WebSocketMessage::DebugPrint()
    {
        std::cout << OutputServerTime_GetTime() << "Web-Socket Message:\n";
        std::cout << "Complete: " << bIsComplete << " Opcode: " << (int) mOpCode << " Content Length: " << mContentLen << "\n\n";
        std::cout << "--------------------WS-MESSAGE-START--------------------\n\n";
        std::cout << mContent;
        std::cout << "\n\n---------------------WS-MESSAGE-END---------------------\n\n";
    }

#pragma endregion   //WebSocketMessage

}