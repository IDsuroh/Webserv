#include "../include/ServerRunner.hpp"
#include "../include/HttpParser.hpp"
#include "../include/HttpSerializer.hpp"
#include "../include/HttpBody.hpp"

ServerRunner::ServerRunner(const std::vector<Server>& servers)
    :   _servers(servers), _nowMs(0)
{}

//**************************************************************************************************


// Helper Error function
static void	printSocketError(const char* msg)	{
	std::cerr << msg << ": " << std::strerror(errno) << std::endl;
}


//**************************************************************************************************

void	ServerRunner::housekeeping()	{
	const long	NOW =  _nowMs;

	const long	HEADER_TIMEOUT_MS	= 15000;	// 15s to receive headers
	const long	BODY_TIMEOUT_MS		= 30000;	// 30s to receive body
	const long	KA_IDLE_MS			= 5000;		// 5s idle on keep-alive

	// Sweep connections (close stale ones)
	for (std::map<int, Connection>:: iterator	it = _connections.begin(); it != _connections.end();)	{

		int			fd = it->first;
		Connection& connection = it->second;
		bool		closeIt = false;
	
		switch (connection.state)	{
			case	S_HEADERS:
					// 15s to deliver (next) headers
					const bool	headerTimeOut = (NOW - connection.lastActiveMs > HEADER_TIMEOUT_MS);
					// 5s only when we *already* finished a KA response and are waiting for the next request
					const bool	kaIdleTooLong = (connection.kaIdleStartMs != 0) && (NOW - connection.kaIdleStartMs > KA_IDLE_MS);
					if (headerTimeOut || kaIdleTooLong)
						closeIt = true;
					break;
			case	S_BODY:
				if (NOW - connection.lastActiveMs > BODY_TIMEOUT_MS)
					closeIt = true;
				break;
			default:
				break;
		}

		if (closeIt)	{
			++it;
			closeConnection(fd);
		}
		else
			++it;
	}
}

void    ServerRunner::run() {
    
	setupListeners(_servers, _listeners);
    
	setupPollFds();

	if (_fds.empty())	{
		std::cerr << "No listeners configured/opened. \n";
		return;
	}
    
	const int	POLL_TICK_MS = 250;

	while (true)    {
        int n = poll(_fds.empty() ? 0 : &_fds[0], static_cast<nfds_t>(_fds.size()), POLL_TICK_MS);
		// points to the first entry of _fds.size() entries and blocks for up to POLL_TICK_MS (or until an event arrives).
        if (n < 0)  {
			if (errno == EINTR)
				continue;
			printSocketError("poll");
            break;
        }
		if (n == 0)	{
			// No events for a full tick
			_nowMs += POLL_TICK_MS;
			housekeeping();
			continue;
		}
        
		// Handle events first
		handleEvents();
		// Run periodic maintenance once per tick (timeout or after events):
		housekeeping();	// close slow headers/body, idle keep-alive, etc.
    }
	// setupListeners(): opens listening sockets (via openAndListen) and fills _listeners.
	//                   It deduplicates equivalent specs so the same IP:port is opened once.
	// setupPollFds(): registers ONE pollfd per unique listening fd in _fds (events=POLLIN).
	// Event loop:
	//   - poll() sleeps up to POLL_TICK_MS, or wakes sooner on I/O.
	//   - if a listener fd is readable, handleEvents() accepts and adds the client fd to _fds.
	//   - if a client needs to write, handleEvents() flips its poll events to POLLOUT.
	//   - housekeeping() runs once per tick to enforce header/body/keep-alive timeouts and
	//     close stale connections (requires lastActiveMs to be updated on activity).

}

//**************************************************************************************************

