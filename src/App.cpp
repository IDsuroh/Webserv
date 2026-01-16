/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   App.cpp                                            :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: hugo-mar <hugo-mar@student.42.fr>          +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2025/12/06 13:09:28 by hugo-mar          #+#    #+#             */
/*   Updated: 2026/01/16 22:49:33 by hugo-mar         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

/*	---------------------------------------------------------------------------
	Application Layer ("App") of the Webserver

	This module receives a fully parsed HTTP_Request from the core layer and
	decides how to generate the corresponding HTTP_Response.

	Responsibilities:
		- Parse and normalise the request target (path + query)
		- Select the Server block and its matching Location
		- Build the EffectiveConfig merging server/location directives
		- Apply redirects, method validation and body constraints
		- Map logical paths to the filesystem and enforce anti-traversal rules
		- Classify the request (static file, directory, CGI, upload, etc.)
		- Delegate to the appropriate handler to produce the final response
		- Set connection headers (keep-alive / close)

	Why "App"?
		In a typical HTTP server architecture, the "Application Layer" is the
		component that interprets the request semantically and makes the logical
		decisions about how it should be served. It is separate from:
		- the core networking layer (poll/select, buffering, parsing),
		- the configuration parser,
		- the HTTP serializer.

	The App therefore represents everything that turns a valid HTTP request
	into a meaningful HTTP response using the server configuration and the
	filesystem/CGI handlers.
	------------------------------------------------------------------------- */

#include "App.hpp"

namespace {


	// --------------------------------
	// --- 1. Target (path + query) ---
	// --------------------------------

	/*
	Parses request.target and extracts (path, query) from:
	- "*" (OPTIONS *)
	- origin-form: "/path?query"
	- absolute-form: "http(s)://host/path?query"

	Returns false for malformed/unsupported targets.
	*/
	bool parseTarget(const HTTP_Request& req, std::string& path, std::string& query) {

		if (req.target.empty())
			return false;

		if (req.target == "*") {                                                // 1) "*" form
			path = "/";
			query.clear();
			return true;
		}

		std::string normalizedTarget;                                           // normalized to origin-form: "/path?query"

		if (req.target[0] == '/') {                                             // 2) origin-form
			normalizedTarget = req.target;
		}
		else {
			std::string::size_type scheme = req.target.find("://");             // 3) absolute-form
			if (scheme == std::string::npos)
				return false;

			std::string::size_type slash = req.target.find('/', scheme + 3);
			if (slash == std::string::npos) {                                   // absolute URI with no path
				path = "/";
				query.clear();
				return true;
			}

			normalizedTarget = req.target.substr(slash);                        // "/path?query..."
		}

		std::string::size_type questionMark = normalizedTarget.find('?');

		if (questionMark == std::string::npos) {
			path = normalizedTarget;
			query.clear();
		} else {
			path = normalizedTarget.substr(0, questionMark);
			query = (questionMark + 1 < normalizedTarget.size()) ? normalizedTarget.substr(questionMark + 1) : "";
		}

		if (path.empty() || path[0] != '/')
			return false;

		return true;
	}


	// -----------------------------------
	// --- 2. Server selection (vhost) ---
	// -----------------------------------

	/*
	 Selects the appropriate virtual server based on the Host header.
	 Returns the first matching server_name; falls back to servers[0] if none match.
	*/
	const Server& selectServer(const std::vector<Server>& servers, const HTTP_Request& request) {

		for (size_t i = 0; i < servers.size(); ++i) {

			std::vector<std::string>::const_iterator it;

			it = std::find(servers[i].server_name.begin(), servers[i].server_name.end(), request.host);

			if (it != servers[i].server_name.end())
				return servers[i];
		}

		return servers[0];				// Falls back to the first server, matching real-world default vhost behavior (NGINX/Apache).
	}


	// ----------------------------------------------
	// --- 3. Location selection (longest prefix) ---
	// ----------------------------------------------

	/*
	 Performs longest-prefix matching to find the best location for the request path.
	 A location matches only if the prefix boundary is valid. Returns NULL if none match.
	*/
	const Location* matchLocation(const Server& server, const std::string& requestPath) {

		const Location*	bestMatchLocation = NULL;
		
		for (std::vector<Location>::const_iterator it = server.locations.begin(); it != server.locations.end(); ++ it) {

			const std::string& locationPath = it->path;
			bool match = (requestPath.compare(0, locationPath.size(), locationPath) == 0);										// Is locationPath a prefix of requestPath?
			bool boundary = (requestPath.size() == locationPath.size() ||														// '/ap' cannot match '/api'.
							(requestPath.size() > locationPath.size() && requestPath[locationPath.size()] == '/'));

			if (match && boundary && (bestMatchLocation == NULL || bestMatchLocation->path.size() < locationPath.size()))		// Longest prefix match
				bestMatchLocation = &(*it);																						// Gives the address of the Location object pointed to by the iterator
		}

		return bestMatchLocation;
	}
	

	// -----------------------------------------------------
	// --- 4. Effective config (merge Server + Location) ---
	// -----------------------------------------------------

	// --- 4.1. Effective Config Utilities (helper functions and structs) ---

	/*
	 Default limits for maximum request body size and CGI execution timeout (default configuration constants).
	*/
	const std::size_t	kDefaultClientMaxBodySize =	0;				// 0 means no limit / unlimited unless configured
	const std::size_t	kDefaultCgiTimeout = 		30;				// 30 seconds

	/*
	 Holds the merged Server and Location directives for handling a request.
	 Provides resolved defaults, limits, CGI options, and redirect configuration.
	*/
	struct EffectiveConfig {
		const Server*						server;					// never NULL
		const Location*						location;				// may be NULL

		std::string							root;
		bool								autoindex;
		std::vector<std::string>			indexFiles;
		std::vector<std::string>			allowedMethods;
		std::map<int, std::string>			errorPages;

		std::size_t							clientMaxBodySize;
		std::string							uploadStore;
		std::map<std::string, std::string>	cgiPass;
		std::size_t							cgiTimeout;
		std::vector<std::string>			cgiAllowedMethods;

		int									redirectStatus;			// 0 - no redirect
		std::string							redirectTarget;

		EffectiveConfig()
			: server(NULL)
			, location(NULL)
			, root(".")
			, autoindex(false)
			, indexFiles()
			, allowedMethods()
			, errorPages()
			, clientMaxBodySize(kDefaultClientMaxBodySize)
			, uploadStore()
			, cgiPass()
			, cgiTimeout(kDefaultCgiTimeout)
			, cgiAllowedMethods()
			, redirectStatus(0)
			, redirectTarget()
			{}
	};

	/*
	 Splits a string into whitespace-separated words and returns them as a vector.
	*/
	std::vector<std::string> splitWords(const std::string& input) {

		std::vector<std::string>	words;
		std::istringstream			iss(input);
		std::string					currentWord;

		while (iss >> currentWord)
			words.push_back(currentWord);

		return words;
	}

