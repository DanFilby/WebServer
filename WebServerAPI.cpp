#include "WebServerAPI.h"

#include "WebServer.h"
#include "Common.h"

namespace WebServer
{
    int Initialise(WSADATA* WSAData)
    {
        std::cout << "Initialising server\n";

        // Initialise Winsock
        int Result = WSAStartup(MAKEWORD(2, 2), WSAData);
        if(Result != 0)
        {
            std::cout << "WSAStartup failed: " << Result << "\n";
            return 1;
        }

        return 0;
    }
}

namespace WebServerAPI
{
    static std::map<int, WebServer::ListenServer> ActiveServers;

    bool WebServerGlobalInit()
    {
        WSADATA WSAData;
        return WebServer::Initialise(&WSAData) == 0;
    }

    int StartSever(std::string Port)
    {
        int PortNumber = std::stoi(Port);
        if(ActiveServers.find(PortNumber) != ActiveServers.end())
        {
            return -1;
        }

        WebServer::ListenServer& ListenServer = ActiveServers[PortNumber];

        int ReturnCode = ListenServer.Initialise(Port.c_str());
        if(ReturnCode != 0)
        {
            std::cout << "Server start failed - Error: " << ReturnCode << "\n";
            return -1;
        }

        ListenServer.AsyncStart();
        return PortNumber;
    }

    void UploadData(int ServerID, DataUploadParams Params)
    {
        WebServer::ListenServer& Server = ActiveServers.at(ServerID);
        Server.UploadData(Params.Url, Params.Data, Params.ContentType, Params.MessageHeaders);
    }

    void InitWebSocket(int ServerID, std::string WebSocketURL, InitWebSocketParams Params)
    {
        WebServer::ListenServer& Server = ActiveServers.at(ServerID);
        Server.CreateWebSocket(WebSocketURL, Params.ReceiveDataCallback, Params.ClientJoinedCallback);
    }

    void SendWebSocketMessage(int ServerID, const std::string& Url, uint64_t ClientId, SendWebSocketMessageParams Params)
    {
        WebServer::ListenServer& Server = ActiveServers.at(ServerID);
        Server.SendWebSocketMessage(Url, ClientId, Params.Content, Params.ContentLen, Params.OpCode);
    }
}