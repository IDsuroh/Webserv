/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   HttpSerializer.hpp                                 :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: suroh <suroh@student.42.fr>                +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2026/01/24 10:43:24 by hugo-mar          #+#    #+#             */
/*   Updated: 2026/01/24 14:10:14 by suroh            ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef HTTP_SERIALIZE_HPP
#define HTTP_SERIALIZE_HPP

#include "Headers.hpp"
#include "Structs.hpp"

namespace http  {
    std::string build_error_response(const Server& srv, int status, const std::string& reason, bool keep_alive);
    std::string serialize_response(const HTTP_Response& res, const std::string& version);
} // namespace http

#endif