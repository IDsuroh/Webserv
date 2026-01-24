/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   HttpHeader.hpp                                     :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: hugo-mar <hugo-mar@student.42.fr>          +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2026/01/24 10:43:17 by hugo-mar          #+#    #+#             */
/*   Updated: 2026/01/24 10:43:18 by hugo-mar         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef HTTP_HEADER_HPP
#define HTTP_HEADER_HPP

#include "Headers.hpp"
#include "Structs.hpp"

namespace http  {

    bool        		parse_head(const std::string& head, HTTP_Request& request, int& status, std::string& reason);
	bool				extract_next_head(std::string& buffer, std::string& out_head);

}

#endif