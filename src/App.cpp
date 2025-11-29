#include "App.hpp"

namespace {

	// --- 2.1. Target (path + query) ---

	// Extracts the path and query from req.target.
	// Returns false if the target is invalid (e.g. empty or in an unsupported format).
	bool parseTarget(const HTTP_Request& request, std::string& path, std::string& query) {

		if (request.target.empty())
			return false;
		
		std::size_t questionMark = request.target.find('?');

		if (questionMark == std::string::npos) {
			path = request.target;
			query = "";
		} else {
			path = request.target.substr(0, questionMark);
			query = request.target.substr(questionMark + 1);
		}

		if (path.empty() || path[0] != '/')
			return false;

		return true;
	}

	// --- 2.2. Server selection (vhost) ---

	// Selects the Server based on the Host header / server_name.
	// Always returns a valid Server (e.g. the default one if no match is found).
	const Server& selectServer(const std::vector<Server>& servers, const HTTP_Request& request) {

		for (size_t i = 0; i < servers.size(); ++i) {
			std::vector<std::string>::const_iterator it;
			it = std::find(servers[i].server_name.begin(), servers[i].server_name.end(), request.host);
			if (it != servers[i].server_name.end())
				return servers[i];
		}

		return servers[0];
	}

	// --- 2.3. Location selection (longest prefix) ---

	// Performs a longest prefix match against the path.
	// Returns a pointer to the matching Location, or NULL if none matches.
	const Location* matchLocation(const Server& server, const std::string& requestPath) {

		const Location*	bestMatchLocation = NULL;
		
		for (std::vector<Location>::const_iterator it = server.locations.begin(); it != server.locations.end(); ++ it) {

			const std::string& locationPath = it->path;
			bool match = (requestPath.compare(0, locationPath.size(), locationPath) == 0);
			bool boundary = (requestPath.size() == locationPath.size() ||
							(requestPath.size() > locationPath.size() && requestPath[locationPath.size()] == '/'));

			if (match && boundary && (bestMatchLocation == NULL || bestMatchLocation->path.size() < locationPath.size()))
				bestMatchLocation = &(*it);
		}

		return bestMatchLocation;
	}

	// --- 2.4. Effective config (merge Server + Location) ---

	// Effective configuration for a request, produced by merging Server- and Location-level directives.
	const std::size_t	kDefaultClientMaxBodySize =	1024 * 1024;	// 1 MiB - use MiB for an exact binary body-size limit
	const std::size_t	kDefaultCgiTimeout = 		30;				// 30 seconds

	struct EffectiveConfig {
		const Server*						server;			// never NULL
		const Location*						location;		// may be NULL

		std::string							root;
		bool								autoindex;
		std::vector<std::string>			index_files;
		std::vector<std::string>			allowed_methods;
		std::map<int, std::string>			error_pages;

		std::size_t							client_max_body_size;
		std::string							upload_store;
		std::map<std::string, std::string>	cgi_pass;
		std::size_t							cgi_timeout;
		std::vector<std::string>			cgi_allowed_methods;

		EffectiveConfig()
			: server(NULL)
			, location(NULL)
			, root(".")
			, autoindex(false)
			, index_files()
			, allowed_methods()
			, error_pages()
			, client_max_body_size(kDefaultClientMaxBodySize)
			, upload_store()
			, cgi_pass()
			, cgi_timeout(kDefaultCgiTimeout)
			, cgi_allowed_methods()
			{}
	};

	std::vector<std::string> splitWords(const std::string& input) {

		std::vector<std::string>	words;
		std::istringstream			iss(input);
		std::string					currentWord;

		while (iss >> currentWord)
			words.push_back(currentWord);

		return words;
	}

	int parseHttpStatus(const std::string& str) {

		if (str.empty())
			throw std::runtime_error("Empty HTTP status code");

		for (size_t i = 0; i < str.size(); ++i) {
			if (!(std::isdigit(static_cast<unsigned char>(str[i]))))					// static_cast for protection against UB if signed (locale dependent)
				throw std::runtime_error("Invalid HTTP status code: " + str);
		}

		errno = 0;
		unsigned long long value = std::strtoull(str.c_str(), NULL, 10);				// Parse into ULL first to avoid overflow: ULL is guaranteed to hold any value we may receive.

		if ((value == ULLONG_MAX && errno == ERANGE))
			throw std::runtime_error("HTTP status code overflow: " + str);

		if (value < 100 || value > 599)
			throw std::runtime_error("HTTP status code out of range: " + str);

		return static_cast<int>(value);
	}

	size_t parseSizeT(const std::string& str) {

		if (str.empty())
			throw std::runtime_error("Empty numeric value");

		for (size_t i = 0; i < str.size(); ++i) {
			if (!(std::isdigit(static_cast<unsigned char>(str[i]))))
				throw std::runtime_error("Invalid numeric value: " + str);
		}

		errno = 0;
		unsigned long long value = std::strtoull(str.c_str(), NULL, 10);

		if (value == ULLONG_MAX && errno == ERANGE)
			throw std::runtime_error("Numeric value exceeds size_t range: " + str);
		
		if (value > std::numeric_limits<std::size_t>::max())							// size_t depends on platform; on 64-bit systems this check is effectively redundant
			throw std::runtime_error("Value exceeds size_t range: " + str);

		return static_cast<size_t>(value);
	}

	bool getDirectiveValue(const Location* loc, const Server& srv, const std::string& key, std::string& value) {
		
		std::map<std::string, std::string>::const_iterator	it;
	
		if (loc) {
			it = loc->directives.find(key);
			if (it != loc->directives.end()) {
				value = it->second;
				return true;
			}
		}

		it = srv.directives.find(key);
		if (it != srv.directives.end()) {
			value = it->second;
			return true;
		}

		return false;
	}

	void resolveErrorPages(EffectiveConfig& cfg, const Server& srv, const Location* loc) {

		std::map<std::string, std::string>::const_iterator it;
		
		for (it = srv.error_pages.begin(); it != srv.error_pages.end(); ++it)
			cfg.error_pages[parseHttpStatus(it->first)] = it->second;
		
		if (!loc)
			return;

		it = loc->directives.find("error_page");
		if (it == loc->directives.end())
			return;

		std::vector<std::string> tokens = splitWords(it->second);
		if (tokens.size() < 2)
			throw std::runtime_error("Bad error_page configuration");

		const std::string& uri = tokens.back();							// URI - Uniform Resource Identifier - identifies a resource (e.g. page, file, image, etc.)

		for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
			cfg.error_pages[parseHttpStatus(tokens[i])] = uri;
	}

