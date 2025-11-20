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

    http::BodyResult  body_fail(int status, const char* reason, int& outStatus, std::string& outReason)   {
        outStatus = status;
        outReason = reason;
        return http::BODY_ERROR;
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

        errno = 0;
        unsigned long   v = std::strtoul(hex.c_str(), NULL, 16);
        if (errno == ERANGE)
            return false;
        // For strtoul the C standard guarantees -> On overflow (value would exceed ULONG_MAX): it returns ULONG_MAX and sets errno = ERANGE

        if (v > static_cast<unsigned long>(std::numeric_limits<std::size_t>::max()))
            return false;

        out = static_cast<std::size_t>(v);
        return true;
    }

} // anonymous namespace

namespace http  {

    BodyResult  consume_body_content_length(Connection& connection, std::size_t max_body, int& status, std::string& reason) {
        HTTP_Request&   r = connection.request;

        // if CL itself exceeds limit, reject immediately
        if (r.content_length > max_body)
            return body_fail(413, "Payload Too Large", status, reason);

        const std::size_t   have = r.body_received;
        if (have >= r.content_length)
            return BODY_COMPLETE;

        const std::size_t   need = r.content_length - have;

        if (connection.readBuffer.empty())
            return BODY_INCOMPLETE;

        std::size_t avail = connection.readBuffer.size();
        std::size_t take = (avail < need) ? avail : need;

        if (have + take > max_body)
            return body_fail(413, "Payload Too Large", status, reason);

        r.body.append(connection.readBuffer.data(), take);
        r.body_received += take;

        connection.readBuffer.erase(0, take);

        return (r.body_received == r.content_length) ? BODY_COMPLETE : BODY_INCOMPLETE;
    }

    BodyResult  consume_body_chunked(Connection& connection, std::size_t max_body, int& status, std::string& reason)    {
        HTTP_Request&   r = connection.request;

        static const std::size_t    MAX_LINE = 16 * 1024;

        for (;;)    {
            switch (r.chunk_state)  {
                case CS_SIZE:   {
                    std::size_t  pos = connection.readBuffer.find("\r\n");
                    if (pos == std::string::npos)   {
                        if (connection.readBuffer.size() > MAX_LINE)
                            return body_fail(413, "Payload Too Large", status, reason);
                        return BODY_INCOMPLETE;
                    }
                    if (pos > MAX_LINE)
                        return body_fail(413, "Payload Too Large", status, reason);
                    
                    std::string line = connection.readBuffer.substr(0, pos);
                    connection.readBuffer.erase(0, pos + 2);

                    std::size_t sz = 0;
                    if (!parse_hex_size(line, sz)) // Chunked transfer encoding (RFC 7230 §4.1) sends each chunk size in hexadecimal
                        return body_fail(413, "Payload Too Large", status, reason);

                    if (sz > 0 && sz > max_body - r.body_received)
                        return body_fail(413, "Payload Too Large", status, reason);

                    r.chunk_bytes_left = sz;

                    r.chunk_state = (sz == 0) ? CS_TRAILERS : CS_DATA;

                    break;
                }

                case CS_DATA:   {
                    if (connection.readBuffer.empty())
                        return BODY_INCOMPLETE;

                    std::size_t avail = connection.readBuffer.size();
                    std::size_t take = (avail < r.chunk_bytes_left) ? avail : r.chunk_bytes_left;

                    if (take > max_body - r.body_received)
                        return body_fail(413, "Payload Too Large", status, reason);

                    r.body.append(connection.readBuffer.data(), take);
                    r.body_received += take;
                    r.chunk_bytes_left -= take;

                    connection.readBuffer.erase(0, take);

                    if (r.chunk_bytes_left == 0)
                        r.chunk_state = CS_DATA_CRLF;
                    break;
                }

                case CS_DATA_CRLF:  {
                    if (connection.readBuffer.size() < 2)
                        return BODY_INCOMPLETE;
                    if (!(connection.readBuffer[0] =='\r' && connection.readBuffer[1] == '\n'))
                        return body_fail(400, "Bad Request", status, reason);
                    connection.readBuffer.erase(0, 2);
                    r.chunk_state = CS_SIZE;
                    break;
                }

                case CS_TRAILERS:   {
                    BodyResult  tr = http::consume_all_trailers(connection.readBuffer, MAX_LINE, status, reason);

                    if (tr != BODY_COMPLETE)
                        return tr;  // BODY_INCOMPLETE or BODY_ERROR (propagate)

					r.chunk_state = CS_DONE;
					return BODY_COMPLETE;
				}

                case CS_DONE:
                    return BODY_COMPLETE;
            }
        }
    }


	BodyResult	consume_all_trailers(std::string& buffer, std::size_t max_line, int& status, std::string& reason)	{
		for (;;)	{
			std::size_t	pos = buffer.find("\r\n");
			if (pos == std::string::npos)   {
                if (buffer.size() > max_line)
                    return body_fail(413, "Payload Too Large", status, reason);
                return BODY_INCOMPLETE;
            }
            if (pos > max_line)
                return body_fail(413, "Payload Too Large", status, reason);

			if (pos == 0)	{
				// blank line -> end of trailers
                buffer.erase(0, 2);
                return BODY_COMPLETE;
			}
			
			// Drop one trailer line (line + CRLF)
            buffer.erase(0, pos + 2);
		}
	}

} // namespace http