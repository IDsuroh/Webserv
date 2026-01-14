#!/usr/bin/env python3
import os
import urllib.parse

qs = os.environ.get("QUERY_STRING", "")
params = urllib.parse.parse_qs(qs)
name = params.get("name", ["World"])[0]

print("Status: 200 OK")
print("Content-Type: text/html; charset=utf-8")
print()
print(f"""<!doctype html>
<html>
<head><meta charset="utf-8"><title>CGI Hello</title></head>
<body style="font-family: sans-serif;">
  <h1>Hello, {name}!</h1>
  <p>This is a CGI response (dynamic).</p>
  <p><a href="/demo/cgi.html">Back</a></p>
</body>
</html>""")