	// Builds a unified view of the config (root, methods, error_pages, etc.)
	EffectiveConfig buildEffectiveConfig(const Server& srv, const Location* loc) {

		EffectiveConfig	cfg;

		cfg.server = &srv;
		cfg.location = loc;

		std::string	value;

		if (getDirectiveValue(loc, srv, "root", value))
			cfg.root = value;

		if (getDirectiveValue(loc, srv, "autoindex", value))
			cfg.autoindex = (value == "on");

		if (getDirectiveValue(loc, srv, "index", value))
			cfg.index_files = splitWords(value);

		if (getDirectiveValue(loc, srv, "methods", value))
			cfg.allowed_methods = splitWords(value);
		else {
			cfg.allowed_methods.push_back("GET");
			cfg.allowed_methods.push_back("POST");
		}

		resolveErrorPages(cfg, srv, loc);

		if (getDirectiveValue(loc, srv, "client_max_body_size", value))
			cfg.client_max_body_size = parseSizeT(value);

		if (getDirectiveValue(loc, srv, "upload_store", value))
			cfg.upload_store = value;

		if (getDirectiveValue(loc, srv, "cgi_pass", value)) {
			std::vector<std::string> tokens = splitWords(value);
			if (tokens.size() == 2)
				cfg.cgi_pass[tokens[0]] = tokens[1];
		}

		if (getDirectiveValue(loc, srv, "cgi_timeout", value))
			cfg.cgi_timeout = parseSizeT(value);

		if (getDirectiveValue(loc, srv, "cgi_allowed_methods", value))
			cfg.cgi_allowed_methods = splitWords(value);
		else
			cfg.cgi_allowed_methods = cfg.allowed_methods;

		return cfg;
	}

	// --- 2.5. Allowed methods / 405 ---

	// Checks whether the method is allowed by the effective configuration.
	bool isMethodAllowed(const EffectiveConfig& cfg, const std::string& method) {
		
		for (std::vector<std::string>::const_iterator it = cfg.allowed_methods.begin(); it != cfg.allowed_methods.end(); ++it)
		if (*it == method)
		return true;
		
		return false;
	}

	// Builds a 405 response with the Allow header and (if configured) an error_page.
	HTTP_Response make405(const EffectiveConfig& cfg) {

		HTTP_Response response = makeErrorResponse(405, &cfg);

		std::string allowed;
		for (std::vector<std::string>::const_iterator it = cfg.allowed_methods.begin(); it != cfg.allowed_methods.end(); ++it) {
			if (!allowed.empty())
				allowed += ", ";
			allowed += *it;
		}

		if (!allowed.empty())
			response.headers["Allow"] = allowed;

		return response;
	}

	// --- 2.6. Body validation (size, policy) ---

	// Checks whether the request body is acceptable (size, transfer-encoding, etc.).
	// On error, writes the status into statusCode (e.g. 413, 400, 501) and returns false.
	bool checkRequestBodyAllowed(const EffectiveConfig& cfg, const HTTP_Request& req, int& statusCode) {

		if (cfg.client_max_body_size != 0 && req.content_length > cfg.client_max_body_size) {					// client_max_body_size == 0, means no limit
			statusCode = 413;
			return false;
		}

		if (req.transfer_encoding.empty() || req.transfer_encoding == "chunked")
			return true;

		statusCode = 501;
		return false;
	}

	// --- 2.7. Root + path → secure filesystem path ---

	// Builds the base filesystem path from the root + logical path.
	std::string makeFilesystemPath(const EffectiveConfig& cfg, const std::string& path) {

		std::string			locationPath = cfg.location ? cfg.location->path : "/";		// location may be NULL
		std::string			subPath;

		if (path.compare(0, locationPath.size(), locationPath) == 0)
			subPath = path.substr(locationPath.size());
		else
			subPath = path;

		if (!subPath.empty() && subPath[0] == '/')
			return cfg.root + subPath;
		else
			return cfg.root + '/' + subPath;
	}

	// Normalises the path (., .., //) and ensures canonical form.
	// If normalisation fails (e.g. invalid path), returns false.
	bool normalizePath(std::string& fsPath, const std::string& root) {

		if (fsPath.compare(0, root.size(), root) != 0)
			return false;

		std::string relative = fsPath.substr(root.size());

		if (!relative.empty() && relative[0] == '/')
			relative.erase(0, 1);

		std::vector<std::string> stack;
		std::string token;

		for (size_t i = 0; i <= relative.size(); ++i) {
			char c = (i < relative.size()) ? relative[i] : '/';
			if (c == '/') {
				if (token == "..") {
					if (stack.empty())
						return false;
					else
						stack.pop_back();
				} else if (!token.empty() && token != ".")
					stack.push_back(token);
				token.clear();
			} else
				token += c;
		}

		fsPath = root;

		if (!stack.empty()) {
			for (size_t i = 0; i < stack.size(); ++i)
				fsPath += '/' + stack[i]; 
		}
	
		return true;
	}

	// --- 2.8. Classificação do pedido ---

	enum RequestKind {
		RK_STATIC_FILE,
		RK_DIRECTORY,
		RK_CGI,
		RK_UPLOAD,
		RK_NOT_FOUND,
		RK_FORBIDDEN
	};

	bool isCgiRequest(const EffectiveConfig& cfg, const std::string& path);

	// Decides which type of handling should be applied to fsPath given the effective configuration.
	RequestKind classifyRequest(const EffectiveConfig& cfg, const std::string& path, const std::string& fsPath, const HTTP_Request& req) {

		const bool	cgi = isCgiRequest(cfg, path);
		
		if (req.method == "POST" && !cfg.upload_store.empty() && !cgi)
			return RK_UPLOAD;

		struct stat st;
		if (stat(fsPath.c_str(), &st) != 0) {
			if (errno == EACCES)
				return RK_FORBIDDEN;
			return RK_NOT_FOUND;
		}
		
		if (S_ISDIR(st.st_mode))
			return RK_DIRECTORY;
		
		if (S_ISREG(st.st_mode)) {
			if (cgi)
				return RK_CGI;
			return RK_STATIC_FILE;
		}

		return RK_FORBIDDEN;
	}