static bool	parseListenToken(const std::string& spec, std::string& host, int& port)	{
	
	const int	DEFAULT_PORT = 80;

	std::string	pstr;	// candidate port string (maybe empty)
	std::size_t	colon = spec.find(':');

	if (colon == std::string::npos)	{
		// No colon -> either "8080" (all digits) or "host"
		bool	allDigits = !spec.empty();
		for (std::size_t i = 0; i < spec.size(); ++i)	{
			unsigned char	uc = static_cast<unsigned char>(spec[i]);
			if (!std::isdigit(uc))	{
				allDigits = false;
				break;
			}
		}

		if (allDigits)	{
			host = "";
			pstr = spec;
		}	// port-only "8080"
		else	{
			host = spec;
			pstr.clear();
		} // host-only → default port
	}
	else	{
		// Has colon -> split into host + (maybe-empty) port
		host = (colon == 0) ? "" : spec.substr(0, colon);
		pstr = (colon + 1 < spec.size()) ? spec.substr(colon + 1) : "";
	}

	if (pstr.empty())
		port = DEFAULT_PORT;
	else	{
		char*	endptr = 0;
		long	v = std::strtol(pstr.c_str(), &endptr, 10);
		if (endptr == pstr.c_str() || *endptr != '\0' || v < 1 || v > 65535)
			return false;	// invalid port, no junk values, no alphabets, within port values
		port = static_cast<int>(v);
	}

	// Normalize wildcard/empty host
	if (host.empty() || host == "*")
		host = "0.0.0.0";

	return true;
}

static std::string	normalizeListenKey(const std::string& spec)	{
	std::string	host;
	int			port;
	if (!parseListenToken(spec, host, port))
		return spec;
	std::ostringstream	oss;
	oss << host << ":" << port;
	return oss.str();
}

// Setting up Listeners functions
void	setupListeners(const std::vector<Server>& servers, std::vector<Listener>& outListeners)	{

	outListeners.clear();

	std::map<std::string, int> specToFd;
	// To prevent opening the same IP:port more than once because Servers can share the same IP:ports.
	
	for (std::size_t s = 0; s < servers.size(); ++s)	{ // server blocks
		const Server&	srv = servers[s];
		
		for (std::size_t i = 0; i < srv.listen.size(); ++i)	{ // each server's listen entries (server can listen on multiple specifications).
			const std::string&	spec = srv.listen[i];
			const std::string	key = normalizeListenKey(spec);
			
            std::map<std::string, int>::iterator it = specToFd.find(key);
			if (it != specToFd.end())	{
				// Duplicate listen in another server{} — reuse the same socket, do NOT add another Listener row
				std::cerr << " Note: Duplicate listen \"" << spec << "\" in server #" << s << " - reusing " << key << std::endl;
				continue;
			}

			int	fd = openAndListen(spec); // use the original spec for getaddrinfo
            if (fd < 0) {
				std::cerr << "Warning: Failed to open listen \"" << spec << "\"\n";
				continue;
			}

			specToFd[key] = fd;

			//	_listeners array populated
			Listener	L;
			L.fd = fd;
			L.config = &srv;
			outListeners.push_back(L);	// Adds to _listeners array
            std::cout	<< "Listening on " << srv.listen[i] << " for server #" << s << "\n\n";
		}

	}

}

