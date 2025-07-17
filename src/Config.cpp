#include "../include/Config.hpp"

Config::Config(const std::string& filename)
    :   _filename(filename)
    {}

void    Config::parse() {
    
    std::ifstream   in(_filename.c_str());
    if (!in)
        throw std::runtime_error("Cannot open config file: " + _filename);

    std::ostringstream  contents;
    std::string         line;
    while (std::getline(in, line))  {
        size_t  pos = line.find('#');
        if (pos != std::string::npos)
            line.erase(pos);
        contents << line << '\n';
    }

    std::vector<std::string>    tokens;
    tokenize(contents.str(), tokens);
    parseTokens(tokens);

}

const std::vector<Server>& Config::getServers() const  {
    return _servers;
}

void    Config::tokenize(const std::string& contents, std::vector<std::string>& tokens) {

    std::string current;
    for (size_t i = 0; i < contents.size(); ++i)    {
        char    c = contents[i];
        if (isspace(c)) {
            if (!current.empty())   {
                tokens.push_back(current);
                current.clear();
            }
        }
        else if (c == '{' || c == '}' || c == ';')  {
            if (!current.empty())   {
                tokens.push_back(current);
                current.clear();
            }
            tokens.push_back(std::string(1, c));
        }
        else    {
            current.push_back(c);
        }
    }
    if (!current.empty())
        tokens.push_back(current);
}

void    Config::parseTokens(const std::vector<std::string>& tokens)   {

    size_t  i = 0;
    while (i < tokens.size())   {
        if (tokens[i] == "server")  {
            ++i;
            if (i >= tokens.size() || tokens[i] != "{")
                throw std::runtime_error("Expected '{' after server");
            ++i;
            parseServerBlock(tokens, i, _servers);
        }
        else
            throw std::runtime_error("Unexpected token: " + tokens[i]);
    }
}

static void parseServerBlock(const std::vector<std::string>& tokens, size_t& i, std::vector<Server>& servers)   {
    
    Server  srv;
    while (i < tokens.size() && tokens[i] != "}")   {
        const std::string&  key = tokens[i++];
        if (key == "listen")
            handleListen(srv, tokens, i);
        else if (key == "server_name")
            handleServerName(srv, tokens, i);
        else if (key == "location")
            handleLocation(srv, tokens, i);
        else
            handleGenericDirective(srv, key, tokens, i);
    }

    if (i >= tokens.size() || tokens[i] != "}")
        throw   std::runtime_error("Missing '}' at the end of server block");
    ++i;
    servers.push_back(srv);
}

static void handleListen(Server& srv, const std::vector<std::string>& tokens, size_t& i)  {
    
    srv.listen.push_back(tokens[i++]);
    if (tokens[i++] != ";");
        throw   std::runtime_error("Missing ';' after listen");
}

static void handleServerName(Server& srv, const std::vector<std::string>& tokens, size_t& i){

    while (tokens[i] != ";")
        srv.server_name.push_back(tokens[i++]);
    ++i;
}

static void handleLocation(Server& srv, const std::vector<std::string>& tokens, size_t& i)  {
    
    Location    loc;
    loc.path = tokens[i++];
    if (tokens[i++] != "{")
        throw   std::runtime_error("Expected '{' after location");
    parseLocationBlock(tokens, i, loc);
    srv.locations.push_back(loc);
}

static void parseLocationBlock(const std::vector<std::string>& tokens, size_t& i, Location& loc)  {

    while (i < tokens.size() && tokens[i] != "}")   {
        std::string key = tokens[i++];
        std::string value = tokens[i++];
        if (tokens[i++] != ";")
            throw   std::runtime_error("Missing ';' after " + key);
        loc.directives[key] = value;
    }

    if (i >= tokens.size() || tokens[i] != "}")
        throw   std::runtime_error("Missing '}' at the end of the location block");
    ++i;
}

static void handleGenericDirective(Server& srv, const std::string& key, const std::vector<std::string>& tokens, size_t& i)  {
    std::string value = tokens[i++];
    if (tokens[i++] != ";")
        throw   std::runtime_error("Missing ';' after " + key);
    srv.directives[key] = value;
}