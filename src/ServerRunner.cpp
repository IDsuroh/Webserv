#include "../include/ServerRunner.hpp"
#include "../include/HttpHeader.hpp"
#include "../include/HttpSerializer.hpp"
#include "../include/HttpBody.hpp"
#include "../include/App.hpp"

ServerRunner::ServerRunner(const std::vector<Server>& servers)
    :   _servers(servers), _nowMs(0)
{}

//**************************************************************************************************


// Helper Error function
static void	printSocketError(const char* msg)	{
	std::cerr << msg << ": " << std::strerror(errno) << std::endl;
}


//**************************************************************************************************

// void	ServerRunner::housekeeping()	{	// kill the zombies
// 	const long	NOW =  _nowMs;

// 	const long	HEADER_TIMEOUT_MS	= 15000;	// 15s to receive headers
// 	const long	BODY_TIMEOUT_MS		= 30000;	// 30s to receive body
// 	const long	KA_IDLE_MS			= 5000;		// 5s idle on keep-alive
// 	const long	WRITE_TIMEOUT_MS	= 30000;	// 30s to finish sending response

// 	// Sweep connections (close stale ones)
// 	for (std::map<int, Connection>:: iterator	it = _connections.begin(); it != _connections.end();)	{

// 		int			fd = it->first;
// 		Connection& connection = it->second;
// 		bool		closeIt = false;	// To close or not switch
	
// 		switch (connection.state)	{
// 			case	S_HEADERS:	{
// 					// 15s to deliver (next) headers (current time in milliseconds - last activiy = how long has it been)
// 					const bool	headerTimeOut = (NOW - connection.lastActiveMs > HEADER_TIMEOUT_MS);
// 					// 5s only when we *already* finished a KA response and are waiting for the next request
// 					const bool	kaIdleTooLong = (connection.kaIdleStartMs != 0) && (NOW - connection.kaIdleStartMs > KA_IDLE_MS);
// 					if (headerTimeOut || kaIdleTooLong)
// 						closeIt = true;
// 					break;
// 			}
// 			case	S_BODY:	{
// 				if (NOW - connection.lastActiveMs > BODY_TIMEOUT_MS)
// 					closeIt = true;
// 				break;
// 			}
// 			case	S_WRITE:	{
// 				// If we have been trying to write for too long without progress, drop.
// 				if (NOW - connection.lastActiveMs > WRITE_TIMEOUT_MS)
// 					closeIt = true;
// 				break;
// 			}
// 			case	S_CLOSED:	{
// 				closeIt = true;
// 				break;
// 			}
// 			default:
// 				break;
// 		}

// 		if (closeIt)	{
// 			++it;
// 			closeConnection(fd);
// 		}
// 		else
// 			++it;
// 	}
// }

