#ifndef LISTENINGSOCKET_HPP
#define LISTENINGSOCKET_HPP

#include "BindingSocket.hpp"

namespace   wsrv    {

    class   ListeningSocket :   public  BindingSocket   {
        private:
            int backlog;
            int listening;
            
        public:
            // Constructor
            ListeningSocket(int domain, int service, int protocol, int port, unsigned long interface, int bklg);


            void    start_listening();

            // Getters
            int getListening();
            int getBacklog();
    };

};

#endif