int openAndListen(const std::string& spec)  {

	std::size_t		colon = spec.find(':');	// spec.find() returns the index position
	std::string		host;
	std::string		port;

	if (colon == std::string::npos)	{
		host = "";
		port = spec;
	}
	else	{
		host = (colon == 0) ? "" : spec.substr(0, colon);
		port = (colon + 1 >= spec.size()) ? "" : spec.substr(colon + 1);
	}

	struct	addrinfo	hints;	// recipe for what type of addresses we want
	hints.ai_flags		= 0;	// Behavioral Flags (hints.ai_flags = 0; initially no special behavior)
	hints.ai_family 	= AF_INET;	// IPv4 addresses only
	hints.ai_socktype	= SOCK_STREAM;	// TCP connections only
	hints.ai_protocol	= 0;	// default protocol -> default for that type -> TCP
	hints.ai_addrlen	= 0;	// address length => let getaddrinfo() fill this section
	hints.ai_addr		= NULL;	// socket address => should be set to NULL for getaddrinfo() to fill
	hints.ai_canonname	= NULL;	// official hostname
	hints.ai_next		= NULL;

	if (host.empty() || host == "*")
		hints.ai_flags |= AI_PASSIVE; // Listening on all available network interfaces

	struct	addrinfo*	res	= NULL;
	int	rc = getaddrinfo((host.empty() || host == "*") ? NULL : host.c_str(), // make it socket-compatible
						port.c_str(), &hints, &res); // this function instanciates/builds a linked list of addresses -> addrinfo nodes which represent candidate local address to bind.
	if (rc != 0)	{
		std::cerr << "getaddrinfo(" << spec << "): " << gai_strerror(rc) << std::endl;
		return -1; // get address info str error converts errors into human readable message
	}

	int	sockfd = -1;

	for (struct addrinfo* p = res; p != NULL; p = p->ai_next)	{
		int	fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol); // like buying a phone machine
		// creating an endpoint so that there is one end of a communication channel to send or receive data over a network.
		if (fd < 0)	{
			printSocketError("socket");
			continue;
		}

		#ifdef __APPLE__
		{
		/*
		    macOS (42 rules): DO NOT set FD_CLOEXEC here.
		    Allowed fcntl flags are limited to F_SETFL + O_NONBLOCK, so we skip F_SETFD.

		    Consequence:
		      - Accepted sockets (clientFd) will inherit across execve() on macOS.

		    TODO(CGI child, before execve):
		      - In the forked CHILD path of the CGI launcher:
		          * Keep only 0,1,2 and the CGI pipe ends (stdin/stdout as dup2 targets).
		          * Close EVERYTHING else: all listener fds, all other client fds,
		            any extra poll/kqueue/pipe fds that were opened.
		      - This prevents fd leaks and ensures CGI doesn’t keep sockets alive.

		    Where to implement:
		      - In launchCgi(...) right after fork(), inside the CHILD branch,
		        under #ifdef __APPLE__.

		    Rationale:
		      - On Linux we use FD_CLOEXEC so execve() auto-closes unrelated fds.
		      - On macOS we can’t set FD_CLOEXEC per subject; we must close manually in the child.
		*/
		}
		#else
		{
			// close-on-exec for the listening socket as well
			int fdflags = fcntl(fd, F_GETFD);
			if (fdflags != -1 || fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC) == -1)
			        printSocketError("fcntl F_SETFD FD_CLOEXEC");
		}
		#endif

		const int	enable = 1;
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
			printSocketError("setsockopt SO_REUSEADDR");
		// For this socket fd, go to the socket menu (SOL_SOCKET) and turn on (1) the REUSEADDR setting (SO_REUSEADDR).
		// SO_REUSEADDR, allows immediate re-binding, but without it, binding might fail due to the old connection still being in TIME_WAIT. Is a safety method.

		if (!makeNonBlocking(fd))	{
			close(fd);
			continue;
		}

		if (bind(fd, p->ai_addr, p->ai_addrlen) == 0)	{ // plugging the phone into a specific wall jack: a local (IP, port)
			// attaches the socket (the fd) to a local endpoint = (local IP:port)
			if (listen(fd, SOMAXCONN) == 0)	{ // making the phone ready to accept calls; keeping a waiting line, backlog is SOMAXCONN which is the maximum number of connections.
				sockfd = fd; // listen makes the fd into a listening TCP socket and associates SYN queue and Accept queue
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

	// it is the listen() function that actually makes a queue of:
	//	SYN (half-open) queue: connections that have started the TCP 3-way handshake (SYN/SYN-ACK/ACK) but aren’t done yet.
	//	Accept (fully-established) queue: connections whose handshake finished. These are ready for the process to accept().
	// check last notes of the file to read more about TCP handshake.
}

bool	makeNonBlocking(int fd)	{

#ifdef __APPLE__
	if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1)	{ // fcntl = file control, changes properties of an open file descriptor.
		printSocketError("fcntl F_SETFL O_NONBLOCK"); // int fcntl(int fd, int cmd, ... /* arg */);
		return false;
	}

#else
	int	flags = fcntl(fd, F_GETFL, 0); // the return value is the current status flags bitmask for that fd.
	if (flags == -1)	{
		printSocketError("fcntl F_GETFL");
		return false;
	}
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)	{ // If flags = 0b0010 and O_NONBLOCK = 0b0100 → newFlags = 0b0110
		// To preserve the previous flags. so we don’t accidentally turn OFF other behaviors that were intentionally enabled on that fd.
		printSocketError("fcntl F_SETFL O_NONBLOCK");
		return false;
	}