	// --- 2.9. Static file ---

	std::string getMimeType(const std::string& extension);
	std::string getReasonPhrase(int status);

	// Generates a 200 response for a static file (or an appropriate error).
	HTTP_Response handleStaticFile(const HTTP_Request& req, const EffectiveConfig& cfg, const std::string& fsPath) {

		(void)req;														// For now we don't differentiate by method here (GET only in this project).
		
		std::ifstream staticFile(fsPath.c_str(), std::ios::binary);
		if (!staticFile) {
			if (errno == EACCES)
				return makeErrorResponse(403, &cfg);
			else
				return makeErrorResponse(404, &cfg);
		}
		
		HTTP_Response	response;

		response.status = 200;
		response.reason = getReasonPhrase(200);

		std::string::size_type dotPos = fsPath.rfind('.');
		if (dotPos != std::string::npos && dotPos + 1 < fsPath.size())
			response.headers["Content-Type"] = getMimeType(fsPath.substr(dotPos + 1));
		else
			response.headers["Content-Type"] = "application/octet-stream";
		
		std::ostringstream oss;
		oss << staticFile.rdbuf();
		if (!staticFile && !staticFile.eof())							// This is ok, but could be better with stat (and stat can also give us modification date)
			return makeErrorResponse(500, &cfg);
		response.body = oss.str();
		
		std::ostringstream length;
		length << response.body.size();
		response.headers["Content-Length"] = length.str();
		
		return response;
	}

	
	// --- 2.10. Directory / index / autoindex ---

	// Escapes special HTML characters to prevent broken markup and basic XSS vectors.
	// Converts &, <, >, " and ' into their corresponding HTML entities.
	std::string htmlEscape(const std::string& input) {

		std::string out;
		out.reserve(input.size() * 2); // small optimization

		for (std::string::const_iterator it = input.begin(); it != input.end(); ++it) {

			unsigned char c = static_cast<unsigned char>(*it);

			switch (c) {
				case '&':
					out += "&amp;";
					break;
				case '<':
					out += "&lt;";
					break;
				case '>':
					out += "&gt;";
					break;
				case '"':
					out += "&quot;";
					break;
				case '\'':
					out += "&#39;";   // single quote
					break;
				default:
					out += c;
					break;
			}
		}

		return out;
	}

	// Handles directory requests:
	// - tries index files;
	// - if there is no index and autoindex is enabled → generates a listing;
	// - otherwise → returns an appropriate error.
	// Precondition: dirFsPath is already a valid directory (classifyRequest ensures this).
	// URL should already have been normalized (redirect /dir -> /dir/ done earlier, if needed).
	HTTP_Response handleDirectoryRequest(const HTTP_Request& req, const EffectiveConfig& cfg, const std::string& fsPath, const std::string& requestPath) {

		std::string fixedRequestPath = requestPath;
		if (!fixedRequestPath.empty() && fixedRequestPath.back() != '/')
			fixedRequestPath += '/';

		for (size_t i = 0; i < cfg.index_files.size(); ++ i) {

			std::string indexFsPath;
			if (!fsPath.empty() && fsPath.back() == '/')
				indexFsPath = fsPath + cfg.index_files[i];
			else
				indexFsPath = fsPath + '/' + cfg.index_files[i];

			struct stat	st;

			if (stat(indexFsPath.c_str(), &st) == 0 && S_ISREG(st.st_mode))
				return handleStaticFile(req, cfg, indexFsPath);
		}
		
		if (cfg.autoindex) {

			DIR* currentDir = opendir(fsPath.c_str());
			if (!currentDir) {
				if (errno == EACCES)
					return makeErrorResponse(403, &cfg);
				else
					return makeErrorResponse(500, &cfg);
			}
			
			HTTP_Response response;
			
			std::ostringstream oss;
			oss	<< "<!DOCTYPE html>\n"
			<< "<html><head><meta charset=\"utf-8\">"
			<< "<title>Index of " << htmlEscape(fixedRequestPath) << "</title>"
			<< "</head><body>"
			<< "<h1>Index of " << htmlEscape(fixedRequestPath) << "</h1>"
			<< "<ul>";
		    if (fixedRequestPath != "/")
				oss << "<li><a href=\"../\">Parent directory</a></li>";
						
			struct dirent* entry;
			while ((entry = readdir(currentDir)) != NULL) {
				
				if (strcmp(entry->d_name, ".") == 0  || strcmp(entry->d_name, "..") == 0 || entry->d_name[0] == '.')
					continue;
				
				std::string fullPath;
				if (!fsPath.empty() && fsPath.back() == '/')
					fullPath = fsPath + entry->d_name;
				else
					fullPath = fsPath + '/' + entry->d_name;

				struct stat st;

				if (stat(fullPath.c_str(), &st) != 0)
					continue;
				if (S_ISREG(st.st_mode))
					oss << "<li><a href=\"" << htmlEscape(entry->d_name) << "\">" << htmlEscape(entry->d_name) << "</a></li>";
				else if (S_ISDIR(st.st_mode))
					oss << "<li><a href=\"" << htmlEscape(entry->d_name) << "/\">" << htmlEscape(entry->d_name) << "/</a></li>";
			}
			
			closedir(currentDir);
			
			oss << "</ul></body></html>\n";
			
			response.body = oss.str();

			response.status = 200;
			response.reason = getReasonPhrase(200);
			response.headers["Content-Type"] = "text/html; charset=utf-8";

			std::ostringstream length;
			length << response.body.size();
			response.headers["Content-Length"] = length.str();
			
			return response;
		}
		
		return makeErrorResponse(403, &cfg);
	}

    // --- 2.11. CGI (esqueleto) ---

	// Decides whether a path should be treated as CGI.
	bool isCgiRequest(const EffectiveConfig& cfg, const std::string& path) {

		if (cfg.cgi_pass.empty())
			return false;

		std::string::size_type	dotPosition = path.rfind('.');
		if (dotPosition == std::string::npos)
			return false;

		std::string fileExtension = path.substr(dotPosition);

		if (cfg.cgi_pass.find(fileExtension) == cfg.cgi_pass.end())
			return false;

		return true;
	}

