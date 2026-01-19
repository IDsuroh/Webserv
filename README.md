*This project has been created as part of the 42 curriculum by suroh and hugo-mar.

# Webserv

## Description

**Webserv** is a C++98 project that consists of implementing a fully functional HTTP server from scratch.
The goal is to understand how the HTTP protocol works internally, how web servers handle multiple clients concurrently, and how configuration-driven servers (such as NGINX) are designed.

The server is **non-blocking**, event-driven, and capable of handling multiple clients simultaneously using a single polling mechanism ('poll()' or equivalent).
It supports serving static content, handling uploads, executing CGI scripts, and interpreting an NGINX-inspired configuration file.

This project emphasizes low-level networking, file I/O, process management, and protocol compliance, while respecting strict constraints on allowed system calls and libraries.

---

## Features

* HTTP/1.x compliant request parsing and response generation
* Non-blocking I/O using a single 'poll()'-based event loop
* Multiple listening ports and servers
* NGINX-like configuration file
* Static file serving
* Supported HTTP methods:
  * 'GET'
  * 'POST'
  * 'DELETE'
* File upload support
* Custom error pages
* Directory listing (autoindex)
* Redirections
* CGI execution (e.g. Python)
* Graceful client disconnection handling
* Default error handling when configuration is incomplete

---

## Instructions

### Requirements

* **Compiler:** 'c++'
* **Standard:** C++98
* **Flags:** '-Wall -Wextra -Werror'
* **Operating system:** Unix-like (Linux recommended)

### Execution

Run the server with a configuration file:

./webserv path/to/configuration.conf


If no configuration file is provided, the server will attempt to load a default configuration path (if implemented).

### Example


./webserv configs/webserv.conf


Then open the browser and navigate to:


http://127.0.0.1:8080


You can also test the server using:

curl

---

## Configuration File

The configuration syntax is inspired by **NGINX** and allows:

* Defining multiple servers and ports
* Setting root directories and index files
* Restricting allowed HTTP methods per route
* Enabling/disabling directory listing
* Setting maximum request body size
* Upload directories
* HTTP redirections
* CGI execution based on file extensions
* Custom error pages

Example snippet:

'''conf
server {
    listen 127.0.0.1:8080;
    root ./www;
    index index.html;

    location /upload {
        methods GET POST;
        upload_store ./www/upload;
    }
}
'''

---

## Project Structure (Overview)

* 'Config.*' – Configuration file tokenizer and parser
* 'ServerRunner.*' – Main event loop and socket handling
* 'HttpHeader.*' – HTTP header parsing
* 'HttpBody.*' – Request body handling
* 'HttpSerializer.*' – HTTP response generation
* 'App.*' – Application-level orchestration
* 'main.cpp' – Entry point

---

## Resources

### Technical References

* RFC 2616 – HTTP/1.1
  [https://www.rfc-editor.org/rfc/rfc2616](https://www.rfc-editor.org/rfc/rfc2616)
* RFC 7230–7235 – HTTP/1.1 Semantics
* NGINX Documentation
  [https://nginx.org/en/docs/](https://nginx.org/en/docs/)
* Common Gateway Interface (CGI)
  [https://en.wikipedia.org/wiki/Common_Gateway_Interface](https://en.wikipedia.org/wiki/Common_Gateway_Interface)
* Unix Network Programming – W. Richard Stevens

### AI Usage Disclosure

AI tools were used **as a learning and productivity aid**, not as a code generator.
Specifically, AI was used for:

* Clarifying HTTP protocol concepts
* Understanding RFC terminology
* Reviewing configuration design ideas
* Explaining system calls and edge cases
* Improving documentation clarity (including this README)

All generated content was reviewed, understood, and manually implemented or rewritten where necessary.
No critical logic was blindly copied into the project.


---

## License

This project is for educational purposes as part of the 42 curriculum.
