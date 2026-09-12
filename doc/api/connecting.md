# Connecting

The service speaks Protobuf over a WebSocket. One binary message carries
exactly one serialized frame — WebSocket already delimits messages, so there
is no length prefix of your own to write.

```
ws://127.0.0.1:9111          the default, loopback only
```

`--host` and `--port` move it, with one refusal: a routable `--host` and no
token is refused at startup rather than served.

## The handshake

Offer the subprotocol **`qlservice.v2`**. The service answers with exactly one
protocol and never echoes a token back, because a browser fails a handshake in
which it named protocols and the server selected none.

```python
import websockets

ws = await websockets.connect("ws://127.0.0.1:9111",
                              subprotocols=["qlservice.v2"],
                              max_size=16 << 20)
```

```javascript
const ws = new WebSocket("ws://127.0.0.1:9111", ["qlservice.v2"]);
ws.binaryType = "arraybuffer";
```

Two refusals happen before a socket exists, and both are HTTP rather than
frames:

| Answer | Means |
| --- | --- |
| `403 Forbidden` | an `Origin` header was present and is not on the allowed list |
| `401 Unauthorized` | the service was started with a token and yours was wrong or missing |

An absent `Origin` is let through: only browsers send one, so the check closes
the browser path and leaves a script, a CLI or a proxy exactly as they were.

## The token, when there is one

Started with `--token-file`, every upgrade must present the secret. Where it
goes depends on what your client can set:

| Client | Where the token goes |
| --- | --- |
| anything that can set headers | `Authorization: Bearer <token>` |
| a browser | `token.<token>` in the subprotocol list, **beside** `qlservice.v2` |

```python
ws = await websockets.connect(URL, subprotocols=["qlservice.v2"],
                              additional_headers={"Authorization": f"Bearer {token}"})
```

```javascript
new WebSocket(url, ["qlservice.v2", `token.${token}`]);
```

Never put it in the query string: a URL reaches logs, history and referrers.

## Frames, ids and the one guarantee

Every frame you send is a `ClientFrame` with a `request_id` you allocate, and
a `session_id` on everything after the session exists. Both come back on every
reply, because one socket can hold several sessions and several requests at
once.

**Every request gets exactly one terminal frame**, including ones that fail
and including the cancel itself. A client can therefore key a pending map on
`request_id` and know that every entry is eventually removed by something. The
one non-terminal frame is `Progress`, which arrives from a batched Monte
Carlo, a sweep or a batch, and may be shed under backpressure — treat it as a
hint, never as a count you have to receive.

```python
async def roundtrip(ws, frame):
    """Send one frame; return its terminal reply, ignoring Progress."""
    await ws.send(frame.SerializeToString())
    while True:
        reply = ServerFrame()
        reply.ParseFromString(await ws.recv())
        if reply.WhichOneof("payload") != "progress":
            return reply
```

A real client does not block like this — it keeps a map from `request_id` to a
future, because frames for several requests interleave on one socket. The
frontend's `WireClient` is that shape: monotonic ids, a pending registry, and
a watchdog that flags a request which has heard nothing, since a missing
terminal frame is a service bug rather than something to hide behind a
timeout.

## Limits worth knowing before you hit them

| Limit | Default | What you get |
| --- | --- | --- |
| sockets at once | 32 | `503` at the upgrade |
| sessions per socket | 16 | `OVERLOADED` |
| frame size | 4 MB | the transport closes the connection |
| points per sweep | 100,000 | `INVALID_ARGUMENT` |

The sweep and batch ceilings are also published in `Capabilities`, so a client
can refuse a too-large request before spending the round trip.

## Liveness without the protocol

`GET /healthz` answers over plain HTTP for a proxy or an orchestrator that
cannot speak this protocol:

```
{"status":"ok","build":"ql-backend","quantlib":"1.43","uptimeSeconds":142,
 "connections":3,"sessions":7,"maxConnections":32}
```

It proves the event loop is turning, which is the honest scope of a liveness
check. It says nothing about whether a particular session is healthy.
