#include "HttpSerialize.hpp"

namespace http  {

    std::string build_simple_response(int status, const std::string& reason, const std::string& body)   {
        std::ostringstream  oss;
        oss << "HTTP/1.1 " << status << ' ' << reason << "\r\n";
        oss << "Server: webserv\r\n";
        oss << "Constent-Length: " << body.size() << "\r\n";
        oss << "Content-Type: text/plain\r\n";
        oss << "Connection: close\r\n";
        oss << "\r\n";
        oss << body;
        return oss.str();
    }

}