void    ServerRunner::housekeeping() {

    const long NOW = _nowMs;

    const long HEADER_TIMEOUT_MS    = 15000;
    const long BODY_TIMEOUT_MS      = 30000;
    const long KA_IDLE_MS           = 5000;
    const long WRITE_TIMEOUT_MS     = 30000;

    for (std::map<int, Connection>::iterator it = _connections.begin();
         it != _connections.end(); )
    {
        int fd = it->first;
        Connection& connection = it->second;
        bool closeIt = false;

        switch (connection.state) {
            case S_HEADERS: {
                const bool headerTimeOut = (NOW - connection.lastActiveMs > HEADER_TIMEOUT_MS);
                const bool kaIdleTooLong = (connection.kaIdleStartMs != 0)
                                        && (NOW - connection.kaIdleStartMs > KA_IDLE_MS);
                if (headerTimeOut || kaIdleTooLong)
                    closeIt = true;
                break;
            }
            case S_BODY:
            case S_DRAIN: { // <-- NOVO: drenar também tem timeout de BODY
                if (NOW - connection.lastActiveMs > BODY_TIMEOUT_MS)
                    closeIt = true;
                break;
            }
            case S_WRITE: {
                if (NOW - connection.lastActiveMs > WRITE_TIMEOUT_MS)
                    closeIt = true;
                break;
            }
            case S_CLOSED: {
                closeIt = true;
                break;
            }
            default:
                break;
        }

        if (closeIt) {
            ++it;
            closeConnection(fd);
        } else {
            ++it;
        }
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
        int n = poll(_fds.empty() ? 0 : &_fds[0], static_cast<nfds_t>(_fds.size()), POLL_TICK_MS);	// runs every 0.25s
		// int poll(struct pollfd *fds, nfds_t nfds, int timeout);
		// points to the first entry of _fds.size() entries and blocks for up to POLL_TICK_MS (0.25s) (or until an event arrives).
        if (n < 0)  {
			if (errno == EINTR)
				continue;
			printSocketError("poll");
            break;
        }

		_nowMs += POLL_TICK_MS;	// value increases every loop -> 250, 500, 750, 1000, ...
        
		// Handle events first
		if (n > 0)
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
			return false;	// invalid port, no junk values, no alphabets, within port values / pstr.c_str() means the first char
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
	int	rc = getaddrinfo((host.empty() || host == "*") ? NULL : host.c_str(), port.c_str(), &hints, &res);
	// make it socket-compatible
	// this function instanciates/builds a linked list of addresses
	// -> addrinfo nodes which represent candidate local address to bind.
	
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

		int fdflags = fcntl(fd, F_GETFD);
		if (fdflags == -1 || fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC) == -1)
				printSocketError("fcntl F_SETFD FD_CLOEXEC");

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
    _fds.reserve(_listeners.size());	// save memory before populating _fds

	std::set<int> added;
	// We can have multiple Listener records pointing to the same underlying fd
	// (e.g., two server {} blocks both listening on 127.0.0.1:8080).
	// make sure only deduplicates happen. Deduplicate => having no duplicates of the same fd.
	// Deduplicate by FD: many _listeners can share the same fd which was populated by setupListeners().
	// (virtual hosts on the same ip:port), but poll() needs exactly ONE pollfd per unique fd.
    for (std::size_t i = 0; i < _listeners.size(); i++)  {
        int fd = _listeners[i].fd;
		if (!added.insert(fd).second)	// .second = true if the element was successfully inserted, false when it cannot be inserted
			continue;

		struct pollfd   p; // tells the kernel which fd and what events we want
        p.fd = _listeners[i].fd;
        p.events = POLLIN; // "What to expect"
        p.revents = 0; // the poll() is the function that fills in the revents to tell what exactly happened.
        _fds.push_back(p);
		_fdIndex[p.fd] = _fds.size() - 1;	// - 1 because we should check the index number

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

		// set close-on-exec for the accepted client socket
		int fdflags = fcntl(clientFd, F_GETFD);
		if (fdflags != -1)	{
			if (fcntl(clientFd, F_SETFD, fdflags | FD_CLOEXEC) == -1)
				printSocketError("fcntl F_SETFD FD_CLOEXEC");
		}

		if (!makeNonBlocking(clientFd)) {
			close(clientFd);
			continue;
		}

		Connection	connection;						// initiation of Connection
		connection.fd = clientFd;					// fd for this client socket
		connection.srv = srv;						// which Server config this connection uses
		connection.listenFd = listenFd;				// which listening socket it came from
		connection.readBuffer.clear();				// incoming data buffer is resetted
		connection.writeBuffer.clear();				// outgoing data buffer as well
		connection.headersComplete = false;			// haven't finished reading headers
		connection.sentContinue = false;
		connection.state = S_HEADERS;				// where to start
		connection.request.body.clear();			// no body yet
		connection.request.chunk_state = CS_SIZE;
		connection.request.chunk_bytes_left = 0;
		connection.writeOffset = 0;					// nothing written yet
		connection.clientMaxBodySize = std::numeric_limits<size_t>::max();
		connection.kaIdleStartMs = 0;				// not in idle keep-alive
		connection.lastActiveMs = _nowMs;			// The last time there was activity on this connection.
			// Bit shift: 1u (unsigned 1) shifted 20 bits → 1,048,576 bytes (1 MiB) default.

	connection.draining = false;       // <-- NOVO
	connection.drainedBytes = 0;       // <-- NOVO

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
	std::istringstream	iss(string);	// e.g. string = "10k"
	iss >> n;	// reads number from a string. e.g. "10k" -> n = 10 and iss is now just before 'k'
	if (iss && !iss.eof())	// means if the reading worked and we haven't reached the end of the string
		iss >> suf;	// add the last whatever to suf. e.g. suf = "k"
	
	unsigned long long	mult = 1;
	if (suf == 'k' || suf == 'K')
		mult = 1024ULL;
	else if (suf == 'm' || suf == 'M')
		mult = 1024ULL * 1024ULL;
	else if (suf == 'g' || suf == 'G')
		mult = 1024ULL * 1024ULL * 1024ULL;
	return static_cast<size_t>(n * mult);
}

static const Location*	longestPrefixMatch(const Server& srv, const std::string& path)	{	// finds the most specific location
	const Location*	best = NULL;
	size_t			bestLen = 0;
	for (size_t i = 0; i < srv.locations.size(); ++i)	{
		const Location&	L = srv.locations[i];
		if (path.compare(0, L.path.size(), L.path) == 0 && L.path.size() > bestLen)	{
			// Compare path starting at index 0, length L.path.size() with L.path if result is 0, the substrings are equal
			best = &L;
			bestLen = L.path.size();
		}
	}
	return best;
}

void	ServerRunner::handleRequest(Connection& connection) {
    /*
		Ask the App layer to build the response for this request
		Not the same function. It is calling handleRequest from the App.hpp
		which is a free function in the global namespace.
    */
	HTTP_Response appRes = ::handleRequest(connection.request, _servers);

	// Serialize to wire format (use request version if present)
    const std::string httpVersion = connection.request.version;
    connection.writeBuffer = http::serialize_response(appRes, httpVersion);
    
	// // HEAD method must send headers only (no body bytes)
	// if (connection.request.method == "HEAD")	{
	// 	const std::string	crlf = "\r\n\r\n";
	// 	size_t				position = connection.writeBuffer.find(crlf);
	// 	if (position != std::string::npos)
	// 		connection.writeBuffer.erase(position + crlf.size());
	// }

	// HEAD method must send headers only (no body bytes)
	if (connection.request.method == "HEAD") {
		const std::string crlf = "\r\n\r\n";
		size_t position = connection.writeBuffer.find(crlf);
		if (position != std::string::npos) {
			// Keep only status line + headers + CRLFCRLF
			connection.writeBuffer.resize(position + crlf.size());
		}
	}


	connection.writeOffset = 0;
    connection.response = appRes;
    connection.state = S_WRITE;

    // If App says “close”, override keep-alive
    if (appRes.close)
        connection.request.keep_alive = false;

    // Flip poll interest to POLLOUT for this fd
    std::map<int, std::size_t>::iterator pit = _fdIndex.find(connection.fd);
    if (pit != _fdIndex.end())
        _fds[pit->second].events = POLLOUT;
}

// void	ServerRunner::readFromClient(int clientFd)	{

// 	std::map<int, Connection>::iterator it = _connections.find(clientFd);
// 	if (it == _connections.end())
// 		return;

// 	Connection& connection = it->second;

// 	// 1) Drain readable bytes into readBuffer (non-blocking)
// 	char	buffer[4096];
// 	ssize_t	n = read(clientFd, buffer, sizeof(buffer));

// 	if (n > 0)	{	// number of bytes read
// 		connection.readBuffer.append(buffer, static_cast<std::size_t>(n));
// 		connection.lastActiveMs = _nowMs;	// refresh activity on data
// 		if (connection.state == S_HEADERS && connection.kaIdleStartMs != 0)
// 			connection.kaIdleStartMs = 0;
// 	}
// 	else if (n == 0)	{ // peer closed - EOF (no more bytes)
// 		closeConnection(clientFd);
// 		return ;
// 	}
// 	else
// 		return ;

// 	// 2) If we are in HEADERS state, try to parse the head block
// 	if (connection.state == S_HEADERS)	{
		
// 		// Header-size cap while waiting for terminator
// 		static const std::size_t	MAX_HEADER_BYTES = 16 * 1024;	// 16 KiB
// 		if (connection.readBuffer.size() > MAX_HEADER_BYTES && connection.readBuffer.find("\r\n\r\n") == std::string::npos)	{
// 				//	Too large without CRLFCRLF -> reject
// 				int			st = 431;
// 				std::string	reason = "Request Header Fields Too Large";
// 				const Server&	active = connection.srv ? *connection.srv : _servers[0];
				
// 				connection.writeBuffer = http::build_error_response(active, st, reason, connection.request.keep_alive);
// 				connection.writeOffset = 0;

//     			std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//     			if (pit != _fdIndex.end())
// 					_fds[pit->second].events = POLLOUT;

// 				connection.state = S_WRITE;
// 				return ;
// 		}

// 		std::string	head;
// 		if (!http::extract_next_head(connection.readBuffer, head))
// 			return ;
// 		// Not enough for a real head yet: either no CRLFCRLF at all, or only empty heads seen so far so need to read more.

// 		int			status = 0;
// 		std::string	reason;
// 		if (!http::parse_head(head, connection.request, status, reason))	{
// 			if (!connection.srv)	{
// 				throw	std::runtime_error("Internal bug: connection.srv is NULL");
// 			}
// 			const Server&	active = *connection.srv;

// 			if (status == 413)
// 				connection.request.keep_alive = false;
			
// 			connection.writeBuffer = http::build_error_response(active, status, reason, connection.request.keep_alive);
// 			connection.writeOffset = 0;
    	
// 			std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//     		if (pit != _fdIndex.end())
// 				_fds[pit->second].events = POLLOUT;
		
// 			connection.state = S_WRITE;	// set connection to write mode.
// 			return ;
// 		}

// 		connection.headersComplete = true;

// 		size_t	limit = std::numeric_limits<size_t>::max();	// default: no limit unless configured
// 		bool	fromLoc = false;
// 		const Location*	loc = NULL;
		
// 		if (connection.srv)	{
// 			if ((loc = longestPrefixMatch(*connection.srv, connection.request.path)))	{

// 				if (loc->directives.count("client_max_body_size"))	{	// #1
// 					limit = parseSize(loc->directives.find("client_max_body_size")->second);
// 					fromLoc = true;
// 				}
// 			}

// 			if (!fromLoc && connection.srv->directives.count("client_max_body_size"))	// #2
// 				limit = parseSize(connection.srv->directives.find("client_max_body_size")->second);
// 		}

// 		connection.clientMaxBodySize = limit;	// #3

// 		{	// Early check scope
// 			const Server&	active = connection.srv ? *connection.srv : _servers[0];

// 			bool 		isCgiRequest = false;
// 			std::string matchedExt;
// 			std::string interpreterPath;

// 			if (loc && loc->directives.count("cgi_pass"))	{
				
// 				const std::string&	cgiVal = loc->directives.find("cgi_pass")->second;

// 				// tokenize by whitespace (C++98, no stringstream requirement)
// 				std::vector<std::string>	tokens;
// 				std::string 				current;
// 				for (size_t i = 0; i <= cgiVal.size(); ++i)	{
// 					char	c = (i < cgiVal.size()) ? cgiVal[i] : ' ';
// 					if (!std::isspace(static_cast<unsigned char>(c)))
// 					    current += c;
// 					else if (!current.empty())	{
// 					    tokens.push_back(current);
// 					    current.clear();
// 					}
// 				}

// 				const std::string&	p = connection.request.path;

// 				for (size_t i = 0; i + 1 < tokens.size(); i += 2)	{
// 					const std::string&	ext = tokens[i];
// 					const std::string&	bin = tokens[i + 1];

// 					if (!ext.empty() && p.size() >= ext.size() && p.compare(p.size() - ext.size(), ext.size(), ext) == 0)	{
// 						isCgiRequest = true;
// 						matchedExt = ext;
// 						interpreterPath = bin;
// 						break;
// 					}
// 				}
// 			}

// 			// Helper: compute CGI script fs path the SAME way we should later exec it.
// 			// NOTE: This assumes "root + (uri minus location prefix)" mapping.
// 			std::string	scriptFsPath;
// 			if (isCgiRequest)	{
// 				std::string	root = ".";
// 				if (loc && loc->directives.count("root"))
// 					root = loc->directives.find("root")->second;
// 				else if (connection.srv && connection.srv->directives.count("root"))
// 					root = connection.srv->directives.find("root")->second;

// 				std::string	subPath = connection.request.path;
// 				if (loc)	{
// 					const std::string&	lp = loc->path; // "/directory"
// 					if (subPath.size() >= lp.size() && subPath.compare(0, lp.size(), lp) == 0)
// 						subPath = subPath.substr(lp.size());
// 				}
// 				if (subPath.empty())
// 					subPath = "/";

// 				bool	rootEnds = (!root.empty() && root[root.size() -1] == '/');
// 				bool	subStarts = (!subPath.empty() && subPath[0] == '/');

// 				if (rootEnds && subStarts)
// 					scriptFsPath = root.substr(0, root.size() - 1) + subPath;
// 				else if (rootEnds ^ subStarts)	// bitwise XOR: if just one of them is true.
// 					scriptFsPath = root + subPath;
// 				else
// 					scriptFsPath = root + "/" + subPath;
// 			}

// 			// (1) Early 413: known CL too large
// 			if (connection.request.body_reader_state == BR_CONTENT_LENGTH
// 				&& connection.request.content_length > connection.clientMaxBodySize)	{
				
// 				connection.request.keep_alive = false;
// 				connection.request.expectContinue = false;
// 				connection.sentContinue = false;

// 				connection.writeBuffer = http::build_error_response(active, 413, "Payload Too Large", false);
// 				connection.writeOffset = 0;

// 				std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
// 				if (pit != _fdIndex.end())
// 					_fds[pit->second].events = POLLOUT;

// 				connection.state = S_WRITE;
// 				return ;
// 			}

// 			// (2) Early 405: method not allowed (token-based; avoids "POST" substring pitfalls)
// 			const std::string*	methodsStr = NULL;
// 			if (loc && loc->directives.count("methods"))
// 				methodsStr = &loc->directives.find("methods")->second;
// 			else if (connection.srv && connection.srv->directives.count("methods"))
// 				methodsStr = &connection.srv->directives.find("methods")->second;

// 			if (methodsStr)	{
// 				std::string	reqM = connection.request.method;
// 				for (size_t i = 0; i < reqM.size(); ++i)
// 					reqM[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(reqM[i])));

// 				bool		allowed = false;
// 				std::string	token;

// 				for (size_t i = 0; i <= methodsStr->size(); ++i)	{
// 					char	c = (i < methodsStr->size()) ? (*methodsStr)[i] : ' ';
// 					if (std::isalpha(static_cast<unsigned char>(c)))
// 						token += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
// 					else if (!token.empty())	{
// 						if (token == reqM)	{
// 							allowed = true;
// 							break;
// 						}
// 						token.clear();
// 					}
// 				}

// 				if (!allowed)	{

// 					// Important: even for errors, HTTP/1.1 can keep the TCP connection alive.
// 					bool	keep = connection.request.keep_alive;

// 					if (connection.request.body_reader_state != BR_NONE && !connection.request.expectContinue)
// 						keep = false;
// 					// If the request has a body and the client did NOT use Expect: 100-continue,
// 					// the client may already be streaming a large body.
// 					// If we keep the connection alive *without draining* that body, leftover bytes can desync the next request.
// 					// So in this case we force Connection: close.

// 					connection.request.keep_alive = keep;
// 					connection.sentContinue = false;

// 					// This enables: 405 responses that can remain keep-alive (nginx-like) when safe.
// 					connection.writeBuffer = http::build_error_response(active, 405, "Method Not Allowed", keep);
// 					connection.writeOffset = 0;

// 					std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
// 					if (pit != _fdIndex.end())
// 						_fds[pit->second].events = POLLOUT;

// 					connection.state = S_WRITE;
// 					return ;
//                 }
//             }

// 			// (3) Early 404: CGI script missing (runs for GET/POST/etc — not only “has body”)
// 			if (isCgiRequest)	{
// 				struct stat	st;
// 				if (stat(scriptFsPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode))	{
					
// 					bool	keep = connection.request.keep_alive;
// 					// If a body might arrive, only keep-alive if Expect: 100-continue was used.
// 					if (connection.request.body_reader_state != BR_NONE && !connection.request.expectContinue)
// 						keep = false;

// 					connection.request.keep_alive = keep;
// 					connection.sentContinue = false;

// 					connection.writeBuffer = http::build_error_response(active, 404, "Not Found", keep);
// 					connection.writeOffset = 0;

// 					std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
// 					if (pit != _fdIndex.end())
// 						_fds[pit->second].events = POLLOUT;

// 					connection.state = S_WRITE;
// 					return ;
// 				}
// 			}
// 		}

// 		// Transition depending on body presence
// 		if (connection.request.body_reader_state == BR_NONE)	{ // has no body to read

// 			std::cerr << "[PARSER-READY] t=" << _nowMs
// 			<< " fd=" << clientFd
// 			<< " m=" << connection.request.method
// 			<< " target=" << connection.request.target
// 			<< " path=" << connection.request.path
// 			<< " br=" << connection.request.body_reader_state
// 			<< " cl=" << connection.request.content_length
// 			<< " te=\"" << connection.request.transfer_encoding << "\""
// 			<< " ka=" << (connection.request.keep_alive ? 1 : 0)
// 			<< " body=" << connection.request.body.size()
// 			<< "\n";


// 			// minimal dispatcher -> handle Request
// 			handleRequest(connection);

// 			return ;
// 		}

// 		// --- EXPECT: 100-CONTINUE HANDSHAKE ---
// 		if (connection.request.expectContinue == true)	{

// 			connection.writeBuffer = "HTTP/1.1 100 Continue\r\n\r\n";
// 			connection.writeOffset = 0;
// 			connection.sentContinue = true;

// 			std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
// 			if (pit != _fdIndex.end())
// 				_fds[pit->second].events = POLLOUT;

// 			connection.state = S_WRITE;
// 			return ;
// 		}

// 		connection.state = S_BODY;
// 	}

// 	// 3) If we are in BODY state, consume body incrementally
// 	if (connection.state == S_BODY)	{
// 		int					status = 0;
// 		std::string			reason;
// 		http::BodyResult	result = http::BODY_INCOMPLETE;

// 		const std::size_t	maxBody = connection.clientMaxBodySize;

// 		switch (connection.request.body_reader_state)	{
// 			case BR_CONTENT_LENGTH:
// 				result = http::consume_body_content_length(connection, maxBody, status, reason);
// 				break;
// 			case BR_CHUNKED:
// 				result = http::consume_body_chunked(connection, maxBody, status, reason);
// 				break;
// 			default:
// 				status = 400;
// 				reason = "Bad Request";
// 				result = http::BODY_ERROR;
// 		}
// 		// BR_NONE shouldn't happen; no body was expected

// 		if (result == http::BODY_COMPLETE)	{

// 			std::cerr << "[PARSER-READY] t=" << _nowMs
// 				<< " fd=" << clientFd
// 				<< " m=" << connection.request.method
// 				<< " target=" << connection.request.target
// 				<< " path=" << connection.request.path
// 				<< " br=" << connection.request.body_reader_state
// 				<< " cl=" << connection.request.content_length
// 				<< " te=\"" << connection.request.transfer_encoding << "\""
// 				<< " ka=" << (connection.request.keep_alive ? 1 : 0)
// 				<< " body=" << connection.request.body.size()
// 				<< "\n";


// 			// minimal dispatcher -> handle Request
// 			handleRequest(connection);
    		
// 			return ;
// 		}
// 		if (result == http::BODY_ERROR)	{

// 			const Server&	active = connection.srv ? *connection.srv : _servers[0];

// 			if (status == 413)
// 				connection.request.keep_alive = false;

// 			connection.writeBuffer = http::build_error_response(active, status, reason, connection.request.keep_alive);
// 			connection.writeOffset = 0;
			
// 			std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//     		if (pit != _fdIndex.end())
// 				_fds[pit->second].events = POLLOUT;

// 			connection.state = S_WRITE;
// 			return ;
// 		}

// 		return ;	// still incomplete, wait for more POLLIN
// 	}

// }

// void	ServerRunner::readFromClient(int clientFd)	{

// 	std::map<int, Connection>::iterator it = _connections.find(clientFd);
// 	if (it == _connections.end())
// 		return;

// 	Connection& connection = it->second;

// 	// 1) Drain readable bytes into readBuffer (non-blocking)
// 	char buffer[4096];
// 	std::size_t totalRead = 0;

// 	for (;;)	{
// 		ssize_t n = read(clientFd, buffer, sizeof(buffer));

// 		if (n > 0)	{
// 			connection.readBuffer.append(buffer, static_cast<std::size_t>(n));
// 			totalRead += static_cast<std::size_t>(n);

// 			connection.lastActiveMs = _nowMs;

// 			// If we were idle in keep-alive waiting for the next request, stop idle timer now
// 			if (connection.state == S_HEADERS && connection.kaIdleStartMs != 0)
// 				connection.kaIdleStartMs = 0;

// 			continue; // drain more
// 		}

// 		if (n == 0)	{
// 			std::cerr << "[READ-EOF] t=" << _nowMs
// 			          << " fd=" << clientFd
// 			          << " totalRead=" << totalRead
// 			          << " rb=" << connection.readBuffer.size()
// 			          << " state=" << connection.state
// 			          << "\n";
// 			closeConnection(clientFd);
// 			return;
// 		}

// 		// n < 0
// 		if (errno == EINTR)
// 			continue;
// 		if (errno == EAGAIN || errno == EWOULDBLOCK)
// 			break;

// 		std::cerr << "[READ-ERR] t=" << _nowMs
// 		          << " fd=" << clientFd
// 		          << " errno=" << errno
// 		          << " msg=" << std::strerror(errno)
// 		          << " totalRead=" << totalRead
// 		          << " rb=" << connection.readBuffer.size()
// 		          << " state=" << connection.state
// 		          << "\n";
// 		closeConnection(clientFd);
// 		return;
// 	}

// 	if (totalRead > 0)	{
// 		std::cerr << "[READ] t=" << _nowMs
// 		          << " fd=" << clientFd
// 		          << " got=" << totalRead
// 		          << " rb=" << connection.readBuffer.size()
// 		          << " state=" << connection.state
// 		          << "\n";
// 	}

// 	// If we're currently writing a response, do NOT parse next requests now.
// 	// Keep any extra bytes in readBuffer for after the keep-alive reset.
// 	if (connection.state == S_WRITE)	{
// 		std::cerr << "[READ-SKIP-PARSE] t=" << _nowMs
// 		          << " fd=" << clientFd
// 		          << " rb=" << connection.readBuffer.size()
// 		          << " (state=S_WRITE)\n";
// 		return;
// 	}

// 	// 2) Parse as much as possible from readBuffer (headers + maybe body)
// 	for (;;)	{

// 		// ---------------- HEADERS ----------------
// 		if (connection.state == S_HEADERS)	{

// 			// Header-size cap while waiting for terminator
// 			static const std::size_t MAX_HEADER_BYTES = 16 * 1024; // 16 KiB
// 			if (connection.readBuffer.size() > MAX_HEADER_BYTES
// 				&& connection.readBuffer.find("\r\n\r\n") == std::string::npos)	{

// 				int st = 431;
// 				std::string reason = "Request Header Fields Too Large";
// 				const Server& active = connection.srv ? *connection.srv : _servers[0];

// 				std::cerr << "[HDR-TOO-LARGE] t=" << _nowMs
// 				          << " fd=" << clientFd
// 				          << " rb=" << connection.readBuffer.size()
// 				          << "\n";

// 				connection.writeBuffer = http::build_error_response(active, st, reason, connection.request.keep_alive);
// 				connection.writeOffset = 0;

// 				std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
// 				if (pit != _fdIndex.end())
// 					_fds[pit->second].events = POLLOUT;

// 				connection.state = S_WRITE;
// 				return;
// 			}

// 			std::string head;
// 			if (!http::extract_next_head(connection.readBuffer, head))	{
// 				std::cerr << "[HDR-INCOMPLETE] t=" << _nowMs
// 				          << " fd=" << clientFd
// 				          << " rb=" << connection.readBuffer.size()
// 				          << "\n";
// 				return; // need more bytes
// 			}

// 			std::cerr << "[HDR-EXTRACT] t=" << _nowMs
// 			          << " fd=" << clientFd
// 			          << " headBytes=" << head.size()
// 			          << " rb_after=" << connection.readBuffer.size()
// 			          << "\n";

// 			int status = 0;
// 			std::string reason;
// 			if (!http::parse_head(head, connection.request, status, reason))	{

// 				if (!connection.srv)
// 					throw std::runtime_error("Internal bug: connection.srv is NULL");
// 				const Server& active = *connection.srv;

// 				std::cerr << "[HDR-PARSE-FAIL] t=" << _nowMs
// 				          << " fd=" << clientFd
// 				          << " status=" << status
// 				          << " reason=\"" << reason << "\""
// 				          << " ka=" << (connection.request.keep_alive ? 1 : 0)
// 				          << "\n";

// 				if (status == 413)
// 					connection.request.keep_alive = false;

// 				connection.writeBuffer = http::build_error_response(active, status, reason, connection.request.keep_alive);
// 				connection.writeOffset = 0;

// 				std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
// 				if (pit != _fdIndex.end())
// 					_fds[pit->second].events = POLLOUT;

// 				connection.state = S_WRITE;
// 				return;
// 			}

// 			connection.headersComplete = true;

// 			std::cerr << "[HDR-OK] t=" << _nowMs
// 			          << " fd=" << clientFd
// 			          << " m=" << connection.request.method
// 			          << " target=" << connection.request.target
// 			          << " path=" << connection.request.path
// 			          << " br=" << connection.request.body_reader_state
// 			          << " cl=" << connection.request.content_length
// 			          << " te=\"" << connection.request.transfer_encoding << "\""
// 			          << " ka=" << (connection.request.keep_alive ? 1 : 0)
// 			          << " exp=" << (connection.request.expectContinue ? 1 : 0)
// 			          << " rb=" << connection.readBuffer.size()
// 			          << "\n";

// 			// ---- client_max_body_size resolution (mesma lógica) ----
// 			size_t			limit = std::numeric_limits<size_t>::max();
// 			bool			fromLoc = false;
// 			const Location*	loc = NULL;

// 			if (connection.srv)	{
// 				if ((loc = longestPrefixMatch(*connection.srv, connection.request.path)))	{
// 					if (loc->directives.count("client_max_body_size"))	{
// 						limit = parseSize(loc->directives.find("client_max_body_size")->second);
// 						fromLoc = true;
// 					}
// 				}

// 				if (!fromLoc && connection.srv->directives.count("client_max_body_size"))
// 					limit = parseSize(connection.srv->directives.find("client_max_body_size")->second);
// 			}

// 			connection.clientMaxBodySize = limit;

// 			// ---- (mantive o teu bloco de early checks exactamente como estava) ----
// 			{
// 				const Server& active = connection.srv ? *connection.srv : _servers[0];

// 				bool		isCgiRequest = false;
// 				std::string matchedExt;
// 				std::string interpreterPath;

// 				if (loc && loc->directives.count("cgi_pass"))	{

// 					const std::string& cgiVal = loc->directives.find("cgi_pass")->second;

// 					std::vector<std::string> tokens;
// 					std::string current;
// 					for (size_t i = 0; i <= cgiVal.size(); ++i)	{
// 						char c = (i < cgiVal.size()) ? cgiVal[i] : ' ';
// 						if (!std::isspace(static_cast<unsigned char>(c)))
// 							current += c;
// 						else if (!current.empty())	{
// 							tokens.push_back(current);
// 							current.clear();
// 						}
// 					}

// 					const std::string& p = connection.request.path;

// 					for (size_t i = 0; i + 1 < tokens.size(); i += 2)	{
// 						const std::string& ext = tokens[i];
// 						const std::string& bin = tokens[i + 1];

// 						if (!ext.empty() && p.size() >= ext.size()
// 							&& p.compare(p.size() - ext.size(), ext.size(), ext) == 0)	{
// 							isCgiRequest = true;
// 							matchedExt = ext;
// 							interpreterPath = bin;
// 							break;
// 						}
// 					}
// 				}

// 				std::string scriptFsPath;
// 				if (isCgiRequest)	{
// 					std::string root = ".";
// 					if (loc && loc->directives.count("root"))
// 						root = loc->directives.find("root")->second;
// 					else if (connection.srv && connection.srv->directives.count("root"))
// 						root = connection.srv->directives.find("root")->second;

// 					std::string subPath = connection.request.path;
// 					if (loc)	{
// 						const std::string& lp = loc->path;
// 						if (subPath.size() >= lp.size() && subPath.compare(0, lp.size(), lp) == 0)
// 							subPath = subPath.substr(lp.size());
// 					}
// 					if (subPath.empty())
// 						subPath = "/";

// 					bool rootEnds = (!root.empty() && root[root.size() - 1] == '/');
// 					bool subStarts = (!subPath.empty() && subPath[0] == '/');

// 					if (rootEnds && subStarts)
// 						scriptFsPath = root.substr(0, root.size() - 1) + subPath;
// 					else if (rootEnds ^ subStarts)
// 						scriptFsPath = root + subPath;
// 					else
// 						scriptFsPath = root + "/" + subPath;
// 				}

// 				if (connection.request.body_reader_state == BR_CONTENT_LENGTH
// 					&& connection.request.content_length > connection.clientMaxBodySize)	{

// 					std::cerr << "[EARLY-413] t=" << _nowMs
// 					          << " fd=" << clientFd
// 					          << " cl=" << connection.request.content_length
// 					          << " max=" << connection.clientMaxBodySize
// 					          << "\n";

// 					connection.request.keep_alive = false;
// 					connection.request.expectContinue = false;
// 					connection.sentContinue = false;

// 					connection.writeBuffer = http::build_error_response(active, 413, "Payload Too Large", false);
// 					connection.writeOffset = 0;

// 					std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
// 					if (pit != _fdIndex.end())
// 						_fds[pit->second].events = POLLOUT;

// 					connection.state = S_WRITE;
// 					return;
// 				}

// 				const std::string* methodsStr = NULL;
// 				if (loc && loc->directives.count("methods"))
// 					methodsStr = &loc->directives.find("methods")->second;
// 				else if (connection.srv && connection.srv->directives.count("methods"))
// 					methodsStr = &connection.srv->directives.find("methods")->second;

// 				if (methodsStr)	{
// 					std::string reqM = connection.request.method;
// 					for (size_t i = 0; i < reqM.size(); ++i)
// 						reqM[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(reqM[i])));

// 					bool allowed = false;
// 					std::string token;

// 					for (size_t i = 0; i <= methodsStr->size(); ++i)	{
// 						char c = (i < methodsStr->size()) ? (*methodsStr)[i] : ' ';
// 						if (std::isalpha(static_cast<unsigned char>(c)))
// 							token += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
// 						else if (!token.empty())	{
// 							if (token == reqM)	{
// 								allowed = true;
// 								break;
// 							}
// 							token.clear();
// 						}
// 					}

// 					if (!allowed)	{

// 						bool keep = connection.request.keep_alive;
// 						if (connection.request.body_reader_state != BR_NONE && !connection.request.expectContinue)
// 							keep = false;

// 						std::cerr << "[EARLY-405] t=" << _nowMs
// 						          << " fd=" << clientFd
// 						          << " method=" << connection.request.method
// 						          << " keep=" << (keep ? 1 : 0)
// 						          << "\n";

// 						connection.request.keep_alive = keep;
// 						connection.sentContinue = false;

// 						connection.writeBuffer = http::build_error_response(active, 405, "Method Not Allowed", keep);
// 						connection.writeOffset = 0;

// 						std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
// 						if (pit != _fdIndex.end())
// 							_fds[pit->second].events = POLLOUT;

// 						connection.state = S_WRITE;
// 						return;
// 					}
// 				}

// 				if (isCgiRequest)	{
// 					struct stat st;
// 					if (stat(scriptFsPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode))	{

// 						bool keep = connection.request.keep_alive;
// 						if (connection.request.body_reader_state != BR_NONE && !connection.request.expectContinue)
// 							keep = false;

// 						std::cerr << "[EARLY-CGI-404] t=" << _nowMs
// 						          << " fd=" << clientFd
// 						          << " script=\"" << scriptFsPath << "\""
// 						          << " keep=" << (keep ? 1 : 0)
// 						          << "\n";

// 						connection.request.keep_alive = keep;
// 						connection.sentContinue = false;

// 						connection.writeBuffer = http::build_error_response(active, 404, "Not Found", keep);
// 						connection.writeOffset = 0;

// 						std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
// 						if (pit != _fdIndex.end())
// 							_fds[pit->second].events = POLLOUT;

// 						connection.state = S_WRITE;
// 						return;
// 					}
// 				}
// 			}

// 			// ---- Transition depending on body presence ----
// 			if (connection.request.body_reader_state == BR_NONE)	{

// 				std::cerr << "[DISPATCH-NO-BODY] t=" << _nowMs
// 				          << " fd=" << clientFd
// 				          << " rb=" << connection.readBuffer.size()
// 				          << "\n";

// 				handleRequest(connection);
// 				return;
// 			}

// 			// EXPECT: 100-continue
// 			if (connection.request.expectContinue == true)	{

// 				std::cerr << "[SEND-100] t=" << _nowMs
// 				          << " fd=" << clientFd
// 				          << " rb=" << connection.readBuffer.size()
// 				          << "\n";

// 				connection.writeBuffer = "HTTP/1.1 100 Continue\r\n\r\n";
// 				connection.writeOffset = 0;
// 				connection.sentContinue = true;

// 				std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
// 				if (pit != _fdIndex.end())
// 					_fds[pit->second].events = POLLOUT;

// 				connection.state = S_WRITE;
// 				return;
// 			}

// 			connection.state = S_BODY;

// 			std::cerr << "[STATE->BODY] t=" << _nowMs
// 			          << " fd=" << clientFd
// 			          << " rb=" << connection.readBuffer.size()
// 			          << "\n";

// 			// continue loop: if body bytes are already buffered, consume them now
// 			continue;
// 		}

// 		// ---------------- BODY ----------------
// 		if (connection.state == S_BODY)	{

// 			int status = 0;
// 			std::string reason;
// 			http::BodyResult result = http::BODY_INCOMPLETE;

// 			const std::size_t maxBody = connection.clientMaxBodySize;

// 			switch (connection.request.body_reader_state)	{
// 				case BR_CONTENT_LENGTH:
// 					result = http::consume_body_content_length(connection, maxBody, status, reason);
// 					break;
// 				case BR_CHUNKED:
// 					result = http::consume_body_chunked(connection, maxBody, status, reason);
// 					break;
// 				default:
// 					status = 400;
// 					reason = "Bad Request";
// 					result = http::BODY_ERROR;
// 			}

// 			std::cerr << "[BODY-CONSUME] t=" << _nowMs
// 			          << " fd=" << clientFd
// 			          << " res=" << result
// 			          << " rb=" << connection.readBuffer.size()
// 			          << " body=" << connection.request.body.size()
// 			          << "\n";

// 			if (result == http::BODY_COMPLETE)	{

// 				std::cerr << "[DISPATCH-BODY] t=" << _nowMs
// 				          << " fd=" << clientFd
// 				          << " body=" << connection.request.body.size()
// 				          << " rb=" << connection.readBuffer.size()
// 				          << "\n";

// 				handleRequest(connection);
// 				return;
// 			}

// 			if (result == http::BODY_ERROR)	{

// 				const Server& active = connection.srv ? *connection.srv : _servers[0];

// 				std::cerr << "[BODY-ERR] t=" << _nowMs
// 				          << " fd=" << clientFd
// 				          << " status=" << status
// 				          << " reason=\"" << reason << "\""
// 				          << " ka=" << (connection.request.keep_alive ? 1 : 0)
// 				          << "\n";

// 				if (status == 413)
// 					connection.request.keep_alive = false;

// 				connection.writeBuffer = http::build_error_response(active, status, reason, connection.request.keep_alive);
// 				connection.writeOffset = 0;

// 				std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
// 				if (pit != _fdIndex.end())
// 					_fds[pit->second].events = POLLOUT;

// 				connection.state = S_WRITE;
// 				return;
// 			}

// 			// incomplete
// 			return;
// 		}

// 		// Any other state -> stop
// 		return;
// 	}
// }




// void    ServerRunner::readFromClient(int clientFd) {

//     std::map<int, Connection>::iterator it = _connections.find(clientFd);
//     if (it == _connections.end())
//         return;

//     Connection& connection = it->second;

//     // 1) Drain readable bytes into readBuffer (non-blocking)
//     char buffer[4096];
//     std::size_t totalRead = 0;

//     for (;;) {
//         ssize_t n = read(clientFd, buffer, sizeof(buffer));

//         if (n > 0) {
//             connection.readBuffer.append(buffer, static_cast<std::size_t>(n));
//             totalRead += static_cast<std::size_t>(n);

//             connection.lastActiveMs = _nowMs;

//             if (connection.state == S_HEADERS && connection.kaIdleStartMs != 0)
//                 connection.kaIdleStartMs = 0;

//             continue;
//         }

//         if (n == 0) {
//             std::cerr << "[READ-EOF] t=" << _nowMs
//                       << " fd=" << clientFd
//                       << " totalRead=" << totalRead
//                       << " rb=" << connection.readBuffer.size()
//                       << " state=" << connection.state
//                       << "\n";
//             closeConnection(clientFd);
//             return;
//         }

//         if (errno == EINTR)
//             continue;
//         if (errno == EAGAIN || errno == EWOULDBLOCK)
//             break;

//         std::cerr << "[READ-ERR] t=" << _nowMs
//                   << " fd=" << clientFd
//                   << " errno=" << errno
//                   << " msg=" << std::strerror(errno)
//                   << " totalRead=" << totalRead
//                   << " rb=" << connection.readBuffer.size()
//                   << " state=" << connection.state
//                   << "\n";
//         closeConnection(clientFd);
//         return;
//     }

//     if (totalRead > 0) {
//         std::cerr << "[READ] t=" << _nowMs
//                   << " fd=" << clientFd
//                   << " got=" << totalRead
//                   << " rb=" << connection.readBuffer.size()
//                   << " state=" << connection.state
//                   << "\n";
//     }

//     // Se estamos a escrever, não mexer.
//     if (connection.state == S_WRITE) {
//         std::cerr << "[READ-SKIP-PARSE] t=" << _nowMs
//                   << " fd=" << clientFd
//                   << " rb=" << connection.readBuffer.size()
//                   << " (state=S_WRITE)\n";
//         return;
//     }

//     // ---------------- DRAIN ----------------
//     if (connection.state == S_DRAIN) {

//         int status = 0;
//         std::string reason;
//         http::BodyResult result = http::BODY_INCOMPLETE;

//         const std::size_t maxBody = connection.clientMaxBodySize;

//         if (connection.request.body_reader_state == BR_CONTENT_LENGTH)
//             result = http::consume_body_content_length_drain(connection, maxBody, status, reason);
//         else if (connection.request.body_reader_state == BR_CHUNKED)
//             result = http::consume_body_chunked_drain(connection, maxBody, status, reason);
//         else {
//             // nada para drenar
//             result = http::BODY_COMPLETE;
//         }

//         std::cerr << "[DRAIN-CONSUME] t=" << _nowMs
//                   << " fd=" << clientFd
//                   << " res=" << result
//                   << " rb=" << connection.readBuffer.size()
//                   << " drained=" << connection.drainedBytes
//                   << "\n";

//         if (result == http::BODY_COMPLETE) {
//             std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//             if (pit != _fdIndex.end())
//                 _fds[pit->second].events = POLLOUT;

//             connection.state = S_WRITE;

//             std::cerr << "[DRAIN-DONE->WRITE] t=" << _nowMs
//                       << " fd=" << clientFd
//                       << " rb=" << connection.readBuffer.size()
//                       << "\n";
//             return;
//         }

//         if (result == http::BODY_ERROR) {
//             // Já íamos fechar de qualquer forma; responde com 400/413 se quiseres,
//             // mas como já tens writeBuffer preparado (405/404), o mais simples é fechar.
//             std::cerr << "[DRAIN-ERR] t=" << _nowMs
//                       << " fd=" << clientFd
//                       << " status=" << status
//                       << " reason=\"" << reason << "\"\n";
//             closeConnection(clientFd);
//             return;
//         }

//         return; // incomplete
//     }

//     // 2) Parse as much as possible from readBuffer (headers + maybe body)
//     for (;;) {

//         // ---------------- HEADERS ----------------
//         if (connection.state == S_HEADERS) {

//             static const std::size_t MAX_HEADER_BYTES = 16 * 1024;
//             if (connection.readBuffer.size() > MAX_HEADER_BYTES
//                 && connection.readBuffer.find("\r\n\r\n") == std::string::npos) {

//                 int st = 431;
//                 std::string reason = "Request Header Fields Too Large";
//                 const Server& active = connection.srv ? *connection.srv : _servers[0];

//                 std::cerr << "[HDR-TOO-LARGE] t=" << _nowMs
//                           << " fd=" << clientFd
//                           << " rb=" << connection.readBuffer.size()
//                           << "\n";

//                 connection.writeBuffer = http::build_error_response(active, st, reason, connection.request.keep_alive);
//                 connection.writeOffset = 0;

//                 std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                 if (pit != _fdIndex.end())
//                     _fds[pit->second].events = POLLOUT;

//                 connection.state = S_WRITE;
//                 return;
//             }

//             std::string head;
//             if (!http::extract_next_head(connection.readBuffer, head)) {
//                 std::cerr << "[HDR-INCOMPLETE] t=" << _nowMs
//                           << " fd=" << clientFd
//                           << " rb=" << connection.readBuffer.size()
//                           << "\n";
//                 return;
//             }

//             std::cerr << "[HDR-EXTRACT] t=" << _nowMs
//                       << " fd=" << clientFd
//                       << " headBytes=" << head.size()
//                       << " rb_after=" << connection.readBuffer.size()
//                       << "\n";

//             int status = 0;
//             std::string reason;
//             if (!http::parse_head(head, connection.request, status, reason)) {

//                 if (!connection.srv)
//                     throw std::runtime_error("Internal bug: connection.srv is NULL");
//                 const Server& active = *connection.srv;

//                 std::cerr << "[HDR-PARSE-FAIL] t=" << _nowMs
//                           << " fd=" << clientFd
//                           << " status=" << status
//                           << " reason=\"" << reason << "\""
//                           << " ka=" << (connection.request.keep_alive ? 1 : 0)
//                           << "\n";

//                 if (status == 413)
//                     connection.request.keep_alive = false;

//                 connection.writeBuffer = http::build_error_response(active, status, reason, connection.request.keep_alive);
//                 connection.writeOffset = 0;

//                 std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                 if (pit != _fdIndex.end())
//                     _fds[pit->second].events = POLLOUT;

//                 connection.state = S_WRITE;
//                 return;
//             }

//             connection.headersComplete = true;

//             std::cerr << "[HDR-OK] t=" << _nowMs
//                       << " fd=" << clientFd
//                       << " m=" << connection.request.method
//                       << " target=" << connection.request.target
//                       << " path=" << connection.request.path
//                       << " br=" << connection.request.body_reader_state
//                       << " cl=" << connection.request.content_length
//                       << " te=\"" << connection.request.transfer_encoding << "\""
//                       << " ka=" << (connection.request.keep_alive ? 1 : 0)
//                       << " exp=" << (connection.request.expectContinue ? 1 : 0)
//                       << " rb=" << connection.readBuffer.size()
//                       << "\n";

//             // ---- client_max_body_size resolution (igual ao teu) ----
//             size_t limit = std::numeric_limits<size_t>::max();
//             bool fromLoc = false;
//             const Location* loc = NULL;

//             if (connection.srv) {
//                 if ((loc = longestPrefixMatch(*connection.srv, connection.request.path))) {
//                     if (loc->directives.count("client_max_body_size")) {
//                         limit = parseSize(loc->directives.find("client_max_body_size")->second);
//                         fromLoc = true;
//                     }
//                 }
//                 if (!fromLoc && connection.srv->directives.count("client_max_body_size"))
//                     limit = parseSize(connection.srv->directives.find("client_max_body_size")->second);
//             }
//             connection.clientMaxBodySize = limit;

//             // ---- EARLY CHECKS (o teu bloco) ----
//             {
//                 const Server& active = connection.srv ? *connection.srv : _servers[0];

//                 bool isCgiRequest = false;
//                 std::string matchedExt;
//                 std::string interpreterPath;

//                 if (loc && loc->directives.count("cgi_pass")) {

//                     const std::string& cgiVal = loc->directives.find("cgi_pass")->second;

//                     std::vector<std::string> tokens;
//                     std::string current;
//                     for (size_t i = 0; i <= cgiVal.size(); ++i) {
//                         char c = (i < cgiVal.size()) ? cgiVal[i] : ' ';
//                         if (!std::isspace(static_cast<unsigned char>(c)))
//                             current += c;
//                         else if (!current.empty()) {
//                             tokens.push_back(current);
//                             current.clear();
//                         }
//                     }

//                     const std::string& p = connection.request.path;

//                     for (size_t i = 0; i + 1 < tokens.size(); i += 2) {
//                         const std::string& ext = tokens[i];
//                         const std::string& bin = tokens[i + 1];

//                         if (!ext.empty() && p.size() >= ext.size()
//                             && p.compare(p.size() - ext.size(), ext.size(), ext) == 0) {
//                             isCgiRequest = true;
//                             matchedExt = ext;
//                             interpreterPath = bin;
//                             break;
//                         }
//                     }
//                 }

//                 std::string scriptFsPath;
//                 if (isCgiRequest) {
//                     std::string root = ".";
//                     if (loc && loc->directives.count("root"))
//                         root = loc->directives.find("root")->second;
//                     else if (connection.srv && connection.srv->directives.count("root"))
//                         root = connection.srv->directives.find("root")->second;

//                     std::string subPath = connection.request.path;
//                     if (loc) {
//                         const std::string& lp = loc->path;
//                         if (subPath.size() >= lp.size() && subPath.compare(0, lp.size(), lp) == 0)
//                             subPath = subPath.substr(lp.size());
//                     }
//                     if (subPath.empty())
//                         subPath = "/";

//                     bool rootEnds = (!root.empty() && root[root.size() - 1] == '/');
//                     bool subStarts = (!subPath.empty() && subPath[0] == '/');

//                     if (rootEnds && subStarts)
//                         scriptFsPath = root.substr(0, root.size() - 1) + subPath;
//                     else if (rootEnds ^ subStarts)
//                         scriptFsPath = root + subPath;
//                     else
//                         scriptFsPath = root + "/" + subPath;
//                 }

//                 if (connection.request.body_reader_state == BR_CONTENT_LENGTH
//                     && connection.request.content_length > connection.clientMaxBodySize) {

//                     std::cerr << "[EARLY-413] t=" << _nowMs
//                               << " fd=" << clientFd
//                               << " cl=" << connection.request.content_length
//                               << " max=" << connection.clientMaxBodySize
//                               << "\n";

//                     connection.request.keep_alive = false;
//                     connection.request.expectContinue = false;
//                     connection.sentContinue = false;

//                     connection.writeBuffer = http::build_error_response(active, 413, "Payload Too Large", false);
//                     connection.writeOffset = 0;

//                     // se há body, drena antes de responder (para não bloquear cliente)
//                     if (connection.request.body_reader_state != BR_NONE) {
//                         connection.drainedBytes = 0;
//                         connection.state = S_DRAIN;
//                         std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                         if (pit != _fdIndex.end())
//                             _fds[pit->second].events = POLLIN;
//                         return;
//                     }

//                     std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                     if (pit != _fdIndex.end())
//                         _fds[pit->second].events = POLLOUT;

//                     connection.state = S_WRITE;
//                     return;
//                 }

//                 const std::string* methodsStr = NULL;
//                 if (loc && loc->directives.count("methods"))
//                     methodsStr = &loc->directives.find("methods")->second;
//                 else if (connection.srv && connection.srv->directives.count("methods"))
//                     methodsStr = &connection.srv->directives.find("methods")->second;

//                 if (methodsStr) {
//                     std::string reqM = connection.request.method;
//                     for (size_t i = 0; i < reqM.size(); ++i)
//                         reqM[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(reqM[i])));

// // Treat HEAD as GET for allow-list purposes
// if (reqM == "HEAD")
//     reqM = "GET";

//                     bool allowed = false;
//                     std::string token;

//                     for (size_t i = 0; i <= methodsStr->size(); ++i) {
//                         char c = (i < methodsStr->size()) ? (*methodsStr)[i] : ' ';
//                         if (std::isalpha(static_cast<unsigned char>(c)))
//                             token += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
//                         else if (!token.empty()) {
//                             if (token == reqM) {
//                                 allowed = true;
//                                 break;
//                             }
//                             token.clear();
//                         }
//                     }

//                     if (!allowed) {

//                         bool keep = connection.request.keep_alive;
//                         if (connection.request.body_reader_state != BR_NONE && !connection.request.expectContinue)
//                             keep = false;

//                         std::cerr << "[EARLY-405] t=" << _nowMs
//                                   << " fd=" << clientFd
//                                   << " method=" << connection.request.method
//                                   << " keep=" << (keep ? 1 : 0)
//                                   << "\n";

//                         connection.request.keep_alive = keep;
//                         connection.sentContinue = false;

//                         connection.writeBuffer = http::build_error_response(active, 405, "Method Not Allowed", keep);
//                         connection.writeOffset = 0;

//                         // ✅ se keep==0 e há body => DRAIN
//                         if (!keep && connection.request.body_reader_state != BR_NONE && !connection.request.expectContinue) {
//                             connection.drainedBytes = 0;
//                             connection.state = S_DRAIN;
//                             std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                             if (pit != _fdIndex.end())
//                                 _fds[pit->second].events = POLLIN;
//                             return;
//                         }

//                         std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                         if (pit != _fdIndex.end())
//                             _fds[pit->second].events = POLLOUT;

//                         connection.state = S_WRITE;
//                         return;
//                     }
//                 }

//                 // if (isCgiRequest) {
//                 //     struct stat st;
//                 //     if (stat(scriptFsPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {

//                 //         bool keep = connection.request.keep_alive;
//                 //         if (connection.request.body_reader_state != BR_NONE && !connection.request.expectContinue)
//                 //             keep = false;

//                 //         std::cerr << "[EARLY-CGI-404] t=" << _nowMs
//                 //                   << " fd=" << clientFd
//                 //                   << " script=\"" << scriptFsPath << "\""
//                 //                   << " keep=" << (keep ? 1 : 0)
//                 //                   << "\n";

//                 //         connection.request.keep_alive = keep;
//                 //         connection.sentContinue = false;

//                 //         connection.writeBuffer = http::build_error_response(active, 404, "Not Found", keep);
//                 //         connection.writeOffset = 0;

//                 //         if (!keep && connection.request.body_reader_state != BR_NONE && !connection.request.expectContinue) {
//                 //             connection.drainedBytes = 0;
//                 //             connection.state = S_DRAIN;
//                 //             std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                 //             if (pit != _fdIndex.end())
//                 //                 _fds[pit->second].events = POLLIN;
//                 //             return;
//                 //         }

//                 //         std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                 //         if (pit != _fdIndex.end())
//                 //             _fds[pit->second].events = POLLOUT;

//                 //         connection.state = S_WRITE;
//                 //         return;
//                 //     }
//                 // }

// if (isCgiRequest) {
//     // Only pre-check CGI file existence for methods that actually "execute" the script directly.
//     // For POST/PUT, let the App layer decide (uploads, create, etc.)
//     std::string m = connection.request.method;
//     for (size_t i = 0; i < m.size(); ++i)
//         m[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(m[i])));

//     if (m == "GET" || m == "HEAD") {
//         struct stat st;
//         if (stat(scriptFsPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
//             bool keep = connection.request.keep_alive;
//             if (connection.request.body_reader_state != BR_NONE && !connection.request.expectContinue)
//                 keep = false;

//             std::cerr << "[EARLY-CGI-404] t=" << _nowMs
//                       << " fd=" << clientFd
//                       << " script=\"" << scriptFsPath << "\""
//                       << " keep=" << (keep ? 1 : 0)
//                       << "\n";

//             connection.request.keep_alive = keep;
//             connection.sentContinue = false;

//             connection.writeBuffer = http::build_error_response(active, 404, "Not Found", keep);
//             connection.writeOffset = 0;

//             std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//             if (pit != _fdIndex.end())
//                 _fds[pit->second].events = POLLOUT;

//             connection.state = S_WRITE;
//             return;
//         }
//     }
// }



//             }

//             // ---- Transition depending on body presence ----
//             if (connection.request.body_reader_state == BR_NONE) {

//                 std::cerr << "[DISPATCH-NO-BODY] t=" << _nowMs
//                           << " fd=" << clientFd
//                           << " rb=" << connection.readBuffer.size()
//                           << "\n";

//                 handleRequest(connection);
//                 return;
//             }

//             if (connection.request.expectContinue == true) {

//                 std::cerr << "[SEND-100] t=" << _nowMs
//                           << " fd=" << clientFd
//                           << " rb=" << connection.readBuffer.size()
//                           << "\n";

//                 connection.writeBuffer = "HTTP/1.1 100 Continue\r\n\r\n";
//                 connection.writeOffset = 0;
//                 connection.sentContinue = true;

//                 std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                 if (pit != _fdIndex.end())
//                     _fds[pit->second].events = POLLOUT;

//                 connection.state = S_WRITE;
//                 return;
//             }

//             connection.state = S_BODY;

//             std::cerr << "[STATE->BODY] t=" << _nowMs
//                       << " fd=" << clientFd
//                       << " rb=" << connection.readBuffer.size()
//                       << "\n";

//             continue;
//         }

//         // ---------------- BODY ----------------
//         if (connection.state == S_BODY) {

//             int status = 0;
//             std::string reason;
//             http::BodyResult result = http::BODY_INCOMPLETE;

//             const std::size_t maxBody = connection.clientMaxBodySize;

//             switch (connection.request.body_reader_state) {
//                 case BR_CONTENT_LENGTH:
//                     result = http::consume_body_content_length(connection, maxBody, status, reason);
//                     break;
//                 case BR_CHUNKED:
//                     result = http::consume_body_chunked(connection, maxBody, status, reason);
//                     break;
//                 default:
//                     status = 400;
//                     reason = "Bad Request";
//                     result = http::BODY_ERROR;
//             }

//             std::cerr << "[BODY-CONSUME] t=" << _nowMs
//                       << " fd=" << clientFd
//                       << " res=" << result
//                       << " rb=" << connection.readBuffer.size()
//                       << " body=" << connection.request.body.size()
//                       << "\n";

//             if (result == http::BODY_COMPLETE) {

//                 std::cerr << "[DISPATCH-BODY] t=" << _nowMs
//                           << " fd=" << clientFd
//                           << " body=" << connection.request.body.size()
//                           << " rb=" << connection.readBuffer.size()
//                           << "\n";

//                 handleRequest(connection);
//                 return;
//             }

//             if (result == http::BODY_ERROR) {

//                 const Server& active = connection.srv ? *connection.srv : _servers[0];

//                 std::cerr << "[BODY-ERR] t=" << _nowMs
//                           << " fd=" << clientFd
//                           << " status=" << status
//                           << " reason=\"" << reason << "\""
//                           << " ka=" << (connection.request.keep_alive ? 1 : 0)
//                           << "\n";

//                 if (status == 413)
//                     connection.request.keep_alive = false;

//                 connection.writeBuffer = http::build_error_response(active, status, reason, connection.request.keep_alive);
//                 connection.writeOffset = 0;

//                 std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                 if (pit != _fdIndex.end())
//                     _fds[pit->second].events = POLLOUT;

//                 connection.state = S_WRITE;
//                 return;
//             }

//             return; // incomplete
//         }

//         return;
//     }
// }




// void	ServerRunner::writeToClient(int clientFd)	{

// 	std::map<int, Connection>::iterator it = _connections.find(clientFd);	// make sure this clientFd exists
// 	if (it == _connections.end())
// 		return;

// 	Connection& connection = it->second;

// 	// writeOffset is the index of the first byte NOT YET sent
// 	if (connection.writeOffset >= connection.writeBuffer.size())	{
// 		// nothing left to send → back to POLLIN
// 		std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//     	if (pit != _fdIndex.end())
// 			_fds[pit->second].events = POLLIN;
// 			// only about read so overwrite everything.
// 		return ;
// 	}

// 	const char*	base = connection.writeBuffer.data();	// pointer to char. Need a raw pointer for pointer arithmetic
// 	while (connection.writeOffset < connection.writeBuffer.size())	{
// 		const char*	buf = base + connection.writeOffset;	// pointer to the element that is (n) char after &writeBuffer[0] 
// 		std::size_t	remaining = connection.writeBuffer.size() - connection.writeOffset;	// from the writeBuffer, subtract how much were written already.
// 		ssize_t		n = write(clientFd, buf, remaining);
// 		if (n > 0)	{
// 			connection.writeOffset += static_cast<std::size_t>(n);
// 			connection.lastActiveMs = _nowMs;
// 			continue;	// try to flush more
// 		}
// 		// Ensure we stay interested in POLLOUT so we'll retry writing. Only when n <= 0.
// 		std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//     	if (pit != _fdIndex.end())
// 			_fds[pit->second].events = POLLOUT;

// 		return ;	// Stop if the socket cannot accept more
// 	}

// 	// If we just sent "100 Continue", continue reading the same request body.
// 	if (connection.sentContinue)	{
// 		connection.sentContinue = false;
// 		connection.writeBuffer.clear();
// 		connection.writeOffset = 0;

// 		connection.state = S_BODY;

// 		std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
// 		if (pit != _fdIndex.end())
// 			_fds[pit->second].events = POLLIN;

// 		return ;
// 	}

// 	// Finished sending the full response
// 	const bool	keep = connection.request.keep_alive;
// 	if (keep)	{
// 		connection.writeBuffer.clear();
// 		connection.writeOffset = 0;
// 		connection.readBuffer.clear();
// 		connection.headersComplete = false;
// 		connection.sentContinue = false;

// std::cerr << "[KA-RESET] t=" << _nowMs
//           << " fd=" << clientFd
//           << " before: ka=" << (connection.request.keep_alive ? 1 : 0)
//           << " exp=" << (connection.request.expectContinue ? 1 : 0)
//           << " br=" << connection.request.body_reader_state
//           << " cs=" << connection.request.chunk_state
//           << " cbl=" << connection.request.chunk_bytes_left
//           << " te=\"" << connection.request.transfer_encoding << "\""
//           << " cl=" << connection.request.content_length
//           << " brx=" << connection.request.body_received
//           << " body=" << connection.request.body.size()
//           << "\n";

// connection.request = HTTP_Request();

// std::cerr << "[KA-RESET] t=" << _nowMs
//           << " fd=" << clientFd
//           << " after:  ka=" << (connection.request.keep_alive ? 1 : 0)
//           << " exp=" << (connection.request.expectContinue ? 1 : 0)
//           << " br=" << connection.request.body_reader_state
//           << " cs=" << connection.request.chunk_state
//           << " cbl=" << connection.request.chunk_bytes_left
//           << " te=\"" << connection.request.transfer_encoding << "\""
//           << " cl=" << connection.request.content_length
//           << " brx=" << connection.request.body_received
//           << " body=" << connection.request.body.size()
//           << "\n";



// 		connection.response = HTTP_Response();
// 		connection.state = S_HEADERS;
// 		connection.kaIdleStartMs = _nowMs;
// 		connection.lastActiveMs = _nowMs;

// 		// Back to read-only interest
// 		std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//     	if (pit != _fdIndex.end())
// 			_fds[pit->second].events = POLLIN;
// 	}
// 	else
// 		closeConnection(clientFd);
// }


// void    ServerRunner::readFromClient(int clientFd) {

//     std::map<int, Connection>::iterator it = _connections.find(clientFd);
//     if (it == _connections.end())
//         return;

//     Connection& connection = it->second;

//     // 1) Drain readable bytes into readBuffer (non-blocking)
//     char buffer[4096];
//     std::size_t totalRead = 0;

//     for (;;) {
//         ssize_t n = read(clientFd, buffer, sizeof(buffer));

//         if (n > 0) {
//             connection.readBuffer.append(buffer, static_cast<std::size_t>(n));
//             totalRead += static_cast<std::size_t>(n);

//             connection.lastActiveMs = _nowMs;

//             if (connection.state == S_HEADERS && connection.kaIdleStartMs != 0)
//                 connection.kaIdleStartMs = 0;

//             continue;
//         }

//         if (n == 0) {
//             std::cerr << "[READ-EOF] t=" << _nowMs
//                       << " fd=" << clientFd
//                       << " totalRead=" << totalRead
//                       << " rb=" << connection.readBuffer.size()
//                       << " state=" << connection.state
//                       << "\n";
//             closeConnection(clientFd);
//             return;
//         }

//         if (errno == EINTR)
//             continue;
//         if (errno == EAGAIN || errno == EWOULDBLOCK)
//             break;

//         std::cerr << "[READ-ERR] t=" << _nowMs
//                   << " fd=" << clientFd
//                   << " errno=" << errno
//                   << " msg=" << std::strerror(errno)
//                   << " totalRead=" << totalRead
//                   << " rb=" << connection.readBuffer.size()
//                   << " state=" << connection.state
//                   << "\n";
//         closeConnection(clientFd);
//         return;
//     }

//     if (totalRead > 0) {
//         std::cerr << "[READ] t=" << _nowMs
//                   << " fd=" << clientFd
//                   << " got=" << totalRead
//                   << " rb=" << connection.readBuffer.size()
//                   << " state=" << connection.state
//                   << "\n";
//     }

//     // Se estamos a escrever, não mexer.
//     if (connection.state == S_WRITE) {
//         std::cerr << "[READ-SKIP-PARSE] t=" << _nowMs
//                   << " fd=" << clientFd
//                   << " rb=" << connection.readBuffer.size()
//                   << " (state=S_WRITE)\n";
//         return;
//     }

//     // ---------------- DRAIN ----------------
//     if (connection.state == S_DRAIN) {

//         int status = 0;
//         std::string reason;
//         http::BodyResult result = http::BODY_INCOMPLETE;

//         const std::size_t maxBody = connection.clientMaxBodySize;

//         if (connection.request.body_reader_state == BR_CONTENT_LENGTH)
//             result = http::consume_body_content_length_drain(connection, maxBody, status, reason);
//         else if (connection.request.body_reader_state == BR_CHUNKED)
//             result = http::consume_body_chunked_drain(connection, maxBody, status, reason);
//         else
//             result = http::BODY_COMPLETE;

//         std::cerr << "[DRAIN-CONSUME] t=" << _nowMs
//                   << " fd=" << clientFd
//                   << " res=" << result
//                   << " rb=" << connection.readBuffer.size()
//                   << " drained=" << connection.drainedBytes
//                   << "\n";

//         if (result == http::BODY_COMPLETE) {
//             std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//             if (pit != _fdIndex.end())
//                 _fds[pit->second].events = POLLOUT;

//             connection.state = S_WRITE;

//             std::cerr << "[DRAIN-DONE->WRITE] t=" << _nowMs
//                       << " fd=" << clientFd
//                       << " rb=" << connection.readBuffer.size()
//                       << "\n";
//             return;
//         }

//         if (result == http::BODY_ERROR) {
//             std::cerr << "[DRAIN-ERR] t=" << _nowMs
//                       << " fd=" << clientFd
//                       << " status=" << status
//                       << " reason=\"" << reason << "\"\n";
//             closeConnection(clientFd);
//             return;
//         }

//         return; // incomplete
//     }

//     // 2) Parse as much as possible from readBuffer (headers + maybe body)
//     for (;;) {

//         // ---------------- HEADERS ----------------
//         if (connection.state == S_HEADERS) {

//             static const std::size_t MAX_HEADER_BYTES = 16 * 1024;
//             if (connection.readBuffer.size() > MAX_HEADER_BYTES
//                 && connection.readBuffer.find("\r\n\r\n") == std::string::npos) {

//                 int st = 431;
//                 std::string reason = "Request Header Fields Too Large";
//                 const Server& active = connection.srv ? *connection.srv : _servers[0];

//                 std::cerr << "[HDR-TOO-LARGE] t=" << _nowMs
//                           << " fd=" << clientFd
//                           << " rb=" << connection.readBuffer.size()
//                           << "\n";

//                 connection.writeBuffer = http::build_error_response(active, st, reason, connection.request.keep_alive);
//                 connection.writeOffset = 0;

//                 std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                 if (pit != _fdIndex.end())
//                     _fds[pit->second].events = POLLOUT;

//                 connection.state = S_WRITE;
//                 return;
//             }

//             std::string head;
//             if (!http::extract_next_head(connection.readBuffer, head)) {
//                 std::cerr << "[HDR-INCOMPLETE] t=" << _nowMs
//                           << " fd=" << clientFd
//                           << " rb=" << connection.readBuffer.size()
//                           << "\n";
//                 return;
//             }

//             std::cerr << "[HDR-EXTRACT] t=" << _nowMs
//                       << " fd=" << clientFd
//                       << " headBytes=" << head.size()
//                       << " rb_after=" << connection.readBuffer.size()
//                       << "\n";

//             int status = 0;
//             std::string reason;
//             if (!http::parse_head(head, connection.request, status, reason)) {

//                 if (!connection.srv)
//                     throw std::runtime_error("Internal bug: connection.srv is NULL");
//                 const Server& active = *connection.srv;

//                 std::cerr << "[HDR-PARSE-FAIL] t=" << _nowMs
//                           << " fd=" << clientFd
//                           << " status=" << status
//                           << " reason=\"" << reason << "\""
//                           << " ka=" << (connection.request.keep_alive ? 1 : 0)
//                           << "\n";

//                 if (status == 413)
//                     connection.request.keep_alive = false;

//                 connection.writeBuffer = http::build_error_response(active, status, reason, connection.request.keep_alive);
//                 connection.writeOffset = 0;

//                 std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                 if (pit != _fdIndex.end())
//                     _fds[pit->second].events = POLLOUT;

//                 connection.state = S_WRITE;
//                 return;
//             }

//             connection.headersComplete = true;

//             std::cerr << "[HDR-OK] t=" << _nowMs
//                       << " fd=" << clientFd
//                       << " m=" << connection.request.method
//                       << " target=" << connection.request.target
//                       << " path=" << connection.request.path
//                       << " br=" << connection.request.body_reader_state
//                       << " cl=" << connection.request.content_length
//                       << " te=\"" << connection.request.transfer_encoding << "\""
//                       << " ka=" << (connection.request.keep_alive ? 1 : 0)
//                       << " exp=" << (connection.request.expectContinue ? 1 : 0)
//                       << " rb=" << connection.readBuffer.size()
//                       << "\n";

//             // ---- client_max_body_size resolution ----
//             size_t limit = std::numeric_limits<size_t>::max();
//             bool fromLoc = false;
//             const Location* loc = NULL;

//             if (connection.srv) {
//                 if ((loc = longestPrefixMatch(*connection.srv, connection.request.path))) {
//                     if (loc->directives.count("client_max_body_size")) {
//                         limit = parseSize(loc->directives.find("client_max_body_size")->second);
//                         fromLoc = true;
//                     }
//                 }
//                 if (!fromLoc && connection.srv->directives.count("client_max_body_size"))
//                     limit = parseSize(connection.srv->directives.find("client_max_body_size")->second);
//             }
//             connection.clientMaxBodySize = limit;

//             // ---- EARLY CHECKS (apenas os que fazem sentido no core) ----
//             {
//                 const Server& active = connection.srv ? *connection.srv : _servers[0];

//                 // EARLY 413 para Content-Length
//                 if (connection.request.body_reader_state == BR_CONTENT_LENGTH
//                     && connection.request.content_length > connection.clientMaxBodySize) {

//                     std::cerr << "[EARLY-413] t=" << _nowMs
//                               << " fd=" << clientFd
//                               << " cl=" << connection.request.content_length
//                               << " max=" << connection.clientMaxBodySize
//                               << "\n";

//                     connection.request.keep_alive = false;
//                     connection.request.expectContinue = false;
//                     connection.sentContinue = false;

//                     connection.writeBuffer = http::build_error_response(active, 413, "Payload Too Large", false);
//                     connection.writeOffset = 0;

//                     // se há body, drena antes de responder
//                     if (connection.request.body_reader_state != BR_NONE) {
//                         connection.drainedBytes = 0;
//                         connection.state = S_DRAIN;
//                         std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                         if (pit != _fdIndex.end())
//                             _fds[pit->second].events = POLLIN;
//                         return;
//                     }

//                     std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                     if (pit != _fdIndex.end())
//                         _fds[pit->second].events = POLLOUT;

//                     connection.state = S_WRITE;
//                     return;
//                 }

//                 // EARLY 405 via allow-list de methods
//                 const std::string* methodsStr = NULL;
//                 if (loc && loc->directives.count("methods"))
//                     methodsStr = &loc->directives.find("methods")->second;
//                 else if (connection.srv && connection.srv->directives.count("methods"))
//                     methodsStr = &connection.srv->directives.find("methods")->second;

//                 if (methodsStr) {
//                     std::string reqM = connection.request.method;
//                     for (size_t i = 0; i < reqM.size(); ++i)
//                         reqM[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(reqM[i])));

//                     // Treat HEAD as GET for allow-list purposes
//                     if (reqM == "HEAD")
//                         reqM = "GET";

//                     bool allowed = false;
//                     std::string token;

//                     for (size_t i = 0; i <= methodsStr->size(); ++i) {
//                         char c = (i < methodsStr->size()) ? (*methodsStr)[i] : ' ';
//                         if (std::isalpha(static_cast<unsigned char>(c)))
//                             token += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
//                         else if (!token.empty()) {
//                             if (token == reqM) {
//                                 allowed = true;
//                                 break;
//                             }
//                             token.clear();
//                         }
//                     }

//                     if (!allowed) {

//                         bool keep = connection.request.keep_alive;
//                         if (connection.request.body_reader_state != BR_NONE && !connection.request.expectContinue)
//                             keep = false;

//                         std::cerr << "[EARLY-405] t=" << _nowMs
//                                   << " fd=" << clientFd
//                                   << " method=" << connection.request.method
//                                   << " keep=" << (keep ? 1 : 0)
//                                   << "\n";

//                         connection.request.keep_alive = keep;
//                         connection.sentContinue = false;

//                         connection.writeBuffer = http::build_error_response(active, 405, "Method Not Allowed", keep);
//                         connection.writeOffset = 0;

//                         // se keep==0 e há body => DRAIN
//                         if (!keep && connection.request.body_reader_state != BR_NONE && !connection.request.expectContinue) {
//                             connection.drainedBytes = 0;
//                             connection.state = S_DRAIN;
//                             std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                             if (pit != _fdIndex.end())
//                                 _fds[pit->second].events = POLLIN;
//                             return;
//                         }

//                         std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                         if (pit != _fdIndex.end())
//                             _fds[pit->second].events = POLLOUT;

//                         connection.state = S_WRITE;
//                         return;
//                     }
//                 }
//             }

//             // ---- Transition depending on body presence ----
//             if (connection.request.body_reader_state == BR_NONE) {

//                 std::cerr << "[DISPATCH-NO-BODY] t=" << _nowMs
//                           << " fd=" << clientFd
//                           << " rb=" << connection.readBuffer.size()
//                           << "\n";

//                 handleRequest(connection);
//                 return;
//             }

//             if (connection.request.expectContinue == true) {

//                 std::cerr << "[SEND-100] t=" << _nowMs
//                           << " fd=" << clientFd
//                           << " rb=" << connection.readBuffer.size()
//                           << "\n";

//                 connection.writeBuffer = "HTTP/1.1 100 Continue\r\n\r\n";
//                 connection.writeOffset = 0;
//                 connection.sentContinue = true;

//                 std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                 if (pit != _fdIndex.end())
//                     _fds[pit->second].events = POLLOUT;

//                 connection.state = S_WRITE;
//                 return;
//             }

//             connection.state = S_BODY;

//             std::cerr << "[STATE->BODY] t=" << _nowMs
//                       << " fd=" << clientFd
//                       << " rb=" << connection.readBuffer.size()
//                       << "\n";

//             continue;
//         }

//         // ---------------- BODY ----------------
//         if (connection.state == S_BODY) {

//             int status = 0;
//             std::string reason;
//             http::BodyResult result = http::BODY_INCOMPLETE;

//             const std::size_t maxBody = connection.clientMaxBodySize;

//             switch (connection.request.body_reader_state) {
//                 case BR_CONTENT_LENGTH:
//                     result = http::consume_body_content_length(connection, maxBody, status, reason);
//                     break;
//                 case BR_CHUNKED:
//                     result = http::consume_body_chunked(connection, maxBody, status, reason);
//                     break;
//                 default:
//                     status = 400;
//                     reason = "Bad Request";
//                     result = http::BODY_ERROR;
//             }

//             std::cerr << "[BODY-CONSUME] t=" << _nowMs
//                       << " fd=" << clientFd
//                       << " res=" << result
//                       << " rb=" << connection.readBuffer.size()
//                       << " body=" << connection.request.body.size()
//                       << "\n";

//             if (result == http::BODY_COMPLETE) {

//                 std::cerr << "[DISPATCH-BODY] t=" << _nowMs
//                           << " fd=" << clientFd
//                           << " body=" << connection.request.body.size()
//                           << " rb=" << connection.readBuffer.size()
//                           << "\n";

//                 handleRequest(connection);
//                 return;
//             }

//             if (result == http::BODY_ERROR) {

//                 const Server& active = connection.srv ? *connection.srv : _servers[0];

//                 std::cerr << "[BODY-ERR] t=" << _nowMs
//                           << " fd=" << clientFd
//                           << " status=" << status
//                           << " reason=\"" << reason << "\""
//                           << " ka=" << (connection.request.keep_alive ? 1 : 0)
//                           << "\n";

//                 if (status == 413)
//                     connection.request.keep_alive = false;

//                 connection.writeBuffer = http::build_error_response(active, status, reason, connection.request.keep_alive);
//                 connection.writeOffset = 0;

//                 std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                 if (pit != _fdIndex.end())
//                     _fds[pit->second].events = POLLOUT;

//                 connection.state = S_WRITE;
//                 return;
//             }

//             return; // incomplete
//         }

//         return;
//     }
// }


void    ServerRunner::readFromClient(int clientFd) {

    std::map<int, Connection>::iterator it = _connections.find(clientFd);
    if (it == _connections.end())
        return;

    Connection& connection = it->second;

    // 1) Drain readable bytes into readBuffer (non-blocking)
    char buffer[4096];
    std::size_t totalRead = 0;

    for (;;) {
        ssize_t n = read(clientFd, buffer, sizeof(buffer));

        if (n > 0) {
            connection.readBuffer.append(buffer, static_cast<std::size_t>(n));
            totalRead += static_cast<std::size_t>(n);

            connection.lastActiveMs = _nowMs;

            if (connection.state == S_HEADERS && connection.kaIdleStartMs != 0)
                connection.kaIdleStartMs = 0;

            continue;
        }

        if (n == 0) {
            std::cerr << "[READ-EOF] t=" << _nowMs
                      << " fd=" << clientFd
                      << " totalRead=" << totalRead
                      << " rb=" << connection.readBuffer.size()
                      << " state=" << connection.state
                      << "\n";
            closeConnection(clientFd);
            return;
        }

        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;

        std::cerr << "[READ-ERR] t=" << _nowMs
                  << " fd=" << clientFd
                  << " errno=" << errno
                  << " msg=" << std::strerror(errno)
                  << " totalRead=" << totalRead
                  << " rb=" << connection.readBuffer.size()
                  << " state=" << connection.state
                  << "\n";
        closeConnection(clientFd);
        return;
    }

    if (totalRead > 0) {
        std::cerr << "[READ] t=" << _nowMs
                  << " fd=" << clientFd
                  << " got=" << totalRead
                  << " rb=" << connection.readBuffer.size()
                  << " state=" << connection.state
                  << "\n";
    }

    // Se estamos a escrever, não mexer.
    if (connection.state == S_WRITE) {
        std::cerr << "[READ-SKIP-PARSE] t=" << _nowMs
                  << " fd=" << clientFd
                  << " rb=" << connection.readBuffer.size()
                  << " (state=S_WRITE)\n";
        return;
    }

    // ---------------- DRAIN ----------------
    if (connection.state == S_DRAIN) {

        int status = 0;
        std::string reason;
        http::BodyResult result = http::BODY_INCOMPLETE;

        const std::size_t maxBody = connection.clientMaxBodySize;

        if (connection.request.body_reader_state == BR_CONTENT_LENGTH)
            result = http::consume_body_content_length_drain(connection, maxBody, status, reason);
        else if (connection.request.body_reader_state == BR_CHUNKED)
            result = http::consume_body_chunked_drain(connection, maxBody, status, reason);
        else
            result = http::BODY_COMPLETE;

        std::cerr << "[DRAIN-CONSUME] t=" << _nowMs
                  << " fd=" << clientFd
                  << " res=" << result
                  << " rb=" << connection.readBuffer.size()
                  << " drained=" << connection.drainedBytes
                  << "\n";

        if (result == http::BODY_COMPLETE) {
            std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
            if (pit != _fdIndex.end())
                _fds[pit->second].events = POLLOUT;

            connection.state = S_WRITE;

            std::cerr << "[DRAIN-DONE->WRITE] t=" << _nowMs
                      << " fd=" << clientFd
                      << " rb=" << connection.readBuffer.size()
                      << "\n";
            return;
        }

        if (result == http::BODY_ERROR) {
            std::cerr << "[DRAIN-ERR] t=" << _nowMs
                      << " fd=" << clientFd
                      << " status=" << status
                      << " reason=\"" << reason << "\"\n";
            closeConnection(clientFd);
            return;
        }

        return; // incomplete
    }

    // 2) Parse as much as possible from readBuffer (headers + maybe body)
    for (;;) {

        // ---------------- HEADERS ----------------
        if (connection.state == S_HEADERS) {

            static const std::size_t MAX_HEADER_BYTES = 16 * 1024;
            if (connection.readBuffer.size() > MAX_HEADER_BYTES
                && connection.readBuffer.find("\r\n\r\n") == std::string::npos) {

                int st = 431;
                std::string reason = "Request Header Fields Too Large";
                const Server& active = connection.srv ? *connection.srv : _servers[0];

                std::cerr << "[HDR-TOO-LARGE] t=" << _nowMs
                          << " fd=" << clientFd
                          << " rb=" << connection.readBuffer.size()
                          << "\n";

                connection.writeBuffer = http::build_error_response(active, st, reason, connection.request.keep_alive);
                connection.writeOffset = 0;

                std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
                if (pit != _fdIndex.end())
                    _fds[pit->second].events = POLLOUT;

                connection.state = S_WRITE;
                return;
            }

            std::string head;
            if (!http::extract_next_head(connection.readBuffer, head)) {
                std::cerr << "[HDR-INCOMPLETE] t=" << _nowMs
                          << " fd=" << clientFd
                          << " rb=" << connection.readBuffer.size()
                          << "\n";
                return;
            }

            std::cerr << "[HDR-EXTRACT] t=" << _nowMs
                      << " fd=" << clientFd
                      << " headBytes=" << head.size()
                      << " rb_after=" << connection.readBuffer.size()
                      << "\n";

            int status = 0;
            std::string reason;
            if (!http::parse_head(head, connection.request, status, reason)) {

                if (!connection.srv)
                    throw std::runtime_error("Internal bug: connection.srv is NULL");
                const Server& active = *connection.srv;

                std::cerr << "[HDR-PARSE-FAIL] t=" << _nowMs
                          << " fd=" << clientFd
                          << " status=" << status
                          << " reason=\"" << reason << "\""
                          << " ka=" << (connection.request.keep_alive ? 1 : 0)
                          << "\n";

                if (status == 413)
                    connection.request.keep_alive = false;

                connection.writeBuffer = http::build_error_response(active, status, reason, connection.request.keep_alive);
                connection.writeOffset = 0;

                std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
                if (pit != _fdIndex.end())
                    _fds[pit->second].events = POLLOUT;

                connection.state = S_WRITE;
                return;
            }

            connection.headersComplete = true;

            std::cerr << "[HDR-OK] t=" << _nowMs
                      << " fd=" << clientFd
                      << " m=" << connection.request.method
                      << " target=" << connection.request.target
                      << " path=" << connection.request.path
                      << " br=" << connection.request.body_reader_state
                      << " cl=" << connection.request.content_length
                      << " te=\"" << connection.request.transfer_encoding << "\""
                      << " ka=" << (connection.request.keep_alive ? 1 : 0)
                      << " exp=" << (connection.request.expectContinue ? 1 : 0)
                      << " rb=" << connection.readBuffer.size()
                      << "\n";

            // ---- client_max_body_size resolution ----
            size_t limit = std::numeric_limits<size_t>::max();
            bool fromLoc = false;
            const Location* loc = NULL;

            if (connection.srv) {
                if ((loc = longestPrefixMatch(*connection.srv, connection.request.path))) {
                    if (loc->directives.count("client_max_body_size")) {
                        limit = parseSize(loc->directives.find("client_max_body_size")->second);
                        fromLoc = true;
                    }
                }
                if (!fromLoc && connection.srv->directives.count("client_max_body_size"))
                    limit = parseSize(connection.srv->directives.find("client_max_body_size")->second);
            }
            connection.clientMaxBodySize = limit;

            // ---- EARLY CHECKS (apenas os que fazem sentido no core) ----
            {
                const Server& active = connection.srv ? *connection.srv : _servers[0];

                // EARLY 413 para Content-Length
                if (connection.request.body_reader_state == BR_CONTENT_LENGTH
                    && connection.request.content_length > connection.clientMaxBodySize) {

                    std::cerr << "[EARLY-413] t=" << _nowMs
                              << " fd=" << clientFd
                              << " cl=" << connection.request.content_length
                              << " max=" << connection.clientMaxBodySize
                              << "\n";

                    connection.request.keep_alive = false;
                    connection.request.expectContinue = false;
                    connection.sentContinue = false;

                    connection.writeBuffer = http::build_error_response(active, 413, "Payload Too Large", false);
                    connection.writeOffset = 0;

                    // se há body, drena antes de responder
                    if (connection.request.body_reader_state != BR_NONE) {
                        connection.drainedBytes = 0;
                        connection.state = S_DRAIN;
                        std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
                        if (pit != _fdIndex.end())
                            _fds[pit->second].events = POLLIN;
                        return;
                    }

                    std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
                    if (pit != _fdIndex.end())
                        _fds[pit->second].events = POLLOUT;

                    connection.state = S_WRITE;
                    return;
                }

                // EARLY 405 via allow-list de methods
                const std::string* methodsStr = NULL;
                if (loc && loc->directives.count("methods"))
                    methodsStr = &loc->directives.find("methods")->second;
                else if (connection.srv && connection.srv->directives.count("methods"))
                    methodsStr = &connection.srv->directives.find("methods")->second;

                if (methodsStr) {
                    std::string reqM = connection.request.method;
                    for (size_t i = 0; i < reqM.size(); ++i)
                        reqM[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(reqM[i])));

                    // Treat HEAD as GET for allow-list purposes
                    if (reqM == "HEAD")
                        reqM = "GET";

                    bool allowed = false;
                    std::string token;

                    for (size_t i = 0; i <= methodsStr->size(); ++i) {
                        char c = (i < methodsStr->size()) ? (*methodsStr)[i] : ' ';
                        if (std::isalpha(static_cast<unsigned char>(c)))
                            token += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        else if (!token.empty()) {
                            if (token == reqM) {
                                allowed = true;
                                break;
                            }
                            token.clear();
                        }
                    }

                    if (!allowed) {

                        bool keep = connection.request.keep_alive;
                        if (connection.request.body_reader_state != BR_NONE && !connection.request.expectContinue)
                            keep = false;

                        std::cerr << "[EARLY-405] t=" << _nowMs
                                  << " fd=" << clientFd
                                  << " method=" << connection.request.method
                                  << " keep=" << (keep ? 1 : 0)
                                  << "\n";

                        connection.request.keep_alive = keep;
                        connection.sentContinue = false;

                        connection.writeBuffer = http::build_error_response(active, 405, "Method Not Allowed", keep);
                        connection.writeOffset = 0;

                        // se keep==0 e há body => DRAIN
                        if (!keep && connection.request.body_reader_state != BR_NONE && !connection.request.expectContinue) {
                            connection.drainedBytes = 0;
                            connection.state = S_DRAIN;
                            std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
                            if (pit != _fdIndex.end())
                                _fds[pit->second].events = POLLIN;
                            return;
                        }

                        std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
                        if (pit != _fdIndex.end())
                            _fds[pit->second].events = POLLOUT;

                        connection.state = S_WRITE;
                        return;
                    }
                }
            }

            // ---- Transition depending on body presence ----
            if (connection.request.body_reader_state == BR_NONE) {

                std::cerr << "[DISPATCH-NO-BODY] t=" << _nowMs
                          << " fd=" << clientFd
                          << " rb=" << connection.readBuffer.size()
                          << "\n";

                handleRequest(connection);
                return;
            }

            if (connection.request.expectContinue == true) {

                std::cerr << "[SEND-100] t=" << _nowMs
                          << " fd=" << clientFd
                          << " rb=" << connection.readBuffer.size()
                          << "\n";

                connection.writeBuffer = "HTTP/1.1 100 Continue\r\n\r\n";
                connection.writeOffset = 0;
                connection.sentContinue = true;

                std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
                if (pit != _fdIndex.end())
                    _fds[pit->second].events = POLLOUT;

                connection.state = S_WRITE;
                return;
            }

            connection.state = S_BODY;

            std::cerr << "[STATE->BODY] t=" << _nowMs
                      << " fd=" << clientFd
                      << " rb=" << connection.readBuffer.size()
                      << "\n";

            continue;
        }

        // ---------------- BODY ----------------
        if (connection.state == S_BODY) {

            int status = 0;
            std::string reason;
            http::BodyResult result = http::BODY_INCOMPLETE;

            const std::size_t maxBody = connection.clientMaxBodySize;

            switch (connection.request.body_reader_state) {
                case BR_CONTENT_LENGTH:
                    result = http::consume_body_content_length(connection, maxBody, status, reason);
                    break;
                case BR_CHUNKED:
                    result = http::consume_body_chunked(connection, maxBody, status, reason);
                    break;
                default:
                    status = 400;
                    reason = "Bad Request";
                    result = http::BODY_ERROR;
            }

            std::cerr << "[BODY-CONSUME] t=" << _nowMs
                      << " fd=" << clientFd
                      << " res=" << result
                      << " rb=" << connection.readBuffer.size()
                      << " body=" << connection.request.body.size()
                      << "\n";

            if (result == http::BODY_COMPLETE) {

                std::cerr << "[DISPATCH-BODY] t=" << _nowMs
                          << " fd=" << clientFd
                          << " body=" << connection.request.body.size()
                          << " rb=" << connection.readBuffer.size()
                          << "\n";

                handleRequest(connection);
                return;
            }

            if (result == http::BODY_ERROR) {

                const Server& active = connection.srv ? *connection.srv : _servers[0];

                std::cerr << "[BODY-ERR] t=" << _nowMs
                          << " fd=" << clientFd
                          << " status=" << status
                          << " reason=\"" << reason << "\""
                          << " ka=" << (connection.request.keep_alive ? 1 : 0)
                          << "\n";

                if (status == 413)
                    connection.request.keep_alive = false;

                connection.writeBuffer = http::build_error_response(active, status, reason, connection.request.keep_alive);
                connection.writeOffset = 0;

                std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
                if (pit != _fdIndex.end())
                    _fds[pit->second].events = POLLOUT;

                connection.state = S_WRITE;
                return;
            }

            return; // incomplete
        }

        return;
    }
}




