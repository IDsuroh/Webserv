#include "../include/Config.hpp"
#include "../include/ServerRunner.hpp"

// Print Structures Tester Function
/*static void	printConfig(const std::vector<Server>& servers)	{

	for (size_t s = 0; s < servers.size(); ++s)	{
		const Server&	srv = servers[s];
		std::cout << "Server #" << s << ":\n";


		std::cout << "  listen:\n";
		for (size_t i = 0; i <srv.listen.size(); ++i)
			std::cout << "     - " << srv.listen[i] << std::endl;


		std::cout << "	server_name:\n";
		for (size_t i = 0; i < srv.server_name.size(); ++i)
			std::cout << "     - " << srv.server_name[i] << std::endl;


		std::cout << "	directives:\n";
		for (std::map<std::string, std::string>::const_iterator it = srv.directives.begin();
			it != srv.directives.end(); ++it)
			std::cout << "		" << it->first << " -> " << it->second << std::endl;

		
		std::cout << "	error_pages:\n";
		for (std::map<std::string, std::string>::const_iterator it = srv.error_pages.begin();
			it != srv.error_pages.end(); ++it)
			std::cout << "		" << it->first << " -> " << it->second << std::endl;


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
}*/

int	main(int argc, char** argv) {

	if (argc != 2) {
		std::cerr << "Usage: " << argv[0] << " <config_file>\n";
		return 1;
	}

	signal(SIGPIPE, SIG_IGN);
		// ignore SIGPIPE signal and sets errno = EPIPE, don’t crash if a peer vanishes mid-write
		// SIGPIPE = a signal raised when writing to a broken pipe/socket. Default kills the process.
		// SIG_IGN means "Ignore the signal"
		// EPIPE = the error code write() returns when SIGPIPE is ignored (or suppressed) and the peer is closed.

	try	{

		Config	config(argv[1]);
		const std::vector<Server>&	servers = config.getServers();

		//printConfig(servers);

		ServerRunner	runner(servers);
		runner.run();
	}
	catch(const std::exception& e)	{
		std::cerr << "Fatal error: " << e.what() << '\n';
		return 1;
	}
	catch(...)	{
		std::cerr << "Fatal error: unknown exception\n";
		return 1;
	}
	
	return 0;
}