#include "BindingSocket.hpp"

// Constructor
wsrv::BindingSocket::BindingSocket(int domain, int service, int protocol, int port, unsigned long interface)
    :   Socket(domain, service, protocol, port, interface)  {

        set_connection(connect_to_network(getSock(), getAddr()));
        test_connection(getConn());

    }

// Definition of connect_to_network virtual function
int wsrv::BindingSocket::connect_to_network(int sock, struct sockaddr_in address)   {

    return bind(sock, (struct sockaddr *)&address, sizeof(address));

}