	// Returns true if the request's HTTP method is permitted for CGI.
	bool isCgiMethodAllowed(const HTTP_Request& req, const EffectiveConfig& cfg) {

		const std::vector<std::string>& methodList = !cfg.cgi_allowed_methods.empty() ? cfg.cgi_allowed_methods : cfg.allowed_methods;

		for (std::vector<std::string>::const_iterator it = methodList.begin(); it != methodList.end(); ++it) {
			if (*it == req.method)
				return true;
		}

		return false;
	}

	// Returns the file's extension if valid, or an empty string.
	std::string getFileExtension(const std::string& fsPath) {

		std::string::size_type slashPos = fsPath.rfind('/');
		std::string::size_type dotPos = fsPath.rfind('.');

		if (dotPos == std::string::npos)
			return "";

		if (slashPos != std::string::npos && dotPos < slashPos)								// Dot before the last slash - it's in a directory name, not in the filename
			return "";

		if (dotPos == fsPath.size() - 1)													// File without extension
			return "";

		if (dotPos == 0 || (slashPos != std::string::npos && dotPos == slashPos + 1))		// It's a hidden file
			return "";

		return fsPath.substr(dotPos + 1);
	}

	// Prepares interpreter and arguments for executing a CGI script.
	bool prepareCgiExecutor(const EffectiveConfig& cfg, const std::string& fsPath, std::string& outInterpreter,	std::vector<std::string>& outArgv) {
		
		std::string ext = getFileExtension(fsPath);
		
		if (ext.empty())
			return false;

		ext = '.' + ext;
		
		std::map<std::string, std::string>::const_iterator it = cfg.cgi_pass.find(ext);
		if (it == cfg.cgi_pass.end())
			return false;
		
		if (it->second.empty())
			return false;
		
		if (access(it->second.c_str(), X_OK) != 0)
			return false;

		outInterpreter = it->second;

		outArgv.clear();
		outArgv.push_back(outInterpreter);
		outArgv.push_back(fsPath);

		return true;
	}

	// Converts SizeT to String
	std::string toStringSizeT(std::size_t n) {

		std::ostringstream oss;
		
		oss << n;

		return oss.str();
	}

	// Converts HTTP header names into uppercase CGI environment variable format.
	// Returns empty string when header contains invalid or unsupported characters.
	std::string formatCgiEnvHeader(const std::string& header) {

		std::string cgiEnvHeader = "HTTP_";
		cgiEnvHeader.reserve(header.size() + 5);		// "HTTP_" = 5

		for (std::string::size_type i = 0; i < header.size(); ++i) {
			if (header[i] == '-')
				cgiEnvHeader += '_';
			else if (header[i] >= 'a' && header[i] <= 'z')
				cgiEnvHeader += header[i] - 32;
			else if ((header[i] >= 'A' && header[i] <= 'Z') || (header[i] >= '0' && header[i] <= '9'))
				cgiEnvHeader += header[i];
			else
				return "";								// Signals an invalid header
		}

		return cgiEnvHeader;
	}

	// Builds CGI environment variables from HTTP request and config.
	std::vector<std::string> buildCgiEnv(const HTTP_Request& req, const EffectiveConfig& cfg, const std::string& fsPath) {
		
		std::vector<std::string> env;

		env.push_back("GATEWAY_INTERFACE=CGI/1.1");
		env.push_back("REQUEST_METHOD=" + req.method);
		env.push_back("SCRIPT_FILENAME=" + fsPath);
		env.push_back("REQUEST_URI=" + req.target);
		env.push_back("SERVER_PROTOCOL=" + req.version);

		std::string::size_type qMarkPos = req.target.find('?');
		env.push_back("QUERY_STRING=" + (qMarkPos == std::string::npos ? "" : req.target.substr(qMarkPos + 1)));
		env.push_back("SCRIPT_NAME=" + (qMarkPos == std::string::npos ? req.target : req.target.substr(0, qMarkPos)));

		env.push_back("PATH_INFO=");
		env.push_back("PATH_TRANSLATED=");

		if (req.content_length > 0)
			env.push_back("CONTENT_LENGTH=" + toStringSizeT(req.content_length));

		std::map<std::string, std::string>::const_iterator it = req.headers.find("content-type");
		env.push_back("CONTENT_TYPE=" + (it != req.headers.end() ? it->second : ""));

		std::string::size_type colonPos = req.host.find(':');
		if (colonPos != std::string::npos)										// If Host header has port we use this
			env.push_back("SERVER_PORT=" + req.host.substr(colonPos + 1));
		else if (!cfg.server->listen.empty()) {									// Otherwise fall back to this server's first listen directive
			const std::string& listenStr = cfg.server->listen[0];
			std::string::size_type listenColonPos = listenStr.find(':');
			if (listenColonPos != std::string::npos)
				env.push_back("SERVER_PORT=" + listenStr.substr(listenColonPos + 1));
			else
				env.push_back("SERVER_PORT=" + listenStr);
		} else
			env.push_back("SERVER_PORT=80");

		env.push_back("SERVER_NAME=" + (colonPos == std::string::npos ? req.host : req.host.substr(0, colonPos)));

		env.push_back("REMOTE_ADDR=127.0.0.1"); // “If you don’t have a real IP, you can use 127.0.0.1 or something neutral. (For 42 webserver it's ok) In an ideal world you’d fetch the real IP from the Connection (which lives in the core), but that would mean exposing more information to the App. It’s not worth complicating things for the project.”

		for (it = req.headers.begin(); it != req.headers.end(); ++ it) {
			if (it->first != "content-type") {
				std::string envHeader = formatCgiEnvHeader(it->first);
				if (!envHeader.empty())
					env.push_back(envHeader + '=' + it->second);
			}
		}

		env.push_back("REDIRECT_STATUS=200");
		
		return env;
	}

	// Internal structure used to keep CGI process pipes.
	struct CgiPipes {
		int		stdinParent;		// parent writes here (child reads from STDIN)
		int		stdoutParent;  		// parent reads here (child writes to STDOUT)
		pid_t	pid;
	};
	
		// Leitura + timeout:
		struct CgiRawOutput {
			bool timedOut;
			int exitStatus;          // -1 se não foi possível obter
			std::string data;         // tudo o que veio do STDOUT
		};

