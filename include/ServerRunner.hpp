#ifndef SERVERRUNNER_HPP
#define SERVERRUNNER_HPP

#include "Config.hpp"

class   ServerRunner  {
    
    public:
        explicit    ServerRunner(const std::vector<Server>& servers);

        void    run();

    private:
        std::vector<Server>         _servers;
        std::vector<Listener>       _listeners;
        std::vector<struct pollfd>         _fds;
        std::map<int, Connection>   _connections;
    
        void    buildListeners();
        void    buildPollFds();
        void    handleEvents(); 
        void    acceptNewClient(int listenFd, const Server* srv);
        void    readFromClient(int clientFd);
        void    writeToClient(int clientFd);
        void    closeConnection(int clientFd);   
};


#endif