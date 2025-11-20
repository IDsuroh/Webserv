#include "HttpHeader.hpp"

namespace   {

    /* Lowercase a copy of a string (ASCII). */
    std::string toLowerCopy(const std::string& s)   {
        std::string out(s);	// copy constructor
        for (std::size_t i = 0; i < out.size(); ++i)    {
            unsigned char   c = static_cast<unsigned char>(out[i]);
            out[i] = static_cast<char>(std::tolower(c));
        }
        return out;
    }

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

    bool    fail(int status, const char* reason, int& outStatus, std::string& outReason)    {
        outStatus = status;
        outReason = reason;
        return false;
    }

    /* RFC7230 tchar (rough, ASCII) for METHOD token validation. */
    bool    istokenChar(unsigned char c)    {
        if (c > 0x7F)
            return false;

        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
            return true;
        
        switch(c)   {
            case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
            case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
                return true;
        }
        return false;
    }

    bool    isSpaceTab(unsigned char c) {
        return c == ' ' || c == '\t';
    }

    bool    parseRequestLine(const std::string& line, HTTP_Request& request, int& outStatus, std::string& outReason) {
        const std::string&  s = line;
        std::size_t         n = s.size();
        std::size_t         i = 0;

        // --- METHOD ---------------------------------------------------------------------------
        if (n == 0 || isSpaceTab(static_cast<unsigned char>(s[0])))
            return fail(400, "Bad Request", outStatus, outReason);

        while (i < n && !isSpaceTab(static_cast<unsigned char>(s[i])))  {
            unsigned char   c = static_cast<unsigned char>(s[i]);
            if (!istokenChar(c))
                return fail(400, "Bad Request", outStatus, outReason);
            ++i;
        }

        request.method = s.substr(0, i);

        // 1+ SP/HTAB between fields
        if (i >= n || !isSpaceTab(static_cast<unsigned char>(s[i])))
            return fail(400, "Bad Request", outStatus, outReason);
        while (i < n && isSpaceTab(static_cast<unsigned char>(s[i])))
            ++i;
        if (i >= n)
            return fail(400, "Bad Request", outStatus, outReason);         

        // --- REQEUST-TARGET ---------------------------------------------------------------------------
        // We accept:
        // - origin-form: "/path" or "/path?query"
        // - absolute-form: "http://host/path" (we'll still store whole thing in target)
        // - "*" only for OPTIONS
        std::size_t tStart = i;
        while (i < n && !isSpaceTab(static_cast<unsigned char>(s[i])))  {
            unsigned char   c = static_cast<unsigned char>(s[i]);
            // no controls, ASCII only
            if (c >= 0x7F || c < 0x20)   // c >= (int)127 || c < (int)32
                return fail(400, "Bad Request", outStatus, outReason);
            ++i;
        }

        request.target = s.substr(tStart, i - tStart);

        if (request.target == "*" && request.method != "OPTIONS")
            return fail(400, "Bad Request", outStatus, outReason);

        if (request.target != "*")  {
            std::size_t qpos = request.target.find('?');
            if (qpos == std::string::npos)
                request.path = request.target;
            else    {
                request.path = request.target.substr(0, qpos);
                request.query = request.target.substr(qpos + 1);
            }
        }

        // 1+ SP/HTAB between target and version
        if (i >= n || !isSpaceTab(static_cast<unsigned char>(s[i])))
            return fail(400, "Bad Request", outStatus, outReason);
        while (i < n && isSpaceTab(static_cast<unsigned char>(s[i])))
            ++i;
        if (i >= n)
            return fail(400, "Bad Request", outStatus, outReason);  

        // --- HTTP-Version ---------------------------------------------------------------------------
        // Accept "HTTP/1.0" and "HTTP/1.1"
        // Allow optional trailing spaces, but no other junk.
        std::size_t vStart = i;
        while (i < n && !isSpaceTab(static_cast<unsigned char>(s[i])))
            ++i;

        request.version = s.substr(vStart, i - vStart);

        // Allow trailing SP/HTAB only
        while (i < n && isSpaceTab(static_cast<unsigned char>(s[i])))
            ++i;
        if (i != n)
            return fail(400, "Bad Request", outStatus, outReason);

        if (request.version == "HTTP/1.0")  // HTTP/1.0: RFC 1945 – Host header not required
            return true;                    // Later can treat keep-alive differently based on version.

        if (request.version == "HTTP/1.1")  // HTTP/1.1: RFC 7230 – Host header required
            return true;

        return fail(505, "HTTP Version Not Supported", outStatus, outReason);;
    }

