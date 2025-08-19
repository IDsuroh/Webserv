#include "../include/ServerRunner.hpp"

ServerRunner::ServerRunner(const std::vector<Server>& servers)
    :   _servers(servers)
{}

//**************************************************************************************************


// Helper Error function
static void	printSocketError(const char* msg)	{
	std::cerr << msg << ": " << std::strerror(errno) << std::endl;
}


//**************************************************************************************************

void    ServerRunner::run() {
    
	setupListeners(_servers, _listeners);
    
	setupPollFds();

	if (_fds.empty())	{
		std::cerr << "No listeners configured/opened. \n";
		return;
	}
    
	while (true)    {
        int n = poll(&_fds[0], _fds.size(), -1); // points to the first entry of _fds.size() entries and blocks indefinitely.
        if (n < 0)  {
            printSocketError("poll");
            break;
        }
        handleEvents();
    }

}

//**************************************************************************************************

// Setting up Listeners functions
void	setupListeners(const std::vector<Server>& servers, std::vector<Listener>& outListeners)	{

	for (size_t s = 0; s < servers.size(); ++s)	{
		
		const Server&	srv = servers[s];
		
		for (size_t i = 0; i < srv.listen.size(); ++i)	{
			
			int	fd = openAndListen(srv.listen[i]);
            if (fd < 0)
                continue;
			Listener	L;
			L.fd = fd;
			L.config = &srv;
			outListeners.push_back(L);
            std::cout << "Listening on " << srv.listen[i] << " for server #" << s << std::endl << std::endl;
		
		}

	}

}

int openAndListen(const std::string& spec)  {
    
	std::string::size_type	colon = spec.find(':');
	std::string				host;
	std::string				port;

	if (colon == std::string::npos)	{
		host = "";
		port = spec;
	}
	else	{
		host = (colon == 0) ? "" : spec.substr(0, colon);
		port = (colon + 1 >= spec.size()) ? "" : spec.substr(colon + 1);
	}

	struct	addrinfo	hints;
	hints.ai_flags		= 0;
	hints.ai_family 	= AF_INET;
	hints.ai_socktype	= SOCK_STREAM;
	hints.ai_protocol	= 0;
	hints.ai_addrlen	= 0;
	hints.ai_addr		= NULL;
	hints.ai_canonname	= NULL;
	hints.ai_next		= NULL;

	if (host.empty() || host == "*")
		hints.ai_flags |= AI_PASSIVE;

	struct	addrinfo*	res	= NULL;
	int	rc = getaddrinfo((host.empty() || host == "*") ? NULL : host.c_str(),
						port.c_str(), &hints, &res);
	if (rc != 0)	{
		std::cerr << "getaddrinfo(" << spec << "): " << gai_strerror(rc) << std::endl;
		return -1;
	}

	int	sockfd = -1;

	for (struct addrinfo* p = res; p != NULL; p = p->ai_next)	{
		int	fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (fd < 0)
			continue;
		
		int	yes = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	
		int	flags = fcntl(fd, F_GETFL, 0);
		if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)	{
			printSocketError("fcntl");
			close(fd);
			continue;
		}

		if (bind(fd, p->ai_addr, p->ai_addrlen) == 0)	{
			if (listen(fd, SOMAXCONN) == 0)	{
				sockfd = fd;
				break;
			}
			else
				printSocketError("listen");
		}
		else
			printSocketError("bind");
		close(fd);
	}

	freeaddrinfo(res);
	return sockfd;

}

bool	makeNonBlocking(int fd)	{
	
	int	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)	{
		printSocketError("fcntl GETFL");
		return false;
	}
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)	{
		printSocketError("fcntl SETFL");
		return false;
	}
	return true;

}

//**************************************************************************************************

