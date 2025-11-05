#ifndef HTTP_PARSER_HPP
#define HTTP_PARSER_HPP

#include "Headers.hpp"
#include "Structs.hpp"

namespace http  {

    bool        		parse_head(const std::string& head, HTTP_Request& request, int& status, std::string& reason);
	bool				extract_next_head(std::string& buffer, std::string& out_head);

}

#endif