#endif
	return true;

	// F_SETFL => set file status flag. Returns a bitmask containing:
	//		the access mode (read-only, write-only, read-write)
	// 		status flags like O_NONBLOCK, O_APPEND, O_SYNC, O_ASYNC, …
	// F_GETFL => get file status flag.
	//		can turn on/off flags such as O_NONBLOCK or O_APPEND.
	// O_NONBLOCK => “If I did this now, I’d have to wait (block), but you asked me not to. Come back when it’s ready.”
	//		if (errno == EAGAIN || errno == EWOULDBLOCK) { /* not an error; try later */ }
}

//**************************************************************************************************

// Register one pollfd per unique listening socket (startup only).
// NOTE: _fds.clear() assumes there are no client pollfds yet.
// Can never call this after clients have been added to _fds.
void    ServerRunner::setupPollFds()    { // only for listening sockets. setupPollFds() = listeners only (startup). Clients get added as they connect.

    _fds.clear();                        // to have a clean list on multiple calls
	_fdIndex.clear();
    _fds.reserve(_listeners.size());

	std::set<int> added;
	// We can have multiple Listener records pointing to the same underlying fd (e.g., two server {} blocks both listening on 127.0.0.1:8080).
	// make sure only deduplicates happen. Deduplicate => having no duplicates of the same fd.
	// Deduplicate by FD: many _listeners can share the same fd which was populated by setupListeners().
	// (virtual hosts on the same ip:port), but poll() needs exactly ONE pollfd per unique fd.
    for (std::size_t i = 0; i < _listeners.size(); i++)  {
        int fd = _listeners[i].fd;
		if (!added.insert(fd).second)
			continue;

		struct pollfd   p; // tells the kernel which fd and what events we want
        p.fd = _listeners[i].fd;
        p.events = POLLIN; // "What to expect"
        p.revents = 0; // the poll() is the function that fills in the revents to tell what exactly happened.
        _fds.push_back(p);
		_fdIndex[p.fd] = _fds.size() - 1;

		std::cout << "listener[" << i << "] fd=" << fd << " registered in poll()\n";

	}
	// this process is important after opening the listening socket because this is the part where we are telling the poll()
	//		what to do and what to expect. setupPollFds() builds _fds with one pollfd per unique listening fd and sets events = POLLIN
	//		“kernel, wake me when this listener is readable (i.e., there’s a connection to accept).”
	// Without setupPollFds() => poll() would have nothing (or the wrong things) to watch and the loop would never wake for new connections => accept() would never run.
}

//**************************************************************************************************


void    ServerRunner::handleEvents()    {

	for (std::size_t i = _fds.size(); i-- > 0; )    {
		// Iterate backward because we modify _fds inside this loop:
		// - closeConnection() erases entries -> backward traversal avoids index shifts skipping items. *main reason.
		// - acceptNewClient() push_back()s new entries -> they won’t be (accidentally) handled this pass. => avoid wasted iterations
        // Example:
		//		_fds = [ L0, L1 ] (size = 2)
		//		i = 0 → L0 has POLLIN → acceptNewClient() accepts 2 clients → push_back [ C2, C3 ].
		//		_fds is now [ L0, L1, C2, C3 ] (size = 4).
		//	Forward loop continues: i = 1 (L1), then i = 2 (C2), i = 3 (C3).
		//	They have revents=0, so they should be skipped but they are still iterated.
		//	Backward loop avoids that: it starts at the last index from the original size and never touches things appended during this pass.
		// Also will skip on situations where we delete one index.

		int     fd = _fds[i].fd;
        short   re = _fds[i].revents;

		// 1. Skip if nothing happened
		if (re == 0)
			continue;

		// 2. Check for errors first
		if (re & (POLLERR | POLLHUP | POLLNVAL))	{
			closeConnection(fd);
			continue;
		}
		/*
		POLLERR = "Error condition" (socket error, network problem)
		POLLHUP = "Hang up" (peer closed the connection)
		POLLNVAL = "Invalid request" (fd is not open, invalid fd)
		*/

		// 3. Figure out what type of socket this is
        bool    		isListener = false;
        const Server*   srv = NULL;
		for (size_t j = 0; j < _listeners.size(); ++j)  {
        	if (_listeners[j].fd == fd) {
            	isListener = true;
                srv = _listeners[j].config;
                break;
            }
        }

		// 4. Handle listener sockets
        if (isListener)	{
            if (re & POLLIN)
				acceptNewClient(fd, srv); 	// New connection waiting
			continue;	// Skip client handling for listeners
		}

		// 5. Handle client sockets
		if (re & POLLIN)
			readFromClient(fd);	// HTTP request data
		if (re & POLLOUT)
			writeToClient(fd);	// Send HTTP response
	}

	// Checks on bitwise operations of the revents of the pollfd struct.
	/*
	What POLLIN means:
		For listeners: "New connection waiting to be accepted"
		For clients: "HTTP request data arrived"

	What POLLOUT means:
		"Socket buffer has space - you can write() without blocking"
		"Kernel is ready to accept more bytes for transmission"
	*/
}

