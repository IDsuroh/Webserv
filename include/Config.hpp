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
        explicit                    Config(const std::string& filename);
        void                        parse();
        const std::vector<Server>&  servers() const;
    
    public:
        std::string         _filename;
        std::vector<Server> _servers;

        void    tokenize(const std::string& contents, std::vector<std::string>& tokens);
        void    parseTokens(const std::vector<std::string>& tokens);

};

#endif