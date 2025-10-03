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

// ****************************************************************************

// Config Constructor
Config::Config(const std::string& filename)
    :   _filename(filename)	{
    parse();
}

// Get the list of servers
const std::vector<Server>& Config::getServers() const  {
    return _servers;
}

// ****************************************************************************

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

// Tokenize the contents of the configuration file
void    Config::tokenize(const std::string& contents, std::vector<std::string>& tokens) {

    std::string current;
    for (size_t i = 0; i < contents.size(); ++i)    {
        
        char            c = contents[i];
        unsigned char   uc = static_cast<unsigned char>(c);

        if (std::isspace(uc)) {
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


// ****************************************************************************



// Static functions to handle different parts of the configuration
static void parseServerBlock(const std::vector<std::string>& tokens, size_t& i, std::vector<Server>& servers)   {
    
    Server  srv;
    while (i < tokens.size() && tokens[i] != "}")   {
        const std::string&  key = tokens[i];
        i++;
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
    std::cerr << "<<< leaving parseServerBlock now at token[" << i
              << "] = '" << (i < tokens.size() ? tokens[i] : "<EOF>") << "'\n";
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

	if (i >= tokens.size())
		throw	std::runtime_error("Unexpected EOF after server_name");

	while (i < tokens.size() && tokens[i] != ";")
		srv.server_name.push_back(tokens[i++]);

	if (i >= tokens.size() || tokens[i] != ";")
		throw	std::runtime_error("Missing ';' after server_name");
	
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
        
        std::string                 key = tokens[i++];
        std::vector<std::string>    args;
        
        while (i < tokens.size() && tokens[i] != ";")
            args.push_back(tokens[i++]);
        if (i >= tokens.size() || tokens[i] != ";")
            throw   std::runtime_error("Missing ';' after " + key);
        i++;

        std::string                 joined;
        for (size_t j = 0; j < args.size(); ++j)    {
            if (j)
                joined += ' ';
            joined += args[j];
        }
        
        loc.directives[key] = joined;
    }

    if (i >= tokens.size() || tokens[i] != "}")
        throw   std::runtime_error("Missing '}' at the end of the location block");
    ++i;

}



// Handle generic directives in the server block
static void handleGenericDirective(Server& srv, const std::string& key, const std::vector<std::string>& tokens, size_t& i)  {

	if (i >= tokens.size())
		throw	std::runtime_error("Unexpected EOF after " + key);

	std::vector<std::string>	args;
	while (i < tokens.size() && tokens[i] != ";")
		args.push_back(tokens[i++]);

	if (i >= tokens.size() || tokens[i] != ";")
		throw	std::runtime_error("Missing ';' after " + key);
	
	++i;

	std::string	joined;
	for (size_t j = 0; j < args.size(); ++j)	{
		if (j)
			joined += ' ';
		joined += args[j];
	}
	srv.directives[key] = joined;

}