	// Spawns the CGI child process and sets up stdin/stdout pipes.
	bool spawnCgiProcess(const std::vector<std::string>& argv, const std::vector<std::string>& env, CgiPipes& pipes) {

		int pipeStdin[2];
		int pipeStdout[2];

		if (pipe(pipeStdin) == -1)
			return false;

		if (pipe(pipeStdout) == -1) {
			close(pipeStdin[0]); close(pipeStdin[1]);
			return false;
		}

		pid_t pid = fork();

		if (pid == -1) {
			close(pipeStdin[0]); close(pipeStdin[1]); close(pipeStdout[0]);	close(pipeStdout[1]);
			return false;
		}

		if (pid == 0) {

			if (dup2(pipeStdin[0], STDIN_FILENO) == -1 || dup2(pipeStdout[1], STDOUT_FILENO) == -1) {
				std::cerr << "CGI: dup2() failed";
				_exit(1);
			}

			close(pipeStdin[0]); close(pipeStdin[1]); close(pipeStdout[0]);	close(pipeStdout[1]);	// Close all original pipe fds in the child

			std::vector<char*> cArgv;										// Prepare argv in execve-compatible (NULL-terminated) format
			cArgv.reserve(argv.size() + 1);
			for (size_t i = 0; i < argv.size(); ++i)
				cArgv.push_back(const_cast<char*>(argv[i].c_str()));
			cArgv.push_back(NULL);

			std::vector<char*> cEnvp;										// The same for envp
			cEnvp.reserve(env.size() + 1);
			for (size_t i = 0; i < env.size(); ++i)
				cEnvp.push_back(const_cast<char*>(env[i].c_str()));
			cEnvp.push_back(NULL);

			execve(cArgv[0], &cArgv[0], &cEnvp[0]);
			_exit(1);
		}

		close(pipeStdin[0]); close(pipeStdout[1]);

		pipes.stdinParent = pipeStdin[1];
		pipes.stdoutParent = pipeStdout[0];
		pipes.pid = pid;

		return true;
	}

	// Writes request body to CGI and reads its output with timeout.
	// Handles non-blocking pipes, EOF detection, and child exit status.
	CgiRawOutput readCgiOutput(const CgiPipes& pipes, std::size_t timeoutSeconds, const std::string& requestBody) {

		if (!requestBody.empty()) {													// 1. Write the request body into the CGI's stdin

			size_t		written = 0;
			size_t		remaining = requestBody.size();
			const char*	data = requestBody.data();

			while (remaining > 0) {
				ssize_t n = write(pipes.stdinParent, data + written, remaining);

				if (n > 0) {
					written += n;
					remaining -= n;
					continue;
				}

				break;																// (n <= 0) pipe closed or write error (we cannot inspect errno here) - stop writing immediately
			}
		}
		close(pipes.stdinParent);													// Close stdin so the CGI knows the request body has ended

		int flags = fcntl(pipes.stdoutParent, F_GETFL, 0);							// 2. Make the CGI stdout pipe non-blocking
		if (flags != -1)
			fcntl(pipes.stdoutParent, F_SETFL, flags | O_NONBLOCK);

		struct timeval start;														// 3. Build a timeout deadline using gettimeofday
		gettimeofday(&start, NULL);
		long long deadlineMs = (static_cast<long long>(start.tv_sec) * 1000 ) + (static_cast<long long>(start.tv_usec) / 1000) + (static_cast<long long>(timeoutSeconds) * 1000);		// Cast to long long prevents overflow when multiplying by 1000

		CgiRawOutput cgiOutput;														// Struct holding the final CGI result (timeout, exit code, data)
		cgiOutput.timedOut = false;
		cgiOutput.exitStatus = -1;

		bool eof = false;															// Indicates whether end-of-output (EOF) has been reached
		int childStatus = 0;														// Stores the child's exit status for waitpid()

		while (!eof && !cgiOutput.timedOut) {										// Main loop: read CGI stdout until EOF or timeout

			struct timeval now;														// Check the absolute timeout
			gettimeofday(&now, NULL);
			long long nowMs = (static_cast<long long>(now.tv_sec) * 1000) + (static_cast<long long>(now.tv_usec) / 1000);

			long long remainingMs = deadlineMs - nowMs;
			if (remainingMs <= 0) {
				cgiOutput.timedOut = true;
				break;
			}

			struct pollfd pfd;														// poll() avoids blocking and detects readable data or EOF
			pfd.fd = pipes.stdoutParent;
			pfd.events = POLLIN | POLLHUP | POLLERR;
			pfd.revents = 0;

			int res = poll(&pfd, 1, static_cast<int>(remainingMs));

			if (res == 0) {															// poll timeout reached
				cgiOutput.timedOut = true;
				break;
			}

			if (res < 0) {
				if (errno == EINTR)													// Interrupted by signal - retry poll
					continue;
				eof = true;															// Permanent poll error (EBADF, EINVAL, ...) - stop reading
				break;
			}

			waitpid(pipes.pid, &childStatus, WNOHANG);								// Reap child if it finished (prevents zombies, enables POLLHUP timing)
			
			if (pfd.revents & POLLERR) {											// Fatal error on the file descriptor - no further reads are possible
				eof = true;
				continue;
			}

			if (pfd.revents & POLLIN) {												// There is data available to read

				char buf[4096];														// 4096 = typical memory page size - efficient for pipe I/O

				for (;;) {

					ssize_t n = read(pfd.fd, buf, sizeof(buf));

					if (n > 0) {
						cgiOutput.data.append(buf, static_cast<std::size_t>(n));
						continue;
					}

					if (n == 0) {													// EOF (pipe is fully drained)
						eof = true;
						break;
					}
					
					break;															// n < 0: non-blocking read and nothing available right now (could be EAGAIN / EWOULDBLOCK / EINTR, but we cannot use errno after read)
				}
			}
			else if (pfd.revents & POLLHUP) {										// HUP without POLLIN - Writer side (CGI) closed stdout - definitive EOF and no data signaled
				eof = true;
			}
		} 

		if (cgiOutput.timedOut) {													// 4. Timeout - kill the CGI process
			kill(pipes.pid, SIGKILL);
			waitpid(pipes.pid, NULL, 0);
			close(pipes.stdoutParent);
			return cgiOutput;
		}

		if (waitpid(pipes.pid, &childStatus, 0) > 0) {								// Retrieve the final exit status of the CGI
			if (WIFEXITED(childStatus))
				cgiOutput.exitStatus = WEXITSTATUS(childStatus);
			else if (WIFSIGNALED(childStatus))
				cgiOutput.exitStatus = 128 + WTERMSIG(childStatus);
		}

		close(pipes.stdoutParent);													// Close the reading end of the pipe

		return cgiOutput;
	}

