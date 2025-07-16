#ifndef SERVER_HPP
#define SERVER_HPP

#include "../wsrvlibc_networking.hpp"

namespace   wsrv    {

    class Server    {
        private:
            ListeningSocket*    socket;

            virtual void    accepter() = 0;
            virtual void    handler() = 0;
            virtual void    responder() = 0;

        public:
            Server(int domain, int service, int protocol, int port, unsigned long interface, int bklg);

            virtual void    launch() = 0;

            //Getter
            ListeningSocket*    getSocket();

    };
    
};

#endif