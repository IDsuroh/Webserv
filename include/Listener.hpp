#ifndef LISTENER_HPP
#define LISTENER_HPP

#include "Config.hpp"

struct  Listener    {
    int             fd;
    const Server*   config;
};

#endif