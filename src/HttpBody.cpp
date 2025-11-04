#include "HttpBody.hpp"

namespace   {

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
    
    bool    parse_hex_size(const std::string& line, std::size_t& out)   {
        // strip chunk extensions: "1A;foo=bar" -> "1A"
        std::size_t semi = line.find(';');
        std::string hex = (semi == std::string::npos) ? line : line.substr(0, semi);
        hex = trim(hex);
        if (hex.empty())
            return false;
        
        // hex digits only
        for (std::size_t i = 0; i < hex.size(); ++i)    {
            unsigned char   c = static_cast<unsigned char>(hex[i]);
            if (!std::isxdigit(c))  // '0' ~ '9', 'a' ~ 'f', 'A' ~ 'F'
                return false;
        }

        char*   endp = 0;   // null pointer value saying that this pointer doesn't point anywhere yet.
        unsigned long   v = std::strtoul(hex.c_str(), &endp, 16);
        // unsigned long strtoul(const char* str, char** endptr, int base);
        if (*endp != '\0')
            return false;
        out = static_cast<std::size_t>(v);
        return true;
    }

} // anonymous namespace

namespace http  {

    BodyResult  consume_body_content_length(Connection& connection, std::size_t max_body, int& status, std::string& reason) {
        HTTP_Request&   r = connection.request;

        // if CL itself exceeds limit, reject immediately
        if (r.content_length > max_body)    {
            status = 413;
            reason = "Payload Too Large";
            return BODY_ERROR;
        }

        const std::size_t   have = r.body_received;
        if (have >= r.content_length)
            return BODY_COMPLETE;

        const std::size_t   need = r.content_length - have;

        if (connection.readBuffer.empty())
            return BODY_INCOMPLETE;

        std::size_t avail = connection.readBuffer.size();
        std::size_t take = (avail < need) ? avail : need;

        if (have + take > max_body) {
            status = 413;
            reason = "Payload Too Large";
            return BODY_ERROR;
        }

        r.body.append(connection.readBuffer.data(), take);
        r.body_received += take;

        if (take == avail)
            connection.readBuffer.clear();
        else    {
            std::string after = connection.readBuffer.substr(take);
            connection.readBuffer.swap(after);
        }

        if (r.body_received == r.content_length)
            return BODY_COMPLETE;
        return BODY_INCOMPLETE;
    }

    BodyResult  consume_body_chunked(Connection& connection, std::size_t max_body, int& status, std::string& reason)    {
        HTTP_Request&   r = connection.request;

        for (;;)    {
            switch (r.chunk_state)  {
                case CS_SIZE:   {
                    std:size_t  pos = connection.readBuffer.find("\r\n");
                    if (pos == std::string::npos)
                        return BODY_INCOMPLETE;
                    
                    std::string line = connection.readBuffer.substr(0, pos);
                    std::string after = connection.readBuffer.substr(pos + 2);
                    connection.readBuffer.swap(after);

                    std::size_t sz = 0;
                    if (!parse_hex_size(line, sz))   { // Chunked transfer encoding (RFC 7230 §4.1) sends each chunk size in hexadecimal
                        status = 400;
                        reason = "Bad Request";
                        return BODY_ERROR;
                    }

                    r.chunk_bytes_left = sz;

                    if (sz == 0)
                        r.chunk_state = CS_TRAILERS;
                    else
                        r.chunk_state = CS_DATA;
                    break;
                }

                case CS_DATA:   {
                    if (connection.readBuffer.empty())
                        return BODY_INCOMPLETE;

                    std::size_t avail = connection.readBuffer.size();
                    std::size_t take = (avail < r.chunk_bytes_left) ? avail : r.chunk_bytes_left;

                    if (r.body_received + take > max_body)  {
                        status = 413;
                        reason = "Payload Too Large";
                        return BODY_ERROR;
                    }

                    r.body.append(connection.readBuffer.data(), take);
                    r.body_received += take;
                    r.chunk_bytes_left -= take;

                    if (take == avail)
                        connection.readBuffer.clear();
                    else    {
                        std::string after = connection.readBuffer.substr(take);
                        connection.readBuffer.swap(after);
                    }

                    if (r.chunk_bytes_left == 0)
                        r.chunk_state = CS_DATA_CRLF;
                    break;
                }

                case CS_DATA_CRLF:  {
                    if (connection.readBuffer.size() < 2)
                        return BODY_INCOMPLETE;
                    if (!(connection.readBuffer[0] =='\r' && connection.readBuffer[1] == '\n')) {
                        status = 400;
                        reason = "Bad Request";
                        return BODY_ERROR;
                    }
                    if (connection.readBuffer.size() == 2)
                        connection.readBuffer.clear();
                    else    {
                        std::string after = connection.readBuffer.substr(2);
                        connection.readBuffer.swap(after);
                    }
                    r.chunk_state = CS_SIZE;
                    break;
                }

                case CS_TRAILERS:   {
					for (;;)	{
                    	std::size_t pos = connection.readBuffer.find("\r\n");
                    	if (pos == std::string::npos)
                    	    return BODY_INCOMPLETE;

						if (pos == 0)	{
							// Blank line: trailers end
							std::string	after = connection.readBuffer.substr(2);
							connection.readBuffer.swap(after);
							r.chunk_state = CS_DONE;
							return BODY_COMPLETE;
						}
                    // If trailers are kept => std::string  trailers = connection.readBuffer.substr(0, pos);
                    std::string after = connection.readBuffer.substr(pos + 2);
                    connection.readBuffer.swap(after);
                	}
				}

                case CS_DONE:
                    return BODY_COMPLETE;
            }
        }
    }

} // namespace http