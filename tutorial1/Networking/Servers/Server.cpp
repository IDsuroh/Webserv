#include "Server.hpp"

wsrv::Server::Server(int domain, int service, int protocol, int port, unsigned long interface, int bklg)    {

    socket = new ListeningSocket(domain, service, protocol, port, interface, bklg);
    
}

wsrv::ListeningSocket*    wsrv::Server::getSocket()   {
    return socket;
}