#ifndef CONFIG_HPP
#define CONFIG_HPP

#include "Headers.hpp"

struct  Location    {
    std::map<std::string, std::string>  directives;
    std::string                         path;
};

struct Server   {
    std::vector<std::string>            listen;
    std::vector<std::string>            server_name;
    std::vector<Location>               locations;
    std::map<std::string, std::string>  directives;
};

class Config    {
    
    private:
        std::vector<Server> _servers;
        std::string         _filename;

        void                        parse();
        void    tokenize(const std::string& contents, std::vector<std::string>& tokens);
        void    parseTokens(const std::vector<std::string>& tokens);

    public:
        explicit        Config(const std::string& filename);

        const std::vector<Server>&  getServers() const;

};

#endif