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

    // --- 2.2. Escolha do Server (vhost) ---

    // Escolhe o Server com base no Host header / server_name.
    // Garante sempre um Server (ex: default).
    const Server& selectServer(
        const std::vector<Server>& servers,
        const HTTP_Request& req
    );

    // --- 2.3. Escolha do Location (longest prefix) ---

    // Longest prefix match sobre o path.
    // Devolve ponteiro para Location ou NULL se nenhuma fizer match.
    const Location* matchLocation(
        const Server& server,
        const std::string& path
    );

    // --- 2.4. Config efetiva (merge Server + Location) ---

    struct EffectiveConfig {
        const Server*                 server;        // nunca NULL na prática
        const Location*               location;      // pode ser NULL

        std::string                   root;
        bool                          autoindex;
        std::vector<std::string>      index_files;
        std::vector<std::string>      allowed_methods;
        std::map<int, std::string>    error_pages;

        // Campos adicionais típicos:
        std::size_t                   client_max_body_size;
        std::string                   upload_store;
        // Mapa/extensões CGI, etc.
        // std::map<std::string, std::string> cgi_pass;
    };

    // Constrói uma visão unificada da config (root, métodos, error_pages, etc.)
    EffectiveConfig buildEffectiveConfig(
        const Server& srv,
        const Location* loc
    );

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