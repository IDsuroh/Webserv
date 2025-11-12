#include "HttpSerializer.hpp"

namespace   {

    bool    read_file(const std::string& path, std::string& out)    {
        std::ifstream   in(path.c_str(), std::ios::in | std::ios::binary);
        if (!in)
            return false;
        std::string s;
        char        buf[4096];
        while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
            s.append(buf, static_cast<std::size_t>(in.gcount()));
        out.swap(s);
        return true;
    }

    std::string default_error_html(int status, const std::string& reason)   {
        std::ostringstream html;
        html << "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            << "<title>" << status << ' ' << reason << "</title>"
            << "<style>body{font-family:sans-serif;margin:2rem}"
               "h1{font-size:1.4rem;margin:0 0 .5rem}</style>"
            << "</head><body><h1>" << status << ' ' << reason << "</h1>"
            << "<p>The request could not be fulfilled.</p>"
            << "<hr><p>webserv</p></body></html>";  
        return html.str();      
    }

}

namespace http  {

    static std::string  http_date() {
        char        buf[128];
        std::time_t t = std::time(NULL);
        std::tm     g = *std::gmtime(&t);
        std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &g);
        return std::string(buf);
    }

    std::string build_simple_response(int status, const std::string& reason, const std::string& body, bool keep_alive)   {
        std::ostringstream  oss;
        oss << "HTTP/1.1 " << status << ' ' << reason << "\r\n";
        oss << "Server: webserv\r\n";
        oss << "Date: " << http_date() << "\r\n";
        oss << "Content-Length: " << body.size() << "\r\n";
        oss << "Content-Type: text/plain\r\n";
        if (keep_alive) {
            oss << "Connection: keep-alive\r\n";
            oss << "Keep-Alive: timeout=5\r\n";
        }
        else
            oss << "Connection: close\r\n";
        oss << "\r\n";
        oss << body;
        return oss.str();
    }

    std::string build_simple_response(int status, const std::string& reason, const std::string& body)   {
        return build_simple_response(status, reason, body, false);
    }

    std::string build_error_response(const Server& srv, int status, const std::string& reason, bool keep_alive) {
        // 1) decide body (configured file -> fallback default)
        std::string body;
        const char* content_type = "text/html";

        // status -> "404" etc.
        std::ostringstream  code_ss;
        code_ss << status;
        const std::string   code_str = code_ss.str();

        std::map<std::string, std::string>::const_iterator  it = srv.error_pages.find(code_str);
        if (it != srv.error_pages.end())    {
            if (!read_file(it->second, body))
                body = default_error_html(status, reason);
        }
        else
            body = default_error_html(status, reason);

        // 2) serialize response (mirrors the simple builder)
        std::ostringstream  oss;
        oss << "HTTP/1.1 " << status << ' ' << reason << "\r\n";
        oss << "Server: webserv\r\n";
        oss << "Date: " << http_date() << "\r\n";
        oss << "Content-Length: " << body.size() << "\r\n";
        oss << "Content-Type: " << content_type << "\r\n";
        if (keep_alive) {
            oss << "Connection: keep-alive\r\n";
            oss << "Keep-Alive: timeout=5\r\n";
        }
        else
            oss << "Connection: close\r\n";
        oss << "\r\n";
        oss << body;
        return oss.str();
    }

}