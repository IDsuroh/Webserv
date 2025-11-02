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
	S_READY,
	S_WRITE,
	S_CLOSED
};	// Per-connection state for the HTTP parser

enum BodyReaderState	{
	BR_NONE,
	BR_CONTENT_LENGTH,
	BR_CHUNKED,
	BR_DONE,
	BR_ERROR
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
	std::string							method;
	std::string							target;
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
	,	method()
	,	target()
	,	version()
	,	host()
	,	transfer_encoding()
	,	body()
	,	headers()
	,	content_length(0)
	,	body_reader_state(BR_NONE)
	,	body_received(0)
	,	chunk_state(CS_SIZE)
	,	chunk_bytes_left(0)
	{}
};

struct HTTP_Response	{
	int									status;
	bool								close;
	std::string							reason;
	std::string							body;
	std::map<std::string, std::string>	headers;

	HTTP_Response()
	: status(200), reason("OK"), close(false)
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
    bool            requestParsed;
	ConnectionState	state;
	HTTP_Request	request;
	HTTP_Response	response;
	std::size_t		writeOffset;
	std::size_t		clientMaxBodySize;

	Connection()
	:	fd(-1)
	,	listenFd(-1)
	,	srv(NULL)
	,	readBuffer()
	,	writeBuffer()
	,	headersComplete(false)
	,	requestParsed(false)
	,	state(S_HEADERS)
	,	request()
	,	response()
	,	writeOffset(0)
	,	clientMaxBodySize(1048576)	// default 1 MiB; override from config
	{}
};

#endif