#include "../include/Config.hpp"
#include "../include/ServerRunner.hpp"

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

	ServerRunner	runner(servers);
	runner.run();

	return 0;
}