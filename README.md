# BulletinBoardServer
The bulletin board server accepts one-line messages from the clients, stores them in a local file, and serves them back on request. Messages are identified upon storage by a unique number established by the server. Each client has an associated name and the name is stored along with the message sent by the client. The username can be changed. A client can request to replace the message with another one. The communication between the client and server is not encrypted. Any client able to send and receive plain text (like telnet) can be used to communicate with a BB server.
BB Server might have zero or more peer BB servers. The request to store or replace a message by a client is performed on all peer BB servers and is only considered successful if it is successfully performed on all BB servers. This ensures the integrity of messages on the Bulletin Board across all servers. The protocol2pc.txt explains the synchronization protocol used by BB servers.
You need to provide a configuration file to the server as the only command line argument. If no command line argument is provided then the server searches the configuration file named "bbserv.conf" in its current working directory.

## Configuration options:
- THMAX: number of pre-allocated threads used by the server.
- BBPORT: port number for client-server communication.
- SYNCPORT: port number for inter-server communication.
- BBFILE: name of the bulletin board file manipulated by the server.
- PEER: Pair of hostname and port number used to denote a synchronization BB server peer.
- DAEMON: run the server as a daemon.
- DEBUG: run the server in debug mode.
- DELAY: add delays to the processing of client requests.
> - Bulletin file name (BBFILE) must be present in the configuration. All other values are optional.
> - A sample configuration file is provided: bbserv.conf.
 
## Commands available to the client of BBServer:
- HELP : Display available commands.
- USER \<name> : change the user name to \<name>.
- SIZE : query the total number of messages on the Bulletin Board.
- READ \<msg-number> : query the message with number \<msg-number>.
- LIST \<first> \<last> : List all messages in the range [first, last].
- WRITE \<message> : write the message to the Bulletin Board and get its message number.
- REPLACE \<msg-number>/\<message> : replace the message with msg-number with the given message.
- QUIT : quit the current session.

## Notes:
- User name should consist of alphanumeric characters.
- The default username is "nobody". To change it use the USER command.
- Messages should consist of printable characters.
- Any whitespaces in the front or the back of the message string are removed.
- Messages are assigned numbers in the sequence they are written, starting with 0. So if the total number of messages written is N then the last message written will have the number N-1.