// Setting up Pollfds before running poll() => important process! registering each listening socket.
void    ServerRunner::setupPollFds()    {
    
	//_fds.clear();		=> might need for future use.
    for (size_t i = 0; i < _listeners.size(); i++)  {
        
		struct pollfd   p;

        p.fd = _listeners[i].fd;
        p.events = POLLIN;
        p.revents = 0;
        _fds.push_back(p);
		std::cout << "on position " << i << " => " <<_listeners[i].fd << " <- pollfd structure constructed\n";
    
	}

}

//**************************************************************************************************


void    ServerRunner::handleEvents()    {
    
	for (size_t i = 0; i < _fds.size(); ++i)    {
        
		int     fd = _fds[i].fd;
        short   re = _fds[i].revents;

        if (re & POLLIN)    {
            bool    isListener = false;
            const Server*   srv = NULL;
            
			for (size_t j = 0; j < _listeners.size(); ++j)  {
                if (_listeners[j].fd == fd) {
                    isListener = true;
                    srv = _listeners[j].config;
                    break;
                }
            }

            if (isListener)	// When one of the listening sockets becomes ready for a new connection.
                acceptNewClient(fd, srv);
            else
                readFromClient(fd);
				
        }
        else if (re & POLLOUT)
            writeToClient(fd);
        else if (re & (POLLERR | POLLHUP | POLLNVAL))
            closeConnection(fd);
    
	}

}

void	ServerRunner::acceptNewClient(int listenFd, const Server* srv)	{
	
	int	clientFd = accept(listenFd, NULL, NULL);
	
	if (clientFd < 0)	{
		printSocketError("accept");
		return;
	}
	makeNonBlocking(clientFd);

	Connection	connection;
	connection.fd = clientFd;
	connection.srv = srv;
	connection.readBuffer.clear();
	connection.writeBuffer.clear();
	connection.headersComplete = false;
	_connections[clientFd] = connection;

	struct pollfd	p;
	p.fd = clientFd;
	p.events = POLLIN;
	p.revents = 0;
	_fds.push_back(p);

}

void	ServerRunner::readFromClient(int clientFd)	{
	
	char	buffer[4096];
	ssize_t	n = read(clientFd, buffer, sizeof(buffer));
	if (n < 0)	{
		if (errno != EAGAIN && errno != EWOULDBLOCK)	{
			printSocketError("read");
			closeConnection(clientFd);
		}
		return ;
	}
	if (n == 0)	{
		closeConnection(clientFd);
		return ;
	}

	Connection& connection = _connections[clientFd];
	connection.readBuffer.append(buffer, n);

	if (!connection.headersComplete && connection.readBuffer.find("\r\n\r\n") != std::string::npos)	{
		connection.headersComplete = true;
		// TODO: parse HTTP request from conn.readBuf
        // TODO: generate HTTP response into conn.writeBuf

		std::string			body = "This is a TestRun... I need to make it more than this...";
		std::ostringstream	hdr;
			hdr	<<	"HTTP/1.1 200 OK\r\n"
				<<	"Content-Length: " << body.size() << "\r\n"
				<<	"Connection: close\r\n\r\n";
			
		connection.writeBuffer = hdr.str() + body;

		for (size_t i = 0; i < _fds.size(); ++i)	{
			if (_fds[i].fd == clientFd)	{
				_fds[i].events = POLLOUT;
				_fds[i].revents = 0;
				break;
			}
		}

	}

}

void	ServerRunner::writeToClient(int clientFd)	{
	
	Connection&	connection = _connections[clientFd];
	ssize_t	n = write(clientFd, connection.writeBuffer.c_str(), connection.writeBuffer.size());
	if (n < 0)	{
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			printSocketError("write");
			closeConnection(clientFd);
		}
		return ;
	}
	connection.writeBuffer.erase(0, n);
	if (connection.writeBuffer.empty())	{
		closeConnection(clientFd);
	}

}

void	ServerRunner::closeConnection(int clientFd)	{
	
	close(clientFd);
	_connections.erase(clientFd);

	for (size_t	i = 0; i < _fds.size(); ++i)	{
		if (_fds[i].fd == clientFd)	{
			_fds.erase(_fds.begin() + i);
			break;
		}
	}

}