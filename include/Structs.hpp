#ifndef STRUCTS_HPP
#define STRUCTS_HPP

// ----------------- Core config types -----------------

struct  Location    {
    std::map<std::string, std::string>  directives;
    std::string                         path;
};

struct Server   {
    std::vector<std::string>            listen;
    std::vector<std::string>            server_name;
    std::vector<Location>               locations;
    std::map<std::string, std::string>  directives;
    std::map<std::string, std::string>  error_pages;
};

// ----------------- HTTP core types -----------------

enum ConnectionState	{
	S_HEADERS,
	S_BODY,
	S_DRAIN,   // NOVO: drenar body antes de enviar resposta early
	S_WRITE,
	S_CLOSED
};	// Per-connection state for the HTTP parser

enum BodyReaderState	{
	BR_NONE,
	BR_CONTENT_LENGTH,
	BR_CHUNKED
};

enum ChunkState	{
	CS_SIZE,
	CS_DATA,
	CS_DATA_CRLF,
	CS_TRAILERS,
	CS_DONE
};

struct HTTP_Request	{
	bool								keep_alive;
	bool								expectContinue;
	std::string							method;
	std::string							target;
	std::string							path;
	std::string							query;
	std::string							version;
	std::string							host;
	std::string							transfer_encoding;
	std::string							body;
	std::map<std::string, std::string>	headers;
	std::size_t							content_length;
	std::size_t							body_received;
	std::size_t							chunk_bytes_left;
	BodyReaderState						body_reader_state;
	ChunkState							chunk_state;

	HTTP_Request()
	:	keep_alive(true)
	,	expectContinue(false)
	,	method()
	,	target()
	,	path()
	,	query()
	,	version()
	,	host()
	,	transfer_encoding()
	,	body()
	,	headers()
	,	content_length(0)
	,	body_received(0)
	,	chunk_bytes_left(0)
	,	body_reader_state(BR_NONE)
	,	chunk_state(CS_SIZE)
	{}
};

struct HTTP_Response	{
	int									status;
	bool								close;
	std::string							reason;
	std::string							body;
	std::map<std::string, std::string>	headers;

	HTTP_Response()
	:	status(200)
	,	close(false)
	,	reason("OK")
	,	body()
	,	headers()
	{}
};

// ----------------- Runtime types -----------------

struct  Listener    {
    int             fd;
    const Server*   config;
};

struct Connection   {
    int             fd;
    int             listenFd;
    const Server*   srv;
    std::string     readBuffer;
    std::string     writeBuffer;
    bool            headersComplete;
	bool			sentContinue;
	ConnectionState	state;
	HTTP_Request	request;
	HTTP_Response	response;
	std::size_t		writeOffset;
	std::size_t		clientMaxBodySize;
	long			kaIdleStartMs;
	long			lastActiveMs;

	bool            draining;        // estamos a drenar body?
	std::size_t     drainedBytes;    // quantos bytes já drenámos (para limite/diagnóstico)

	bool            peerClosedRead;   // <-- NOVO: peer fez half-close (shutdown(SHUT_WR)) / EOF no read()

	Connection()
	:	fd(-1)
	,	listenFd(-1)
	,	srv(NULL)
	,	readBuffer()
	,	writeBuffer()
	,	headersComplete(false)
	,	sentContinue(false)
	,	state(S_HEADERS)
	,	request()
	,	response()
	,	writeOffset(0)
	,	clientMaxBodySize(std::numeric_limits<size_t>::max())	// default: unlimited unless configured
	,	kaIdleStartMs()
	,	lastActiveMs()
	,	draining(false)			// <-- NOVO
	,	drainedBytes(0)			// <-- NOVO
	,	peerClosedRead(false)        // <-- NOVO
	{}
};




#endif