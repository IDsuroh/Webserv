/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   App.hpp                                            :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: hugo-mar <hugo-mar@student.42.fr>          +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2026/01/24 10:42:37 by hugo-mar          #+#    #+#             */
/*   Updated: 2026/01/24 10:42:39 by hugo-mar         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef APP_HPP
# define APP_HPP

# include "Headers.hpp"
# include "Structs.hpp"

HTTP_Response handleRequest(const HTTP_Request& req, const Server& activeServer);

#endif