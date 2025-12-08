#ifndef APP_HPP
# define APP_HPP

# include "Headers.hpp"
# include "Structs.hpp"

HTTP_Response	handleRequest(const HTTP_Request& request, const std::vector<Server>& servers);

#endif