#include "../include/Config.hpp"

// Print Structures Tester Function
static void	printConfig(const std::vector<Server>& servers)	{
	
	for (std::vector<Server>::size_type s = 0; s < servers.size(); ++s)	{
		const Server&	srv = servers[s];
		std::cout << "Server #" << s << ":\n";
	

		std::cout << "  listen:\n";
		for (std::vector<std::string>::size_type i = 0; i <srv.listen.size(); ++i)
			std::cout << "     - " << srv.listen[i] << std::endl;
	

		std::cout << "	server_name:\n";
		for (std::map<std::string, std::string>::const_iterator	it = srv.directives.begin();
			it != srv.directives.end(); ++it)
				std::cout << "	" << it->first << " -> " << it->second << std::endl;
		

		std::cout << "	locations:\n";
		for (std::vector<Location>::size_type l = 0; l < srv.locations.size(); ++l)	{
			const Location&	loc = srv.locations[l];
			std::cout << "	path = " << loc.path << std::endl;
			std::cout << "	directives:\n";
			for (std::map<std::string, std::string>::const_iterator	it = loc.directives.begin();
				it != loc.directives.end(); ++it)
					std::cout << "	" << it->first << " = " << it->second << std::endl;
		}

	}
	std::cout << std::endl;
}

int	main(int argc, char** argv) {
	if (argc != 2) {
		std::cerr << "Usage: " << argv[0] << " <config_file>\n";
		return 1;
	}

	Config config(argv[1]);
	const std::vector<Server>& servers = config.getServers();
	printConfig(servers);

	for (std::vector<Server>::size_type s = 0; s < servers.size(); ++s)	{
		std::cout
			<< "Server #" << s << " listens on:\n";
		
		const std::vector<std::string>& listenAddrs = servers[s].listen;
		for (std::vector<std::string>::size_type i = 0;
			i < listenAddrs.size(); ++i)	{
				std::cout
					<< "  - " << listenAddrs[i] << std::endl;
		}
	}

	return 0;
}


// int main()  {
    
//     const int   port = 8080;
//     int         server_fd = socket(AF_INET, SOCK_STREAM, 0);
//     if (server_fd < 0)  {
//         perror("socket");
//         return 1;
//     }

//     sockaddr_in addr;
//     std::memset(&addr, 0, sizeof(addr));
//     addr.sin_family = AF_INET;
//     addr.sin_addr.s_addr = INADDR_ANY;
//     addr.sin_port = htons(port);

//     if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0)    {
//         perror("bind");
//         return 1;
//     }

//     if (listen(server_fd, 1) < 0)   {
//         perror("listen");
//         return 1;
//     }

//     std::cout
//         << "Listening on port " << port << "...\n";

//     int client_fd = accept(server_fd, NULL, NULL);
//     if (client_fd < 0)  {
//         perror("accept");
//         return 1;
//     }

//     char    buffer[1024];
//     read(client_fd, buffer, sizeof(buffer));

//     const char* response = 
//         "HTTP/1.1 200 OK\r\n"
//         "Content-Length: 13\r\n"
//         "Content-Type: text/plain\r\n"
//         "\r\n"
//         "Hello, world!";
//     write(client_fd, response, strlen(response));

//     close(client_fd);
//     close(server_fd);

//     return 0;

// }