#include "../include/Config.hpp"

// Forward declarations of static functions
static void parseServerBlock(const std::vector<std::string>& tokens, std::size_t& i, std::vector<Server>& servers);
static void handleListen(Server& srv, const std::vector<std::string>& tokens, std::size_t& i);
static void handleServerName(Server& srv, const std::vector<std::string>& tokens, std::size_t& i);
static void handleErrorPage(Server& srv, const std::vector<std::string>& tokens, std::size_t& i);
static void parseLocationBlock(const std::vector<std::string>& tokens, std::size_t& i, Location& loc);
static void handleLocation(Server& srv, const std::vector<std::string>& tokens, std::size_t& i);
static void handleGenericDirective(Server& srv, const std::string& key, const std::vector<std::string>& tokens, std::size_t& i);
// ****************************************************************************

// Print Tokens Tester Function
/*
static void	testTokens(const std::vector<std::string>& tokens)	{
	
    if (tokens.empty())	{
		std::cerr << "No tokens found in config file\n";
		return ;
	}

	for (std::size_t i = 0; i < tokens.size(); ++i)
		std::cout << "Token[" << i << "]: " << tokens[i] << std::endl;
	std::cout << "\nParsing Tokens\n";
    
}
*/

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
    
    std::ifstream   in(_filename.c_str());	// pipe connected to a file, can pull characters out
    if (!in)
        throw std::runtime_error("Cannot open config file: " + _filename);

    std::ostringstream  contents;	// dynamic string builder -> to write text into a buffer
    std::string         line;
    while (std::getline(in, line))	// getline pulls one line at a time and strips the \n
        contents << line << '\n';
	// one giant string containing the entire file
	
    std::vector<std::string>    tokens;
    tokenize(contents.str(), tokens);
	//testTokens(tokens);
    parseTokens(tokens);

	std::cout<< "\nResult:\n";

}

// Tokenize the contents of the configuration file
void    Config::tokenize(const std::string& contents, std::vector<std::string>& tokens) {
	tokens.clear();

    std::string current;
	bool		inSingle = false;
	bool		inDouble = false;

    for (std::size_t i = 0; i < contents.size(); ++i)    {
        
        char            c = contents[i];
        unsigned char   uc = static_cast<unsigned char>(c);

		// simple escapes inside quotes: \" or \'
		if ((inSingle || inDouble) && c == '\\')	{
			if (i + 1 < contents.size())	{	// normal case: when there is a next character
				current.push_back(contents[i + 1]);
				++i;
			}
			else
				current.push_back('\\');
			continue;
		}
        
		// Toggle quotes (not including the quote chars)
		if (!inDouble && c == '\'')	{
			inSingle = !inSingle;
			continue;
		}
		if (!inSingle && c == '\"')	{
			inDouble = !inDouble;
			continue;
		}

		// Outside quotes: when '#' starts a comment.
		if (!inSingle && !inDouble && c == '#')	{
			while (i < contents.size() && contents[i] != '\n')
				++i;
			continue;
		}

		if (!inSingle && !inDouble)	{
			// Structural tokens stay separate
			if (c == '{' || c == '}' || c == ';')  {
        	    if (!current.empty())   {
        	        tokens.push_back(current);
        	        current.clear();
            	}
            	tokens.push_back(std::string(1, c));
				continue;
			}
			
			// Whitespace splits tokens (only outside quotes)
			if (std::isspace(uc))	{
				if (!current.empty())	{
					tokens.push_back(current);
					current.clear();
				}
				continue;
			}
		}

		// Regular character
		current.push_back(c);
    }

    if (!current.empty())	{
        tokens.push_back(current);
	}

	if (inSingle || inDouble)	{
		throw	std::runtime_error("Unterminated quoted string in config");
	}
}

