/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   HttpBody.hpp                                       :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: hugo-mar <hugo-mar@student.42.fr>          +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2026/01/24 10:43:07 by hugo-mar          #+#    #+#             */
/*   Updated: 2026/01/24 10:43:09 by hugo-mar         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef HTTP_BODY_HPP
#define HTTP_BODY_HPP

#include "Headers.hpp"
#include "Structs.hpp"

namespace http  {

    enum    BodyResult  {
        BODY_INCOMPLETE,
        BODY_COMPLETE,
        BODY_ERROR
    };

    BodyResult  consume_body_content_length(Connection& connection, std::size_t max_body, int& status, std::string& reason);
    BodyResult  consume_body_chunked(Connection& connection, std::size_t max_body, int& status, std::string& reason);

    // NEW: drain variants (consume bytes but do not store into request.body)
    BodyResult  consume_body_content_length_drain(Connection& connection, std::size_t max_body, int& status, std::string& reason);
    BodyResult  consume_body_chunked_drain(Connection& connection, std::size_t max_body, int& status, std::string& reason);

}   //namespace http

#endif