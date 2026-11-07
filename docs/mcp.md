# mcpd: the board as tools for an AI agent

The Model Context Protocol is how an AI agent finds out what a thing can do and
asks it to. `mcpd` speaks it over HTTP, so an agent on the computer at the other
end of the cable can ask the board how it is, read its log and its sensors, and
look at the card, without a serial console or a script written for the purpose.

An MCP server is only ever asked. Nothing happens on the board unless somebody
calls, so a machine with no agent in the room is doing what it always did, and
`mcpd` is a process asleep in `accept`.

## Getting started

Start it on the board:

```
ubiqos:/> mcpd &
mcpd: 11 tools on port 8080, to the USB cable only
```

To have it start with the board, add it to the card's `startup` file. It waits
up to 30 seconds for the network, which is not up yet when the file runs:

```
ubiqos:/> echo mcpd & >> /sd/startup
```

Then tell the agent where it is. With Claude Code, in a terminal on the computer
at the other end of the cable:

```
claude mcp add --transport http --scope user ubiqos http://192.168.7.1:8080/mcp
claude mcp list
```

`--scope user` makes it available in every project; without it the server
belongs to the directory the command was typed in. A session reads its servers
when it starts, so a session already running will not see this one: start a new
one, or restart the app.

Any MCP client that speaks Streamable HTTP will do. Its setting is the same URL.

The board can also be tried by hand, with no agent at all:

```
$ curl http://192.168.7.1:8080/mcp -H 'MCP-Protocol-Version: 2025-06-18' \
    -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"uptime"}}'
{"jsonrpc":"2.0","id":1,"result":{"resultType":"complete","content":[{"type":
"text","text":"up 15 min 45 s, since 2026-11-06 19:40\n"}],"isError":false,...
```

## What to use it for

Ask in ordinary words. The agent picks the tools; there is no syntax to learn.

- **How is the board?** "How much memory is free, and what is running?" calls
  `free` and `ps`. An agent that sees a process it does not expect, or a number
  that has moved since it last looked, can say so.
- **Why did it do that?** "Is there anything odd in the kernel log?" calls
  `dmesg`. This is the most useful one during development: the agent reading the
  code on the computer can read what the board said about it, in one step.
- **What do the sensors say?** "Which room has the highest CO2?" calls `sensors`,
  which needs `hibouair` running. It answers with the last reading and nothing
  older: for "what happened last night" something has to write a log to the
  card, and `read_file` can then read that.
- **What is on the card?** `list_files` and `read_file`.

And a board can offer what only that board has, by naming it in `/sd/mcp.txt`.
This one watches its WiFi and its I2C bus, and reads a log directory:

```
# name: command: what to tell the agent
rssi: wifi rssi: The signal strength of the access point
i2c: i2c: What answers on the I2C bus
log: cat /sd/logs/*: A file from the log directory; args is its name
```

The file replaces the built-in list rather than adding to it, so copy over the
lines worth keeping from the table below. It is read when `mcpd` starts: `kill
mcpd`, then `mcpd &`. A line can be added at the prompt:

```
ubiqos:/> echo "rssi: wifi rssi: The signal strength of the access point" >> /sd/mcp.txt
```

A `|` cannot be used in such a line, even inside quotes: the shell splits on it
regardless.

Write the description for the agent, since it is all the agent knows about the
tool. "A file from the log directory; args is its name" tells it what to pass.

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

Every tool above only reads. A tool whose program this board does not have is
left out of the list, flash and the search path both having been looked in, and
`mcpd` says so when it starts:

```
mcpd: no tool usbstat: this board has no such program
mcpd: 11 tools on port 8080, to the USB cable only
```

`sockstat` is left out in the same way on a board without a NINA coprocessor,
such as a Fruit Jam on ESP-Hosted or the NUCLEO on Ethernet: it reports what
that chip thinks of its sockets, and with no chip it could only answer with ten
rows of `no answer`. The sockets lwIP holds are in `free`.

On the search path the check only sees that a file of that name is there, and
loads nothing. A file that is not a module, or a module copied to a name that is
not its own, stays on the list and answers `this board has no such program`
when called.

A tool that needs a card is a different matter: on a board without one,
`list_files` and `read_file` are still offered, and answer that there is no such
file.

In `/sd/mcp.txt` a description may hold further colons; the first two end the name and the
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

A tool that changes something is a program that changes something, run by a
caller who cannot be asked twice. Weigh a line in `/sd/mcp.txt` as you would
weigh giving the agent that command at a prompt.

`read_file` cannot open `/sd/config.txt`, `/sd/wificfg.txt` or the key file. That
is the file server's own rule, and it holds for every program.

## By address or by name

Use the address, `192.168.7.1`. It is always the cable, and it is what the
default lets in.

`fruit-jam.local` resolves to the cable, the WiFi and several IPv6 addresses
together, and which of them a client picks is up to the client. Without `-a`,
curl landed on the WiFi or IPv6 address and was given a 403; Node's `fetch` tried
them in turn and got through by the cable, after 5 s on the first call and about
half a second on each one after. With `-a` every client got an answer, by
whichever way it happened to choose. An IPv6 client is refused on the cable too,
without `-a`: the check for the cable's subnet can only recognise IPv4.

## When it does not answer

| What you see | What it means |
|---|---|
| `403 Forbidden` | The request did not come over the cable: use `192.168.7.1`, or start `mcpd -a`. A request with an `Origin` header gets the same answer |
| No connection | `mcpd` is not running: look for it in `ps`. It says on the console what it did at start |
| `mcpd` printed nothing at start | It waits up to 30 seconds for the network; after that it says the stack would not take the port |
| `claude mcp list` shows it, a session has no tools | That session was started before the server was added |
| A tool from `/sd/mcp.txt` is missing | Its program is not on this board. `mcpd` said so when it started |
| A tool answers `the program could not be started` | The process table is full, or there is no memory for it: `ps`, `free` |
| `sensors` says `no such file` | Nothing is scanning: `hibouair -q &` |
| `[stopped: it did not finish in time]` | The program ran past 5 seconds and was killed |

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
