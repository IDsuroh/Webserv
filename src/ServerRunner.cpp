#include "../include/ServerRunner.hpp"

ServerRunner::ServerRunner(const std::vector<Server>& servers)
    :   _servers(servers)
{}

//**************************************************************************************************

// Setting up Listeners functions
void    makeNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)  {
        perror("fcntl GETFL");
        std::exit(1);
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl SETFL");
        std::exit(1);
    }
}

int openAndListen(const std::string& spec)  {
    size_t      colon = spec.find(':');
    std::string ip = spec.substr(0, colon);
    int         port = std::atoi(spec.substr(colon + 1).c_str());

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        std::exit(1);
    }
    makeNonBlocking(sockfd);

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = (ip.empty() || ip == "*") ? INADDR_ANY : inet_addr(ip.c_str());

    if (bind(sockfd, (sockaddr*)&addr, sizeof(addr)) < 0)  {
        perror("bind");
        std::exit(1);
    }

    if (listen(sockfd, SOMAXCONN) < 0)  {
        perror("listen");
        std::exit(1);
    }

    return sockfd;
}

void	setupListeners(const std::vector<Server>& servers, std::vector<Listener>& outListeners)	{

	for (size_t s = 0; s < servers.size(); ++s)	{
		const Server&	srv = servers[s];
		for (size_t i = 0; i < srv.listen.size(); ++i)	{
			int	fd = openAndListen(srv.listen[i]);
			Listener	L;
			L.fd = fd;
			L.config = &srv;
			outListeners.push_back(L);
            std::cout << "Listening on " << srv.listen[i] << " for server #" << s << std::endl << std::endl;
		}
	}
}

//**************************************************************************************************



void    ServerRunner::run() {
    setupListeners(_servers, _listeners);
    setupPollFds();
    while (true)    {
        int n = poll(&_fds[0], _fds.size(), -1);
        if (n < 0)  {
            perror("poll");
            break;
        }
        handleEvents();
    }
}



void    ServerRunner::setupPollFds()    {
    _fds.clear();
    for (size_t i = 0; i < _listeners.size(); i++)  {
        struct pollfd   p;
        p.fd = _listeners[i].fd;
        p.events = POLLIN | POLLOUT;
        p.revents = 0;
        _fds.push_back(p);
    }
}


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
            if (isListener)
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