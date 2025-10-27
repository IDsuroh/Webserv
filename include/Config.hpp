#ifndef CONFIG_HPP
#define CONFIG_HPP

#include "Headers.hpp"
#include "Structs.hpp"

class Config    {
    
    private:
        std::vector<Server> _servers;
        std::string         _filename;

        void                        parse();
        void    tokenize(const std::string& contents, std::vector<std::string>& tokens);
        void    parseTokens(const std::vector<std::string>& tokens);

    public:
        Config(const std::string& filename);

        const std::vector<Server>&  getServers() const;

};

#endif