void	ServerRunner::writeToClient(int clientFd)	{

	std::map<int, Connection>::iterator it = _connections.find(clientFd);
	if (it == _connections.end())
		return;

	Connection& connection = it->second;

	if (connection.writeOffset >= connection.writeBuffer.size())	{
		std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
		if (pit != _fdIndex.end())
			_fds[pit->second].events = POLLIN;

		std::cerr << "[WRITE-NOOP] t=" << _nowMs
		          << " fd=" << clientFd
		          << " wb=" << connection.writeBuffer.size()
		          << " off=" << connection.writeOffset
		          << "\n";
		return;
	}

	const char* base = connection.writeBuffer.data();
	std::size_t sentThisCall = 0;

	while (connection.writeOffset < connection.writeBuffer.size())	{

		const char* buf = base + connection.writeOffset;
		std::size_t remaining = connection.writeBuffer.size() - connection.writeOffset;

		ssize_t n = write(clientFd, buf, remaining);

		if (n > 0)	{
			connection.writeOffset += static_cast<std::size_t>(n);
			sentThisCall += static_cast<std::size_t>(n);
			connection.lastActiveMs = _nowMs;
			continue;
		}

		if (n < 0 && errno == EINTR)
			continue;

		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))	{
			std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
			if (pit != _fdIndex.end())
				_fds[pit->second].events = POLLOUT;

			std::cerr << "[WRITE-BLOCK] t=" << _nowMs
			          << " fd=" << clientFd
			          << " sent=" << sentThisCall
			          << " wb=" << connection.writeBuffer.size()
			          << " off=" << connection.writeOffset
			          << "\n";
			return;
		}

		std::cerr << "[WRITE-ERR] t=" << _nowMs
		          << " fd=" << clientFd
		          << " errno=" << errno
		          << " msg=" << std::strerror(errno)
		          << " sent=" << sentThisCall
		          << " wb=" << connection.writeBuffer.size()
		          << " off=" << connection.writeOffset
		          << "\n";

		closeConnection(clientFd);
		return;
	}

	std::cerr << "[WRITE-DONE] t=" << _nowMs
	          << " fd=" << clientFd
	          << " sent=" << sentThisCall
	          << " wb=" << connection.writeBuffer.size()
	          << " off=" << connection.writeOffset
	          << " sentContinue=" << (connection.sentContinue ? 1 : 0)
	          << "\n";

	// If we just sent "100 Continue", continue reading the same request body.
	if (connection.sentContinue)	{

		std::cerr << "[WRITE-100-SENT] t=" << _nowMs
		          << " fd=" << clientFd
		          << " rb=" << connection.readBuffer.size()
		          << " -> state=S_BODY\n";

		connection.sentContinue = false;
		connection.writeBuffer.clear();
		connection.writeOffset = 0;

		connection.state = S_BODY;

		std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
		if (pit != _fdIndex.end())
			_fds[pit->second].events = POLLIN;

		return;
	}

	// Finished sending the full response
	const bool keep = connection.request.keep_alive;

	if (keep)	{

		// IMPORTANT FIX: do NOT clear readBuffer on KA reset.
		const std::size_t bufferedNext = connection.readBuffer.size();

		std::cerr << "[KA-RESET-BEGIN] t=" << _nowMs
		          << " fd=" << clientFd
		          << " bufferedNext=" << bufferedNext
		          << " (readBuffer preserved)\n";

		connection.writeBuffer.clear();
		connection.writeOffset = 0;

		connection.headersComplete = false;
		connection.sentContinue = false;

		connection.request = HTTP_Request();
		connection.response = HTTP_Response();

		connection.state = S_HEADERS;

		// Only start KA idle timer if there's truly nothing already buffered for the next request
		if (bufferedNext == 0)
			connection.kaIdleStartMs = _nowMs;
		else
			connection.kaIdleStartMs = 0;

		connection.lastActiveMs = _nowMs;

		std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
		if (pit != _fdIndex.end())
			_fds[pit->second].events = POLLIN;

		std::cerr << "[KA-RESET-END] t=" << _nowMs
		          << " fd=" << clientFd
		          << " bufferedNext=" << connection.readBuffer.size()
		          << " state=" << connection.state
		          << " kaIdle=" << connection.kaIdleStartMs
		          << "\n";

		// Optional: if we already have bytes for the next request, you can trigger parsing immediately
		// (WITHOUT waiting for a new POLLIN). This is safe because we're back in S_HEADERS.
		if (!connection.readBuffer.empty())	{
			std::cerr << "[KA-IMMEDIATE-PARSE] t=" << _nowMs
			          << " fd=" << clientFd
			          << " rb=" << connection.readBuffer.size()
			          << "\n";
			readFromClient(clientFd); // will parse buffered bytes (no extra read due to EAGAIN)
		}

		return;
	}

	std::cerr << "[CLOSE-AFTER-RESP] t=" << _nowMs
	          << " fd=" << clientFd
	          << " keep=0\n";

	closeConnection(clientFd);
}



