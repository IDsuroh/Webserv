#ifndef BINDINGSOCKET_HPP
#define BINDINGSOCKET_HPP

#include "Socket.hpp"

namespace   wsrv    {

    class   BindingSocket   :   public Socket   {

        public:
            // Constructor
            BindingSocket(int domain, int service, int protocol, int port, unsigned long interface);
            
            // Virtual function from the parent class
            int         connect_to_network(int sock, struct sockaddr_in address);
    };

};

#endif