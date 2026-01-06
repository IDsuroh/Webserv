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

void ServerRunner::handleEvents() {

    for (std::size_t i = _fds.size(); i-- > 0; ) {

        int   fd = _fds[i].fd;
        short re = _fds[i].revents;

        if (re == 0)
            continue;

        // Detecta se é listener
        bool isListener = false;
        const Server* srv = NULL;
        for (size_t j = 0; j < _listeners.size(); ++j) {
            if (_listeners[j].fd == fd) {
                isListener = true;
                srv = _listeners[j].config;
                break;
            }
        }

        // Erros "hard" -> fechar sempre
        if (re & (POLLERR | POLLNVAL)) {
            closeConnection(fd);
            continue;
        }

        // Listener: POLLHUP é fatal (não faz sentido manter)
        if (isListener) {
            if (re & POLLHUP) {
                closeConnection(fd);
                continue;
            }
            if (re & POLLIN)
                acceptNewClient(fd, srv);
            continue;
        }

        // Cliente: POLLHUP NÃO é motivo para fechar imediatamente.
        // Pode ser half-close (shutdown(SHUT_WR)) e ainda tens de responder.
        if (re & POLLHUP) {
            std::map<int, Connection>::iterator it = _connections.find(fd);
            if (it != _connections.end())
                it->second.peerClosedRead = true;

            // Força tentativa de leitura para apanhar read()==0 e avançar estados com o que já houver em buffer.
            // (Mesmo que não haja POLLIN setado, em TCP isto costuma resultar em read()==0.)
            readFromClient(fd);
        }

        if (re & POLLIN)
            readFromClient(fd);

        if (re & POLLOUT)
            writeToClient(fd);
    }
}


