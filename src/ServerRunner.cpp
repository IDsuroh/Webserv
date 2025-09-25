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

	std::map<std::string, int> specToFd;
	
	for (std::size_t s = 0; s < servers.size(); ++s)	{
		const Server&	srv = servers[s];
		
		for (std::size_t i = 0; i < srv.listen.size(); ++i)	{
			const std::string& spec = srv.listen[i];
			
			int	fd;
            std::map<std::string, int>::iterator it = specToFd.find(spec);
			if (it != specToFd.end())
				fd = it->second;
			else {
				fd = openAndListen(spec);
				if (fd < 0)
					continue;
				specToFd[spec] = fd;
			}
			
			Listener	L;
			L.fd = fd;
			L.config = &srv;
			outListeners.push_back(L);
            std::cout	<< "Listening on " << srv.listen[i] << " for server #"
						<< s << std::endl << std::endl;
		
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

	struct	addrinfo	hints; // recipe for what type of addresses we want
	hints.ai_flags		= 0;
	hints.ai_family 	= AF_INET; // IPv4 addresses only
	hints.ai_socktype	= SOCK_STREAM; // TCP connections only
	hints.ai_protocol	= 0; // default protocol -> default for that type -> TCP
	hints.ai_addrlen	= 0;
	hints.ai_addr		= NULL;
	hints.ai_canonname	= NULL;
	hints.ai_next		= NULL;

	if (host.empty() || host == "*")
		hints.ai_flags |= AI_PASSIVE; // Listening on all available network interfaces

	struct	addrinfo*	res	= NULL;
	int	rc = getaddrinfo((host.empty() || host == "*") ? NULL : host.c_str(), // make it socket-compatible
						port.c_str(), &hints, &res); // this function instanciates/builds a linked list of addresses
	if (rc != 0)	{
		std::cerr << "getaddrinfo(" << spec << "): " << gai_strerror(rc) << std::endl;
		return -1; // get address info str error converts errors into human readable message
	}

	int	sockfd = -1;

	for (struct addrinfo* p = res; p != NULL; p = p->ai_next)	{
		int	fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol); // like buying a phone machine
		if (fd < 0)	{
			printSocketError("socket");
			continue;
		}
		
		const int	enable = 1;
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
			printSocketError("setsockopt SO_REUSEADDR");
		// For this socket fd, go to the socket menu (SOL_SOCKET) and turn on (1) the REUSEADDR setting (SO_REUSEADDR).
		// SO_REUSEADDR, allows immediate re-binding, but without it, binding might fail due to the old connection still being in TIME_WAIT.
		
		if (!makeNonBlocking(fd))	{
			close(fd);
			continue;
		}

		if (bind(fd, p->ai_addr, p->ai_addrlen) == 0)	{ // plugging the phone into a specific wall jack: a local (IP, port)
			if (listen(fd, SOMAXCONN) == 0)	{ // making the phone ready to accept calls; keeping a waiting line
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

#ifdef __APPLE__
	if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1)	{ // fcntl = file control, changes properties of an open file descriptor.
		printSocketError("fcntl F_SETFL O_NONBLOCK"); // int fcntl(int fd, int cmd, ... /* arg */);
		return false;
	}

#else
	int	flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)	{
		printSocketError("fcntl F_GETFL");
		return false;
	}
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)	{ // If flags = 0b0010 and O_NONBLOCK = 0b0100 → newFlags = 0b0110
		printSocketError("fcntl F_SETFL O_NONBLOCK");
		return false;
	}

#endif
	return true;

}

//**************************************************************************************************

// Setting up Pollfds before running poll() => important process! registering each listening socket.
void    ServerRunner::setupPollFds()    {
    
	_fds.clear();
	std::set<int> added;

    for (std::size_t i = 0; i < _listeners.size(); i++)  {
        int fd = _listeners[i].fd;
		if (added.find(fd) != added.end())
			continue;
		
		struct pollfd   p;

        p.fd = _listeners[i].fd;
        p.events = POLLIN;
        p.revents = 0;
        _fds.push_back(p);
		added.insert(fd);
		
		std::cout << "on position " << i << " => " <<_listeners[i].fd << " <- pollfd structure constructed\n";
    
	}

}

