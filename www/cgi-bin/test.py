#!/usr/bin/env python3

import os
import cgi

print("Content-Type: text/html")
print()  # blank line separates headers from body

print("<!DOCTYPE html>")
print("<html>")
print("<head>")
print("  <meta charset='UTF-8'>")
print("  <title>CGI Test</title>")
print("</head>")
print("<body>")
print("  <h1>Hello from CGI!</h1>")
print("  <p>If you can see this, it means that CGI handling is working.</p>")

# Show request method (GET or POST)
method = os.environ.get("REQUEST_METHOD", "UNKNOWN")
print(f"  <p>Request method: <strong>{method}</strong></p>")

print("</body>")
print("</html>")
