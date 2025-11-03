#include "HttpParser.hpp"

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
        if (std::isalnum(c))
            return true;
        switch(c)   {
            case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
            case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
                return true;
        }
        return false;
    }

    /* Parse "METHOD SP target SP HTTP/1.1" into req. Set status/reason on error. */
    bool    parseRequestLine(const std::string& line, HTTP_Request& request, int& outStatus, std::string& outReason) {
        std::size_t p1 = line.find(' ');
        if (p1 == std::string::npos)    {
            outStatus = 400;
            outReason = "Bad Request";
            return false;
        }
        std::size_t p2 = line.find(' ', p1 + 1);
        if (p2 == std::string::npos)    {
            outStatus = 400;
            outReason = "Bad Request";
            return false;
        }

        request.method  = line.substr(0, p1);
        request.target  = line.substr(p1 + 1, p2 - (p1 + 1));
        request.version = line.substr(p2 + 1);

        if (request.version != "HTTP/1.1")  {
            outStatus = 505;
            outReason = "HTTP Version Not Supported";
            return false;
        }
        if (request.method.empty() || request.target.empty())   {
            outStatus = 400;
            outReason = "Bad Request";
            return false;
        }

        for (std::size_t i = 0; i < request.method.size(); ++i) {
            if (!isTokenChar(static_cast<unsigned char>(request.method[i])))    {
                outStatus = 400;
                outReason = "Bad Request";
                return false;
            }
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
                        // Continuation (obs-fold) of splitted sentence
                        if (!lastKey.empty())   {
                            std::string v = trim(current);
                            if (!v.empty()) {
                                if (!request.headers[lastKey].empty())
                                    request.headers[lastKey] += ' ';
                                request.headers[lastKey] += v;
                            }
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

        // Connection: default keep-alive in HTTP/1.1
        std::map<std::string, std::string>::const_iterator chit = request.headers.find("connection");
        if (chit != request.headers.end())  {
            std::string v = toLowerCopy(chit->second);
            if (v.find("close") != std::string::npos)
                request.keep_alive = false;
            else if (v.find("keep-alive") != std::string::npos)
                request.keep_alive = true;
        }
        else
            request.keep_alive = true;

        // Content-Length
        request.content_length = 0;
        std::map<std::string, std::string>::const_iterator clit = request.headers.find("content-length");
        if (clit != request.headers.end())  {
            std::string s = trim(clit->second);
            if (s.empty())  {
                outStatus = 400;
                outReason = "Bad Request";
                return false;
            }
            for (std::size_t i = 0; i < s.size(); ++i)  {
                if (!std::isdigit(static_cast<unsigned char>(s[i])))    {
                    outStatus = 400;
                    outReason = "Bad Request";
                    return false;
                }
            }
            errno = 0;
            unsigned long   v = std::strtoul(s.c_str(), NULL, 10);
            // unsigned long strtoul(const char* str, char** endptr, int base);
            if (errno == ERANGE)    {
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
            std::string v = toLowerCopy(teit->second);
            request.transfer_encoding = v;
            // Safe policy: if both TE and CL -> reject to avoid request smuggling classes for bugs.
            if (clit != request.headers.end())  {
                outStatus = 400;
                outReason = "Bad Request";
                return false;
            }
        }

        // Decide body reader mode
        if (!request.transfer_encoding.empty()) {
            if (request.transfer_encoding.find("chunked") != std::string::npos)
                request.body_reader_state = BR_CHUNKED;
            else    {
                outStatus = 501;
                outReason = "Not Implemented";
                return false;
            }
        }
        else if (request.content_length > 0)
            request.body_reader_state = BR_CONTENT_LENGTH;
        else
            request.body_reader_state = BR_NONE;
        
        return true;
    }

}   // anonymous namespace

namespace http  {

    std::size_t find_header_terminator(const std::string& buf)  {
        return buf.find("\r\n\r\n");
    }

    bool    parse_head(const std::string& head, HTTP_Request& request, int& status, std::string& reason)    {
        std::size_t eol = head.find("\r\n");
        if (eol == std::string::npos)   {
            status = 400;
            reason = "Bad Request";
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

} // namespace http