void	ServerRunner::acceptNewClient(int listenFd, const Server* srv)	{

	for (;;) {
		// If accept() is only called once, there would be extra ready connections sitting in the accept queue,
		// forcing another immediate poll() wakeup. It’s more efficient to drain the queue now.
		int	clientFd = accept(listenFd, NULL, NULL);
		// accept() takes one fully-established connection off the listener’s accept queue and returns a new fd dedicated to that client

		if (clientFd < 0)	{
			if (errno == EINTR) // “The system call was interrupted by a signal.”
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) // “There are no more connections to accept right now.”
				break;
			printSocketError("accept");
			break;
		}

		#ifdef __APPLE__
		{
		/*
		    macOS (42 rules): DO NOT set FD_CLOEXEC here.
		    Allowed fcntl flags are limited to F_SETFL + O_NONBLOCK, so we skip F_SETFD.

		    Consequence:
		      - Accepted sockets (clientFd) will inherit across execve() on macOS.

		    TODO(CGI child, before execve):
		      - In the forked CHILD path of the CGI launcher:
		          * Keep only 0,1,2 and the CGI pipe ends (stdin/stdout as dup2 targets).
		          * Close EVERYTHING else: all listener fds, all other client fds,
		            any extra poll/kqueue/pipe fds that were opened.
		      - This prevents fd leaks and ensures CGI doesn’t keep sockets alive.

		    Where to implement:
		      - In launchCgi(...) right after fork(), inside the CHILD branch,
		        under #ifdef __APPLE__.

		    Rationale:
		      - On Linux we use FD_CLOEXEC so execve() auto-closes unrelated fds.
		      - On macOS we can’t set FD_CLOEXEC per subject; we must close manually in the child.
		*/
		}
		#else
		{
			// close-on-exec for the listening socket as well
			int fdflags = fcntl(clientFd, F_GETFD);
			if (fdflags != -1)	{
			    if (fcntl(clientFd, F_SETFD, fdflags | FD_CLOEXEC) == -1)
			        printSocketError("fcntl F_SETFD FD_CLOEXEC");
			}
		}
		#endif

		if (!makeNonBlocking(clientFd)) {
			close(clientFd);
			continue;
		}

		Connection	connection;
		connection.fd = clientFd;
		connection.srv = srv;
		connection.listenFd = listenFd;
		connection.readBuffer.clear();
		connection.writeBuffer.clear();
		connection.headersComplete = false;
		connection.requestParsed = false;
		connection.state = S_HEADERS;
		connection.request.body.clear();
		connection.request.body_received = 0;
		connection.request.chunk_state = CS_SIZE;
		connection.request.chunk_bytes_left = 0;
		connection.writeOffset = 0;
		connection.clientMaxBodySize = (1u << 20);
		connection.kaIdleStartMs = 0;
		connection.lastActiveMs = _nowMs;
			// Bit shift: 1u (unsigned 1) shifted 20 bits → 1,048,576 bytes (1 MiB) default.
		_connections[clientFd] = connection;

		struct pollfd	p;
		p.fd = clientFd;
		p.events = POLLIN;
		p.revents = 0;
		_fds.push_back(p);
		_fdIndex[p.fd] = _fds.size() - 1;
		// Add the new client fd to the poll() std::vector,
		// initially watching for readability (request bytes).
		// This is what lets poll() wake again when the client sends the HTTP request.

	}

}

