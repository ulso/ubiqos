# mcpd: the board as tools for an AI agent

The Model Context Protocol is how an AI agent finds out what a thing can do and
asks it to. `mcpd` speaks it over HTTP, so an agent on the computer at the other
end of the cable can ask the board how it is:

```
ubiqos:/> mcpd &
mcpd: 12 tools on port 8080, to the USB cable only
```

and from the computer, for Claude Code:

```
claude mcp add --transport http ubiqos http://192.168.7.1:8080/mcp
```

An MCP server is only ever asked. Nothing happens on the board unless somebody
calls, so a machine with no agent in the room is doing what it always did, and
`mcpd` is a process asleep in `accept`.

## The tools

| Tool | Runs |
|---|---|
| `ps`, `free`, `uptime`, `date`, `lsmod`, `sockstat`, `usbstat` | the program of that name |
| `uname` | `uname -a` |
| `dmesg` | `cat /var/dmesg` |
| `sensors` | `cat /tmp/sensors.json`, what `hibouair` last published |
| `list_files` | `ls /sd/<args>` |
| `read_file` | `cat /sd/<args>` |

A tool is a program that already exists. Calling one starts that module with a
pipe on its descriptors, the way `netcon` starts the shell, and what it printed
is the answer. So `ps` over MCP cannot differ from `ps` at the prompt.

Every tool above only reads. `/sd/mcp.txt` replaces the list on a board that
wants another one, a tool to a line:

```
# name: command: what to tell the agent
rssi: wifi rssi: The signal strength of the access point
i2c: i2c: What answers on the I2C bus
log: cat /sd/logs/*: A file from the log directory; args is its name
```

The description may hold further colons; the first two end the name and the
command. A `*` in the command is where the caller's word goes. That word cannot
carry a space or a `..`, and the command is not given to `sh`, so a pipe or a
redirection in it is only text. An agent can reach what the list names and
nothing else.

A tool gets five seconds and then is killed, and its answer says so. Output
stops at 8 kB and says that too. One request is served at a time, each on its
own connection: `ps` takes about 40 ms, and listing the card about 280.

## What it is not

**It is not encrypted and it asks for no password.** That is why the default is
the USB cable's own subnet, as for `netcon`. Anything else gets a 403:

```
$ curl -i http://192.168.68.55:8080/mcp -d '{}'
HTTP/1.1 403 Forbidden
```

`mcpd -a` opens it to the WiFi, which is a decision rather than a convenience.
Before doing that, keep `/sd/mcp.txt` to things that only read. A request that
carries an `Origin` header is refused on every interface: only a browser sends
one, and a web page must not be able to reach the board by way of a name that
resolves to it.

`read_file` cannot open `/sd/config.txt`, `/sd/wificfg.txt` or the key file. That
is the file server's own rule, and it holds for every program.

## The protocol

One endpoint, `POST /mcp`, answering with one JSON object. There is no event
stream, so `GET` is a 405.

Both eras of MCP are served there. Revision 2026-07-28 made it stateless: every
request carries its version, in the `MCP-Protocol-Version` header and in `_meta`,
the headers must agree with the body, and `server/discover` says what is
supported. Up to 2025-11-25 a client opens with `initialize`. Nothing here
remembers anything between requests, so serving the older ones takes only an
answer to `initialize` and no session id.

The parts left out: resources, prompts, subscriptions and streaming. A base64
`Mcp-Name` header is not decoded, because tool names are plain words and such a
header could only name a tool that does not exist.

## Two things that went wrong

**The port would not come back.** `kill mcpd` followed by `mcpd` listened on
nothing for up to two minutes, in silence. Every answer here ends the
connection, and this server closed first, so each connection left a TIME_WAIT
holding the port; lwIP is built without `SO_REUSE`, so the bind failed until the
last one had aged out. Now the client is given 250 ms to close first, which it
does at once, having read `Connection: close`.

**`|` was the obvious separator and could not be typed.** The first format for
`/sd/mcp.txt` used it, and the shell splits on a pipe even inside quotes, so
`echo "a | b | c" >> /sd/mcp.txt` ran three programs. Hence the colons.
