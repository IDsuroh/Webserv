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
        explicit        Config(const std::string& filename);

        const std::vector<Server>&  getServers() const;

};

void    makeNonBlocking(int fd);
int     openAndListen(const std::string& spec);
void    setupListeners(const std::vector<Server>& servers, std::vector<Listener>& outListeners);

#endif