//**************************************************************************************************


void    ServerRunner::handleEvents()    {
    
	for (std::size_t i = _fds.size(); i-- > 0; )    {
        
		int     fd = _fds[i].fd;
        short   re = _fds[i].revents;

		if (re == 0)
			continue;

		if (re & (POLLERR | POLLHUP | POLLNVAL))	{
			closeConnection(fd);
			continue;
		}
		
        bool    isListener = false;
        const Server*   srv = NULL;
		for (size_t j = 0; j < _listeners.size(); ++j)  {
        	if (_listeners[j].fd == fd) {
            	isListener = true;
                srv = _listeners[j].config;
                break;
            }
        }

        if (isListener)	{ // When one of the listening sockets becomes ready for a new connection.
            if (re & POLLIN)    
				acceptNewClient(fd, srv);
			continue;
		}
		if (re & POLLIN)
			readFromClient(fd);
		if (re & POLLOUT)
			writeToClient(fd);
	}

}

void	ServerRunner::acceptNewClient(int listenFd, const Server* srv)	{
	
	for (;;) {
		int	clientFd = accept(listenFd, NULL, NULL);
		
		if (clientFd < 0)	{
			printSocketError("accept");
			return;
		}
		if (!makeNonBlocking(clientFd)) {
			close(clientfd);
			continue;
		}

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
	
}

void	ServerRunner::readFromClient(int clientFd)	{
	
	std::map<int, Connection>::iterator it = _connections.find(clientFd);
	if (it == _connections.end())
		return;

	Connection& connection = it->second;

	char	buffer[4096];

	while (true)	{
		ssize_t	n = read(clientFd, buffer, sizeof(buffer));
		if (n > 0)	{
			connection.readBuffer.append(buffer, static_cast<std::size_t>(n));
			continue;
		}
		if (n == 0)	{
			closeConnection(clientFd);
			return ;
		}
		break;
	}

	if (!connection.headersComplete
		&& connection.readBuffer.find("\r\n\r\n") != std::string::npos)	{
		connection.headersComplete = true;
		// TODO: parse HTTP request from conn.readBuf
        // TODO: generate HTTP response into conn.writeBuf

		std::string			body = "This is a TestRun... I need to make it more than this...";
		std::ostringstream	hdr;
			hdr	<<	"HTTP/1.1 200 OK\r\n"
				<<	"Content-Length: " << body.size() << "\r\n"
				<<	"Connection: close\r\n\r\n";
			
		connection.writeBuffer = hdr.str() + body;

		for (std::size_t i = 0; i < _fds.size(); ++i)	{
			if (_fds[i].fd == clientFd)	{
				_fds[i].events = POLLOUT;
				break;
			}
		}

	}

}

void	ServerRunner::writeToClient(int clientFd)	{
	
	std::map<int, Connection>::iterator it = _connections.find(clientFd);
	if (it == _connections.end())
		return;
	Connection& connection = it->second;
	
	while (!connection.writeBuffer.empty())	{

		const char*	data = connection.writeBuffer.c_str();
		std::size_t	len = connection.writeBuffer.size();

		ssize_t	n = write(clientFd, data, len);
		if (n > 0)	{
			connection.writeBuffer.erase(0, static_cast<std::size_t>(n));
			continue;
		}
		if (n == 0)	{
			return;
		}
		return;
	}
	closeConnection(clientFd);

}

void	ServerRunner::closeConnection(int clientFd)	{
	
	close(clientFd);
	_connections.erase(clientFd);

	for (std::size_t	i = 0; i < _fds.size(); ++i)	{
		if (_fds[i].fd == clientFd)	{
			_fds.erase(_fds.begin() + i);
			break;
		}
	}

}
