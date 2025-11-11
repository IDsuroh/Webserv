#include "HttpSerializer.hpp"

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

}