# BulletinBoardServer
The bulletin board server accepts one-line messages from the clients, stores them in a local file, and serves them back on request. Messages are identified upon storage by a unique number established by the server. The name of the client who sent the message is stored along with the message. The protocol2pc.txt explains the synchronization protocol for BBservers.

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
