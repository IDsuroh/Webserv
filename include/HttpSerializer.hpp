#ifndef HTTP_SERIALIZE_HPP
#define HTTP_SERIALIZE_HPP

#include "Headers.hpp"
#include "Structs.hpp"

namespace http  {
    std::string build_simple_response(int status, const std::string& reason, const std::string& body);
    std::string build_simple_response(int status, const std::string& reason, const std::string& body, bool keep_alive);
    std::string build_error_response(const Server& srv, int status, const std::string& reason, bool keep_alive);
} // namespace http

#endif