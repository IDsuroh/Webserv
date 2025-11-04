#ifndef HTTP_SERIALIZE_HPP
#define HTTP_SERIALIZE_HPP

#include "Headers.hpp"
#include "Structs.hpp"

namespace http  {
    std::string build_simple_response(int status, const std::string& reason, const std::string& body);
} // namespace http

#endif