#ifndef TESTSERVER_HPP
#define TESTSERVER_HPP

#include "Server.hpp"

namespace   wsrv    {

    class   TestServer  :   public Server   {
        
        private:
            char    buffer[30000];
            int     new_socket;
            void    accepter();
            void    handler();
            void    responder();

        public:
            TestServer();
            void    launch();

    };
};

#endif