    /*
    * Parse header lines:
    * - Lowercase header keys.
    * - Coalesce duplicates with ", ".
    * - Support obs-fold: a line starting with SP/HTAB continues previous header value (space-joined).
    * - Validate Host (required by HTTP/1.1).
    * - Set keep_alive, content_length, transfer_encoding, body_reader_state.
    */
    bool    parseHeadersBlock(const std::string& block, HTTP_Request& request, int& outStatus, std::string& outReason)  {
        std::string lastKey;

        for (std::size_t i = 0, start = 0; ; )  {
            bool    at_crlf = (i + 1 < block.size() && block[i] == '\r' && block[i + 1] == '\n');
            bool    at_end  = (i == block.size());
            if (at_end || at_crlf)  {
                std::string current = (at_end ? block.substr(start) : block.substr(start, i - start));

                if (!current.empty())   {
                    if (current[0] == ' ' || current[0] == '\t')    {
                        if (lastKey.empty()) // obs-fold without previous header -> 400
                            return fail(400, "Bad Request", outStatus, outReason);

                        // Continuation (obs-fold) of splitted sentence
                        std::string value = trim(current);
                        if (!value.empty()) {
                            if (!request.headers[lastKey].empty())  // if there was previous key value add space.
                                request.headers[lastKey] += ' ';
                            request.headers[lastKey] += value;
                        }
                    }
                    else    {
                        std::size_t colon = current.find(':');
                        if (colon == std::string::npos)
                            return fail(400, "Bad Request", outStatus, outReason);

                        std::string key = toLowerCopy(trimRight(current.substr(0, colon)));
                        std::string value = trim(current.substr(colon + 1));
                        if (key.empty())
                            return fail(400, "Bad Request", outStatus, outReason);

                        // Validate header-name as ASCII token
                        for (std::size_t j = 0; j < key.size(); ++j)  {
                            if (!istokenChar(static_cast<unsigned char>(key[j]))) // keys should be characters.
                                return fail(400, "Bad Request", outStatus, outReason);
                        }

                        // Duplicate Host must have identical values
                        if (key == "host")    {
                            std::map<std::string, std::string>::const_iterator  hprev = request.headers.find("host");
                            if (hprev != request.headers.end()) {
                                if (toLowerCopy(trim(hprev->second)) != toLowerCopy(value))
                                    return fail(400, "Bad Request", outStatus, outReason);
                            }
                        }   // If Host appears more than once, all values must be identical (case-insensitive)
                            // Host: example.com                Host: example.com
                            // Host: EXAMPLE.COM <- allowed.    Host: other.com     <- 400 error

                        std::map<std::string, std::string>::iterator    it = request.headers.find(key);
                        if (it != request.headers.end())   {
                            if (!it->second.empty())
                                it->second += ", ";
                            it->second += value; // coalesce duplicates
                        }
                        else
                            request.headers[key] = value;
                        lastKey = key;
                    }
                }

                if (at_end)
                    break;
                i += 2; // skip CRLF
                start = i;
            }
            else
                ++i;
        }
            // finished populating request.headers
            // Now time for evaluating and dividing each token of the request.headers to their respective parts.

        // Connection: default keep-alive in HTTP/1.1
        std::map<std::string, std::string>::const_iterator chit = request.headers.find("connection");
        if (request.version == "HTTP/1.1")
            request.keep_alive = true;   // default in HTTP/1.1
        else if (request.version == "HTTP/1.0")
            request.keep_alive = false; // default in HTTP/1.0

        if (chit != request.headers.end())  {
            std::istringstream  iss(toLowerCopy(chit->second));
            std::string         token;
            while (std::getline(iss, token, ',')) {
                token = trim(token);
                if (token == "close") {
                    request.keep_alive = false;
                    break;
                }
                else if (token == "keep-alive")
                    request.keep_alive = true;
            }
        }

        // Host (required in 1.1)
        std::map<std::string, std::string>::const_iterator hit = request.headers.find("host");
        
        if (request.version == "HTTP/1.1")  {
            if (hit == request.headers.end() || trim(hit->second).empty())
                return fail(400, "Bad Request", outStatus, outReason);
        }

        if (hit != request.headers.end())   {
            request.host = trim(hit->second);
            if (request.host.find(',') != std::string::npos)
                return fail(400, "Bad Request", outStatus, outReason);
                // multiple Host values not allowed
        }

        // Content-Length
        std::map<std::string, std::string>::const_iterator clit = request.headers.find("content-length");
        if (clit != request.headers.end())  {
            // Duplicates were coalesced as "v1, v2, ...": require all identical
            std::string         raw = clit->second;
            std::istringstream  iss(raw);
            std::string         part;
            std::string         first;

            while (std::getline(iss, part, ','))    {
                part = trim(part);
                if (part.empty())
                    return fail(400, "Bad Request", outStatus, outReason);

                if (first.empty())
                    first = part;
                else if (part != first)
                    return fail(400, "Bad Request", outStatus, outReason);
            }

            // Parse agreed value
            char*   endp = 0;
            errno = 0;
            unsigned long   v = std::strtoul(first.c_str(), &endp, 10); // function strtoul gives out errno == ERANGE if overflow
            if (errno == ERANGE)
                return fail(413, "Payload Too Large", outStatus, outReason);
            if (endp == first.c_str() || *endp != '\0')
                return fail(400, "Bad Request", outStatus, outReason);

            request.content_length = static_cast<std::size_t>(v);
        }

        // Transfer-Encoding
        std::map<std::string, std::string>::const_iterator teit = request.headers.find("transfer-encoding");
        if (teit != request.headers.end())  {
            if (clit != request.headers.end())  // TE + CL → 400 Cannot have both Content-Length and Transfer-Encoding
                return fail(400, "Bad Request", outStatus, outReason);

            std::istringstream  iss(toLowerCopy(teit->second));
            std::string         token;
            bool                haveChunk = false;
            while (std::getline(iss, token, ',')) {
                token = trim(token);
                if (token.empty())
                    continue;
                if (token != "chunked")
                    return fail(501, "Not Implemented", outStatus, outReason);
                haveChunk = true;
            }
            if (!haveChunk)
                return fail(400, "Bad Request", outStatus, outReason);

            // Normalize to a canonical marker
            request.transfer_encoding = "chunked";
        }

        // Decide body reader mode
        if (!request.transfer_encoding.empty())
            request.body_reader_state = BR_CHUNKED;
        else if (request.content_length > 0)
            request.body_reader_state = BR_CONTENT_LENGTH;
        else
            request.body_reader_state = BR_NONE;
        
        return true;
    }

}   // anonymous namespace

