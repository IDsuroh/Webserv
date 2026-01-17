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

        ++it;
        if (closeIt)
            closeConnection(fd);
    }
}



void ServerRunner::run() {

    setupListeners(_servers, _listeners);
    setupPollFds();

    if (_fds.empty()) {
        std::cerr << "No listeners configured/opened. \n";
        return;
    }

    const int POLL_TICK_MS = 250;

    const std::time_t   start = std::time(NULL);

    while (true) {
        int n = poll(&_fds[0], static_cast<nfds_t>(_fds.size()), POLL_TICK_MS);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            printSocketError("poll");
            break;
        }

        _nowMs = static_cast<long>((std::time(NULL) - start) * 1000);

        if (n > 0)
            handleEvents();

        housekeeping();
    }
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

            // readFromClient(fd); // should not read on POLLHUP.
            // poll() is a contract with the kernel: read() must be driven by POLLIN and write() by POLLOUT.
            // POLLHUP is not a “permission to read”; it only signals a hangup/half-close possibility.
            // We mark peerClosedRead and wait for a real POLLIN event (or finish the response and close).

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

static const Location* longestPrefixMatch(const Server& srv, const std::string& path) {
    const Location* best = NULL;
    size_t          bestLen = 0;

    for (size_t i = 0; i < srv.locations.size(); ++i) {
        const Location&      L  = srv.locations[i];
        const std::string&   lp = L.path;

        bool match = (path.compare(0, lp.size(), lp) == 0);
        bool boundary = (path.size() == lp.size()
                      || (path.size() > lp.size() && path[lp.size()] == '/'));

        if (match && boundary && lp.size() > bestLen) {
            best = &L;
            bestLen = lp.size();
        }
    }
    return best;
}


    void    ServerRunner::dispatchRequest(Connection& connection) {
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
            // EOF / half-close do peer
            connection.peerClosedRead = true;

            

            if (connection.state == S_HEADERS && connection.readBuffer.empty()) {
                closeConnection(clientFd);
                return;
            }

            std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
            if (pit != _fdIndex.end()) {
                bool havePendingWrite = (connection.writeOffset < connection.writeBuffer.size());
                _fds[pit->second].events = havePendingWrite ? POLLOUT : 0;
            }

            break;
        }

        // Subject-compliant: no errno inspection after read().
        // If read() didn't give bytes (n < 0), just stop for now.
        // poll() will wake us again when it's readable.
        break;

    }

    if (connection.state == S_WRITE && !connection.draining) {
        return;
    }

    // =================== DRAIN ===================
    if (connection.state == S_DRAIN || connection.draining) {

        int status = 0;
        std::string reason;
        http::BodyResult result = http::BODY_INCOMPLETE;

        const std::size_t ignoreMax = std::numeric_limits<std::size_t>::max();

        if (connection.request.body_reader_state == BR_CONTENT_LENGTH)
            result = http::consume_body_content_length_drain(connection, ignoreMax, status, reason);
        else if (connection.request.body_reader_state == BR_CHUNKED)
            result = http::consume_body_chunked_drain(connection, ignoreMax, status, reason);
        else
            result = http::BODY_COMPLETE;

        

        if (result == http::BODY_COMPLETE) {
            connection.draining = false;

            if (connection.writeOffset >= connection.writeBuffer.size()) {
                closeConnection(clientFd);
                return;
            }

            std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
            if (pit != _fdIndex.end())
                _fds[pit->second].events = POLLOUT;

            connection.state = S_WRITE;

            
            return;
        }

        if (result == http::BODY_ERROR) {
            if (connection.writeBuffer.empty()) {
                const Server& active = connection.srv ? *connection.srv : _servers[0];
                connection.request.keep_alive = false;
                connection.writeBuffer = http::build_error_response(active, 400, "Bad Request", false);
                connection.writeOffset = 0;
            }

            std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
            if (pit != _fdIndex.end())
                _fds[pit->second].events = POLLOUT;

            connection.state = S_WRITE;
            return;
        }

        if (connection.peerClosedRead) {
            if (connection.writeOffset >= connection.writeBuffer.size()) {
                closeConnection(clientFd);
                return;
            }

            std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
            if (pit != _fdIndex.end())
                _fds[pit->second].events = POLLOUT;
            connection.state = S_WRITE;
        }
        return;
    }

    // =================== PARSE LOOP ===================
    for (;;) {

        // ---------------- HEADERS ----------------
        if (connection.state == S_HEADERS) {

            static const std::size_t MAX_HEADER_BYTES = 16 * 1024;
            if (connection.readBuffer.size() > MAX_HEADER_BYTES
                && connection.readBuffer.find("\r\n\r\n") == std::string::npos) {

                int st = 431;
                std::string rsn = "Request Header Fields Too Large";
                const Server& active = connection.srv ? *connection.srv : _servers[0];

                

                connection.request.keep_alive = false;
                connection.request.expectContinue = false;
                connection.sentContinue = false;

                connection.writeBuffer = http::build_error_response(active, st, rsn, false);
                connection.writeOffset = 0;

                std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
                if (pit != _fdIndex.end())
                    _fds[pit->second].events = POLLOUT;

                connection.state = S_WRITE;
                return;
            }

            std::string head;
            if (!http::extract_next_head(connection.readBuffer, head)) {

                

                if (connection.peerClosedRead && connection.readBuffer.empty()) {
                    closeConnection(clientFd);
                    return;
                }

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

            

            int status = 0;
            std::string reason;
            if (!http::parse_head(head, connection.request, status, reason)) {

                const Server& active = connection.srv ? *connection.srv : _servers[0];

                

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

            // ---- EARLY CHECKS (CL > limit) ----
            {
                const Server& active = connection.srv ? *connection.srv : _servers[0];

                if (connection.request.body_reader_state == BR_CONTENT_LENGTH
                    && connection.request.content_length > connection.clientMaxBodySize) {

                    

                    connection.request.keep_alive = false;
                    connection.request.expectContinue = false;
                    connection.sentContinue = false;

                    connection.writeBuffer = http::build_error_response(active, 413, "Payload Too Large", false);
                    connection.writeOffset = 0;

                    connection.draining = true;
                    connection.drainedBytes = 0;

                    connection.state = S_WRITE;

                    std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
                    if (pit != _fdIndex.end())
                        _fds[pit->second].events = POLLOUT;

                    return;
                }

                // (resto do teu EARLY-405/others fica igual)
            }

            // ---- Transition depending on body presence ----
            if (connection.request.body_reader_state == BR_NONE) {
                
                dispatchRequest(connection);
                return;
            }

            if (connection.request.expectContinue == true) {

                

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

            

            if (result == http::BODY_COMPLETE) {
                
                dispatchRequest(connection);
                return;
            }

            if (result == http::BODY_ERROR) {
                const Server& active = connection.srv ? *connection.srv : _servers[0];

                

                if (status == 413) {
                    connection.request.keep_alive = false;
                    connection.request.expectContinue = false;
                    connection.sentContinue = false;

                    connection.writeBuffer = http::build_error_response(active, 413, "Payload Too Large", false);
                    connection.writeOffset = 0;

                    connection.draining = true;
                    connection.drainedBytes = 0;

                    connection.state = S_WRITE;

                    std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
                    if (pit != _fdIndex.end())
                        _fds[pit->second].events = POLLOUT;

                    return;
                }

                connection.writeBuffer = http::build_error_response(active, status, reason, connection.request.keep_alive);
                connection.writeOffset = 0;

                std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
                if (pit != _fdIndex.end())
                    _fds[pit->second].events = POLLOUT;

                connection.state = S_WRITE;
                return;
            }

            // INCOMPLETE:
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

        // n <= 0:
        // Subject-compliant: do not inspect errno after write().
        // Just stop writing now and rely on poll(POLLOUT) to wake us again.
        // If the peer is dead, a future poll() will surface it via POLLERR/POLLHUP/POLLNVAL.
        std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
        if (pit != _fdIndex.end())
            _fds[pit->second].events = POLLOUT;
        return;
    }

    if (connection.writeOffset < connection.writeBuffer.size()) {
        std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
        if (pit != _fdIndex.end())
            _fds[pit->second].events = POLLOUT;

        
        return;
    }


    // ================== FIM DE RESPOSTA (lógica comum) ==================

    // ⚠️ PATCH IMPORTANTE:
    // Calcula isto ANTES de mexer/limpar no writeBuffer.
    const bool responseSaysClose = containsConnectionClose(connection.writeBuffer);

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

    // Se estamos em DRAIN, NÃO fechar após enviar a resposta.
    if (connection.draining || connection.state == S_DRAIN) {

        connection.writeBuffer.clear();
        connection.writeOffset = 0;

        connection.state = S_DRAIN;

        std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
        if (pit != _fdIndex.end())
            _fds[pit->second].events = POLLIN;

        return;
    }

    const bool keep = connection.request.keep_alive
                && !connection.peerClosedRead
                && !responseSaysClose;

    if (keep) {

        const std::size_t bufferedNext = connection.readBuffer.size();

        

        connection.writeBuffer.clear();
        connection.writeOffset = 0;

        connection.headersComplete = false;
        connection.sentContinue = false;

        connection.request = HTTP_Request();
        connection.response = HTTP_Response();

        connection.state = S_HEADERS;

        connection.peerClosedRead = false;

        if (bufferedNext == 0)
            connection.kaIdleStartMs = _nowMs;
        else
            connection.kaIdleStartMs = 0;

        connection.lastActiveMs = _nowMs;

        std::map<int, std::size_t>::iterator pit = _fdIndex.find(clientFd);
        if (pit != _fdIndex.end())
            _fds[pit->second].events = POLLIN;

        

        // if (!connection.readBuffer.empty()) {
            
        //     readFromClient(clientFd);
        // }

        return;
    }

    

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