void	ServerRunner::closeConnection(int clientFd)	{

	close(clientFd);
	_connections.erase(clientFd);

	// must clean up the poll structures so we dont keep polling a dead fd
	std::map<int, std::size_t>::iterator it = _fdIndex.find(clientFd);
    if (it == _fdIndex.end())
		return ;	// Not present in _fds (already removed or was never added)

	std::size_t	index = it->second;
	std::size_t	last = _fds.size() - 1;

	if (index != last)	{
		// Move last entry into the removed slot
		std::swap(_fds[index], _fds[last]);
		// Update its index in the map
		_fdIndex[_fds[index].fd] = index;
	}	// so that we dont shift all later elements

	_fds.pop_back();	// remove the last index
	_fdIndex.erase(it);	// remove the index number of where _fds were
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


Error Codes

200 -> Success

400 - Bad Request
	- Missing or malformed method
	- Missing or malformed target (URI)
	- Missing or malformed version
	- Weird control characters in the the request target
	- Malformed headers

403 - Forbidden (no permission)

404 - Not Found
	- File or Location mapping doesn't exist or match

405 - Method Not Allowed (methods directive)

413 - Payload Too Large
	- If the data trying to upload is too large 

431 - Request Header Fields Too Large
	- The head is too big

500 - Internal Server Error (runtime errors)

501 - Not Implemented
	- Self explanatory

502/503/504 (CGI failures - depending on subject scope)

505 - HTTP Version Not Supported
	- Self explanatory

===================================================================================================================================

HTTP Requests -> has 3 parts. {According to RFC = Request For Comments, official Internet Standards managed by IETF.}
	1.1) Request Line
		- METHOD SP REQUEST_TARGET SP HTTP_VERSION CRLF
		- e.g. = GET /index.html HTTP/1.1\r\n

		A) METHOD <= RFC says methods are case-sensitive
			- What action the client wants
			- GET, POST, DELETE, PUT, HEAD, OPTIONS, CONNECT, TRACE
			
			1. GET <- Give me this resource
				- Browser loading a web page
				- Fetching an image, script, style file, etc.
				- Usually has no body and retrieves content
				- GET /index.html HTTP/1.1

			2. POST <- I am sending you some data
				- HTML forms
				- JSON uploads
				- File uploads
				- 	POST /upload HTTP/1.1
					Content-Length: 20

					{"name":"hello"}
			
			3. DELETE <- Please delete this resource
				- In Webserv, we should delete files on disk


		B) REQUEST_TARGET
			- What the client wants (the "path")
			- The Path and Query -> http://example.com/products/search?q=phone&page=2#top
				- Path -> /products/search
					- the “directory/file path” on the website
					- usually maps to a file or route on the server
				- Query -> q=phone&page=2
					- Parameters the client send to refine the request
					- q=phone -> search text
					- page=2 -> which page of results

			- There are 4 forms (RFC 7230)
				1. origin-form (most common)		=>	/path/to/resource?query=parameters <= /products/search?q=phone&page=2
					- give me this resource on the server, at this path, optionally with those query params
					- Examples:
            			GET / HTTP/1.1
            			GET /index.html HTTP/1.1
            			GET /products/search?q=phone&page=2 HTTP/1.1
					- Path = "/products/search"
        			- Query = "q=phone&page=2"
        			- Used by normal browsers talking directly to a server.

				2. absolute-form (proxies)			=>	http://example.com/path -> "Proxy, please fetch this URL for me."
					- The full URL in the request line, which includes "http://host/..." -> when talking to a proxy (a middleman)
					- Example:
            			GET http://example.com/products/search?q=phone HTTP/1.1
        			- Client sends full URL in the request line to a proxy.
						*What is proxy?
							A proxy server sits between an client and a real server
							like an enforcer which FILTERS websites, LOGs traffic for auditing, ENFORCE authentication, INJECT security systems,
							and to CACHE responses. Also HIDES the real client.

				3. authority-form (CONNECT method)	=>	CONNECT example.com:443 HTTP/1.1 -> only for proxies
					- Example:
            			CONNECT example.com:443 HTTP/1.1
        			- Target is "example.com:443".
        			- Used to create a TCP tunnel via a proxy for HTTPS.
        			- Not needed for normal Webserv mandatory part.

				4. asterisk-form					=>	OPTIONS * HTTP/1.1
					- Example:
            			OPTIONS * HTTP/1.1
        			- Asks about the server's general capabilities, not a single resource.
        			- "What methods/features do you support overall?"
					- can respond with:
						200 or 204
						an Allow header listing what is supported (at least GET, POST, DELETE, OPTIONS).
					e.g.
						HTTP/1.1 204 No Content
						Allow: GET, POST, DELETE, OPTIONS


		C) HTTP_VERSION

	1.2) Header Section
		- field-name ":" [optional whitespace] field-value
		- Field-name is case-insensitive
		- Can appear multiple times.

		A) Host = Tells the server which hostname the client is trying to reach
			- Host: example.com
			- For Virtual host selection
			- Can appear multiple times; duplicates may be combined with ", "

		B) Content-Length
			- Content-Length: 1234
			- Means that the Body is exactly 1234 bytes long.
			- Used for virtual hosts (one IP → many domains).
      		- HTTP/1.1: required. Missing Host → 400 Bad Request.
      		- HTTP/1.0: optional.

		C) Transfer-Encoding: chunked = send the body in chunks (We are only implementing chunked in our project)
			- when the server doesn't know the body size in advance
			- must ignore Content-Length when chunked is present
			- Server MUST read exactly that many bytes after the headers.
      		- Used for POST/PUT requests with a fixed-size body.
      		- If it's wrong → broken request or response handling.

		D) Connection: keep-alive or close
			- Whether to close TCP after response or keep it open for next request

	2) Blank Line -> A single empty line. Marks the end of the headers

	3) Optional Body
		- There could be 3 situations.
			1. No body (common for GET method)
			2. A fixed-length body
			3. A chunked-encoded body

			1. Request with NO body
				- 	GET / HTTP/1.1
					Host: example.com
				- No Content-Length
				- No Transfer-Encoding

			2. Fixed-length body (Content-Length)
				- 	POST /upload HTTP/1.1
					Host: example.com
					Content-Length: 12

					Hello World!
				- Stop reading after exactly 12 bytes
				- Store those as request.body
				- if it exceeds client_max_body_size -> 413
			
			3. Chunked transfer-encoding
				- 	POST /submit HTTP/1.1
					Transfer-Encoding: chunked

					7\r\n
					Mozilla\r\n
					9\r\n
					Developer\r\n
					7\r\n
					Network\r\n
					0\r\n
					\r\n

*/