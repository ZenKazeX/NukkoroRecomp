"""The workstation's side of the development link (runtime gekko/dev_link.h).

The workstation listens on a loopback port; every game it launches connects to it with --dev-link and says who it
is in its first line (the "hello" event, carrying --dev-link-name: the session's token). Requests are JSON objects on
one line with an id; the game answers each with the same id. Everything here runs on the Qt event loop.
"""
from __future__ import annotations

import json

from PySide6.QtCore import QObject, Signal
from PySide6.QtNetwork import QHostAddress, QTcpServer


class DevConnection(QObject):
    hello = Signal(dict)
    event = Signal(dict)
    closed = Signal()

    def __init__(self, socket, parent=None):
        super().__init__(parent)
        self.socket = socket
        self.info = None
        self._buffer = b''
        self._next_id = 1
        self._pending = {}
        socket.readyRead.connect(self._read)
        socket.disconnected.connect(self._closed)

    @property
    def connected(self) -> bool:
        return self.socket is not None and self.socket.state() == self.socket.SocketState.ConnectedState

    def request(self, cmd: str, callback=None, **fields) -> int:
        if not self.connected:
            if callback is not None:
                callback({'ok': False, 'error': 'not connected'})
            return 0
        rid = self._next_id
        self._next_id += 1
        message = {'id': rid, 'cmd': cmd}
        for key, value in fields.items():
            if value is not None:
                message[key] = value
        self._pending[rid] = callback
        self.socket.write((json.dumps(message) + '\n').encode('utf-8'))
        self.socket.flush()
        return rid

    def _read(self):
        self._buffer += bytes(self.socket.readAll())
        while b'\n' in self._buffer:
            line, self._buffer = self._buffer.split(b'\n', 1)
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line.decode('utf-8', errors='replace'))
            except ValueError:
                continue
            if 'event' in message:
                if message['event'] == 'hello':
                    self.info = message
                    self.hello.emit(message)
                else:
                    self.event.emit(message)
            elif 'id' in message:
                callback = self._pending.pop(message['id'], None)
                if callback is not None:
                    callback(message)

    def _closed(self):
        pending, self._pending = self._pending, {}
        for callback in pending.values():
            if callback is not None:
                callback({'ok': False, 'error': 'the game closed the link'})
        self.closed.emit()

    def close(self):
        if self.socket is not None:
            self.socket.disconnectFromHost()


class DevLinkServer(QObject):
    """Accepts the games' connections and hands each to the session whose token its hello names."""
    connection_ready = Signal(object)  # DevConnection, after its hello

    def __init__(self, parent=None):
        super().__init__(parent)
        self.server = QTcpServer(self)
        self.server.newConnection.connect(self._accept)
        if not self.server.listen(QHostAddress(QHostAddress.SpecialAddress.LocalHost), 0):
            raise RuntimeError('could not listen on 127.0.0.1: ' + self.server.errorString())
        self.port = self.server.serverPort()
        self._connections = []

    @property
    def endpoint(self) -> str:
        return f'127.0.0.1:{self.port}'

    def _accept(self):
        while self.server.hasPendingConnections():
            socket = self.server.nextPendingConnection()
            connection = DevConnection(socket, self)
            self._connections.append(connection)
            connection.hello.connect(lambda _info, c=connection: self.connection_ready.emit(c))
            connection.closed.connect(lambda c=connection: self._forget(c))

    def _forget(self, connection):
        if connection in self._connections:
            self._connections.remove(connection)
