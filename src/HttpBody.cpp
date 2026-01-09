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

        out = static_cast<std::size_t>(v);
        return true;
    }

    http::BodyResult	consume_all_trailers(std::string& buffer, std::size_t max_line, int& status, std::string& reason)	{
		for (;;)	{
			std::size_t	pos = buffer.find("\r\n");
			if (pos == std::string::npos)   {
                if (buffer.size() > max_line)
                    return body_fail(413, "Payload Too Large", status, reason);
                return http::BODY_INCOMPLETE;
            }
            if (pos > max_line)
                return body_fail(413, "Payload Too Large", status, reason);

			if (pos == 0)	{
				// blank line -> end of trailers
                buffer.erase(0, 2);
                return http::BODY_COMPLETE;
			}
			
			// Drop one trailer line (line + CRLF)
            buffer.erase(0, pos + 2);
		}
	}

} // anonymous namespace

namespace http  {

    BodyResult  consume_body_content_length(Connection& connection, std::size_t max_body, int& status, std::string& reason) {
        HTTP_Request&   request = connection.request;

        // if CL itself exceeds limit, reject immediately
        if (request.content_length > max_body)  {
            return body_fail(413, "Payload Too Large", status, reason);
        }
        
        const std::size_t   have = request.body_received;
        if (have >= request.content_length)
            return BODY_COMPLETE;

        const std::size_t   need = request.content_length - have;

        if (connection.readBuffer.empty())
            return BODY_INCOMPLETE;

        std::size_t avail = connection.readBuffer.size();
        std::size_t take = (avail < need) ? avail : need;

        if (have + take > max_body)
            return body_fail(413, "Payload Too Large", status, reason);

        request.body.append(connection.readBuffer.data(), take);
        request.body_received += take;

        connection.readBuffer.erase(0, take);

        return (request.body_received == request.content_length) ? BODY_COMPLETE : BODY_INCOMPLETE;
    }

