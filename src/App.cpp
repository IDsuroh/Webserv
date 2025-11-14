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

    // --- 2.5. Métodos permitidos / 405 ---

    // Verifica se o método é permitido pela config efetiva.
    bool isMethodAllowed(
        const EffectiveConfig& conf,
        const std::string& method
    );

    // Constrói uma resposta 405 com header Allow e (se configurado) error_page.
    HTTP_Response make405(
        const EffectiveConfig& conf
    );

    // --- 2.6. Validação de body (tamanho, política) ---

    // Verifica se o body é aceitável (tamanho, método, etc.).
    // Em caso de erro, escreve o status em status_out (ex: 413, 400) e retorna false.
    bool checkRequestBodyAllowed(
        const EffectiveConfig& conf,
        const HTTP_Request& req,
        int& status_out
    );

    // --- 2.7. Root + path → filesystem seguro ---

    // Constrói o caminho base no filesystem a partir do root + path lógico.
    std::string makeFilesystemPath(
        const EffectiveConfig& conf,
        const std::string& path
    );

    // Normaliza o caminho (., .., //) e garante forma canónica.
    // Se a normalização falhar (ex: path inválido), retorna false.
    bool normalizePath(
        std::string& fsPath,
        const std::string& root
    );

    // Verifica se fsPath, já normalizado, permanece dentro de root.
    bool isWithinRoot(
        const std::string& fsPath,
        const std::string& root
    );

    // --- 2.8. Classificação do pedido ---

    enum RequestKind {
        RK_STATIC_FILE,
        RK_DIRECTORY,
        RK_CGI,
        RK_UPLOAD,
        RK_NOT_FOUND,
        RK_FORBIDDEN
    };

    // Decide que tipo de handling deve ser aplicado ao fsPath dado a config.
    RequestKind classifyRequest(
        const EffectiveConfig& conf,
        const std::string& path,
        const std::string& fsPath,
        const HTTP_Request& req
    );

    // --- 2.9. Static file ---

    // Gera resposta 200 para ficheiro estático (ou erro adequado).
    HTTP_Response handleStaticFile(
        const HTTP_Request& req,
        const EffectiveConfig& conf,
        const std::string& fsPath
    );

    // --- 2.10. Diretoria / index / autoindex ---

    // Trata pedidos a diretoria:
    // - tenta index;
    // - se não houver e autoindex ativo → gera listagem;
    // - caso contrário → erro adequado.
    HTTP_Response handleDirectoryRequest(
        const HTTP_Request& req,
        const EffectiveConfig& conf,
        const std::string& dirFsPath,
        const std::string& urlPath
    );

    // --- 2.11. CGI (esqueleto) ---

    // Decide se um path deve ser tratado como CGI.
    bool isCgiRequest(
        const EffectiveConfig& conf,
        const std::string& path
    );

    // Handler lógico de CGI (a implementar com fork/exec no futuro).
    HTTP_Response handleCgiRequest(
        const HTTP_Request& req,
        const EffectiveConfig& conf,
        const std::string& fsPath
    );

    // --- 2.12. Uploads ---

    // Verifica se a config define esta location como destino de upload.
    bool isUploadLocation(
        const EffectiveConfig& conf
    );

    // Trata uploads (ex: POST para upload_store).
    HTTP_Response handleUploadRequest(
        const HTTP_Request& req,
        const EffectiveConfig& conf,
        const std::string& fsPathBase
    );

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

    // Determina Content-Type a partir da extensão do ficheiro.
    std::string getMimeType(
        const std::string& extension
    );

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

	// 5) Check if the method is allowed (405)
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
	if (!normalizePath(fsPath, config.root) || !isWithinRoot(fsPath, config.root)) {
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