	// Parse CGI output:
	struct CgiParsedOutput {
		int									status;									// 0 in case of missing status:
		std::string							reason;
		std::map<std::string, std::string>	headers;
		std::string							body;
		bool								headersValid;

		CgiParsedOutput()
			: status(0)
			, reason()
			, headers()
			, body()
			, headersValid(false)
		{}
	};

	/* Trim helpers (ASCII space/tab/CR/LF). */
	std::string trimLeft(const std::string& s)  {
		std::size_t i = 0;
		while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
			++i;
		return s.substr(i);
	}

	std::string trimRight(const std::string& s) {
		if (s.empty())
			return s;
		std::size_t i = s.size();
		while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t' || s[i - 1] == '\r' || s[i - 1] == '\n'))
			--i;
		return s.substr(0, i);
	}

	std::string trim(const std::string& s)  {
		return trimRight(trimLeft(s));
	}

	/* Lowercase a copy of a string (ASCII). */
	std::string toLowerCopy(const std::string& s)   {
		std::string out(s);	// copy constructor
		for (std::size_t i = 0; i < out.size(); ++i)    {
			unsigned char   c = static_cast<unsigned char>(out[i]);
			out[i] = static_cast<char>(std::tolower(c));
		}
		return out;
	}

	// Parses raw CGI response headers and body.
	CgiParsedOutput parseCgiOutput(const std::string& raw) {

		std::string 			remainingHeaders;
		CgiParsedOutput			parsedOutput;
		std::string::size_type	pos;

		pos = raw.find("\r\n\r\n");
		if (pos == std::string::npos) {					// Missing CRLFCRLF: breaks CGI header rules (RFC3875) and should trigger 502.
			parsedOutput.headersValid = false;
			parsedOutput.body = raw;
			return parsedOutput;
		}
		
		parsedOutput.headersValid = true;

		remainingHeaders = raw.substr(0, pos);
		parsedOutput.body = raw.substr(pos + 4);

		for (;;) {

			pos = remainingHeaders.find("\r\n");
			if (pos == std::string::npos)
				break;

			std::string currentHeader = remainingHeaders.substr(0, pos);
			remainingHeaders = remainingHeaders.substr(pos + 2);

			pos = currentHeader.find(':');
			if (pos == std::string::npos)
				continue;
			
			std::string name  = trim(currentHeader.substr(0, pos));
			std::string value = trim(currentHeader.substr(pos + 1));

			if (!name.empty())
				parsedOutput.headers[toLowerCopy(name)] = value;
		}

		std::map<std::string, std::string>::const_iterator it = parsedOutput.headers.find("status");
		if (it != parsedOutput.headers.end()) {
			pos = it->second.find(' ');
			if (pos != std::string::npos) {
				parsedOutput.status = std::atoi(it->second.substr(0, pos).c_str());
				parsedOutput.reason = trim(it->second.substr(pos + 1));
			} else {
				parsedOutput.status = std::atoi(it->second.c_str());
				parsedOutput.reason.clear();
			}
			parsedOutput.headers.erase(it);
		}

		return parsedOutput;
	}

	// std::string getReasonPhrase(int status);

	// Builds a complete HTTP response from the parsed CGI output,
	// applying CGI status fallback rules, copying all headers,
	// recalculating Content-Length for safety, and returning the
	// exact body produced by the CGI script.
	HTTP_Response buildCgiHttpResponse(const CgiParsedOutput& out) {

		HTTP_Response res;
		std::map<std::string, std::string>::const_iterator it;

		if (out.status != 0) {											// CGI provided an explicit "Status:" header - use it
			res.status = out.status;
			if (out.reason.empty())
				res.reason = getReasonPhrase(out.status);
			else
				res.reason = out.reason;
		} else {														// No "Status:" header - apply CGI fallback rules
			it = out.headers.find("location");
			if (it != out.headers.end()) {								// Classic CGI implicit redirect: a Location header without Status implies 302 Found (old CGI scripts)
				res.status = 302;
				res.reason = getReasonPhrase(res.status);
			} else {													// Normal case: successful response without explicit status
				res.status = 200;
				res.reason = getReasonPhrase(res.status);
			}
		}

		for (it = out.headers.begin(); it != out.headers.end(); ++it) {	// Copy CGI headers, skipping those managed by the core
			if (it->first == "content-length" || it->first == "connection" || it->first == "transfer-encoding")
				continue;
			res.headers[it->first] = it->second;
		}

		std::ostringstream oss;											// Always recompute Content-Length - safer than trusting a value provided by the CGI script
		oss << out.body.size();
		res.headers["content-length"] = oss.str();

		res.body = out.body;											// Copy the body exactly as produced by the CGI
		res.close = false;												// Do not decide connection closing here - final keep-alive/close behaviour will be applied later

		return res;
	}
	
	/*
		CGI logical handler
		- It encapsulates the entire lifecycle of a CGI execution
		- It validates, prepares, runs, gathers, and converts the external output into a correct HTTP response that complies with the protocol.
	*/
	HTTP_Response handleCgiRequest(const HTTP_Request& req,	const EffectiveConfig& cfg, const std::string& fsPath)	{

		if (!isCgiMethodAllowed(req, cfg))
			return makeErrorResponse(405, &cfg);

		struct stat st;

		if (stat(fsPath.c_str(), &st) != 0)			// Doesn't exist
			return makeErrorResponse(404, &cfg);

		if (!S_ISREG(st.st_mode))					// Not a regular file
			return makeErrorResponse(403, &cfg);

		if (access(fsPath.c_str(), R_OK) != 0)		// No read permissions
			return makeErrorResponse(403, &cfg);

		std::string	interpreter;
		std::vector<std::string> argv;
		if (!prepareCgiExecutor(cfg, fsPath, interpreter, argv))
			return makeErrorResponse(502, &cfg);

		std::vector<std::string> envp = buildCgiEnv(req, cfg, fsPath);

		CgiPipes pipes;
		if (!spawnCgiProcess(argv, envp, pipes))
			return makeErrorResponse(502, &cfg);

		CgiRawOutput raw = readCgiOutput(pipes, cfg.cgi_timeout, req.body);

		if (raw.timedOut)
			return makeErrorResponse(504, &cfg);

		if (raw.exitStatus == -1 || (raw.exitStatus != 0 && raw.data.empty()))
			return makeErrorResponse(502, &cfg);

		CgiParsedOutput parsedOutput = parseCgiOutput(raw.data);

		if (!parsedOutput.headersValid)
			return makeErrorResponse(502, &cfg);

		std::map<std::string, std::string>::const_iterator itContentType = parsedOutput.headers.find("content-type");
		std::map<std::string, std::string>::const_iterator itRedirection = parsedOutput.headers.find("location");
		if (itContentType == parsedOutput.headers.end() && itRedirection == parsedOutput.headers.end())
			return makeErrorResponse(502, &cfg);

		HTTP_Response response = buildCgiHttpResponse(parsedOutput);

		if (req.keep_alive == false)
			response.close = true;

		return response;
	}

