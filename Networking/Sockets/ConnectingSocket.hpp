#ifndef CONNECTINGSOCKET_HPP
#define CONNECTINGSOCKET_HPP

#include "Socket.hpp"

namespace   wsrv    {

    class   ConnectingSocket   :   public Socket   {

        public:
            // Constructor
            ConnectingSocket(int domain, int service, int protocol, int port, unsigned long interface);
            
            // Virtual function from the parent class
            int         connect_to_network(int sock, struct sockaddr_in address);
    };

};

#endif