#ifndef LISTENER_HPP
#define LISTENER_HPP

#include "Config.hpp"

struct  Listener    {
    int             fd;
    const Server*   config;
};

void    makeNonBlocking(int fd);
int     openAndListen(const std::string& spec);
void    setupListeners(const std::vector<Server>& servers, std::vector<Listener>& outListeners);

#endif