#ifndef SOCKET_HPP
#define SOCKET_HPP

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

namespace   wsrv    {
    
    class Socket    {
        
        private:
            struct  sockaddr_in address;
            int                 sock;
        
        public:
            // Contructor
            Socket(int domain, int service, int protocol, int port, unsigned long interface);

            void            test_connection(int item_to_test);
            
            // Virtual function to Connect to the Network
            virtual int     connect_to_network(int sock, struct sockaddr_in address) = 0;

            // Getters
            struct  sockaddr_in getAddr();
            int                 getSock();

    };

}

#endif