#!/usr/bin/env python3
"""WebDAV server via wsgidav 3.x API + cheroot — no CLI needed."""
import os
import sys

from cheroot import wsgi
from wsgidav.fs_dav_provider import FilesystemProvider
from wsgidav.wsgidav_app import WsgiDAVApp

HOST = "0.0.0.0"
PORT = 8082
ROOT = os.path.expanduser("~/esp32/cloud-drive")
os.makedirs(ROOT, exist_ok=True)

config = {
    "host": HOST,
    "port": PORT,
    "provider_mapping": {"/": FilesystemProvider(ROOT, readonly=False)},
    "http_authenticator": {
        "domain_controller": None,
        "accept_basic": True,
        "accept_digest": False,
        "default_to_basic": True,
        "trusted_auth_header": None,
    },
    "simple_dc": {
        "user_mapping": {
            "*": {"anonymous": {"password": None, "roles": ["reader", "writer"]}}
        }
    },
    "dir_browser": {"enable": True, "davmount": False, "msmount": False},
    "property_manager": True,
    "lock_storage": True,
    "verbose": 1,
    "logging": {"enable": True, "enable_loggers": []},
}

print(f"Starting WebDAV on http://{HOST}:{PORT}/ -> {ROOT}", flush=True)
app = WsgiDAVApp(config)
server = wsgi.Server(bind_addr=(HOST, PORT), wsgi_app=app, server_name="wsgidav")

try:
    server.start()
except KeyboardInterrupt:
    print("Shutting down...", flush=True)
finally:
    server.stop()