	// --- 2.12. Uploads ---

	bool isSanitizedFilename(const std::string& filename) {

		if (filename.empty())
			return false;

		if (filename == "." || filename == "..")
			return false;

		for (std::string::const_iterator it = filename.begin(); it != filename.end(); ++it) {

			unsigned char c = static_cast<unsigned char>(*it);

			if (c < 32)
				return false;

			switch (c) {

				case '/':
				case '\\':
					return false;

				case ':':
				case '*':
				case '?':
				case '"':
				case '<':
				case '>':
				case '|':
					return false;

				default:
					break;
			}
		}

		return true;
	}

	// Handles uploads (e.g., POST to upload_store).
	HTTP_Response handleUploadRequest(const HTTP_Request& req, const EffectiveConfig& cfg, const std::string& fsPath) {

		if (req.method != "POST")										// Redundant (already checked in classifyRequest), but safe
			return make405(cfg);

		std::map<std::string, std::string>::const_iterator it = req.headers.find("content-type");	// Allow only simple uploads (reject multipart/form-data)
		if (it != req.headers.end())
			if (it->second.find("multipart/form-data") != std::string::npos)	// multipart must be rejected (requires boundary parsing)
				return makeErrorResponse(501, &cfg);

		if (cfg.upload_store.empty())									// Config coherence (redundant - already checked in classifyRequest - but safe)
			return makeErrorResponse(500, &cfg);

		std::string::size_type lastSlashPos = fsPath.rfind('/');		// Separate parent directory and filename
		if (lastSlashPos == std::string::npos)
			return makeErrorResponse(400, &cfg);

		std::string filename = fsPath.substr(lastSlashPos + 1);
			
		if (filename.empty() || !isSanitizedFilename(filename))			// Filename validation
			return makeErrorResponse(400, &cfg);
			
		std::string destinationDirectory = cfg.upload_store;
		std::string destinationFilePath;

		if (!destinationDirectory.empty() && destinationDirectory.back() == '/')
			destinationFilePath = destinationDirectory + filename;
		else
			destinationFilePath = destinationDirectory + '/' + filename;

		struct stat st;													// Validate parent directory (must exist and be a directory)
		
		if (stat(destinationDirectory.c_str(), &st) < 0)
			return makeErrorResponse(500, &cfg);						// Parent directory missing or inaccessible

		if (!S_ISDIR(st.st_mode))
			return makeErrorResponse(500, &cfg);						// Parent path is not a directory

		if (stat(destinationFilePath.c_str(), &st) == 0) {							// Destination path: check for existence and disallow overwrite
			if (S_ISDIR(st.st_mode))
				return makeErrorResponse(403, &cfg);					
			return makeErrorResponse(409, &cfg);						// Overwrite not allowed
		} else
			if (errno != ENOENT)										// ENOENT = file does not exist - OK to create
				return makeErrorResponse(500, &cfg);

		std::ofstream uploadedFile(destinationFilePath.c_str(), std::ios::binary);	// Create file for writing

		if (!uploadedFile) {
			if (errno == EACCES)
				return makeErrorResponse(403, &cfg);
			return makeErrorResponse(500, &cfg);
		}

		uploadedFile.write(req.body.data(), req.body.size());			// Write file contents

		if (uploadedFile.fail()) {
			uploadedFile.close();
			std::remove(destinationFilePath.c_str());								// Cleanup incomplete file
			return makeErrorResponse(500, &cfg);
		}

		uploadedFile.close();

		HTTP_Response response;											// It's a success!!

		response.status = 201;
		response.reason = getReasonPhrase(response.status);
		response.headers["Location"] = req.target;						// Logical path
		response.headers["Content-Type"] = "text/plain";
		response.headers["Content-Length"] = "0";
		response.body.clear();

		return response;
	}

    // --- 2.13. Error pages custom / genérico ---

	// Returns the relative or absolute path configured for a given status,
	// or an empty string if no specific error_page is defined.
	std::string findErrorPagePath(const EffectiveConfig& conf, int status) {
		
		std::map<int, std::string>::const_iterator it = conf.error_pages.find(status);

		if (it == conf.error_pages.end())
			return "";
		
		std::string path = it->second; 

		if (path.size() >= 2 && path[0] == '.' && path[1] == '/')
			path.erase(0, 2);
		else if (!path.empty() && path[0] == '/')
			path.erase(0, 1);

		if (!conf.root.empty() && conf.root.back() != '/')
			path = conf.root + '/' + path;
		else
			path = conf.root + path;

		return path;
	}

	std::string getReasonPhrase(int status) {

		switch (status) {

			// 1xx — Informational
			case 100: return "Continue";
			case 101: return "Switching Protocols";
			case 102: return "Processing";

			// 2xx — Success
			case 200: return "OK";
			case 201: return "Created";
			case 202: return "Accepted";
			case 204: return "No Content";
			case 206: return "Partial Content";

			// 3xx — Redirection
			case 301: return "Moved Permanently";
			case 302: return "Found";
			case 303: return "See Other";
			case 304: return "Not Modified";
			case 307: return "Temporary Redirect";
			case 308: return "Permanent Redirect";

			// 4xx — Client errors
			case 400: return "Bad Request";
			case 401: return "Unauthorized";
			case 403: return "Forbidden";
			case 404: return "Not Found";
			case 405: return "Method Not Allowed";
			case 408: return "Request Timeout";
			case 409: return "Conflict";
			case 413: return "Payload Too Large";
			case 414: return "URI Too Long";
			case 415: return "Unsupported Media Type";
			case 431: return "Request Header Fields Too Large";

			// 5xx — Server errors
			case 500: return "Internal Server Error";
			case 501: return "Not Implemented";
			case 502: return "Bad Gateway";
			case 503: return "Service Unavailable";
			case 504: return "Gateway Timeout";
			case 505: return "HTTP Version Not Supported";

			default:  return "Unknown Status";
		}
	}