static size_t	parseSize(const std::string& string)	{
	unsigned long long	n = 0;
	char				suf = 0;
	std::istringstream	iss(string);
	iss >> n;
	if (iss && !iss.eof())
		iss >> suf;
	
	unsigned long long	mult = 1;
	if (suf == 'k' || suf == 'K')
		mult = 1024ULL;
	else if (suf == 'm' || suf == 'M')
		mult = 1024ULL * 1024ULL;
	else if (suf == 'g' || suf == 'G')
		mult = 1024ULL * 1024ULL * 1024ULL;
	return static_cast<size_t>(n * mult);
}

static const Location*	longestPrefixMatch(const Server& srv, const std::string& path)	{
	const Location*	best = NULL;
	size_t			bestLen = 0;
	for (size_t i = 0; i < srv.locations.size(); ++i)	{
		const Location&	L = srv.locations[i];
		if (path.compare(0, L.path.size(), L.path) == 0 && L.path.size() > bestLen)	{
			best = &L;
			bestLen = L.path.size();
		}
	}
	return best;
}

void	ServerRunner::readFromClient(int clientFd)	{

	std::map<int, Connection>::iterator it = _connections.find(clientFd);
	if (it == _connections.end())
		return;

	Connection& connection = it->second;

	// 1) Drain readable bytes into readBuffer (non-blocking)
	char	buffer[4096];
	ssize_t	n = read(clientFd, buffer, sizeof(buffer));
	if (n > 0)	{	// number of bytes read
		connection.readBuffer.append(buffer, static_cast<std::size_t>(n));
		connection.lastActiveMs = _nowMs;	// refresh activity on data
		if (connection.state == S_HEADERS && connection.kaIdleStartMs != 0)
			connection.kaIdleStartMs = 0;
	}
	if (n == 0)	{ // peer closed - EOF (no more bytes)
		closeConnection(clientFd);
		return ;
	}

	// 2) If we are in HEADERS state, try to parse the head block
	if (connection.state == S_HEADERS)	{
		
		// Header-size cap while waiting for terminator
		static const std::size_t	MAX_HEADER_BYTES = 16 * 1024;	// 16 KiB
		if (connection.readBuffer.size() > MAX_HEADER_BYTES
			&& connection.readBuffer.find("\r\n\r\n") == std::string::npos)	{
				//	Too large without CRLFCRLF -> reject
				int			st = 431;
				std::string	reason = "Request Header Fields Too Large";
				connection.writeBuffer = http::build_simple_response(st, reason, reason + "\r\n", connection.request.keep_alive);
				connection.writeOffset = 0;

    			std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
    			if (pit != _fdIndex.end())
					_fds[pit->second].events = POLLOUT;

				connection.state = S_WRITE;
				return ;
		}

		std::string	head;
		if (!http::extract_next_head(connection.readBuffer, head))
			return ;
		// Not enough for a real head yet: either no CRLFCRLF at all, or only empty heads seen so far so need to read more.

		int			status = 0;
		std::string	reason;
		if (!http::parse_head(head, connection.request, status, reason))	{
			// Build a simple error response and switch to write
			std::string	body =	(status == 505) ? "HTTP Version Not Supported\r\n"
							  : (status == 501) ? "Transfer-Encoding not implemented\r\n" : "Bad Request\r\n";
			
			connection.writeBuffer = http::build_simple_response(status, reason, body, connection.request.keep_alive);
			connection.writeOffset = 0;
    	
			std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
    		if (pit != _fdIndex.end())
				_fds[pit->second].events = POLLOUT;
		
			connection.state = S_WRITE;	// set connection to write mode.
			return ;
		}

		connection.headersComplete = true;

		size_t	limit = 1048576;	// default 1 MiB
		if (connection.srv)	{
			if (const Location*	loc = longestPrefixMatch(*connection.srv, connection.request.target))	{
				if (loc->directives.count("client_max_body_size"))
					limit = parseSize(loc->directives.find("client_max_body_size")->second);
			}
			if (limit == 1048576 && connection.srv->directives.count("client_max_body_size"))
				limit = parseSize(connection.srv->directives.find("client_max_body_size")->second);
		}
		connection.clientMaxBodySize = limit;

		// Transition depending on body presence
		if (connection.request.body_reader_state == BR_NONE)	{ // has no body to read
			// minimal dispatcher -> build something and write
			connection.writeBuffer = http::build_simple_response(200, "OK", "Hello\r\n", connection.request.keep_alive);	// to complete later
			connection.writeOffset = 0;

			std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
    		if (pit != _fdIndex.end())
				_fds[pit->second].events = POLLOUT;
			
			connection.state = S_WRITE;	// ready to send
			return ;
		}
		else	{
			connection.state = S_BODY; // still need to read the body
			return ;
		}
	}

	// 3) If we are in BODY state, consume body incrementally
	if (connection.state == S_BODY)	{
		int					st = 0;
		std::string			rsn;
		http::BodyResult	br = http::BODY_INCOMPLETE;

		const std::size_t	maxBody = connection.clientMaxBodySize;

		switch (connection.request.body_reader_state)	{
			case BR_CONTENT_LENGTH:
				br = http::consume_body_content_length(connection, maxBody, st, rsn);
				break;
			case BR_CHUNKED:
				br = http::consume_body_chunked(connection, maxBody, st, rsn);
				break;
			default:
				st = 400;
				rsn = "Bad Request";
				br = http::BODY_ERROR;
		}
		// BR_NONE shouldn't happen; no body was expected

		if (br == http::BODY_COMPLETE)	{
			// Same fix as headers: don't leave the fd in POLLIN
			connection.writeBuffer = http::build_simple_response(200, "OK", "Hello\r\n", connection.request.keep_alive);	// to complete later
			connection.writeOffset = 0;
    		
			std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
    		if (pit != _fdIndex.end())
				_fds[pit->second].events = POLLOUT;

			connection.state = S_WRITE;
			return ;
		}
		if (br == http::BODY_ERROR)	{
			std::string	body;
			if (st == 413)
				body = "Payload Too Large\r\n";
			else
				body = "Bad Request\r\n";
			connection.writeBuffer = http::build_simple_response(st, rsn, body, connection.request.keep_alive);
			connection.writeOffset = 0;
			
			std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
    		if (pit != _fdIndex.end())
				_fds[pit->second].events = POLLOUT;

			connection.state = S_WRITE;
			return ;
		}

		return ;	// still incomplete, wait for more POLLIN
	}

}