namespace http  {

    bool    parse_head(const std::string& head, HTTP_Request& request, int& status, std::string& reason)    { 
        
        // size-limits
        static const std::size_t    MAX_HEADER_BYTES = 16 * 1024;   // total head (request-line + headers)
        static const std::size_t    MAX_REQUEST_LINE = 8 * 1024;    // request-line only

        if (head.size() > MAX_HEADER_BYTES)
            return fail(431, "Request Header Fields Too Large", status, reason);

		std::size_t eol = head.find("\r\n");
        if (eol == std::string::npos)
            return fail(400, "Bad Request", status, reason);

        if (eol > MAX_REQUEST_LINE)
            return fail(431, "Request Header Fields Too Large", status, reason);

        const std::string   requestLine     = head.substr(0, eol);
        const std::string   headersBlock    = head.substr(eol + 2);
        
        if (!parseRequestLine(requestLine, request, status, reason))
            return false;
        if (!parseHeadersBlock(headersBlock, request, status, reason))
            return false;
        
        return true;
    }

	bool	extract_next_head(std::string& buffer, std::string& out_head)	{
		out_head.clear();

        // Remove any number of leading empty heads
        while (buffer.size() >= 4 && buffer.compare(0, 4, "\r\n\r\n") == 0)
            buffer.erase(0, 4);

        // Find next head terminator
        std::size_t delim = buffer.find("\r\n\r\n");
        if (delim == std::string::npos)
            return false;
        // caller will wait for more bytes

        // Produce head and drop in from buffer
        out_head.assign(buffer, 0, delim);
        buffer.erase(0, delim + 4);
        return true;
	}

} // namespace http