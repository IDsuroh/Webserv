#!/usr/bin/env python3
import os
import html
import urllib.parse

qs = os.environ.get("QUERY_STRING", "")
params = urllib.parse.parse_qs(qs)
name = params.get("name", ["Nobody"])[0]
name = html.escape(name)  # prevent HTML injection

print("Status: 200 OK")
print("Content-Type: text/html; charset=utf-8")
print()

print(f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>CGI Hello</title>
  <link rel="stylesheet" href="/static/style.css">
</head>

<body class="demo-page">
  <main class="container demo-wrap">
    <h1 class="demo-title">Hello, {name}!</h1>
    <p class="demo-text">This is a CGI response (dynamic).</p>

    <div class="form-actions form-actions-gap">
      <a class="button button-lg" href="/demo/cgi.html">Back</a>
      <a class="button button-lg" href="/">Home</a>
    </div>
  </main>
</body>
</html>""")