	// Builds an error response:
	// - tries to use a configured error_page if available;
	// - if that fails, generates a default HTML page.
	// conf may be NULL for early errors (before a Server is selected).
	HTTP_Response makeErrorResponse(int status, const EffectiveConfig* conf) {
		
		HTTP_Response	response;

		response.status = status;
		response.reason = getReasonPhrase(status);

		std::string	body;

		// 1) Try custom error_page if configuration is available
		if (conf) {
			std::string errorPagePath = findErrorPagePath(*conf, status);
			if (!errorPagePath.empty()) {
				std::ifstream file(errorPagePath.c_str(), std::ios::binary);
				if (file) {
					std::ostringstream oss;
					oss << file.rdbuf();
					body = oss.str();
				}
			}
		}

		// 2) If no body (no error_page or failed to read it) → generate default HTML
		if (body.empty()) {
			std::ostringstream oss;
			oss	<< "<!DOCTYPE html>\n"
				<< "<html><head><meta charset=\"utf-8\">"
				<< "<title>" << status << ' ' << response.reason << "</title>"
				<< "</head><body>"
				<< "<h1>" << status << ' ' << response.reason << "</h1>"
				<< "</body></html>\n";
			body = oss.str();
		}

		// 3) Fill in the response fields
		response.body = body;

		std::ostringstream contentLength;
		contentLength << body.size();

		response.headers.clear();
		response.headers["Content-Type"] = "text/html";
		response.headers["Content-Length"] = contentLength.str();

		return response;
	}

	// --- 2.14. MIME types ---

	// Determines the Content-Type from the file extension.
	std::string getMimeType(const std::string& ext) {

		std::string lowerExt = ext;

		for (std::string::iterator it = lowerExt.begin(); it != lowerExt.end(); ++ it)
			*it = std::tolower(static_cast<unsigned char>(*it));

		if (lowerExt == "html" || lowerExt == "htm") return "text/html";
		if (lowerExt == "jpeg" || lowerExt == "jpg") return "image/jpeg";
		if (lowerExt == "json") return "application/json";
		if (lowerExt == "js") return "application/javascript";
		if (lowerExt == "css") return "text/css";
		if (lowerExt == "png") return "image/png";
		if (lowerExt == "gif") return "image/gif";
		if (lowerExt == "ico") return "image/x-icon";
		if (lowerExt == "svg") return "image/svg+xml";
		if (lowerExt == "txt") return "text/plain";
		if (lowerExt == "pdf") return "application/pdf";

		return "application/octet-stream";
	}

    // --- 2.15. Connection / keep-alive ---

	// Sets the Connection/close header in the HTTP_Response based on the request and status.
	void applyConnectionHeader(const HTTP_Request& request, HTTP_Response& response) {
		
		if (request.keep_alive) {
			response.close = false;
			response.headers["Connection"] = "keep-alive";
		} else {
			response.close = true;
			response.headers["Connection"] = "close";
		}
	}
}

HTTP_Response	handleRequest(const HTTP_Request& request, const std::vector<Server>& servers) {
	
	// 0) Minimal safeguard: no servers configured → 500
	if (servers.empty()) {
		HTTP_Response response = makeErrorResponse(500, NULL);
		applyConnectionHeader(request, response);
		return response;
	}

	// 1) Parse target → extract path + query
	std::string	path;
	std::string	query;
	if (!parseTarget(request, path, query)) {
		HTTP_Response response = makeErrorResponse(400, NULL);
		applyConnectionHeader(request, response);
		return response;
	}
	
	// 2) Select the Server (vhost)
	const Server& server = selectServer(servers, request);

	// 3) Find the matching Location (longest prefix)
	const Location* location = matchLocation(server, path);

	// 4) Build effective configuration (merge Server + Location)
	EffectiveConfig config = buildEffectiveConfig(server, location);

	// 5.1) Check if method is implemented at all
	if (request.method != "GET"	&& request.method != "POST"	&& request.method != "DELETE") {
		HTTP_Response response = makeErrorResponse(501, &config);
		applyConnectionHeader(request, response);
		return response;
	}

	// 5.2) Check if the method is allowed (405)
	if (!isMethodAllowed(config, request.method)) {
		HTTP_Response response = make405(config);
		applyConnectionHeader(request, response);
		return response;
	}

	// 6) Validate body constraints (size, etc.)
	int bodyErrorStatus = 0;
	if (!checkRequestBodyAllowed(config, request, bodyErrorStatus)) {
		HTTP_Response response = makeErrorResponse(bodyErrorStatus, &config);
		applyConnectionHeader(request, response);
		return response;
	}

	// 7) Map logical path → filesystem
	std::string fsPath = makeFilesystemPath(config, path);
	if (fsPath.empty()) {
		// Se a construção falhar, tratamos como erro interno de config.
		HTTP_Response response = makeErrorResponse(500, &config);
		applyConnectionHeader(request, response);
		return response;
	}

	// 8) Normalizar e garantir que está dentro do root (anti-traversal)
	if (!normalizePath(fsPath, config.root)) {
		// You may choose between 403 (forbidden) or 404 (to avoid exposing structure).
		HTTP_Response response = makeErrorResponse(403, &config);
		applyConnectionHeader(request, response);
		return response;
	}

	// 9) Classify the request (static file, directory, CGI, upload, etc.)
	RequestKind kind = classifyRequest(config, path, fsPath, request);
	HTTP_Response response;
	switch (kind) {

		case RK_UPLOAD:
			response = handleUploadRequest(request, config, fsPath);
			break;

		case RK_CGI:
			response = handleCgiRequest(request, config, fsPath);
			break;

		case RK_DIRECTORY:
			response = handleDirectoryRequest(request, config, fsPath, path);
			break;

		case RK_STATIC_FILE:
			response = handleStaticFile(request, config, fsPath);
			break;

		case RK_FORBIDDEN:
			response = makeErrorResponse(403, &config);
			break;

		case RK_NOT_FOUND:
		default:
			response = makeErrorResponse(404, &config);
			break;
	}

	// 10) Apply Connection / keep-alive header based on the request
	applyConnectionHeader(request, response);
	return response;
}