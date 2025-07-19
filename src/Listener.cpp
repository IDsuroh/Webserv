#include "../include/Listener.hpp"

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