"""Explicit, local access to Society's daemon-owned iisaccson model.

The client owns no weights. ``receive`` blocks until an event arrives; an async
application can call it using its own executor. No request is retried.
"""
from __future__ import annotations

import ctypes
import hashlib
import json
import os
from pathlib import Path
import socket
import stat
import struct
import threading
import uuid

_LIMIT = 512 * 1024


def _directory(value):
    return Path(value or os.environ.get("SOCIETY_LOCAL_BROKER_DIR") or
                (Path.home() / ".iisacc-society-ipc-v1"))


def _unix_trusted(path):
    if not path.is_absolute():
        return False
    try:
        parent, endpoint = os.lstat(path.parent), os.lstat(path)
        uid = os.geteuid()
        return (stat.S_ISDIR(parent.st_mode) and parent.st_uid == uid and not parent.st_mode & 0o077
                and stat.S_ISSOCK(endpoint.st_mode) and endpoint.st_uid == uid and not endpoint.st_mode & 0o077)
    except OSError:
        return False


def _unix_peer(sock):
    if hasattr(socket, "SO_PEERCRED"):
        _, uid, _ = struct.unpack("3i", sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        return uid == os.geteuid()
    libc = ctypes.CDLL(None, use_errno=True)
    uid, gid = ctypes.c_uint(), ctypes.c_uint()
    libc.getpeereid.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
    return libc.getpeereid(sock.fileno(), ctypes.byref(uid), ctypes.byref(gid)) == 0 and uid.value == os.geteuid()


class _WindowsPipe:
    def __init__(self, directory):
        from ctypes import wintypes
        self._kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        advapi = ctypes.WinDLL("advapi32", use_last_error=True)
        k = self._kernel
        k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                                  wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        k.CreateFileW.restype = wintypes.HANDLE
        k.CloseHandle.argtypes = [wintypes.HANDLE]
        k.GetNamedPipeServerProcessId.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.ULONG)]
        k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        k.OpenProcess.restype = wintypes.HANDLE
        k.GetCurrentProcess.restype = wintypes.HANDLE
        k.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
        k.WriteFile.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
        advapi.OpenProcessToken.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)]
        advapi.GetTokenInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
        advapi.EqualSid.argtypes = [wintypes.LPVOID, wintypes.LPVOID]
        normalized = os.path.normpath(str(directory)).replace("\\", "/").casefold()
        name = "iisacc-society-backend-" + hashlib.sha256(normalized.encode()).hexdigest()
        self.handle = k.CreateFileW("\\\\.\\pipe\\" + name, 0xC0000000, 0, None, 3, 0, None)
        if self.handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            pid = wintypes.ULONG()
            if not k.GetNamedPipeServerProcessId(self.handle, ctypes.byref(pid)) or not pid.value:
                raise PermissionError("Cannot identify the Society daemon")
            process = k.OpenProcess(0x1000, False, pid.value)
            own, other = wintypes.HANDLE(), wintypes.HANDLE()
            if not process:
                raise PermissionError("Cannot inspect the Society daemon")
            try:
                if not advapi.OpenProcessToken(k.GetCurrentProcess(), 8, ctypes.byref(own)) or not advapi.OpenProcessToken(process, 8, ctypes.byref(other)):
                    raise PermissionError("Cannot inspect peer identities")
                def sid(token):
                    size = wintypes.DWORD()
                    advapi.GetTokenInformation(token, 1, None, 0, ctypes.byref(size))
                    if not size.value:
                        raise PermissionError("Cannot read peer identity")
                    buffer = ctypes.create_string_buffer(size.value)
                    if not advapi.GetTokenInformation(token, 1, buffer, size, ctypes.byref(size)):
                        raise PermissionError("Cannot read peer identity")
                    return buffer, ctypes.c_void_p.from_buffer(buffer).value
                own_buffer, own_sid = sid(own)
                other_buffer, other_sid = sid(other)
                if not advapi.EqualSid(own_sid, other_sid):
                    raise PermissionError("Society daemon belongs to another user")
            finally:
                if own: k.CloseHandle(own)
                if other: k.CloseHandle(other)
                k.CloseHandle(process)
        except BaseException:
            self.close()
            raise

    def recv(self, size):
        from ctypes import wintypes
        data, read = ctypes.create_string_buffer(size), wintypes.DWORD()
        if not self._kernel.ReadFile(self.handle, data, size, ctypes.byref(read), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return data.raw[:read.value]

    def sendall(self, value):
        from ctypes import wintypes
        offset = 0
        while offset < len(value):
            data = value[offset:offset + 65536]
            buffer = ctypes.create_string_buffer(data)
            written = wintypes.DWORD()
            if not self._kernel.WriteFile(self.handle, buffer, len(data), ctypes.byref(written), None) or not written.value:
                raise ctypes.WinError(ctypes.get_last_error())
            offset += written.value

    def close(self):
        if self.handle:
            self._kernel.CloseHandle(self.handle)
            self.handle = None


class ResidentClient:
    def __init__(self, broker_directory=None):
        self.directory = _directory(broker_directory)
        self._connection = None
        self._active = set()
        self._send_lock = threading.Lock()
        self._state_lock = threading.Lock()

    def connect(self):
        if self._connection is not None:
            return
        if not self.directory.is_absolute():
            raise ValueError("The broker directory must be absolute")
        if os.name == "nt":
            connection = _WindowsPipe(self.directory)
        else:
            path = self.directory / "backend.sock"
            if not _unix_trusted(path):
                raise PermissionError("The Society daemon endpoint is unavailable or untrusted")
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                connection.connect(str(path))
                if not _unix_trusted(path) or not _unix_peer(connection):
                    raise PermissionError("The Society daemon peer is untrusted")
            except BaseException:
                connection.close()
                raise
        self._connection = connection
        try:
            self._send({"id": uuid.uuid4().hex, "op": "iisaccsonSubscribe"})
        except BaseException:
            self.close()
            raise

    def _send(self, value):
        payload = json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8")
        if not 0 < len(payload) <= _LIMIT:
            raise ValueError("Request exceeds the frame limit")
        with self._send_lock:
            if self._connection is None:
                raise ConnectionError("Not connected to the Society daemon")
            try:
                self._connection.sendall(struct.pack(">I", len(payload)) + payload)
            except OSError:
                self.close()
                raise

    def generate(self, messages, options=None):
        ticket = uuid.uuid4().hex
        with self._state_lock:
            self._active.add(ticket)
        try:
            self._send({"id": ticket, "op": "iisaccsonGenerate", "messages": messages, "options": options or {}})
        except BaseException:
            with self._state_lock:
                self._active.discard(ticket)
            raise
        return ticket

    def cancel(self, ticket):
        with self._state_lock:
            if ticket not in self._active:
                raise ValueError("The request is not active on this connection")
        self._send({"id": uuid.uuid4().hex, "op": "iisaccsonCancel", "requestId": ticket})

    def receive(self):
        connection = self._connection
        if connection is None:
            raise ConnectionError("Not connected to the Society daemon")
        def exact(size):
            result = bytearray()
            while len(result) < size:
                part = connection.recv(size - len(result))
                if not part:
                    raise EOFError("The Society daemon disconnected")
                result.extend(part)
            return result
        size = struct.unpack(">I", exact(4))[0]
        if not 0 < size <= _LIMIT:
            self.close()
            raise ValueError("Invalid Society daemon frame")
        try:
            value = json.loads(exact(size))
        except (ValueError, UnicodeError):
            self.close()
            raise
        if not isinstance(value, dict):
            self.close()
            raise ValueError("Invalid Society daemon message")
        ticket = value.get("id")
        if ((value.get("type") == "result" and value.get("accepted") is False)
                or (value.get("type") == "iisaccsonEvent" and value.get("event", {}).get("kind") == "finished")):
            with self._state_lock:
                self._active.discard(ticket)
        return value

    def close(self):
        connection, self._connection = self._connection, None
        if connection is not None:
            connection.close()
        with self._state_lock:
            self._active.clear()

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *_):
        self.close()
