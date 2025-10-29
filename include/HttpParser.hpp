#ifndef HTTP_PARSER_HPP
#define HTTP_PARSER_HPP

#include "Headers.hpp"
#include "Structs.hpp"

namespace http  {
    std::size_t find_header_terminator(const std::string& buf);
    bool        parse_head(const std::string& head, HTTP_Request& request, int& status, std::string& reason);
}

#endif