	/*
	 Comma-aware parsing (methods/index/cgi_allowed_methods):
	  - Config currently joins multi-values with commas;
	  - Splitting on comma or space prevents “GET,POST” from being treated as one token
	    (fixes 405s and missing index files).
	*/
	std::vector<std::string> splitWordsAndCommas(const std::string& input) {
		
		std::vector<std::string> output;
		std::string currentWord;
	
		for (std::size_t i = 0; i <= input.size(); ++i) {
			char c = (i == input.size()) ? ' ' : input[i];
			if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
				if (!currentWord.empty()) {
					output.push_back(currentWord);
					currentWord.clear();
				}
			}
			else
				currentWord += c;
		}
		return output;
	}

	/*
	 Validates and parses an HTTP status code string into an integer (100–599).
	 Throws on non-digits, overflow, or invalid ranges.
	*/
	int parseHttpStatus(const std::string& str) {

		if (str.empty())
			throw std::runtime_error("Empty HTTP status code");

		for (std::string::size_type i = 0; i < str.size(); ++i) {
			if (!(std::isdigit(static_cast<unsigned char>(str[i]))))				// static_cast for protection against UB if signed (locale dependent)
				throw std::runtime_error("Invalid HTTP status code: " + str);
		}

		errno = 0;
		unsigned long value = std::strtoul(str.c_str(), NULL, 10);					// Parse into unsigned long first to detect overflow before casting to int.

		if ((value == ULONG_MAX && errno == ERANGE))
			throw std::runtime_error("HTTP status code overflow: " + str);

		if (value < 100 || value > 599)
			throw std::runtime_error("HTTP status code out of range: " + str);

		return static_cast<int>(value);
	}

	/*
	 Parses a numeric value with an optional binary suffix (k, M, G) and returns
	 the corresponding size in bytes. Throws on invalid syntax or overflow.
	*/
	std::size_t parseSizeWithSuffix(const std::string& str) {

		if (str.empty())
			throw std::runtime_error("Empty numeric value");

		std::istringstream iss(str);

		unsigned long long value = 0;
		if (!(iss >> value))
			throw std::runtime_error("Invalid numeric value: " + str);

		char suffix = 0;
		if (iss >> suffix) {
			char extra = 0;
			if (iss >> extra)
				throw std::runtime_error("Invalid size suffix in: " + str);
		}

		unsigned long long multiplier = 1;
		switch (suffix) {
			case 0:
				multiplier = 1;
				break;

			case 'k':
			case 'K':
				multiplier = 1024ULL;
				break;

			case 'm':
			case 'M':
				multiplier = 1024ULL * 1024ULL;
				break;

			case 'g':
			case 'G':
				multiplier = 1024ULL * 1024ULL * 1024ULL;
				break;

			default:
				throw std::runtime_error("Invalid size suffix in: " + str);
		}

		if (multiplier != 0 && value > (std::numeric_limits<unsigned long long>::max)() / multiplier)	// Detect overflow before multiplication
			throw std::runtime_error("Numeric value overflow: " + str);

		unsigned long long result = value * multiplier;													// unsigned long long to prevent overflow before size_t cast
		if (result > (std::numeric_limits<std::size_t>::max)())
			throw std::runtime_error("Numeric value exceeds size_t range: " + str);

		return static_cast<std::size_t>(result);
	}

	/*
	 Parses a numeric string into size_t, validating digits and overflow conditions.
	*/
	size_t parseSizeT(const std::string& str) {

		if (str.empty())
			throw std::runtime_error("Empty numeric value");

		size_t value = 0;
		const size_t max = std::numeric_limits<size_t>::max();

		for (size_t i = 0; i < str.size(); ++i) {

			if (!(std::isdigit(static_cast<unsigned char>(str[i]))))
				throw std::runtime_error("Invalid numeric value: " + str);

			size_t digit = str[i] - '0';

			if (value > max / 10 || (value == max / 10 && digit > max % 10))					// Prevent overflow before multiplication
				throw std::runtime_error("Numeric value exceeds size_t range: " + str);

			value = value * 10 + digit;
		}

		return value;
	}


	/*
	 Looks up a directive by key, preferring Location over Server.
	 Returns true and sets value if found; otherwise returns false.
	*/
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

	/*
	 Merges error_page configuration from Server and Location into EffectiveConfig.
	 Server-level mappings are loaded first; Location error_page overrides or adds entries.
	*/
	void resolveErrorPages(EffectiveConfig& cfg, const Server& srv, const Location* loc) {

		std::map<std::string, std::string>::const_iterator it;
		
		for (it = srv.error_pages.begin(); it != srv.error_pages.end(); ++it)
			cfg.errorPages[parseHttpStatus(it->first)] = it->second;
		
		if (!loc)
			return;

		it = loc->directives.find("error_page");
		if (it == loc->directives.end())
			return;

		std::vector<std::string> tokens = splitWords(it->second);
		if (tokens.size() < 2)
			throw std::runtime_error("Bad error_page configuration");

		const std::string& uri = tokens.back();						// Follows NGINX-style syntax: multiple status codes followed by a single URI.
																	// URI - Uniform Resource Identifier - identifies a resource (e.g. page, file, image, etc.)
		for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
			cfg.errorPages[parseHttpStatus(tokens[i])] = uri;
	}

	// --- 4.2. Effective Configuration Construction ---

	/*
	 Builds the EffectiveConfig by merging Server and Location directives.
	 Applies defaults for methods, error pages, CGI options, and redirects.
	*/
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
			cfg.indexFiles = splitWordsAndCommas(value);

		if (getDirectiveValue(loc, srv, "methods", value))
			cfg.allowedMethods = splitWordsAndCommas(value);
		else {
			cfg.allowedMethods.push_back("GET");						// Default to GET and POST so locations remain functional without explicit method configuration. 
			cfg.allowedMethods.push_back("POST");
		}

		resolveErrorPages(cfg, srv, loc);

		if (getDirectiveValue(loc, srv, "client_max_body_size", value))
			cfg.clientMaxBodySize = parseSizeWithSuffix(value);

		if (getDirectiveValue(loc, srv, "upload_store", value))
			cfg.uploadStore = value;

		if (getDirectiveValue(loc, srv, "cgi_pass", value)) {
			std::vector<std::string> tokens = splitWords(value);
			if (tokens.size() < 2)
				throw	std::runtime_error("cgi_pass requires 2 arguments: <ext> <bin>");
			cfg.cgiPass[tokens[0]] = tokens[1];
		}

		if (getDirectiveValue(loc, srv, "cgi_timeout", value))
			cfg.cgiTimeout = parseSizeT(value);

		if (getDirectiveValue(loc, srv, "cgi_allowed_methods", value))
			cfg.cgiAllowedMethods = splitWordsAndCommas(value);
		else
			cfg.cgiAllowedMethods = cfg.allowedMethods;					// Default: CGI inherits location methods to ensure consistent behaviour

		if (getDirectiveValue(loc, srv, "return", value)) {				// Handle HTTP 3xx redirects via 'return' directive
			std::vector<std::string> tokens = splitWords(value);
			if (tokens.size() == 2) {									// Ignores malformed configurations
				int status = parseHttpStatus(tokens[0]);
				if (status >= 300 && status <= 399) {
					cfg.redirectStatus = status;
					cfg.redirectTarget = tokens [1];
				}
			}
		}

		return cfg;
	}


	// --------------------------------
	// --- 5. Allowed methods / 405 ---
	// --------------------------------

	/*
	 Checks whether the given HTTP method is allowed by the configuration.
	*/
	bool isMethodAllowed(const EffectiveConfig& cfg, const std::string& method) {
		
		for (std::vector<std::string>::const_iterator it = cfg.allowedMethods.begin(); it != cfg.allowedMethods.end(); ++it)
			if (*it == method)
				return true;
		
		return false;
	}

	HTTP_Response makeErrorResponse(int status, const EffectiveConfig* cfg);

	/*
	 Builds a 405 Method Not Allowed response and sets the Allow header accordingly.
	*/
	HTTP_Response make405(const EffectiveConfig& cfg) {

		HTTP_Response response = makeErrorResponse(405, &cfg);

		std::string allowed;
		for (std::vector<std::string>::const_iterator it = cfg.allowedMethods.begin(); it != cfg.allowedMethods.end(); ++it) {
			if (!allowed.empty())						// Add comma after the first method
				allowed += ", ";
			allowed += *it;
		}

		if (!allowed.empty())
			response.headers["Allow"] = allowed;

		return response;
	}


	// -----------------------------------------
	// --- 6. Body validation (size, policy) ---
	// -----------------------------------------

	/*
	 Validates request body size and supported transfer encodings.
	 Returns true if allowed; otherwise sets the appropriate status code
	 and indicates whether the connection must be closed.
	*/
	bool checkRequestBodyAllowed(const EffectiveConfig& cfg, const HTTP_Request& req, int& status) {
		
		if (cfg.clientMaxBodySize != 0 && req.content_length > cfg.clientMaxBodySize) {			// clientMaxBodySize == 0, means no limit
			status = 413;
			return false;
		}

		if (req.transfer_encoding.empty() || req.transfer_encoding == "chunked")
			return true;

		status = 501;				// Otherwise, not implemented
		return false;
	}


	// -----------------------------------------------
	// --- 7. Root + path → secure filesystem path ---
	// -----------------------------------------------

	/*
	 Resolves a filesystem path from a request URI, taking the matched
	 location into account by stripping its prefix before mapping the
	 remaining path under the effective root directory.
	 EffectiveConfig::root is expected to already reflect server/location merging.
	*/
	std::string makeFilesystemPath(const EffectiveConfig& cfg, const std::string& uriPath) {

		const std::string locationPrefix = (cfg.location ? cfg.location->path : "");	// ex: "/images" (or "")
		std::string uriRemainder;

		if (!locationPrefix.empty() &&											// If there is a matched location, strip its prefix from the URI
			uriPath.compare(0, locationPrefix.size(), locationPrefix) == 0)
			uriRemainder = uriPath.substr(locationPrefix.size());				// ex: "/images/logo.png" -> "/logo.png"
		else
			uriRemainder = uriPath;

		if (uriRemainder.empty())												// Ensure we always join root + "/something"
			uriRemainder = "/";

		if (!cfg.root.empty() && cfg.root[cfg.root.size() - 1] == '/' &&		// Join root + uriRemainder with exactly one '/'
			!uriRemainder.empty() && uriRemainder[0] == '/')
			return cfg.root.substr(0, cfg.root.size() - 1) + uriRemainder;

		if (!uriRemainder.empty() && uriRemainder[0] == '/')
			return cfg.root + uriRemainder;

		return cfg.root + "/" + uriRemainder;
	}


	/*
	 Validates and canonicalizes a filesystem path relative to the given root.
	 Rejects any traversal outside root and rebuilds a normalized absolute path.
	*/
	bool normalizePath(std::string& fsPath, const std::string& root) {

		if (fsPath.compare(0, root.size(), root) != 0)				// Reject paths that do not start inside the configured root
			return false;

		std::string relative = fsPath.substr(root.size());			// Extract the portion after the root to normalize it

		if (!relative.empty() && relative[0] == '/')				// Remove leading slash from the relative segment (if present)
			relative.erase(0, 1);

		std::vector<std::string> stack;
		std::string token;

		for (size_t i = 0; i <= relative.size(); ++i) {				// Split by '/', interpreting '.' and '..' like a real filesystem
			char c = (i < relative.size()) ? relative[i] : '/';		// Use a fake '/' at the end to ensure the last token is processed
			if (c == '/') {
				if (token == "..") {
					if (stack.empty())
						return false;								// Attempt to escape the root directory
					else
						stack.pop_back();							// Go one level up
				} else if (!token.empty() && token != ".")
					stack.push_back(token);							// Normal directory component
				token.clear();
			} else
				token += c;
		}

		fsPath = root;												// Rebuild normalized absolute path inside root

		if (!stack.empty()) {
			for (size_t i = 0; i < stack.size(); ++i)
				fsPath += '/' + stack[i]; 
		}
	
		return true;
	}


	// ----------------------------------
	// --- 8. Request classification ----
	// ----------------------------------

	/*
	 High-level classification of how a request should be handled by the server.
	*/
	enum RequestKind {
		RK_STATIC_FILE,
		RK_DIRECTORY,
		RK_CGI,
		RK_UPLOAD,
		RK_NOT_FOUND,
		RK_FORBIDDEN
	};

	bool isCgiRequest(const EffectiveConfig& cfg, const std::string& path);

	/*
	 Classifies the request as CGI, upload, directory, static file, forbidden,
	 or not found based on request properties, effective configuration,
	 and filesystem state when applicable.
	*/
	RequestKind classifyRequest(const EffectiveConfig& cfg, const std::string& path, const std::string& fsPath, const HTTP_Request& req) {
		
		if (isCgiRequest(cfg, path))							// CGI requests are classified first, independently of filesystem state
			return RK_CGI;

		if (req.method == "POST" && !cfg.uploadStore.empty())	// Upload handling applies to POST requests targeting an upload_store
			return RK_UPLOAD;

		struct stat st;											// Remaining cases rely on filesystem inspection
		if (stat(fsPath.c_str(), &st) != 0) {
			if (errno == EACCES)
				return RK_FORBIDDEN;		// Permission denied by the filesystem
			return RK_NOT_FOUND;			// Any other stat failure - treat as missing
		}

		if (S_ISDIR(st.st_mode))
			return RK_DIRECTORY;			// Directory path

		if (S_ISREG(st.st_mode))
			return RK_STATIC_FILE;			// Regular file - serve as static content

		return RK_FORBIDDEN;				// Other filesystem objects are not allowed (symlinks, devices, etc..)
	}


	// ----------------------
	// --- 9. Static file ---
	// ----------------------

	std::string getMimeType(const std::string& extension);
	std::string getReasonPhrase(int status);

	/*
	 Converts a value to a string using stream insertion.
	*/
	template<typename T>
	std::string toString(T value) {
		std::ostringstream oss;
		oss << value;
		return oss.str();
	}

	/*
	 Serves a static file by reading it into memory and returning a 200 response.
	 Applies basic MIME detection, handles 403/404 errors, and sets Content-Length.
	*/
	HTTP_Response handleStaticFile(const HTTP_Request& req, const EffectiveConfig& cfg, const std::string& fsPath) {

		(void)req;														// For now Only GET is supported for static file.
		
		std::ifstream staticFile(fsPath.c_str(), std::ios::binary);
		if (!staticFile) {
			if (errno == EACCES)
				return makeErrorResponse(403, &cfg);					// "Forbidden"
			else
				return makeErrorResponse(404, &cfg);					// "Not Found"
		}
		
		HTTP_Response res;

		res.status = 200;
		res.reason = getReasonPhrase(200);

		std::string::size_type dotPos = fsPath.rfind('.');
		if (dotPos != std::string::npos && dotPos + 1 < fsPath.size())
			res.headers["Content-Type"] = getMimeType(fsPath.substr(dotPos + 1));
		else
			res.headers["Content-Type"] = "application/octet-stream";
		
		std::ostringstream oss;
		oss << staticFile.rdbuf();
		if (!staticFile && !staticFile.eof())							// Failbit is OK at EOF; otherwise it's a real read error
			return makeErrorResponse(500, &cfg);
		res.body = oss.str();
		
		res.headers["Content-Length"] = toString(res.body.size());

		return res;
	}


	// -------------------------
	// --- 9.1 DELETE method ---
	// -------------------------

	/*
	 Deletes a file on disk using the standard C library remove().
	 Returns 0 on success, or an HTTP-style status code (404/403/500) on failure.
	*/
	int deleteFile(const std::string& path) {

		if (std::remove(path.c_str()) == 0)
			return 0;			// Success

		switch (errno) {		// Map common errno values to HTTP-style status codes.
			case ENOENT:
				return 404;		// File no longer exists
			case EACCES:
			case EPERM:
				return 403;		// Permission denied
			default:
				return 500;		// Any other error - internal problem
		}
	}

	/*
	 Handles HTTP DELETE for a static file. Relies on the standard library to
	 remove the target from disk and returns 204 on success.
	*/
	HTTP_Response handleDeleteRequest(const EffectiveConfig& cfg, const std::string& fsPath) {

		int status = deleteFile(fsPath);
		if (status != 0)
			return makeErrorResponse(status, &cfg);

		HTTP_Response res;
		
		res.status = 204;
		res.reason = getReasonPhrase(204);
		res.body.clear();
		res.headers["content-length"] = "0";
		res.headers["content-type"] = "text/plain";

		return res;
	}


	// -----------------------------------------
	// --- 10. Directory / index / autoindex ---
	// -----------------------------------------

	// --- 10.1. Directory/Index Utilities (Helper Functions) ---

	/*
	 Escapes special HTML characters in a string, producing a safe HTML-encoded output.
	 Converts &, <, >, " and ' into their corresponding HTML entities.
	 Prevents HTML injection and mitigates XSS risks.
	*/
	std::string htmlEscape(const std::string& in) {

		std::string out;
		out.reserve(in.size() * 2); // small optimization

		for (std::string::const_iterator it = in.begin(); it != in.end(); ++it) {

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

	/*
	 Concatenates two path components, inserting '/' only if needed.
	*/
	std::string joinPath(const std::string& directory, const std::string& entry) {
		if (!directory.empty() && directory[directory.size() - 1] == '/')
			return directory + entry;
		return directory + "/" + entry;
	}

	/*
	 Generates a simple HTML autoindex page for a directory, listing files and
	 subdirectories based on the request path and filesystem path.
	*/
	std::string generateAutoIndexPage(const std::string& reqPath, const std::string& fsPath) {

		std::string fixedReqPath = reqPath;
		if (!fixedReqPath.empty() && fixedReqPath[fixedReqPath.size() - 1] != '/')
			fixedReqPath += '/';										// Ensure trailing slash

		DIR* currentDir = opendir(fsPath.c_str());
		if (!currentDir)
			return "";														// Signals an error, handled by the caller

		std::ostringstream oss;

		oss << "<!DOCTYPE html>\n"
			<< "<html><head><meta charset=\"utf-8\">"
			<< "<title>Index of " << htmlEscape(fixedReqPath) << "</title>"
			<< "</head><body>"
			<< "<h1>Index of " << htmlEscape(fixedReqPath) << "</h1>"
			<< "<ul>";

		if (fixedReqPath != "/")
			oss << "<li><a href=\"../\">Parent directory</a></li>";			// Navigation link

		struct dirent* entry;

		while ((entry = readdir(currentDir)) != NULL) {

			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 || entry->d_name[0] == '.')		// Skip hidden/system entries
				continue;

			std::string fullPath = joinPath(fsPath, entry->d_name);

			struct stat st;
			if (stat(fullPath.c_str(), &st) != 0)							// Skip unreadable entries
				continue;

			std::string escapedName = htmlEscape(entry->d_name);
			if (S_ISREG(st.st_mode))
				oss << "<li><a href=\"" << escapedName << "\">" << escapedName << "</a></li>";			// File entry
			else if (S_ISDIR(st.st_mode))
				oss << "<li><a href=\"" << escapedName << "/\">" << escapedName << "/</a></li>";		// Directory entry
		}
		
		closedir(currentDir);												// Done listing directory

		oss << "</ul></body></html>\n";
	
		return oss.str();													// Return final HTML page
	}

	// --- 10.2. Directory Request Handler ---
	
	/*
	 Handles a request targeting a directory: tries index files first, then
	 generates an autoindex page if enabled, or returns 403 otherwise.
	*/
	HTTP_Response handleDirectoryRequest(const HTTP_Request& req, const EffectiveConfig& cfg, const std::string& fsPath, const std::string& reqPath) {

		for (size_t i = 0; i < cfg.indexFiles.size(); ++i) {							// Try each configured index file in order

			std::string indexFileFsPath = joinPath(fsPath, cfg.indexFiles[i]);

			struct stat st;
			if (stat(indexFileFsPath.c_str(), &st) == 0 && S_ISREG(st.st_mode))			// Serve the first existing regular index file
				return handleStaticFile(req, cfg, indexFileFsPath);
		}

		if (cfg.autoindex) {

			DIR* testDir = opendir(fsPath.c_str());						// Checks whether the directory is accessible
			if (!testDir) {
				if (errno == EACCES)
					return makeErrorResponse(403, &cfg);				// Directory exists but is not readable
				else
					return makeErrorResponse(500, &cfg);				// Unexpected filesystem error
			}
			closedir(testDir);											// Directory is accessible

			std::string html = generateAutoIndexPage(reqPath, fsPath);

			if (html.empty())
				return makeErrorResponse(500, &cfg);					// Failed to build autoindex HTML

			HTTP_Response res;
			res.body = html;
			res.status = 200;
			res.reason = getReasonPhrase(200);
			res.headers["Content-Type"] = "text/html; charset=utf-8";
			res.headers["Content-Length"] = toString(res.body.size());

			return res;
		}

		return makeErrorResponse(404, &cfg);							// No index and autoindex is disabled
	}


	// ---------------
    // --- 11. CGI ---
	// ---------------

	/*
	 Determines whether the requested path should be handled as CGI based on
	 the configured cgi_pass extensions.
	*/
	bool isCgiRequest(const EffectiveConfig& cfg, const std::string& path) {

		if (cfg.cgiPass.empty())
			return false;

		std::string::size_type	pos = path.rfind('.');
		if (pos == std::string::npos)
			return false;

		std::string fileExtension = path.substr(pos);

		if (cfg.cgiPass.find(fileExtension) == cfg.cgiPass.end())
			return false;

		return true;
	}

	/*
	 Determines whether the request method is allowed for CGI execution,
	 falling back to the general allowed methods if no CGI-specific list exists.
	*/
	bool isCgiMethodAllowed(const HTTP_Request& req, const EffectiveConfig& cfg) {

		const std::vector<std::string>& effectiveMethods = !cfg.cgiAllowedMethods.empty() ? cfg.cgiAllowedMethods : cfg.allowedMethods;		// Use safe default methods when CGI-specific ones are not configured

		for (std::vector<std::string>::const_iterator it = effectiveMethods.begin(); it != effectiveMethods.end(); ++it) {
			if (*it == req.method)
				return true;
		}

		return false;
	}

	/*
	 Extracts a valid file extension from a filesystem path, ignoring dots in
	 directory names, trailing dots, and hidden files that start with a dot.
	 Returns an empty string on failure, to be handled by the caller.
	*/
	std::string getFileExtension(const std::string& fsPath) {

		std::string::size_type slashPos = fsPath.rfind('/');
		std::string::size_type dotPos = fsPath.rfind('.');

		if (dotPos == std::string::npos)
			return "";																	// No dot - no extension

		if (slashPos != std::string::npos && dotPos < slashPos)
			return "";																	// Dot is in a directory name, not in the filename

		if (dotPos == fsPath.size() - 1)												// File without extension
			return "";

		if (dotPos == 0 || (slashPos != std::string::npos && dotPos == slashPos + 1))	// Hidden file (e.g. .bashrc) - not an extension
			return "";

		return fsPath.substr(dotPos + 1);
	}

	/*
	 Resolves the CGI interpreter from cgi_pass and builds the argv vector
	 for execve execution of the CGI script.
	*/
	bool prepareCgiExecutor(const EffectiveConfig& cfg, const std::string& fsPath, std::vector<std::string>& argv) {
		
		std::string ext = getFileExtension(fsPath);
		
		if (ext.empty())
			return false;

		ext = '.' + ext;
		
		std::map<std::string, std::string>::const_iterator it = cfg.cgiPass.find(ext);
		if (it == cfg.cgiPass.end())
			return false;
		
		if (it->second.empty())
			return false;
		
		if (access(it->second.c_str(), X_OK) != 0)
			return false;

		argv.clear();
		argv.push_back(it->second);
		argv.push_back(fsPath);

		return true;
	}

	/*
	 Converts an HTTP header name into a valid CGI environment variable
	 by applying the HTTP_ prefix, uppercasing letters, replacing dashes
	 with underscores, and rejecting invalid characters.
	*/
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

	/*
	 Builds the CGI environment for a request by resolving the CGI script boundary,
	 deriving SCRIPT_NAME, PATH_INFO, and PATH_TRANSLATED, and populating standard
	 CGI variables, server metadata, and HTTP_* header mappings.
	*/
	std::vector<std::string> buildCgiEnv(const HTTP_Request& req, const EffectiveConfig& cfg, const std::string& fsPath) {
		
		std::vector<std::string>	env;
		env.reserve(64);																// Arbitrary but large enough to avoid reallocations

		// 1) Basic Request Pieces
		const std::string		requestPath = req.path.empty() ? "/" : req.path;							// requestPath: URL path without query
		std::string::size_type	qPos = req.target.find('?');
		const std::string		query = (qPos == std::string::npos) ? "" : req.target.substr(qPos + 1);		// query: without leading '?'

		// 2) nginx-style split (ex, URL: /cgi-bin/app.py/users/42 -> SCRIPT_NAME = /cgi-bin/app.py, PATH_INFO = /users/42)
		std::string			scriptName = requestPath;
		std::string			pathInfo = "";
		std::string			pathTranslated = "";

		bool	foundBoundary = false;										// Track the best matching CGI script boundary 
		size_t	bestBoundary = 0;
		size_t	bestExtLen = 0;

		for (std::map<std::string, std::string>::const_iterator it = cfg.cgiPass.begin(); it != cfg.cgiPass.end(); ++it) {		// Iterate over configured CGI extensions
			const std::string&	ext = it->first;
			if (ext.empty())
				continue;

			size_t	pos = requestPath.find(ext);																				// Find extension occurrences in the request path
			while (pos != std::string::npos)	{
				size_t	boundary = pos + ext.size();																			// Candidate boundary just after the extension

				if (boundary <= requestPath.size() && (boundary == requestPath.size() || requestPath[boundary] == '/'))	{		// Valid boundary: end of path or followed by '/'
					if (!foundBoundary || ext.size() > bestExtLen)	{															// Prefer the longest matching extension
						foundBoundary = true;
						bestExtLen = ext.size();
						bestBoundary = boundary;
					}
					break;
				}
				pos = requestPath.find(ext, pos + 1);																			// Look for the next occurrence of this extension in URL (ex., /php/test.php/foo.php/bar)
			}
		}

		if (foundBoundary) {											// If a valid CGI boundary was found, split script name and extra path
			scriptName = requestPath.substr(0, bestBoundary);			// Script path up to and including the CGI extension
			if (bestBoundary < requestPath.size())		
				pathInfo = requestPath.substr(bestBoundary);			// Remaining path after the script becomes PATH_INFO (begins with '/')
		}

		if (pathInfo.empty()) {											// Fallback
			pathInfo = requestPath;
			pathTranslated = fsPath;
		}

		if (!pathInfo.empty() && pathTranslated.empty()) {				// Translate PATH_INFO to filesystem path (only in case it hasn't been translated yet)

			bool rootEndsWithSlash = (!cfg.root.empty() && cfg.root[cfg.root.size() - 1] == '/');
			bool infoStartsWithSlash = (pathInfo[0] == '/');

			if (rootEndsWithSlash && infoStartsWithSlash)								// Both have slash - strip one
				pathTranslated = cfg.root.substr(0, cfg.root.size() - 1) + pathInfo;
			else if (rootEndsWithSlash ^ infoStartsWithSlash)							// Exclusive OR (XOR) – true if only one of the operands is true
				pathTranslated = cfg.root + pathInfo;
			else																		// None has slash - add one
				pathTranslated = cfg.root + "/" + pathInfo;
		}

		// 3) Core CGI Variables
		env.push_back("GATEWAY_INTERFACE=CGI/1.1");
		env.push_back("SERVER_SOFTWARE=webserv");
		env.push_back("SERVER_PROTOCOL=" + req.version);
		env.push_back("REQUEST_METHOD=" + req.method);
		env.push_back("REQUEST_URI=" + req.target);
		env.push_back("QUERY_STRING=" + query);
		env.push_back("SCRIPT_NAME=" + scriptName);
		env.push_back("SCRIPT_FILENAME=" + fsPath);
		env.push_back("PATH_INFO=" + pathInfo);
		env.push_back("PATH_TRANSLATED=" + pathTranslated);
		env.push_back("DOCUMENT_ROOT=" + cfg.root);
		env.push_back("CONTENT_LENGTH=" + toString(req.content_length));				// Content length should be present even if its 0.
		std::map<std::string, std::string>::const_iterator itCT = req.headers.find("content-type");
		if (itCT != req.headers.end())
			env.push_back("CONTENT_TYPE=" + itCT->second);
																						// SERVER_NAME / SERVER_PORT
		std::string	serverPort;
		size_t		serverColonPos = req.host.find(':');
		if (serverColonPos != std::string::npos)
			serverPort = req.host.substr(serverColonPos + 1);							// Prefer port explicitly given in Host header
		else if (!cfg.server->listen.empty()) {											// Otherwise try to extract port from first listen directive (host:port, or only port)
			const std::string&	listenDirective = cfg.server->listen[0];
			size_t				listenColonPos = listenDirective.find(':');
			serverPort = (listenColonPos != std::string::npos) ? listenDirective.substr(listenColonPos + 1) : listenDirective;
		} else
			serverPort = "80";															// Final fallback: standard HTTP port
		
		std::string	serverName = (serverColonPos == std::string::npos) ? req.host : req.host.substr(0, serverColonPos);
		if (serverName.empty())
			serverName = "localhost";													// Reasonable fallback for missing Host header

		env.push_back("SERVER_PORT=" + serverPort);
		env.push_back("SERVER_NAME=" + serverName);
		env.push_back("REMOTE_ADDR=127.0.0.1");											// Default loopback address when real client IP isn't available (common in NGINX/Apache local setups)

		for (std::map<std::string, std::string>::const_iterator it = req.headers.begin(); it != req.headers.end(); ++it)	{

			if (it->first == "content-type" || it->first == "content-length")
				continue;

			std::string	envHeader = formatCgiEnvHeader(it->first);
			if (!envHeader.empty())
				env.push_back(envHeader + "=" + it->second);
		}

		env.push_back("REDIRECT_STATUS=200");

		return env;
	}
	
	/*
	 Internal structure holding the pipe file descriptors used by the CGI process.
	*/
	struct CgiPipes {
		int		stdinParent;		// parent writes here (becomes child's STDIN)
		int		stdoutParent;  		// parent reads here (comes from child's STDOUT)
		pid_t	pid;				// PID of the forked CGI child process
	};
	
	/*
	 Result container for CGI child execution, storing its output and status.
	*/
	struct CgiRawOutput {
		bool        timedOut;     // true if reading from the child exceeded the timeout
		int         exitStatus;   // Exit code of the CGI process (or -1 if unavailable)
		std::string data;         // everything read from the child's STDOUT

		CgiRawOutput()
			: timedOut(false)
			, exitStatus(-1)
			, data()
		{}
	};

	/*
	 Spawns a CGI child process, sets up pipes for its STDIN and STDOUT,
	 and prepares argv/envp before calling execve in the child. On success,
	 returns the child's PID and the file descriptors the parent uses to
	 write to the process and read from it.
	*/
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
				const char msg[] = "CGI: dup2() failed\n";
				write(2, msg, sizeof(msg) - 1);						// After fork, use write() instead of std::cerr to avoid inherited buffer/lock issues
				_exit(1);
			}

			close(pipeStdin[0]); close(pipeStdin[1]); close(pipeStdout[0]);	close(pipeStdout[1]);

			std::vector<char*> cArgv;								// Prepare  C-style argv in execve-compatible (NULL-terminated) format
			cArgv.reserve(argv.size() + 1);
			for (size_t i = 0; i < argv.size(); ++i)
				cArgv.push_back(const_cast<char*>(argv[i].c_str()));
			cArgv.push_back(NULL);

			std::vector<char*> cEnvp;								// The same for envp (C-style)
			cEnvp.reserve(env.size() + 1);
			for (size_t i = 0; i < env.size(); ++i)
				cEnvp.push_back(const_cast<char*>(env[i].c_str()));
			cEnvp.push_back(NULL);

			execve(cArgv[0], &cArgv[0], &cEnvp[0]);
			
			const char msg[] = "CGI: execve() failed\n";			// only reached if execve fails
			write(2, msg, sizeof(msg) - 1);
			_exit(1);
		}

		close(pipeStdin[0]); close(pipeStdout[1]);

		pipes.stdinParent = pipeStdin[1];							// Parent will write request body here
		pipes.stdoutParent = pipeStdout[0];							// Parent will read CGI output here
		pipes.pid = pid;

		int	flags;

		flags = fcntl(pipes.stdinParent, F_GETFL, 0);					// Make parent-side CGI pipes non-blocking
		if (flags != -1)
			fcntl(pipes.stdinParent, F_SETFL, flags | O_NONBLOCK);

		flags = fcntl(pipes.stdoutParent, F_GETFL, 0);
		if (flags != -1)
			fcntl(pipes.stdoutParent, F_SETFL, flags | O_NONBLOCK);

		return true;
	}

	/*
	 Streams reqBody to the CGI child's stdin while draining its stdout using poll() slices;
	 enforces an inactivity timeout, closes stdin to signal EOF, and returns collected stdout + exit status.
	*/
	CgiRawOutput pumpCgiPipes(const CgiPipes& pipes, std::size_t timeoutSeconds, const std::string& reqBody) {

		CgiRawOutput	cgiOutput;

		// Inactivity timeout implemented via repeated short poll() slices
		const int		timeoutMs = static_cast<int>(timeoutSeconds * 1000);
		const int		sliceMs = 200;						// max wait time per poll() call
		int				idleMs = 0;							// time spent with no I/O activity

		size_t			written = 0;						// how many bytes have already been sent to the CGI

		bool			stdinClosed = false;				// parent closed CGI stdin (EOF sent)
		bool			eof = false;						// reached EOF on CGI stdout

		if (reqBody.size() == 0) {							// no body: close stdin immediately so CGI sees EOF
			stdinClosed = true;
			close(pipes.stdinParent);
		}

		while (!eof && !cgiOutput.timedOut){
			
			struct pollfd	pfds[2];						// stdout always; stdin only while open
			int				nfds = 0;						// number of fds passed to poll()

			pfds[nfds].fd = pipes.stdoutParent;						// stdout: always poll until EOF
			pfds[nfds].events = POLLIN | POLLHUP | POLLERR;
			pfds[nfds].revents = 0;
			nfds++;

			if (!stdinClosed)	{									// stdin: poll while we still have body to send (or until we close)
				pfds[nfds].fd = pipes.stdinParent;
				pfds[nfds].events = POLLOUT | POLLHUP | POLLERR ;
				pfds[nfds].revents = 0;
				nfds++;
			}

			int	pollResult = poll(pfds, nfds, sliceMs);

			if (pollResult < 0)	{
				if (errno == EINTR)						// interrupted by signal: retry
					continue;
				eof = true;								// poll failed: stop I/O loop
				break;
			}

			if (pollResult == 0)	{					// slice timed out: no events occurred
				idleMs += sliceMs;
				if (idleMs >= timeoutMs) {
					cgiOutput.timedOut = true;			// inactivity timeout reached
					break;
				}
				continue;
			}

			bool	progressed = false;							// any I/O progress in this iteration?
			
			// 1) Read CGI stdout (fd index 0)
			{
				if (pfds[0].revents & POLLERR)											// Fatal error on the file descriptor - no further reads are possible
					eof = true;
				
				if (pfds[0].revents & (POLLIN | POLLHUP))	{
					char	buf[4096];													// reasonable chunk size for pipe reads
					for (;;)	{
						
						ssize_t	n = read(pipes.stdoutParent, buf, sizeof(buf));			// signed size type (may be -1 on error)
						
						if (n > 0)	{
							cgiOutput.data.append(buf, static_cast<size_t>(n));
							progressed = true;
							continue;
						}
						
						if (n == 0)	{
							eof = true;
						}

						break;															// no more data to read for now (case, n = -1)
					}

					if ((pfds[0].revents & POLLHUP) && !(pfds[0].revents & POLLIN))		// hangup with no pending data
						eof = true;
				}
			}

			// 2) Write CGI stdin (fd index 1, only if present)
			if (!stdinClosed)	{

				if (pfds[1].revents & (POLLERR | POLLHUP))	{							// Child closed stdin or error
					stdinClosed = true;
					close(pipes.stdinParent);
				}
				else if (pfds[1].revents & POLLOUT)	{									// stdin writable: can send more data to CGI stdin
					while (written < reqBody.size())	{
						
						ssize_t	n = write(pipes.stdinParent, reqBody.data() + written, reqBody.size() - written);

						if (n > 0)	{
							written += static_cast<size_t>(n);
							progressed = true;
							continue;
						}

						break;
					}

					if (!stdinClosed && written == reqBody.size())	{
						stdinClosed = true;
						close(pipes.stdinParent);							// close parent's write end (send EOF to CGI stdin)
					}
				}
			}

			if (progressed)													// Reset the idle timeout when progress occurs
				idleMs = 0;
		}

		if (cgiOutput.timedOut)	{											// timeout: kill CGI
			kill(pipes.pid, SIGKILL);
			waitpid(pipes.pid, NULL, 0);									// avoid zombie
			close(pipes.stdoutParent);
			return cgiOutput;
		}

		int	childStatus = 0;												// Retrieve exit status from CGI
		if (waitpid(pipes.pid, &childStatus, 0) > 0)	{
			if (WIFEXITED(childStatus))
				cgiOutput.exitStatus = WEXITSTATUS(childStatus);
			else if (WIFSIGNALED(childStatus))
				cgiOutput.exitStatus = 128 + WTERMSIG(childStatus);
		}

		close(pipes.stdoutParent);											// done with CGI stdout pipe

		return cgiOutput;
	}

	/*
	 Container for parsed CGI output, storing status, headers, body and a flag
	 indicating whether the header section was syntactically valid.
	*/
	struct CgiParsedOutput {
		int									status;			// 0 in case of missing status:
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

	/*
	 Removes leading spaces, tabs, and CR/LF characters from a string.
	*/
	std::string trimLeft(const std::string& s)  {
		std::size_t i = 0;
		while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
			++i;
		return s.substr(i);
	}

	/*
	 Removes trailing spaces, tabs, and CR/LF characters from a string.
	*/
	std::string trimRight(const std::string& s) {
		if (s.empty())
			return s;
		std::size_t i = s.size();
		while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t' || s[i - 1] == '\r' || s[i - 1] == '\n'))
			--i;
		return s.substr(0, i);
	}

	/*
	 Strips leading and trailing ASCII whitespace using trimLeft/trimRight.
	*/
	std::string trim(const std::string& s)  {
		return trimRight(trimLeft(s));
	}

	/*
	 Returns a lowercase ASCII copy of the input string.
	*/
	std::string toLowerCopy(const std::string& s)   {
		std::string out(s);											// copy constructor
		for (std::size_t i = 0; i < out.size(); ++i)    {
			unsigned char   c = static_cast<unsigned char>(out[i]);
			out[i] = static_cast<char>(std::tolower(c));
		}
		return out;
	}

	/*
	 Parses a single "Header-Name: value" line into the headers map.
	 Skips empty or malformed lines (no ':').
	*/
	void parseCgiHeaderLine(const std::string& line, std::map<std::string, std::string>& headers)	{

		if (line.empty())
			return;

		std::string::size_type colon = line.find(':');
		if (colon == std::string::npos)
			return;											// Skip malformed header line (no colon)

		std::string name  = trim(line.substr(0, colon));
		std::string value = trim(line.substr(colon + 1));

		if (!name.empty())
			headers[toLowerCopy(name)] = value;
	}

	/*
	 Parses raw CGI output into headers and body, extracting an optional
	 Status header into status/reason and marking the header block as valid
	 or invalid according to basic CGI rules.
	*/
	CgiParsedOutput parseCgiOutput(const std::string& raw)	{

		CgiParsedOutput			parsedOutput;

		std::string 			remainingHeaders;
		std::string::size_type	pos;
		std::string				lineDelim;
		std::string::size_type	headerEnd = std::string::npos;
		std::string::size_type	bodyStart = std::string::npos;

		// 1) Find header/body separator
		pos = raw.find("\r\n\r\n");											// Prefer strict CGI delimiter: CRLF CRLF
		if (pos != std::string::npos) {
			headerEnd = pos;
			bodyStart = pos + 4;											// length of "\r\n\r\n"
			lineDelim = "\r\n";
		}
		else {																// Fallback: accept LF LF as well (some scripts use "\n\n")
			pos = raw.find("\n\n");
			if (pos == std::string::npos) {									// Missing CRLFCRLF or LFLF: breaks CGI header rules (RFC3875) and should trigger 502.
				parsedOutput.headersValid = false;
				parsedOutput.body = raw;
				return parsedOutput;
			}
			headerEnd = pos;
			bodyStart = pos + 2;											// length of "\n\n"
			lineDelim = "\n";
		}

		parsedOutput.headersValid = true;

		remainingHeaders = raw.substr(0, headerEnd);
		parsedOutput.body = raw.substr(bodyStart);

		// 2) Parse each header line
		for (;;) {
			pos = remainingHeaders.find(lineDelim);
			if (pos == std::string::npos)
				break;

			std::string currentHeader = remainingHeaders.substr(0, pos);
			remainingHeaders = remainingHeaders.substr(pos + lineDelim.size());
		
			parseCgiHeaderLine(currentHeader, parsedOutput.headers);
		}
		if (!remainingHeaders.empty())										// Handle final header line if there's remaining text with no trailing delimiter
			parseCgiHeaderLine(remainingHeaders, parsedOutput.headers);
	
		// 3) "Status:" header handling
		std::map<std::string, std::string>::iterator it = parsedOutput.headers.find("status");
		if (it != parsedOutput.headers.end())	{
			pos = it->second.find(' ');
			if (pos != std::string::npos) {
				parsedOutput.status = std::atoi(it->second.substr(0, pos).c_str());
				parsedOutput.reason = trim(it->second.substr(pos + 1));
			}
			else {
				parsedOutput.status = std::atoi(it->second.c_str());
				parsedOutput.reason.clear();
			}
			parsedOutput.headers.erase(it);					// Status header was processed separately, so remove it from the general header map
		}

		return parsedOutput;
	}

	/*
	 Builds an HTTP response from parsed CGI output, applying CGI rules for
	 Status and Location, copying safe headers, recomputing Content-Length,
	 and preserving the CGI body as-is.
	*/
	HTTP_Response buildCgiHttpResponse(const CgiParsedOutput& out) {

		HTTP_Response res;
		std::map<std::string, std::string>::const_iterator it;

		if (out.status != 0) {												// CGI provided an explicit "Status:" header - use it
			res.status = out.status;
			if (out.reason.empty())
				res.reason = getReasonPhrase(out.status);
			else
				res.reason = out.reason;
		} else {															// No "Status:" header - apply CGI fallback rules
			it = out.headers.find("location");
			if (it != out.headers.end()) {									// Classic CGI implicit redirect: a Location header without Status implies 302 Found (old CGI scripts)
				res.status = 302;
				res.reason = getReasonPhrase(res.status);
			} else {														// Normal case: successful response without explicit status
				res.status = 200;
				res.reason = getReasonPhrase(res.status);
			}
		}

		for (it = out.headers.begin(); it != out.headers.end(); ++it) {		// Copy CGI headers, skipping those managed by the core
			if (it->first == "content-length" || it->first == "connection" || it->first == "transfer-encoding")
				continue;
			res.headers[it->first] = it->second;
		}

		res.headers["content-length"] = toString(out.body.size());			// Always recompute Content-Length - safer than trusting a value provided by the CGI script

		res.body = out.body;												// Copy the body exactly as produced by the CGI
		res.close = false;													// Do not decide connection closing here - final keep-alive/close behaviour will be applied later

		return res;
	}
	
	/*
	 Handles execution of a CGI script: validates the target file and method,
	 spawns the CGI process, streams the request body, reads its output with
	 a timeout, validates CGI headers, and converts the result into an HTTP
	 response or an appropriate 4xx/5xx error.
	*/
	HTTP_Response handleCgiRequest(const HTTP_Request& req, const EffectiveConfig& cfg, const std::string& fsPath) {
		
		if (!isCgiMethodAllowed(req, cfg))
			return makeErrorResponse(405, &cfg);

		std::vector<std::string> argv;
		if (!prepareCgiExecutor(cfg, fsPath, argv))
			return makeErrorResponse(500, &cfg);

		std::vector<std::string> envp = buildCgiEnv(req, cfg, fsPath);

		CgiPipes pipes;
		if (!spawnCgiProcess(argv, envp, pipes))
			return makeErrorResponse(500, &cfg);
		
		CgiRawOutput raw = pumpCgiPipes(pipes, cfg.cgiTimeout, req.body);

		if (raw.timedOut)
			return makeErrorResponse(504, &cfg);

		if (raw.exitStatus == -1 || (raw.exitStatus != 0 && raw.data.empty())) {	// If the CGI failed or produced no output, try to map the error

			if (access(fsPath.c_str(), F_OK) != 0) {
				if (errno == ENOENT)
					return makeErrorResponse(404, &cfg);
				if (errno == EACCES)
					return makeErrorResponse(403, &cfg);
			}
			return makeErrorResponse(500, &cfg);
		}

		CgiParsedOutput parsedOutput = parseCgiOutput(raw.data);

		if (!parsedOutput.headersValid)
			return makeErrorResponse(500, &cfg);

		std::map<std::string, std::string>::const_iterator itContentType = parsedOutput.headers.find("content-type");
		std::map<std::string, std::string>::const_iterator itLocation = parsedOutput.headers.find("location");
		if (itContentType == parsedOutput.headers.end() && itLocation == parsedOutput.headers.end())
			return makeErrorResponse(500, &cfg);

		HTTP_Response res = buildCgiHttpResponse(parsedOutput);

		if (!req.keep_alive)
			res.close = true;
		
		return res;
	}


	// -------------------
	// --- 12. Uploads ---
	// -------------------

	// --- 12.1. Upload Utilities (Helper Functions) ---

	/*
	 Validates an upload filename to ensure it is safe: rejects empty names,
	 '.', '..', control characters, path separators, and problematic symbols.
	*/
	bool isSanitizedFilename(const std::string& filename) {

		if (filename.empty())
			return false;

		if (filename == "." || filename == "..")			// Prevent directory traversal via current/parent directory names
			return false;

		for (std::string::const_iterator it = filename.begin(); it != filename.end(); ++it) {

			unsigned char c = static_cast<unsigned char>(*it);

			if (c < 32)
				return false;

			switch (c) {

				case '/':
				case '\\':
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

	/*
	 Detects whether the request uses multipart/form-data based on the Content-Type header.
	*/	
	bool isMultipart(const HTTP_Request& req) {

		std::map<std::string, std::string>::const_iterator it =	req.headers.find("content-type");

		return (it != req.headers.end() && it->second.find("multipart/form-data") != std::string::npos);
	}

	/*
	 Extracts the filename component after the last '/' in a path.
	 Returns an empty string if no '/' is found.
	*/
	std::string extractFilename(const std::string& path) {

		std::string::size_type pos = path.rfind('/');
		
		if (pos == std::string::npos)
			return "";
		
		return path.substr(pos + 1);
	}

	/*
	 Checks whether the given path exists and is a directory for uploads.
	*/
	bool isValidUploadDirectory(const std::string& dir) {

		struct stat st;

		return (stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
	}

	/*
	 Checks whether an upload target already exists and returns an HTTP status:
	 0 if it can be created, 403 for directories, 409 for conflicts, 500 on errors.
	*/
	int getExistingTargetStatus(const std::string& path) {

		struct stat st;

		if (stat(path.c_str(), &st) == 0) {		// Path exists: decide what it is
			if (S_ISDIR(st.st_mode))			// Directory - cannot overwrite a directory with a file
				return 403;
			return 409;							// Regular file (or similar) - conflict, do not overwrite
		}

		if (errno == ENOENT)					// Path does not exist - OK to create
			return 0;

		return 500;								// Any other stat error - internal server error
	}

	/*
	 Writes the upload body to the given path. Returns 0 on success or an
	 HTTP-style status code (403/500) on failure.
	*/
	int writeUploadedFile(const std::string& path, const std::string& body) {

		std::ofstream file(path.c_str(), std::ios::binary);
		if (file.fail()) {
			if (errno == EACCES)
				return 403;
			return 500;
		}

		file.write(body.c_str(), body.size());
		if (file.fail()) {
			file.close();
			std::remove(path.c_str());
			return 500;
		}

		file.close();

		return 0;									// Success
	}

	/*
	 Builds a 201 Created response with a Location header pointing to the target.
	*/
	HTTP_Response makeResponse201(const std::string& target) {

		HTTP_Response res;

		res.status = 201;
		res.reason = getReasonPhrase(201);
		res.headers["Location"] = target;
		res.headers["Content-Type"] = "text/plain";
		res.headers["Content-Length"] = "0";
		res.body.clear();

		return res;
	}

	// --- 12.2. Upload Request Handler ---

	/*
	 Handles a simple upload request: validates the filename and upload directory,
	 checks for conflicts, writes the file, and returns 201 on success.
	*/
	HTTP_Response handleUploadRequest(const HTTP_Request& req, const EffectiveConfig& cfg) {

		if (isMultipart(req))                                           // Allow only simple uploads (reject multipart/form-data - requires boundary parsing)
			return makeErrorResponse(501, &cfg);

		std::string filename = extractFilename(req.path);
		if (filename.empty() || !isSanitizedFilename(filename))         // Filename validation
			return makeErrorResponse(400, &cfg);

		std::string dest = joinPath(cfg.uploadStore, filename);

		if (!isValidUploadDirectory(cfg.uploadStore))
			return makeErrorResponse(500, &cfg);

		int status = getExistingTargetStatus(dest);
		if (status != 0)
			return makeErrorResponse(status, &cfg);

		int writeStatus = writeUploadedFile(dest, req.body);
		if (writeStatus != 0)
			return makeErrorResponse(writeStatus, &cfg);

		return makeResponse201(req.target);
	}


	// -----------------------------------------
	// --- 13. Error pages: custom / generic ---
	// -----------------------------------------

	/*
	Resolves error_page configured value into a filesystem path.

	Rule:
	- If the configured value starts with '/', treat it as a URI under cfg.root.
	- Otherwise, treat it as a filesystem path relative to the current working directory.
	*/
	std::string findErrorPagePath(const EffectiveConfig& cfg, int status) {

		std::map<int, std::string>::const_iterator it = cfg.errorPages.find(status);
		if (it == cfg.errorPages.end())
			return "";

		std::string path = it->second;
		if (path.empty())
			return "";

		if (path[0] == '/') {						// URI under server root (not filesystem absolute) (nginx-like)
			return joinPath(cfg.root, path);
		}

		return path;
	}

	/*
	 Returns the standard HTTP reason phrase for a given status code, or
	 "Unknown Status" if not recognized.
	*/
	std::string getReasonPhrase(int status) {

		switch (status) {

			case 100: return "Continue";							// 1xx — Informational
			case 101: return "Switching Protocols";
			case 102: return "Processing";

			case 200: return "OK";									// 2xx — Success
			case 201: return "Created";
			case 202: return "Accepted";
			case 204: return "No Content";
			case 206: return "Partial Content";

			case 301: return "Moved Permanently";					// 3xx — Redirection
			case 302: return "Found";
			case 303: return "See Other";
			case 304: return "Not Modified";
			case 307: return "Temporary Redirect";
			case 308: return "Permanent Redirect";

			case 400: return "Bad Request";							// 4xx — Client errors
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

			case 500: return "Internal Server Error";				// 5xx — Server errors
			case 501: return "Not Implemented";
			case 502: return "Bad Gateway";
			case 503: return "Service Unavailable";
			case 504: return "Gateway Timeout";
			case 505: return "HTTP Version Not Supported";

			default:  return "Unknown Status";
		}
	}

	/*
	 Builds an HTTP error response for the given status code. Uses a configured
	 custom error_page if available; otherwise falls back to a simple HTML body.
	*/
	HTTP_Response makeErrorResponse(int status, const EffectiveConfig* cfg) {
		
		std::string	body;
		const std::string reason = getReasonPhrase(status);
		
		if (cfg) {																// Try loading a custom error_page
			const std::string errorPagePath = findErrorPagePath(*cfg, status);
			if (!errorPagePath.empty()) {
				std::ifstream file(errorPagePath.c_str(), std::ios::binary);
				if (file) {
					std::ostringstream oss;
					oss << file.rdbuf();
					body = oss.str();
				}
			}
		}
		
		if (body.empty()) {												// Otherwise, generate default HTML (if no error_page or if reading fails)
			std::ostringstream oss;
			oss	<< "<!DOCTYPE html>\n"
				<< "<html><head><meta charset=\"utf-8\">"
				<< "<title>" << status << ' ' << reason << "</title>"
				<< "</head><body>"
				<< "<h1>" << status << ' ' << reason << "</h1>"
				<< "</body></html>\n";
			body = oss.str();
		}
		
		HTTP_Response res;

		res.status = status;													// Fill in the response fields
		res.reason = reason;
		res.body = body;
		res.headers.clear();
		res.headers["Content-Type"] = "text/html";
		res.headers["Content-Length"] = toString(body.size());

		return res;
	}


	// ---------------------------------
	// --- 14. Redirection responses ---
	// ---------------------------------

	/*
	 Builds a 3xx redirect response with Location and a small HTML body.
	 Falls back to 302 if the provided status is not in the 3xx range.
	*/
	HTTP_Response makeRedirectResponse(int status, const std::string& location) {

		if (status < 300 || status > 399)
			status = 302;								// Standard fallback redirect (safe and widely supported)
		
		HTTP_Response res;

		res.status = status;
		res.reason = getReasonPhrase(res.status);
		res.headers["location"] = location;
		res.headers["content-type"] = "text/html";

		std::ostringstream body;

		body << "<!DOCTYPE html>\n"
			<< "<html><head><meta charset=\"utf-8\">"
			<< "<title>" << res.status << ' ' << res.reason << "</title></head>"
			<< "<body><h1>" << res.status << ' ' << res.reason << "</h1>"
			<< "<p>Resource moved to <a href=\"" << htmlEscape(location) << "\">" << htmlEscape(location) << "</a></p>"
			<< "</body></html>";

		res.body = body.str();
		res.headers["content-length"] = toString(res.body.size());

		return res;
	}
	

	// ----------------------
	// --- 15. MIME types ---
	// ----------------------

	/*
	 Returns the MIME type corresponding to a file extension, normalized to
	 lowercase, or "application/octet-stream" if the extension is unknown.
	*/
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

		return "application/octet-stream";					// Standard fallback MIME type for unknown files (RFC 2046)
	}


	// -----------------------------------
	// --- 16. Connection / keep-alive ---
	// -----------------------------------

	/*
	 Sets the Connection header and marks the response to keep the socket open
	 or close it based on the client's keep-alive preference.
	*/
	void applyConnectionHeader(bool keepAlive, HTTP_Response& res) {

		res.headers.erase("Connection");				// patch to avoid duplicates
		res.headers.erase("connection");

		if (keepAlive) {
			res.close = false;
			res.headers["connection"] = "keep-alive";
		} else {
			res.close = true;
			res.headers["connection"] = "close";
		}
	}


	// ----------------------------------------
	// --- 17. Response post-processing     ---
	// ----------------------------------------

	/*
	 Convert a header name to HTTP Title-Case format
	*/
	static std::string toHeaderCase(const std::string& key) {
	
		std::string result;
		bool uppercase_next = true;

		for (std::string::const_iterator it = key.begin(); it != key.end(); ++it) {
		
			if (*it == '-') {
				result += '-';
				uppercase_next = true;
			}
			
			else {
				if (uppercase_next)
					result += std::toupper(static_cast<unsigned char>(*it));
				else
					result += std::tolower(static_cast<unsigned char>(*it));

				uppercase_next = false;
			}
		}
		
		return result;
	}

	/*
	 Normalize HTTP response header names to Title-Case
	*/
	void headersUppercase(HTTP_Response& res) {
	
		std::map<std::string, std::string> normalized;

		for (std::map<std::string, std::string>::iterator it = res.headers.begin(); it != res.headers.end(); ++it) {
			
			normalized[toHeaderCase(it->first)] = it->second;
		}

		res.headers.swap(normalized);
	}
}

/*
 Main application dispatcher: parses the request target, selects the server
 and location, builds the effective configuration, enforces method/body
 rules, maps the path to the filesystem or CGI, and delegates to the
 appropriate handler to produce the final HTTP response.
 */
HTTP_Response handleRequest(const HTTP_Request& req, const std::vector<Server>& servers) {

	bool keepAlive = req.keep_alive;

	if (servers.empty()) {
		HTTP_Response res = makeErrorResponse(500, NULL);
		applyConnectionHeader(keepAlive, res);
		return res;
	}

	std::string path;
	std::string query;
	if (!parseTarget(req, path, query)) {
		HTTP_Response res = makeErrorResponse(400, NULL);
		applyConnectionHeader(keepAlive, res);
		return res;
	}

	const Server&   srv = selectServer(servers, req);
	const Location* loc = matchLocation(srv, path);

	EffectiveConfig cfg;
	try {
		cfg = buildEffectiveConfig(srv, loc);
	} catch (const std::exception&) {
		HTTP_Response res = makeErrorResponse(500, NULL);
		applyConnectionHeader(keepAlive, res);
		return res;
	}

	if (cfg.redirectStatus != 0) {
		HTTP_Response res = makeRedirectResponse(cfg.redirectStatus, cfg.redirectTarget);
		applyConnectionHeader(keepAlive, res);
		return res;
	}

	if (req.method != "GET" && req.method != "POST" && req.method != "DELETE" && req.method != "HEAD") {
		HTTP_Response res = makeErrorResponse(501, &cfg);
		applyConnectionHeader(keepAlive, res);
		return res;
	}

	if (!isMethodAllowed(cfg, req.method)) {
		HTTP_Response res = make405(cfg);
		applyConnectionHeader(keepAlive, res);
		return res;
	}

	int  status = 0;
	if (!checkRequestBodyAllowed(cfg, req, status)) {
		HTTP_Response res = makeErrorResponse(status, &cfg);
		applyConnectionHeader(keepAlive, res);
		return res;
	}

	std::string fsPath = makeFilesystemPath(cfg, path);

	if (fsPath.empty()) {
		HTTP_Response res = makeErrorResponse(500, &cfg);
		applyConnectionHeader(keepAlive, res);
		return res;
	}

	if (!normalizePath(fsPath, cfg.root)) {
		HTTP_Response res = makeErrorResponse(403, &cfg);
		applyConnectionHeader(keepAlive, res);
		return res;
	}

	RequestKind kind = classifyRequest(cfg, path, fsPath, req);

	HTTP_Response res;
	switch (kind) {

		case RK_UPLOAD:
			res = handleUploadRequest(req, cfg);
			break;

		case RK_CGI: {
			res = handleCgiRequest(req, cfg, fsPath);
			break;
		}

		case RK_DIRECTORY:
			res = handleDirectoryRequest(req, cfg, fsPath, path);
			break;

		case RK_STATIC_FILE:
			if (req.method == "DELETE")
				res = handleDeleteRequest(cfg, fsPath);
			else
				res = handleStaticFile(req, cfg, fsPath);
			break;

		case RK_FORBIDDEN:
			res = makeErrorResponse(403, &cfg);
			break;

		case RK_NOT_FOUND:
		default:
			res = makeErrorResponse(404, &cfg);
			break;
	}

	if (res.close)											// Close requested by handler
		keepAlive = false;

	applyConnectionHeader(keepAlive, res);
	headersUppercase(res);
	return res;
}