    BodyResult  consume_body_chunked(Connection& connection, std::size_t max_body, int& status, std::string& reason)    {
        HTTP_Request&   request = connection.request;

        static const std::size_t    MAX_LINE = 16 * 1024;

        for (;;)    {
            switch (request.chunk_state)  {
                case CS_SIZE:   {
                    std::size_t  position = connection.readBuffer.find("\r\n");
                    if (position == std::string::npos)   {
                        if (connection.readBuffer.size() > MAX_LINE)
                            return body_fail(413, "Payload Too Large", status, reason);
                        return BODY_INCOMPLETE;
                    }
                    if (position > MAX_LINE)
                        return body_fail(413, "Payload Too Large", status, reason);
                    
                    std::string line = connection.readBuffer.substr(0, position);
                    connection.readBuffer.erase(0, position + 2);

                    std::size_t size = 0;
                    if (!parse_hex_size(line, size)) // Chunked transfer encoding (RFC 7230 §4.1) sends each chunk size in hexadecimal
                        return body_fail(400, "Bad Request", status, reason);

                    if (size > 0 && size > max_body - request.body_received)
                        return body_fail(413, "Payload Too Large", status, reason);

                    request.chunk_bytes_left = size;

                    request.chunk_state = (size == 0) ? CS_TRAILERS : CS_DATA;

                    break;
                }

                case CS_DATA:   {
                    if (connection.readBuffer.empty())
                        return BODY_INCOMPLETE;

                    std::size_t avail = connection.readBuffer.size();
                    std::size_t take = (avail < request.chunk_bytes_left) ? avail : request.chunk_bytes_left;

                    if (take > max_body - request.body_received)
                        return body_fail(413, "Payload Too Large", status, reason);

                    request.body.append(connection.readBuffer.data(), take);
                    request.body_received += take;
                    request.chunk_bytes_left -= take;

                    connection.readBuffer.erase(0, take);

                    if (request.chunk_bytes_left == 0)
                        request.chunk_state = CS_DATA_CRLF;
                    break;
                }

                case CS_DATA_CRLF:  {
                    if (connection.readBuffer.size() < 2)
                        return BODY_INCOMPLETE;
                    if (!(connection.readBuffer[0] =='\r' && connection.readBuffer[1] == '\n'))
                        return body_fail(400, "Bad Request", status, reason);
                    connection.readBuffer.erase(0, 2);
                    request.chunk_state = CS_SIZE;
                    break;
                }

                case CS_TRAILERS:   {
                    BodyResult  trail = consume_all_trailers(connection.readBuffer, MAX_LINE, status, reason);

                    if (trail != BODY_COMPLETE)
                        return trail;  // BODY_INCOMPLETE or BODY_ERROR (propagate)

					request.chunk_state = CS_DONE;
					return BODY_COMPLETE;
				}

                case CS_DONE:
                    return BODY_COMPLETE;
            }
        }
    }

BodyResult consume_body_content_length_drain(Connection& connection,
                                            std::size_t /*max_body*/,
                                            int& status,
                                            std::string& reason)
{
    HTTP_Request& request = connection.request;

    // Em DRAIN, já decidiste a resposta (ex: 413). Aqui só descartas bytes.
    // Não reapliques max_body, senão podes bloquear o drain e causar RST.
    status = 0;
    reason.clear();

    const std::size_t have = request.body_received;
    if (have >= request.content_length)
        return BODY_COMPLETE;

    const std::size_t need = request.content_length - have;

    if (connection.readBuffer.empty())
        return BODY_INCOMPLETE;

    std::size_t avail = connection.readBuffer.size();
    std::size_t take = (avail < need) ? avail : need;

    // DRAIN: não guardar body
    request.body_received += take;

    // ✅ telemetria: conta bytes drenados
    connection.drainedBytes += take;

    connection.readBuffer.erase(0, take);

    return (request.body_received == request.content_length) ? BODY_COMPLETE : BODY_INCOMPLETE;
}





BodyResult consume_body_chunked_drain(Connection& connection,
                                     std::size_t /*max_body*/,
                                     int& status,
                                     std::string& reason)
{
    HTTP_Request& request = connection.request;

    // Em DRAIN, já decidiste a resposta (ex: 413). Aqui só descartas bytes.
    // Não reapliques max_body, senão paras antes do fim e levas FIN→RST.
    status = 0;
    reason.clear();

    static const std::size_t MAX_LINE = 16 * 1024;

    for (;;) {
        switch (request.chunk_state) {

            case CS_SIZE: {
                std::size_t position = connection.readBuffer.find("\r\n");
                if (position == std::string::npos) {
                    // Proteção anti-DoS de linha interminável (best-effort drain)
                    if (connection.readBuffer.size() > MAX_LINE) {
                        // descarta o que temos para não crescer sem limite
                        connection.drainedBytes += connection.readBuffer.size();
                        connection.readBuffer.clear();
                    }
                    return BODY_INCOMPLETE;
                }
                if (position > MAX_LINE) {
                    // linha gigante: descarta essa "linha" (best-effort)
                    connection.drainedBytes += (position + 2);
                    connection.readBuffer.erase(0, position + 2);
                    return BODY_INCOMPLETE;
                }

                std::string line = connection.readBuffer.substr(0, position);
                connection.readBuffer.erase(0, position + 2);

                // ✅ conta bytes drenados da framing line + CRLF
                connection.drainedBytes += (position + 2);

                std::size_t size = 0;
                if (!parse_hex_size(line, size)) {
                    // framing inválido em DRAIN: discard best-effort e espera EOF / mais dados
                    connection.drainedBytes += connection.readBuffer.size();
                    connection.readBuffer.clear();
                    return BODY_INCOMPLETE;
                }

                request.chunk_bytes_left = size;
                request.chunk_state = (size == 0) ? CS_TRAILERS : CS_DATA;
                break;
            }

            case CS_DATA: {
                if (connection.readBuffer.empty())
                    return BODY_INCOMPLETE;

                std::size_t avail = connection.readBuffer.size();
                std::size_t take = (avail < request.chunk_bytes_left) ? avail : request.chunk_bytes_left;

                // DRAIN: não guardar body
                request.body_received += take;
                request.chunk_bytes_left -= take;

                // ✅ telemetria: conta bytes drenados
                connection.drainedBytes += take;

                connection.readBuffer.erase(0, take);

                if (request.chunk_bytes_left == 0)
                    request.chunk_state = CS_DATA_CRLF;
                break;
            }

            case CS_DATA_CRLF: {
                if (connection.readBuffer.size() < 2)
                    return BODY_INCOMPLETE;

                if (!(connection.readBuffer[0] == '\r' && connection.readBuffer[1] == '\n')) {
                    // framing inválido: discard best-effort
                    connection.drainedBytes += connection.readBuffer.size();
                    connection.readBuffer.clear();
                    return BODY_INCOMPLETE;
                }

                connection.readBuffer.erase(0, 2);

                // ✅ conta CRLF drenado
                connection.drainedBytes += 2;

                request.chunk_state = CS_SIZE;
                break;
            }

            case CS_TRAILERS: {
                // Em DRAIN queremos best-effort: NÃO queremos que o consume_all_trailers gere 413 aqui.
                // Se ele devolver BODY_ERROR, não mudamos resposta; apenas descartamos e continuamos a drenar.

                int st2 = 0;
                std::string rs2;
                BodyResult trail = consume_all_trailers(connection.readBuffer, MAX_LINE, st2, rs2);

                if (trail == BODY_INCOMPLETE)
                    return BODY_INCOMPLETE;

                if (trail == BODY_ERROR) {
                    // descarta buffer e continua até EOF
                    connection.drainedBytes += connection.readBuffer.size();
                    connection.readBuffer.clear();
                    return BODY_INCOMPLETE;
                }

                // BODY_COMPLETE: trailers consumidos
                request.chunk_state = CS_DONE;
                return BODY_COMPLETE;
            }

            case CS_DONE:
                return BODY_COMPLETE;
        }
    }
}





} // namespace http