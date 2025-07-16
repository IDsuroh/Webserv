#include "ListeningSocket.hpp"

wsrv::ListeningSocket::ListeningSocket(int domain, int service, int protocol, int port, unsigned long interface, int bklg) 
    :   BindingSocket(domain, service, protocol, port, interface)   {

        backlog = bklg;
        start_listening();
        test_connection(listening);

    }

void    wsrv::ListeningSocket::start_listening()    {
    listening = listen(getSock(), backlog);
}

int wsrv::ListeningSocket::getListening()   {
    return listening;
}

int wsrv::ListeningSocket::getBacklog() {
    return backlog;
}