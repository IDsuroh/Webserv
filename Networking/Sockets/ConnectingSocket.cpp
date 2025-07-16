#include "ConnectingSocket.hpp"

// Constructor
wsrv::ConnectingSocket::ConnectingSocket(int domain, int service, int protocol, int port, unsigned long interface)
    :   Socket(domain, service, protocol, port, interface)  {

        set_connection(connect_to_network(getSock(), getAddr()));
        test_connection(getConn());

    }

// Definition of connect_to_network virtual function
int wsrv::ConnectingSocket::connect_to_network(int sock, struct sockaddr_in address)   {

    return connect(sock, (struct sockaddr *)&address, sizeof(address));

}