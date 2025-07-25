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
    size_t      colon = spec.find(':');
    std::string ip = spec.substr(0, colon);
    int         port = std::atoi(spec.substr(colon + 1).c_str());

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        printSocketError("socket");
        std::exit(1);
    }
    makeNonBlocking(sockfd);

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = (ip.empty() || ip == "*") ? INADDR_ANY : inet_addr(ip.c_str());

    if (bind(sockfd, (sockaddr*)&addr, sizeof(addr)) < 0)  {
        printSocketError(("bind " + spec).c_str());
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, SOMAXCONN) < 0)  {
        printSocketError(("listen " + spec).c_str());
        close(sockfd);
        return -1;
    }

    return sockfd;
}

void    makeNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);		// 1) Grab the socket’s current “status flags”
    if (flags < 0)  {
        printSocketError("fcntl GETFL");
        std::exit(1);
    }
	int	newFlag = flags | O_NONBLOCK;		// 2) Add (bitwise-OR) the non-blocking flag
    if (fcntl(fd, F_SETFL, newFlag) < 0) {	// 3) Write that back to the socket
        printSocketError("fcntl SETFL");
        std::exit(1);
    }
}

//**************************************************************************************************

// Setting up Pollfds before running poll() => important process! registering each listening socket.
void    ServerRunner::setupPollFds()    {
    //_fds.clear();		=> might need for future use.
    for (size_t i = 0; i < _listeners.size(); i++)  {
        struct pollfd   p;
        p.fd = _listeners[i].fd;
        p.events = POLLIN | POLLOUT;
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
		connection.headersComplete = false;
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