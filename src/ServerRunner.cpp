#include "../include/ServerRunner.hpp"

ServerRunner::ServerRunner(const std::vector<Server>& servers)
    :   _servers(servers)
{}

void    ServerRunner::run() {
    buildListeners();
    buildPollFds();
    while (true)    {
        int n = poll(&_fds[0], _fds.size(), -1);
        if (n < 0)  {
            perror("poll");
            break;
        }
        handleEvents();
    }
}

void    ServerRunner::buildListeners()  {
    setupListeners(_servers, _listeners);
}

void    ServerRunner::buildPollFds()    {
    _fds.clear();
    for (size_t i = 0; i < _listeners.size(); i++)  {
        struct pollfd   p;
        p.fd = _listeners[i].fd;
        p.events = POLLIN;
        p.revents = 0;
        _fds.push_back(p);
    }
}

void    ServerRunner::handleEvents()    {
    for (size_t i = 0; i < _fds.size(); ++i)    {
        
    }
}