#ifndef STRUCTS_HPP
#define STRUCTS_HPP

struct  Location    {
    std::map<std::string, std::string>  directives;
    std::string                         path;
};

struct Server   {
    std::vector<std::string>            listen;
    std::vector<std::string>            server_name;
    std::vector<Location>               locations;
    std::map<std::string, std::string>  directives;
    std::map<std::string, std::string>  error_pages;
};

struct  Listener    {
    int             fd;
    const Server*   config;
};

struct Connection   {
    int             fd;
    const Server*   srv;
    std::string     readBuffer;
    std::string     writeBuffer;
    bool            headersComplete;

};

#endif