void	ServerRunner::writeToClient(int clientFd)	{

	std::map<int, Connection>::iterator it = _connections.find(clientFd);
	if (it == _connections.end())
		return;

	Connection& connection = it->second;

	if (connection.writeOffset >= connection.writeBuffer.size())	{
		// if there is nothing left to send, remove POLLOUT interest
		std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
    	if (pit != _fdIndex.end())
			_fds[pit->second].events = POLLOUT;

		return ;
	}

	const char*	base = connection.writeBuffer.data();
	while (connection.writeOffset < connection.writeBuffer.size())	{
		const char*	buf = base + connection.writeOffset;
		std::size_t	remaining = connection.writeBuffer.size() - connection.writeOffset;
		ssize_t	n = write(clientFd, buf, remaining);
		if (n > 0)	{
			connection.writeOffset += static_cast<std::size_t>(n);
			connection.lastActiveMs = _nowMs;
			continue;	// try to flush more
		}
		// Ensure we stay interested in POLLOUT so we'll retry when writable.
		std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
    	if (pit != _fdIndex.end())
			_fds[pit->second].events = POLLOUT;

		return ;
	}

	// Finished sending the full response
	const bool	keep = connection.request.keep_alive;
	if (keep)	{
		connection.writeBuffer.clear();
		connection.writeOffset = 0;
		connection.readBuffer.clear();
		connection.headersComplete = false;
		connection.requestParsed = false;
		connection.request = HTTP_Request();
		connection.state = S_HEADERS;
		connection.kaIdleStartMs = _nowMs;
		connection.lastActiveMs = _nowMs;

		// Back to read-only interest
		std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
    	if (pit != _fdIndex.end())
			_fds[pit->second].events = POLLOUT;

	}
	else
		closeConnection(clientFd);
}