// Parse the tokens into server configurations
void    Config::parseTokens(const std::vector<std::string>& tokens)   {

    std::size_t  i = 0;
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
static void parseServerBlock(const std::vector<std::string>& tokens, std::size_t& i, std::vector<Server>& servers)   {
    
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
static void handleListen(Server& srv, const std::vector<std::string>& tokens, std::size_t& i)  {

	if (i >= tokens.size())
		throw	std::runtime_error("Listen: Unexpected EOF after listen");

	bool	hadAny = false;

	// Collect one or more args until ';' (e.g. "8080", "127.0.0.1:8080", etc.)
	for (; i < tokens.size() && tokens[i] != ";"; ++i)	{
		const std::string&	arg = tokens[i];	// avoid unnecessary temp
		if (arg == "{" || arg == "}")
			throw	std::runtime_error("Listen: Unexpected token '" + arg + "'");
		
		srv.listen.push_back(arg);
		hadAny = true;
	}

	if (!hadAny)
		throw	std::runtime_error("Listen: Need at least one address/port");

    if (i >= tokens.size() || tokens[i] != ";")	{
        throw   std::runtime_error("Listen: Missing ';' after listen");
	}

	++i;
}



// Handle the "server_name" directive
static void handleServerName(Server& srv, const std::vector<std::string>& tokens, std::size_t& i)	{

	if (i >= tokens.size())	// If there are no tokens after
		throw	std::runtime_error("Server_Name: Unexpected EOF after server_name");

	bool	hadAny = false;

	for (; i < tokens.size() && tokens[i] != ";"; ++i)	{
		const std::string&	name = tokens[i];
		if (name == "{" || name == "}")
			throw	std::runtime_error("Server_Name: Unexpected token '" + name + "'");
	
		srv.server_name.push_back(name);
		hadAny = true;
	}

	if (!hadAny)
		throw	std::runtime_error("Server_Name: Need at least one name");

	if (i >= tokens.size() || tokens[i] != ";")
		throw	std::runtime_error("Server_Name: Missing ';' after server_name");
	
	++i;
}



// Handle the "error_page" directive
static void handleErrorPage(Server& srv, const std::vector<std::string>& tokens, std::size_t& i)  {

	if (i >= tokens.size())	// If there are no tokens after
		throw	std::runtime_error("Error_Page: Missing arguments");

	// Gather args until ';'
	std::vector<std::string>	args;
	for (; i < tokens.size() && tokens[i] != ";"; ++i)	{
		const std::string&	tok = tokens[i];
		if (tok == "{" || tok == "}")
			throw	std::runtime_error("Error_page: Unexpected token '" + tok + "'");
		args.push_back(tok);
	}

	if (args.size() < 2)
		throw	std::runtime_error("Error_Page: Need <code...> <uri>");
	if (i >= tokens.size() || tokens[i] != ";")
		throw	std::runtime_error("Error_Page: Missing ';' after error_page");
	++i;

	// Last arg is URI; all previous are status codes
	const std::string&	uri = args.back();
	bool				uriAllDigits = !uri.empty();	// if uri is not empty, it's true (as of right now)
	for (std::size_t j = 0; j < uri.size() && uriAllDigits; ++j)	{
		unsigned char	c = static_cast<unsigned char>(uri[j]);
		if (!std::isdigit(c))
			uriAllDigits = false;
	}

	if (uriAllDigits)
		return ;	// if the last digit is not uri, return and handle by default error code later on.

	for (std::size_t k = 0; k + 1 < args.size(); ++k)
		srv.error_pages[args[k]] = uri;
}



// Handle the "location" directive
static void handleLocation(Server& srv, const std::vector<std::string>& tokens, std::size_t& i)  {
    
	if (i >= tokens.size() || tokens[i] == "{" || tokens[i] == "}")
		throw	std::runtime_error("Location: Missing path");

    Location    loc;
    loc.path = tokens[i];
	++i;

    if (i >= tokens.size() || tokens[i] != "{")	{
        throw   std::runtime_error("Location: Expected '{' after location");
	}
	++i;

    parseLocationBlock(tokens, i, loc);
    srv.locations.push_back(loc);
}



// Parse the location block
static void parseLocationBlock(const std::vector<std::string>& tokens, std::size_t& i, Location& loc)  {

	for (;;)	{
		// 1) EOF guard
		if (i >= tokens.size())
			throw	std::runtime_error("Location: Unexpected EOF inside block");
		
		// 2) Is it the block end?
		if (tokens[i] == "}")	{
			++i;
			break;	// done with this location block.
		}

		// 3) Read directive key
		const std::string&	key = tokens[i];
		++i;
		
		// If we see '{' here, it means a stray brace or broken syntax inside the location block.
		if (key == "{")
			throw	std::runtime_error("Location: Unexpected '{' where directive name was expected");

		// ------- Known directives with specific arity/validation -------

		if (key == "root")	{
			if (i >= tokens.size() || tokens[i] == ";" || tokens[i] == "}" || tokens[i] == "{")
				throw	std::runtime_error("Location: root: Missing value");
			const std::string&	value = tokens[i];
			++i;
			if (i >= tokens.size() || tokens[i] != ";")
				throw	std::runtime_error("Location: root: Missing ';'");
			++i;
			loc.directives["root"] = value;
		}
		else if (key == "autoindex")	{
			if (i >= tokens.size() || (tokens[i] != "on" && tokens[i] != "off"))
				throw	std::runtime_error("Location: autoindex: expected 'on' or 'off'");
			const std::string&	value = tokens[i];
			++i;
			if (i >= tokens.size() || tokens[i] != ";")
				throw	std::runtime_error("Location: autoindex: Missing ';'");
			++i;
			loc.directives["autoindex"] = value;
		}
		else if (key == "methods")	{
			// multi-value until ';'
			std::vector<std::string>	vals;
			for (; i < tokens.size() && tokens[i] != ";"; ++i)	{
				if (tokens[i] == "{" || tokens[i] == "}")
					throw	std::runtime_error("Location: methods: Unexpected token '" + tokens[i] + "'");
				vals.push_back(tokens[i]);
			}
			if (vals.empty())
				throw	std::runtime_error("Location: methods: Needs at least one method");
			if (i >= tokens.size() || tokens[i] != ";")
				throw	std::runtime_error("Location: methods: Missing ';'");
			++i;
			std::string	joined;
			for (std::size_t k = 0; k < vals.size(); ++k)	{
				if (k)
					joined += ",";
				joined += vals[k];
			}
			loc.directives["methods"] = joined;
		}
		else if (key == "index")	{
			std::vector<std::string>	vals;
			for (; i < tokens.size() && tokens[i] != ";"; ++i)	{
				if (tokens[i] == "{" || tokens[i] == "}")
					throw	std::runtime_error("Location: index: Unexpected token '" + tokens[i] + "'");
				vals.push_back(tokens[i]);
			}
			if (vals.empty())
				throw	std::runtime_error("Location: index: Needs at least one filename");
			if (i >= tokens.size() || tokens[i] != ";")
				throw	std::runtime_error("Location: index: Missing ';'");
			++i;
			std::string	joined;
			for (std::size_t k = 0; k < vals.size(); ++k)	{
				if (k)
					joined += ",";
				joined += vals[k];
			}
			loc.directives["index"] = joined;
		}
		else if (key == "client_max_body_size")	{
			if (i >= tokens.size() || tokens[i] == ";" || tokens[i] == "}" || tokens[i] == "{")
				throw	std::runtime_error("Location: client_max_body_size: Missing value");
			const std::string&	value = tokens[i];
			++i;
			if (i >= tokens.size() || tokens[i] != ";")
				throw	std::runtime_error("Location: client_max_body_size: Missing ';'");
			++i;
			loc.directives["client_max_body_size"] = value;
		}
		else if (key == "return")	{
			// simple 2-arg form: return <status> <uri>;
			if (i + 2 >= tokens.size() || tokens[i] == ";" || tokens[i] == "{" || tokens[i] == "}")
				throw	std::runtime_error("Location: return: Missing <status> <uri>");
			const std::string&	status = tokens[i];
			++i;
			const std::string&	uri = tokens[i];
			++i;
			if (i >= tokens.size() || tokens[i] != ";")
				throw	std::runtime_error("Location: return: Missing ';'");
			++i;
			loc.directives["return"] = status + " " + uri;
		}
		else if (key == "cgi_pass")	{
			// cgi_pass <extension> <executable>
			if (i + 1 >= tokens.size() || tokens[i] == ";" || tokens[i] == "{" || tokens[i] == "}")
				throw	std::runtime_error("Location: cgi_pass: Missing <extension> <executable>");
			
			const std::string	ext = tokens[i++];
			const std::string	bin = tokens[i++];

			if (i >= tokens.size() || tokens[i] != ";")
				throw	std::runtime_error("Location: cgi_pass: Missing ';'");
			++i;

			loc.directives["cgi_pass"] = ext + " " + bin;
		}

		// ------- Fallback: generic directive "key arg1 arg2 ... ;" -------
		else	{
			std::vector<std::string>	vals;
			for (; i < tokens.size() && tokens[i] != ";"; ++i)	{
				if (tokens[i] == "{" || tokens[i] == "}")
					throw	std::runtime_error("Location: " + key + ": unexpected token '" + tokens[i] + "'");
				vals.push_back(tokens[i]);
			}
			if (vals.empty())
				throw	std::runtime_error("Location: " + key + ": requires value(s)");
			if (i >= tokens.size() || tokens[i] != ";")
				throw	std::runtime_error("Location: " + key + ": missing ';'");
			++i;

			std::string	joined;
			for (std::size_t k = 0; k < vals.size(); ++k)	{
				if (k)
					joined += " ";
				joined += vals[k];
			}
			loc.directives[key] = joined;
		}
	}
}



// Handle generic directives in the server block
static void handleGenericDirective(Server& srv, const std::string& key, const std::vector<std::string>& tokens, std::size_t& i)  {

	if (key == "root")	{
			if (i >= tokens.size() || tokens[i] == ";" || tokens[i] == "}" || tokens[i] == "{")
				throw	std::runtime_error("Server: root: Missing value");
			const std::string&	value = tokens[i];
			++i;
			if (i >= tokens.size() || tokens[i] != ";")
				throw	std::runtime_error("Server: root: Missing ';'");
			++i;
			srv.directives["root"] = value;
		}
		else if (key == "autoindex")	{
			if (i >= tokens.size() || (tokens[i] != "on" && tokens[i] != "off"))
				throw	std::runtime_error("Server: autoindex: expected 'on' or 'off'");
			const std::string&	value = tokens[i];
			++i;
			if (i >= tokens.size() || tokens[i] != ";")
				throw	std::runtime_error("Server: autoindex: Missing ';'");
			++i;
			srv.directives["autoindex"] = value;
		}
		else if (key == "index")	{
			std::vector<std::string>	vals;
			for (; i < tokens.size() && tokens[i] != ";"; ++i)	{
				if (tokens[i] == "{" || tokens[i] == "}")
					throw	std::runtime_error("Server: index: Unexpected token '" + tokens[i] + "'");
				vals.push_back(tokens[i]);
			}
			if (vals.empty())
				throw	std::runtime_error("Server: index: Needs at least one filename");
			if (i >= tokens.size() || tokens[i] != ";")
				throw	std::runtime_error("Server: index: Missing ';'");
			++i;
			std::string	joined;
			for (std::size_t k = 0; k < vals.size(); ++k)	{
				if (k)
					joined += ",";
				joined += vals[k];
			}
			srv.directives["index"] = joined;
		}
		else if (key == "client_max_body_size")	{
			if (i >= tokens.size() || tokens[i] == ";" || tokens[i] == "}" || tokens[i] == "{")
				throw	std::runtime_error("Server: client_max_body_size: Missing value");
			const std::string&	value = tokens[i];
			++i;
			if (i >= tokens.size() || tokens[i] != ";")
				throw	std::runtime_error("Server: client_max_body_size: Missing ';'");
			++i;
			srv.directives["client_max_body_size"] = value;
		}
		else if (key == "cgi_pass")	{
			if (i + 1 >= tokens.size() || tokens[i] == ";" || tokens[i] == "{" || tokens[i] == "}")
				throw	std::runtime_error("Server: cgi_pass: Missing <extension> <executable>");

			const std::string	ext = tokens[i++];
			const std::string	bin = tokens[i++];

			if (i >= tokens.size() || tokens[i] != ";")
				throw	std::runtime_error("Server: cgi_pass: Missing ';'");
			++i;

			srv.directives["cgi_pass"] = ext + " " + bin;
		}
		else
			throw	std::runtime_error("Server: Unknown directive '" + key + "'");
}