void ServerRunner::acceptNewClient(int listenFd, const Server* srv) {

    for (;;) {
        int clientFd = accept(listenFd, NULL, NULL);
        if (clientFd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            printSocketError("accept");
            break;
        }

        int fdflags = fcntl(clientFd, F_GETFD);
        if (fdflags != -1) {
            if (fcntl(clientFd, F_SETFD, fdflags | FD_CLOEXEC) == -1)
                printSocketError("fcntl F_SETFD FD_CLOEXEC");
        }

        if (!makeNonBlocking(clientFd)) {
            close(clientFd);
            continue;
        }

        Connection connection;
        connection.fd = clientFd;
        connection.srv = srv;
        connection.listenFd = listenFd;

        connection.readBuffer.clear();
        connection.writeBuffer.clear();

        connection.headersComplete = false;
        connection.sentContinue = false;
        connection.state = S_HEADERS;

        connection.request = HTTP_Request();
        connection.response = HTTP_Response();

        connection.writeOffset = 0;
        connection.clientMaxBodySize = std::numeric_limits<size_t>::max();

        connection.kaIdleStartMs = 0;
        connection.lastActiveMs = _nowMs;

        connection.draining = false;
        connection.drainedBytes = 0;

        connection.peerClosedRead = false; // <-- NOVO

        _connections[clientFd] = connection;

        struct pollfd p;
        p.fd = clientFd;
        p.events = POLLIN;
        p.revents = 0;
        _fds.push_back(p);
        _fdIndex[p.fd] = _fds.size() - 1;
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

void    ServerRunner::handleRequest(Connection& connection) {
    /*
        Ask the App layer to build the response for this request
        Not the same function. It is calling handleRequest from the App.hpp
        which is a free function in the global namespace.
    */
    HTTP_Response appRes = ::handleRequest(connection.request, _servers);

    // If App says “close”, override keep-alive
    if (appRes.close)
        connection.request.keep_alive = false;

    // Serialize to wire format (use request version if present)
    const std::string httpVersion = connection.request.version;
    connection.writeBuffer = http::serialize_response(appRes, httpVersion);

    // HEAD method must send headers only (no body bytes)
    if (connection.request.method == "HEAD") {
        const std::string crlf = "\r\n\r\n";
        std::string::size_type position = connection.writeBuffer.find(crlf);
        if (position != std::string::npos) {
            // Keep only status line + headers + CRLFCRLF
            connection.writeBuffer.resize(position + crlf.size());
        }
    }

    // ---- HALF-CLOSE COHERENCE ----
    // Se o cliente fez shutdown(SHUT_WR) (half-close), não faz sentido manter keep-alive.
    // Enviamos a resposta e fechamos a ligação: forçar "Connection: close" no wire.
    if (connection.peerClosedRead) {

        // 1) garante que a nossa lógica não tenta keep-alive depois
        connection.request.keep_alive = false;

        // 2) patch do header "Connection" na resposta serializada
        const std::string ka = "Connection: keep-alive\r\n";
        const std::string cl = "Connection: close\r\n";

        std::string::size_type pos = connection.writeBuffer.find(ka);
        if (pos != std::string::npos) {
            connection.writeBuffer.replace(pos, ka.size(), cl);
        } else {
            // se não existir header Connection, insere após a status line
            std::string::size_type eol = connection.writeBuffer.find("\r\n");
            if (eol != std::string::npos)
                connection.writeBuffer.insert(eol + 2, cl);
        }
    }

    connection.writeOffset = 0;
    connection.response = appRes;
    connection.state = S_WRITE;

    // Flip poll interest to POLLOUT for this fd
    std::map<int, std::size_t>::iterator pit = _fdIndex.find(connection.fd);
    if (pit != _fdIndex.end())
        _fds[pit->second].events = POLLOUT;
}


// void ServerRunner::readFromClient(int clientFd) {

//     std::map<int, Connection>::iterator it = _connections.find(clientFd);
//     if (it == _connections.end())
//         return;

//     Connection& connection = it->second;

//     const std::size_t READ_BUDGET = 256u * 1024u;

//     char buffer[4096];
//     std::size_t totalRead = 0;

//     // 1) Ler bytes (non-blocking) com cap
//     for (;;) {

//         if (totalRead >= READ_BUDGET)
//             break;

//         std::size_t want = sizeof(buffer);
//         std::size_t remainingBudget = READ_BUDGET - totalRead;
//         if (remainingBudget < want)
//             want = remainingBudget;

//         ssize_t n = read(clientFd, buffer, want);

//         if (n > 0) {
//             connection.readBuffer.append(buffer, static_cast<std::size_t>(n));
//             totalRead += static_cast<std::size_t>(n);

//             connection.lastActiveMs = _nowMs;

//             if (connection.state == S_HEADERS && connection.kaIdleStartMs != 0)
//                 connection.kaIdleStartMs = 0;

//             continue;
//         }

//         if (n == 0) {
//             // HALF-CLOSE: peer fechou o lado de escrita; NÃO fechar já.
//             connection.peerClosedRead = true;

//             std::cerr << "[READ-EOF] t=" << _nowMs
//                       << " fd=" << clientFd
//                       << " totalRead=" << totalRead
//                       << " rb=" << connection.readBuffer.size()
//                       << " state=" << connection.state
//                       << " wb=" << connection.writeBuffer.size()
//                       << " off=" << connection.writeOffset
//                       << "\n";

//             // Desarmar POLLIN (não há mais nada a ler). Manter POLLOUT se houver resposta pendente.
//             std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//             if (pit != _fdIndex.end()) {
//                 bool havePendingWrite = (connection.state == S_WRITE)
//                                      || (connection.writeOffset < connection.writeBuffer.size());
//                 _fds[pit->second].events = havePendingWrite ? POLLOUT : 0;
//             }

//             break; // IMPORTANTÍSSIMO: ainda podemos parsear o que já está em readBuffer
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

//         // Se o peer fechou e ainda estamos a drenar/incompleto, não há mais bytes a chegar -> erro
//         if (connection.peerClosedRead) {
//             const Server& active = connection.srv ? *connection.srv : _servers[0];
//             connection.request.keep_alive = false;
//             connection.writeBuffer = http::build_error_response(active, 400, "Bad Request", false);
//             connection.writeOffset = 0;
//             std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//             if (pit != _fdIndex.end())
//                 _fds[pit->second].events = POLLOUT;
//             connection.state = S_WRITE;
//         }
//         return;
//     }

//     // 2) Parse loop
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
//                           << " peerEOF=" << (connection.peerClosedRead ? 1 : 0)
//                           << "\n";

//                 // Se o peer fechou e não há head completo -> não vai haver mais bytes -> 400 + close
//                 if (connection.peerClosedRead) {
//                     const Server& active = connection.srv ? *connection.srv : _servers[0];
//                     connection.request.keep_alive = false;
//                     connection.writeBuffer = http::build_error_response(active, 400, "Bad Request", false);
//                     connection.writeOffset = 0;

//                     std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                     if (pit != _fdIndex.end())
//                         _fds[pit->second].events = POLLOUT;

//                     connection.state = S_WRITE;
//                 }
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

//             // ---- EARLY CHECKS ----
//             {
//                 const Server& active = connection.srv ? *connection.srv : _servers[0];

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

//                 // (resto do teu EARLY-405 fica igual)
//                 // ...
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
//                       << " peerEOF=" << (connection.peerClosedRead ? 1 : 0)
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

//             // INCOMPLETE:
//             // se o peer já fechou o lado de escrita, nunca mais chega body -> 400 + close
//             if (connection.peerClosedRead) {
//                 const Server& active = connection.srv ? *connection.srv : _servers[0];
//                 connection.request.keep_alive = false;
//                 connection.writeBuffer = http::build_error_response(active, 400, "Bad Request", false);
//                 connection.writeOffset = 0;

//                 std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                 if (pit != _fdIndex.end())
//                     _fds[pit->second].events = POLLOUT;

//                 connection.state = S_WRITE;
//             }
//             return;
//         }

//         return;
//     }
// }

// void ServerRunner::readFromClient(int clientFd) {

//     std::map<int, Connection>::iterator it = _connections.find(clientFd);
//     if (it == _connections.end())
//         return;

//     Connection& connection = it->second;

//     const std::size_t READ_BUDGET = 256u * 1024u;

//     char buffer[4096];
//     std::size_t totalRead = 0;

//     // 1) Ler bytes (non-blocking) com cap
//     for (;;) {

//         if (totalRead >= READ_BUDGET)
//             break;

//         std::size_t want = sizeof(buffer);
//         std::size_t remainingBudget = READ_BUDGET - totalRead;
//         if (remainingBudget < want)
//             want = remainingBudget;

//         ssize_t n = read(clientFd, buffer, want);

//         if (n > 0) {
//             connection.readBuffer.append(buffer, static_cast<std::size_t>(n));
//             totalRead += static_cast<std::size_t>(n);

//             connection.lastActiveMs = _nowMs;

//             if (connection.state == S_HEADERS && connection.kaIdleStartMs != 0)
//                 connection.kaIdleStartMs = 0;

//             continue;
//         }

//         if (n == 0) {
//             // EOF / half-close do peer (shutdown(SHUT_WR) ou close no lado do cliente).
//             connection.peerClosedRead = true;

//             std::cerr << "[READ-EOF] t=" << _nowMs
//                       << " fd=" << clientFd
//                       << " totalRead=" << totalRead
//                       << " rb=" << connection.readBuffer.size()
//                       << " state=" << connection.state
//                       << " wb=" << connection.writeBuffer.size()
//                       << " off=" << connection.writeOffset
//                       << "\n";

//             // ✅ FIX CRÍTICO:
//             // EOF com rb==0 enquanto estamos em S_HEADERS (keep-alive idle) é fecho limpo.
//             // NÃO é "header incompleto" -> fechar silenciosamente e sair.
//             if (connection.state == S_HEADERS && connection.readBuffer.empty()) {
//                 closeConnection(clientFd);
//                 return;
//             }

//             // Caso contrário, mantém-se: pode haver request parcial em buffer e queremos parsear.
//             // Desarmar POLLIN (não há mais nada a ler). Manter POLLOUT se houver resposta pendente.
//             std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//             if (pit != _fdIndex.end()) {
//                 bool havePendingWrite = (connection.state == S_WRITE)
//                                      || (connection.writeOffset < connection.writeBuffer.size());
//                 _fds[pit->second].events = havePendingWrite ? POLLOUT : 0;
//             }

//             break; // IMPORTANTÍSSIMO: ainda podemos parsear o que já está em readBuffer
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

//         // Se o peer fechou e ainda estamos a drenar/incompleto, não há mais bytes a chegar -> erro
//         if (connection.peerClosedRead) {
//             const Server& active = connection.srv ? *connection.srv : _servers[0];
//             connection.request.keep_alive = false;
//             connection.writeBuffer = http::build_error_response(active, 400, "Bad Request", false);
//             connection.writeOffset = 0;
//             std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//             if (pit != _fdIndex.end())
//                 _fds[pit->second].events = POLLOUT;
//             connection.state = S_WRITE;
//         }
//         return;
//     }

//     // 2) Parse loop
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
//                           << " peerEOF=" << (connection.peerClosedRead ? 1 : 0)
//                           << "\n";

//                 // ✅ FIX CRÍTICO (segunda defesa):
//                 // Se o peer fechou e rb==0, isto é keep-alive idle -> fechar silenciosamente.
//                 if (connection.peerClosedRead && connection.readBuffer.empty()) {
//                     closeConnection(clientFd);
//                     return;
//                 }

//                 // Se o peer fechou e há bytes (header parcial), não vai haver mais -> 400 + close
//                 if (connection.peerClosedRead) {
//                     const Server& active = connection.srv ? *connection.srv : _servers[0];
//                     connection.request.keep_alive = false;
//                     connection.writeBuffer = http::build_error_response(active, 400, "Bad Request", false);
//                     connection.writeOffset = 0;

//                     std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                     if (pit != _fdIndex.end())
//                         _fds[pit->second].events = POLLOUT;

//                     connection.state = S_WRITE;
//                 }
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

//             // ---- EARLY CHECKS ----
//             {
//                 const Server& active = connection.srv ? *connection.srv : _servers[0];

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

//                 // (resto do teu EARLY-405 fica igual)
//                 // ...
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
//                       << " peerEOF=" << (connection.peerClosedRead ? 1 : 0)
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

//             // INCOMPLETE:
//             // se o peer já fechou o lado de escrita, nunca mais chega body -> 400 + close
//             if (connection.peerClosedRead) {
//                 const Server& active = connection.srv ? *connection.srv : _servers[0];
//                 connection.request.keep_alive = false;
//                 connection.writeBuffer = http::build_error_response(active, 400, "Bad Request", false);
//                 connection.writeOffset = 0;

//                 std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
//                 if (pit != _fdIndex.end())
//                     _fds[pit->second].events = POLLOUT;

//                 connection.state = S_WRITE;
//             }
//             return;
//         }

//         return;
//     }
// }

void ServerRunner::readFromClient(int clientFd) {

    std::map<int, Connection>::iterator it = _connections.find(clientFd);
    if (it == _connections.end())
        return;

    Connection& connection = it->second;

    const std::size_t READ_BUDGET = 256u * 1024u;

    char buffer[4096];
    std::size_t totalRead = 0;

    // 1) Ler bytes (non-blocking) com cap
    for (;;) {

        if (totalRead >= READ_BUDGET)
            break;

        std::size_t want = sizeof(buffer);
        std::size_t remainingBudget = READ_BUDGET - totalRead;
        if (remainingBudget < want)
            want = remainingBudget;

        ssize_t n = read(clientFd, buffer, want);

        if (n > 0) {
            connection.readBuffer.append(buffer, static_cast<std::size_t>(n));
            totalRead += static_cast<std::size_t>(n);

            connection.lastActiveMs = _nowMs;

            if (connection.state == S_HEADERS && connection.kaIdleStartMs != 0)
                connection.kaIdleStartMs = 0;

            continue;
        }

        if (n == 0) {
            // EOF / half-close do peer (shutdown(SHUT_WR) ou close no lado do cliente).
            connection.peerClosedRead = true;

            std::cerr << "[READ-EOF] t=" << _nowMs
                      << " fd=" << clientFd
                      << " totalRead=" << totalRead
                      << " rb=" << connection.readBuffer.size()
                      << " state=" << connection.state
                      << " wb=" << connection.writeBuffer.size()
                      << " off=" << connection.writeOffset
                      << "\n";

            // ✅ FIX CRÍTICO:
            // EOF com rb==0 enquanto estamos em S_HEADERS (keep-alive idle) é fecho limpo.
            // NÃO é "header incompleto" -> fechar silenciosamente e sair.
            if (connection.state == S_HEADERS && connection.readBuffer.empty()) {
                closeConnection(clientFd);
                return;
            }

            // Caso contrário, mantém-se: pode haver request parcial em buffer e queremos parsear.
            // Desarmar POLLIN (não há mais nada a ler). Manter POLLOUT se houver resposta pendente.
            std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
            if (pit != _fdIndex.end()) {
                bool havePendingWrite = (connection.state == S_WRITE)
                                     || (connection.writeOffset < connection.writeBuffer.size());
                _fds[pit->second].events = havePendingWrite ? POLLOUT : 0;
            }

            break; // IMPORTANTÍSSIMO: ainda podemos parsear o que já está em readBuffer
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

        // Se o peer fechou e ainda estamos a drenar/incompleto, não há mais bytes a chegar -> erro
        if (connection.peerClosedRead) {
            const Server& active = connection.srv ? *connection.srv : _servers[0];
            connection.request.keep_alive = false;
            connection.writeBuffer = http::build_error_response(active, 400, "Bad Request", false);
            connection.writeOffset = 0;
            std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
            if (pit != _fdIndex.end())
                _fds[pit->second].events = POLLOUT;
            connection.state = S_WRITE;
        }
        return;
    }

    // 2) Parse loop
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

                // ✅ robusto: header gigante -> fechar ligação
                connection.request.keep_alive = false;
                connection.request.expectContinue = false;
                connection.sentContinue = false;
                connection.peerClosedRead = true; // garante que não esperamos mais requests

                connection.writeBuffer = http::build_error_response(active, st, reason, false);
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
                          << " peerEOF=" << (connection.peerClosedRead ? 1 : 0)
                          << "\n";

                // ✅ FIX CRÍTICO (segunda defesa):
                // Se o peer fechou e rb==0, isto é keep-alive idle -> fechar silenciosamente.
                if (connection.peerClosedRead && connection.readBuffer.empty()) {
                    closeConnection(clientFd);
                    return;
                }

                // Se o peer fechou e há bytes (header parcial), não vai haver mais -> 400 + close
                if (connection.peerClosedRead) {
                    const Server& active = connection.srv ? *connection.srv : _servers[0];
                    connection.request.keep_alive = false;
                    connection.writeBuffer = http::build_error_response(active, 400, "Bad Request", false);
                    connection.writeOffset = 0;

                    std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
                    if (pit != _fdIndex.end())
                        _fds[pit->second].events = POLLOUT;

                    connection.state = S_WRITE;
                }
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

                // ✅ nunca dar throw aqui: fallback seguro
                const Server& active = connection.srv ? *connection.srv : _servers[0];

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

            // ---- EARLY CHECKS ----
            {
                const Server& active = connection.srv ? *connection.srv : _servers[0];

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

                // (resto do teu EARLY-405 fica igual)
                // ...
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
                      << " peerEOF=" << (connection.peerClosedRead ? 1 : 0)
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

            // INCOMPLETE:
            // se o peer já fechou o lado de escrita, nunca mais chega body -> 400 + close
            if (connection.peerClosedRead) {
                const Server& active = connection.srv ? *connection.srv : _servers[0];
                connection.request.keep_alive = false;
                connection.writeBuffer = http::build_error_response(active, 400, "Bad Request", false);
                connection.writeOffset = 0;

                std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
                if (pit != _fdIndex.end())
                    _fds[pit->second].events = POLLOUT;

                connection.state = S_WRITE;
            }
            return;
        }

        return;
    }
}



static bool containsConnectionClose(const std::string& buf) {
    // Procura de forma case-insensitive por "\nConnection: close"
    // (inclui o caso do header vir com \r\n)
    const std::string needle = "\nconnection: close";
    std::string lower;
    lower.reserve(buf.size());
    for (std::size_t i = 0; i < buf.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(buf[i]);
        lower.push_back(static_cast<char>(std::tolower(c)));
    }
    return (lower.find(needle) != std::string::npos);
}


void ServerRunner::writeToClient(int clientFd) {

    std::map<int, Connection>::iterator it = _connections.find(clientFd);
    if (it == _connections.end())
        return;

    Connection& connection = it->second;

    const std::size_t WRITE_BUDGET = 256u * 1024u;

    // Se não há nada para enviar, trata como "fim de resposta" (não é noop).
    if (connection.writeOffset >= connection.writeBuffer.size()) {
        std::cerr << "[WRITE-DONE-FASTPATH] t=" << _nowMs
                  << " fd=" << clientFd
                  << " wb=" << connection.writeBuffer.size()
                  << " off=" << connection.writeOffset
                  << " sentContinue=" << (connection.sentContinue ? 1 : 0)
                  << " peerEOF=" << (connection.peerClosedRead ? 1 : 0)
                  << "\n";
    } else {

        const char* base = connection.writeBuffer.data();
        std::size_t sentThisCall = 0;

        while (connection.writeOffset < connection.writeBuffer.size()) {

            if (sentThisCall >= WRITE_BUDGET)
                break;

            const char* buf = base + connection.writeOffset;
            std::size_t remaining = connection.writeBuffer.size() - connection.writeOffset;

            std::size_t remainingBudget = WRITE_BUDGET - sentThisCall;
            if (remainingBudget < remaining)
                remaining = remainingBudget;

            ssize_t n = write(clientFd, buf, remaining);

            if (n > 0) {
                connection.writeOffset += static_cast<std::size_t>(n);
                sentThisCall += static_cast<std::size_t>(n);
                connection.lastActiveMs = _nowMs;
                continue;
            }

            if (n < 0 && errno == EINTR)
                continue;

            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
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

        if (connection.writeOffset < connection.writeBuffer.size()) {
            std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
            if (pit != _fdIndex.end())
                _fds[pit->second].events = POLLOUT;

            std::cerr << "[WRITE-YIELD] t=" << _nowMs
                      << " fd=" << clientFd
                      << " sent=" << sentThisCall
                      << " wb=" << connection.writeBuffer.size()
                      << " off=" << connection.writeOffset
                      << "\n";
            return;
        }

        std::cerr << "[WRITE-DONE] t=" << _nowMs
                  << " fd=" << clientFd
                  << " sent=" << sentThisCall
                  << " wb=" << connection.writeBuffer.size()
                  << " off=" << connection.writeOffset
                  << " sentContinue=" << (connection.sentContinue ? 1 : 0)
                  << " peerEOF=" << (connection.peerClosedRead ? 1 : 0)
                  << "\n";
    }

    // ================== FIM DE RESPOSTA (lógica comum) ==================

    // Caso especial: 100-continue
    if (connection.sentContinue) {

        connection.sentContinue = false;
        connection.writeBuffer.clear();
        connection.writeOffset = 0;

        connection.state = S_BODY;

        std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
        if (pit != _fdIndex.end())
            _fds[pit->second].events = POLLIN;

        return;
    }

    // Respeitar a resposta: se ela diz "Connection: close", fecha mesmo que request seja keep-alive.
    const bool responseSaysClose = containsConnectionClose(connection.writeBuffer);

    const bool keep = connection.request.keep_alive
                   && !connection.peerClosedRead
                   && !responseSaysClose;

    if (keep) {

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

        // reset flags por-request
        connection.peerClosedRead = false;

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

        if (!connection.readBuffer.empty()) {
            std::cerr << "[KA-IMMEDIATE-PARSE] t=" << _nowMs
                      << " fd=" << clientFd
                      << " rb=" << connection.readBuffer.size()
                      << "\n";
            readFromClient(clientFd);
        }

        return;
    }

    std::cerr << "[CLOSE-AFTER-RESP] t=" << _nowMs
              << " fd=" << clientFd
              << " keep=0"
              << " respClose=" << (responseSaysClose ? 1 : 0)
              << "\n";

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