void	ServerRunner::closeConnection(int clientFd)	{

	close(clientFd);
	_connections.erase(clientFd);

	std::map<int, std::size_t>::iterator it = _fdIndex.find(clientFd);
    if (it != _fdIndex.end())
		return ;	// Not present in _fds (already removed or was never added)

	std::size_t	idx = it->second;
	std::size_t	last = _fds.size() - 1;

	if (idx != last)	{
		// Move last entry into the removed slot
		std::swap(_fds[idx], _fds[last]);
		// Update its index in the map
		_fdIndex[_fds[idx].fd] = idx;
	}

	_fds.pop_back();
	_fdIndex.erase(it);
}

/* Big Logic Flow of this pile of functions

1. Startup

./webserv webserv.config → you parse the config.

setupListeners(...) opens the listening sockets (e.g., 127.0.0.1:8080).

setupPollFds() puts those listener fds into _fds with events = POLLIN.

2. Enter the loop

poll(&_fds[0], _fds.size(), -1) goes to sleep in the kernel.
(Right now, only listener fds are watched.)

3. When a client connects

	// SETUP
	listen(fd, SOMAXCONN);    // ← Creates queues, then returns immediately
	                          //   Code continues...

	// LATER: EVENT LOOP
	poll(&_fds[0], size, -1); // ← Goes to sleep here

	//   ╔═══════════════════════════════════════════╗
	//   ║  WHILE SLEEPING:                   		 ║
	//   ║  Kernel handles handshakes automatically  ║  
	//   ║  - Receives SYN packets                   ║
	//   ║  - Sends SYN-ACK responses                ║
	//   ║  - Receives final ACK                     ║
	//   ║  - Moves connection to accept queue       ║
	//   ║  - Sets listener fd as readable           ║
	//   ╚═══════════════════════════════════════════╝

	poll() wakes up           // ← "Hey! New connection ready!"
	poll() returns; _fds[i].revents for that listener has POLLIN.


4. handleEvents() runs

Sees the listener POLLIN → calls accept() (often in a loop), getting new client fd(s).

Adds each client fd to _fds with events = POLLIN and a Connection entry in our connection std::map.

	// HANDLE RESULT
	accept(listenFd, ...);    // ← Takes the completed connection


5. Requests & responses

When request bytes arrive on a client fd, kernel marks it POLLIN → next poll() wakes → readFromClient(fd) appends to readBuffer.

When there is a response ready, change that fd’s events to POLLOUT.

When the socket can take more bytes, kernel sets POLLOUT → writeToClient(fd) sends, and either keep the connection (flip back to POLLIN) or close it.

6. Other wakeups

poll() also wakes for errors/hangups (POLLERR|POLLHUP|POLLNVAL) on any fd, not just new connections.
*/

/*TCP 3-way handshake
Purpose: both sides agree to communicate and pick initial sequence numbers for reliable byte streams.
SYN (Client → Server)
	“I want to connect, here’s my initial sequence number ISN(c).” ISN => Initial Sequence Number
	Server puts this half-open attempt in the SYN queue.
SYN-ACK (Server → Client)
	“I heard you. Here’s my initial sequence number ISN(s). I’m acknowledging yours.”
	Still in handshake; not yet in accept queue.
ACK (Client → Server)
	“I acknowledge your ISN(s).”
	Handshake completes → kernel moves the connection to the accept queue (fully established).
	Now the listener becomes readable (POLLIN). The accept() call pops it off the queue and returns a new client fd.

	SYN = SYNchronize sequence numbers.
	ACK = ACKnowledgment.

KERNEL QUEUES DURING HANDSHAKES:

Time 0: listen() called
┌─────────────────┐
│ SYN Queue: []   │
│ Accept Queue:[] │  
└─────────────────┘

Time 1: Client A sends SYN
┌─────────────────┐
│ SYN Queue:      │
│ [A_handshaking] │  ← A is in middle of handshake
│ Accept Queue:[] │
└─────────────────┘

Time 2: Client B sends SYN, A completes handshake  
┌─────────────────┐
│ SYN Queue:      │
│ [B_handshaking] │  ← B still handshaking
│ Accept Queue:   │
│ [A_ready] ✓     │  ← A ready to accept!
└─────────────────┘
                    → Listener becomes POLLIN

Time 3: accept() call
┌─────────────────┐
│ SYN Queue:      │
│ [B_handshaking] │  ← B still in progress
│ Accept Queue:[] │  ← A taken by app
└─────────────────┘

*/