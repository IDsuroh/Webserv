#include "../include/Config.hpp"

// Forward declarations of static functions
static void parseServerBlock(const std::vector<std::string>& tokens, size_t& i, std::vector<Server>& servers);
static void handleListen(Server& srv, const std::vector<std::string>& tokens, size_t& i);
static void handleServerName(Server& srv, const std::vector<std::string>& tokens, size_t& i);
static void handleErrorPage(Server& srv, const std::vector<std::string>& tokens, size_t& i);
static void parseLocationBlock(const std::vector<std::string>& tokens, size_t& i, Location& loc);
static void handleLocation(Server& srv, const std::vector<std::string>& tokens, size_t& i);
static void handleGenericDirective(Server& srv, const std::string& key, const std::vector<std::string>& tokens, size_t& i);
// ****************************************************************************

// Print Tokens Tester Function
static void	testTokens(const std::vector<std::string>& tokens)	{
	if (tokens.empty())	{
		std::cerr << "No tokens found in config file\n";
		return ;
	}

	for (size_t i = 0; i < tokens.size(); ++i)
		std::cout << "Token[" << i << "]: " << tokens[i] << std::endl;
	std::cout << "\nParsing Tokens\n";
}

// Config Constructor
Config::Config(const std::string& filename)
    :   _filename(filename)	{
    parse();}



// Parse the configuration file
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
	testTokens(tokens);
    parseTokens(tokens);

	std::cout<< "\nResult:\n";

}



// Get the list of servers
const std::vector<Server>& Config::getServers() const  {
    return _servers;
}



// Tokenize the contents of the configuration file
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



// Parse the tokens into server configurations
void    Config::parseTokens(const std::vector<std::string>& tokens)   {

    size_t  i = 0;
	while (i < tokens.size() && tokens[i] != "server")
		++i;
    while (i < tokens.size())   {
        if (tokens[i] == "server")  {
            ++i;
            if (i >= tokens.size() || tokens[i] != "{")
                throw std::runtime_error("Expected '{' after server");
            ++i;
            parseServerBlock(tokens, i, _servers);
        }
        else
            ++i;
    }
}



// Static functions to handle different parts of the configuration
static void parseServerBlock(const std::vector<std::string>& tokens, size_t& i, std::vector<Server>& servers)   {
    
    Server  srv;
    while (i < tokens.size() && tokens[i] != "}")   {
        const std::string&  key = tokens[i++];
        if (key == "listen")
            handleListen(srv, tokens, i);
        else if (key == "server_name")
            handleServerName(srv, tokens, i);
		else if (key == "error_page")
	    	handleErrorPage(srv, tokens, i);
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



// Handle the "listen" directive
static void handleListen(Server& srv, const std::vector<std::string>& tokens, size_t& i)  {
    
    srv.listen.push_back(tokens[i++]);
    if (tokens[i++] != ";")
        throw   std::runtime_error("Missing ';' after listen");
}



// Handle the "server_name" directive
static void handleServerName(Server& srv, const std::vector<std::string>& tokens, size_t& i)	{

    while (tokens[i] != ";")
        srv.server_name.push_back(tokens[i++]);
    ++i;
}



// Handle the "error_page" directive
static void handleErrorPage(Server& srv, const std::vector<std::string>& tokens, size_t& i)  {

	std::string	error_code = tokens[i++];
	std::string	uri = tokens[i++];
	if (tokens[i++] != ";")
		throw std::runtime_error("Missing ';' after error_page");
	srv.error_pages[error_code] = uri;
}



// Handle the "location" directive
static void handleLocation(Server& srv, const std::vector<std::string>& tokens, size_t& i)  {
    
    Location    loc;
    loc.path = tokens[i++];
    if (tokens[i++] != "{")
        throw   std::runtime_error("Expected '{' after location");
    parseLocationBlock(tokens, i, loc);
    srv.locations.push_back(loc);
}



// Parse the location block
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



// Handle generic directives in the server block
static void handleGenericDirective(Server& srv, const std::string& key, const std::vector<std::string>& tokens, size_t& i)  {
    std::string value = tokens[i++];
    if (tokens[i++] != ";")
        throw   std::runtime_error("Missing ';' after " + key);
    srv.directives[key] = value;
}






//**************************************************************************************************

// Setting up Listeners
void    makeNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)  {
        perror("fcntl GETFL");
        std::exit(1);
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl SETFL");
        std::exit(1);
    }
}

int openAndListen(const std::string& spec)  {
    size_t      colon = spec.find(':');
    std::string ip = spec.substr(0, colon);
    int         port = std::atoi(spec.substr(colon + 1).c_str());

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        std::exit(1);
    }
    makeNonBlocking(sockfd);

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = (ip.empty() || ip == "*") ? INADDR_ANY : inet_addr(ip.c_str());

    if (bind(sockfd, (sockaddr*)&addr, sizeof(addr)) < 0)  {
        perror("bind");
        std::exit(1);
    }

    if (listen(sockfd, SOMAXCONN) < 0)  {
        perror("listen");
        std::exit(1);
    }

    return sockfd;
}

void	setupListeners(const std::vector<Server>& servers, std::vector<Listener>& outListeners)	{

	for (size_t s = 0; s < servers.size(); ++s)	{
		const Server&	srv = servers[s];
		for (size_t i = 0; i < srv.listen.size(); ++i)	{
			int	fd = openAndListen(srv.listen[i]);
			Listener	L;
			L.fd = fd;
			L.config = &srv;
			outListeners.push_back(L);
            std::cout << "Listening on " << srv.listen[i] << " for server #" << s << std::endl << std::endl;
		}
	}
}