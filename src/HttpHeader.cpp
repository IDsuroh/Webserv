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

    /* RFC7230 tchar (rough, ASCII) for METHOD token validation. */
    bool    isTokenChar(unsigned char c)    {
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

    // Accepts: "METHOD  SP/HTAB+  target  SP/HTAB+  HTTP/1.1[optional-CR]"
    // Rejects: extra junk after version; spaces/tabs inside target; control/non-ASCII in target.
    // Special-cases: OPTIONS * and CONNECT authority-form (light validation).
    bool    parseRequestLine(const std::string& line, HTTP_Request& request, int& outStatus, std::string& outReason) {
        // Work on a local copy so we can trim a trailing CR
        std::string         s = line;
        const std::size_t   n = s.size();
        std::size_t         i = 0;

        // --- METHOD ---------------------------------------------------------------------------
        if (n == 0 || isSpaceTab(static_cast<unsigned char>(s[i]))) {
            outStatus = 400;
            outReason = "Bad Request";
            return false;
        }
        while (i < n && !isSpaceTab(static_cast<unsigned char>(s[i])))  {
            unsigned char   c = static_cast<unsigned char>(s[i]);
            if (!isTokenChar(c))    {
                outStatus = 400;
                outReason = "Bad Request";
                return false;
            }
            ++i;
        }
        request.method = s.substr(0, i);

        // 1+ SP/HTAB between fields
        if (i >= n || !isSpaceTab(static_cast<unsigned char>(s[i])))    {
            outStatus = 400;
            outReason = "Bad Request";
            return false;
        }
        while (i < n && isSpaceTab(static_cast<unsigned char>(s[i])))
            ++i;
        if (i >= n) {
            outStatus = 400;
            outReason = "Bad Request";
            return false;            
        }
        // --- TARGET ---------------------------------------------------------------------------
        std::size_t tStart = i;
        while (i < n && !isSpaceTab(static_cast<unsigned char>(s[i])))  {
            unsigned char   c = static_cast<unsigned char>(s[i]);
            // no controls, ASCII only
            if (c >= 0x7F || c < 0x20)  {   // c >= (int)127 || c < (int)32
                outStatus = 400;
                outReason = "Bad Request";
                return false;
            }
            ++i;
        }
        request.target = s.substr(tStart, i - tStart);

        if (request.target == "*" && request.method != "OPTIONS")   {
            outStatus = 400;
            outReason = "Bad Request";
            return false;
        }

        // 1+ SP/HTAB between target and version
        if (i >= n || !isSpaceTab(static_cast<unsigned char>(s[i])))    {
            outStatus = 400;
            outReason = "Bad Request";
            return false;
        }
        while (i < n && isSpaceTab(static_cast<unsigned char>(s[i])))
            ++i;
        if (i >= n) {
            outStatus = 400;
            outReason = "Bad Request";
            return false;
        }

        // --- HTTP-Version ---------------------------------------------------------------------------
        // Parse a single version token; no trailing bytes allowed after it.
        std::size_t vStart = i;
        while (i < n && !isSpaceTab(static_cast<unsigned char>(s[i])))
            ++i;
        request.version = s.substr(vStart, i - vStart);

        if (i != n)  {
            outStatus = 400;
            outReason = "Bad Request";
            return false;
        }

        if (request.version != "HTTP/1.1")  {
            outStatus = 505;
            outReason = "HTTP Version Not Supported";
            return false;
        }

        return true;
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
            bool    at_end  = (i >= block.size());
            if (at_end || at_crlf)  {
                std::string current = (at_end ? block.substr(start) : block.substr(start, i - start));

                if (!current.empty())   {
                    if (current[0] == ' ' || current[0] == '\t')    {
                        if (lastKey.empty())    {   // obs-fold without previous header -> 400
                            outStatus = 400;
                            outReason = "Bad Request";
                            return false;
                        }
                        // Continuation (obs-fold) of splitted sentence
                        std::string v = trim(current);
                        if (!v.empty()) {
                            if (!request.headers[lastKey].empty())
                                request.headers[lastKey] += ' ';
                            request.headers[lastKey] += v;
                        }
                    }
                    else    {
                        std::size_t colon = current.find(':');
                        if (colon == std::string::npos) {
                            outStatus = 400;
                            outReason = "Bad Request";
                            return false;
                        }
                        std::string k = toLowerCopy(trimRight(current.substr(0, colon)));
                        std::string v = trim(current.substr(colon + 1));
                        if (k.empty())  {
                            outStatus = 400;
                            outReason = "Bad Request";
                            return false;
                        }

                        // Validate header-name as ASCII token
                        for (std::size_t j = 0; j < k.size(); ++j)  {
                            if (!isTokenChar(static_cast<unsigned char>(k[j]))) {
                                outStatus = 400;
                                outReason = "Bad Request";
                                return false;
                            }
                        }

                        // Duplicate Host must have identical values
                        if (k == "host")    {
                            std::string vtrim = trim(v);
                            std::map<std::string, std::string>::const_iterator  hprev = request.headers.find("host");
                            if (hprev != request.headers.end()) {
                                if (toLowerCopy(trim(hprev->second)) != toLowerCopy(vtrim)) {
                                    outStatus = 400;
                                    outReason = "Bad Request";
                                    return false;
                                }
                            }
                        }

                        if (request.headers.find(k) != request.headers.end())   {
                            if (!request.headers[k].empty())
                                request.headers[k] += ", ";
                            request.headers[k] += v; // coalesce duplicates
                        }
                        else
                            request.headers[k] = v;
                        lastKey = k;
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


        // Connection: default keep-alive in HTTP/1.1
        std::map<std::string, std::string>::const_iterator chit = request.headers.find("connection");
        request.keep_alive = true;  // default in HTTP/1.1
        if (chit != request.headers.end())  {
            std::istringstream  iss(toLowerCopy(chit->second));
            std::string         tok;
            while (std::getline(iss, tok, ',')) {
                tok = trim(tok);
                if (tok == "close") {
                    request.keep_alive = false;
                    break;
                }
            }
        }

        // Host (required in 1.1)
        std::map<std::string, std::string>::const_iterator hit = request.headers.find("host");
        if (hit == request.headers.end() || trim(hit->second).empty())  {
            outStatus = 400;
            outReason = "Bad Request";
            return false;
        }
        request.host = trim(hit->second);
        if (request.host.find(',') != std::string::npos)    {
            outStatus = 400;
            outReason = "Bad Request";
            return false;
        } // multiple Host values not allowed

        // Content-Length
        request.content_length = 0;
        std::map<std::string, std::string>::const_iterator clit = request.headers.find("content-length");
        if (clit != request.headers.end())  {
            // Duplicates were coalesced as "v1, v2, ...": require all identical
            std::string         raw = clit->second;
            std::istringstream  iss(raw);
            std::string         part;
            std::string         first;

            while (std::getline(iss, part, ','))    {
                part = trim(part);
                if (part.empty())   {
                    outStatus = 400;
                    outReason = "Bad Request";
                    return false;
                }
                // ASCII digit check only
                for (std::size_t i = 0; i < part.size(); ++i)   {
                    char    c = part[i];
                    if (c < '0' || c > '9') {
                        outStatus = 400;
                        outReason = "Bad Request";
                        return false;
                    }
                }
                if (first.empty())
                    first = part;
                else if (part != first) {
                    outStatus = 400;
                    outReason = "Bad Request";
                    return false;                    
                }
            }

            // Parse agreed value
            char*   endp = 0;
            errno = 0;
            unsigned long   v = std::strtoul(first.c_str(), &endp, 10);
            if (errno == ERANGE || endp == first.c_str() || *endp != '\0')  {
                outStatus = (errno == ERANGE) ? 413 : 400;
                outReason = (errno == ERANGE) ? "Payload Too Large" : "Bad Request";
                return false;
            }
            if (v > static_cast<unsigned long>(std::numeric_limits<std::size_t>::max()))    {
                outStatus = 413;
                outReason = "Payload Too Large";
                return false;
            }
            request.content_length = static_cast<std::size_t>(v);
        }

        // Transfer-Encoding
        request.transfer_encoding.clear();
        std::map<std::string, std::string>::const_iterator teit = request.headers.find("transfer-encoding");
        if (teit != request.headers.end())  {
            if (clit != request.headers.end())  {   // TE + CL → 400
                outStatus = 400;
                outReason = "Bad Request";
                return false;
            }

            std::vector<std::string>    codings;
            std::istringstream          iss(toLowerCopy(teit->second));
            std::string                 tok;
            while (std::getline(iss, tok, ',')) {
                tok = trim(tok);
                if (!tok.empty())
                    codings.push_back(tok);
            }
            if (codings.empty())    {
                outStatus = 400;
                outReason = "Bad Request";
                return false;
            }

            bool    seenChunked = false;
            for (std::size_t i = 0; i < codings.size(); ++i)    {
                const std::string&  c = codings[i];
                if (c == "chunked") {
                    if (i != codings.size() - 1)    {
                        outStatus = 400;
                        outReason = "Bad Request";
                        return false;
                    }
                    seenChunked = true;
                }
                else    {
                    outStatus = 501;
                    outReason = "Not Implemented";
                    return false;
                }
            }
            if (!seenChunked)   {
                outStatus = 501;
                outReason = "Not Implemented";
                return false;
            }

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

        if (head.size() > MAX_HEADER_BYTES) {
            status = 431;
            reason = "Request Header Fields Too Large";
            return false;
        }

		std::size_t eol = head.find("\r\n");
        if (eol == std::string::npos)   {
            status = 400;
            reason = "Bad Request";
            return false;
        }

        if (eol > MAX_REQUEST_LINE) {
            status = 431;
            reason = "Request Header Fields Too Large";
            return false;
        }

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