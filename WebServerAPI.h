#pragma once

#ifdef MATHLIBRARY_EXPORTS
#define WEBSERVERLIBRARY_API __declspec(dllexport)
#else
#define WEBSERVERLIBRARY_API __declspec(dllimport)
#endif

#include "WebServer.h";

#include <string>
#include <functional>

namespace WebServerAPI
{
    extern "C" WEBSERVERLIBRARY_API struct DataUploadParams
    {
        std::string Url;
        std::vector<char> Data;
        std::string ContentType;
        std::vector<std::pair<std::string, std::string>> MessageHeaders;
    };

    extern "C" WEBSERVERLIBRARY_API struct InitWebSocketParams
    {
        WebServer::WebSocketReceiveDataCallBack ReceiveDataCallback;
        WebServer::WebSocketClientJoinedCallback ClientJoinedCallback;
    };

    extern "C" WEBSERVERLIBRARY_API struct SendWebSocketMessageParams
    {
        const char* Content;
        int ContentLen;
        WebServer::WebSocketOpCode OpCode;
    };

    extern "C" WEBSERVERLIBRARY_API bool WebServerGlobalInit();
    extern "C" WEBSERVERLIBRARY_API int StartSever(std::string Port);

    extern "C" WEBSERVERLIBRARY_API void UploadData(int ServerID, DataUploadParams Params);

    extern "C" WEBSERVERLIBRARY_API void InitWebSocket(int ServerID, std::string WebSocketURL, InitWebSocketParams Params);
    extern "C" WEBSERVERLIBRARY_API void SendWebSocketMessage(int ServerID, const std::string& Url, uint64_t ClientId, SendWebSocketMessageParams Params);
}
