#include "Socket.hpp"

// Default Contructor
wsrv::Socket::Socket(int domain, int service, int protocol, int port, unsigned long interface) {
    
    // Defineing adress structure
    address.sin_family = domain;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(interface);
    
    // Establishing Socket
    sock = socket(domain, service, protocol);
    test_connection(sock);

}

// Test Connection Virtual Function
void    wsrv::Socket::test_connection(int item_to_test) {
    
    // Confirmation that the socket or connection has been properly established
    if (item_to_test < 0)   {
        perror("Failed to Connect...");
        exit(EXIT_FAILURE);
    }

}

// Getters
struct  sockaddr_in wsrv::Socket::getAddr() {
    return address;
}

int wsrv::Socket::getSock() {
    return sock;
}

int wsrv::Socket::getConn()  {
    return connection;
}

// Setters
void    wsrv::Socket::set_connection(int con)    {
    connection = con;
}