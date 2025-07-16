#include "TestServer.hpp"
#include <cstring>

wsrv::TestServer::TestServer()
    :   Server(AF_INET, SOCK_STREAM, 0, 80, INADDR_ANY, 10) {
        std::memset(buffer, 0, sizeof(buffer));
        launch();
    }

void    wsrv::TestServer::accepter()    {
    struct sockaddr_in  address = getSocket()->getAddr();
    int                 addrlen = sizeof(address);
    new_socket = accept(getSocket()->getSock(), (struct sockaddr *)&address, (socklen_t *)&addrlen);
    read(new_socket, buffer, 30000);
}

void    wsrv::TestServer::handler() {
    std::cout
        << buffer << std::endl;
}

void    wsrv::TestServer::responder()   {
    const char*   hello = "Hello from server\n";
    write(new_socket, hello, strlen(hello));
    close(new_socket);
}

void    wsrv::TestServer::launch()  {
    while (true)    {
        std::cout
            << "===== WAITING =====\n";
        accepter();
        handler();
        responder();
        std::cout
            << "====== DONE ======\n";
    }
}