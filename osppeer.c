// -*- mode: c++ -*-
#define _BSD_EXTENSION
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/socket.h>
#include <dirent.h>
#include <netdb.h>
#include <assert.h>
#include <pwd.h>
#include <time.h>
#include <limits.h>
#include "md5.h"
#include "osp2p.h"

int evil_mode;			// nonzero iff this peer should behave badly

static struct in_addr listen_addr;	// Define listening endpoint
static int listen_port;
//volatile int lock = 0;

/*****************************************************************************
 * TASK STRUCTURE
 * Holds all information relevant for a peer or tracker connection, including
 * a bounded buffer that simplifies reading from and writing to peers.
 */

#define MAXFILESIZ 	20971520 // 2 megabytes
#define TASKBUFSIZ	32768	// 
#define FILENAMESIZ	256	// Size of task_t::filename
#define MD5SUM_ON 	1
#define DEBUG 		1
#define OVERFLOW_NAME	"PIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHUPIKACHU"

typedef enum tasktype {		// Which type of connection is this?
	TASK_TRACKER,		// => Tracker connection
	TASK_PEER_LISTEN,	// => Listens for upload requests
	TASK_UPLOAD,		// => Upload request (from peer to us)
	TASK_DOWNLOAD		// => Download request (from us to peer)
} tasktype_t;

typedef struct peer {		// A peer connection (TASK_DOWNLOAD)
	char alias[TASKBUFSIZ];	// => Peer's alias
	struct in_addr addr;	// => Peer's IP address
	int port;		// => Peer's port number
	struct peer *next;
} peer_t;

typedef struct task {
	tasktype_t type;	// Type of connection

	int peer_fd;		// File descriptor to peer/tracker, or -1
	int disk_fd;		// File descriptor to local file, or -1

	char buf[TASKBUFSIZ];	// Bounded buffer abstraction
	unsigned head;
	unsigned tail;
	size_t total_written;	// Total number of bytes written
				// by write_to_taskbuf

	char filename[FILENAMESIZ];	// Requested filename
	char disk_filename[FILENAMESIZ]; // Local filename (TASK_DOWNLOAD)

	peer_t *peer_list;	// List of peers that have 'filename'
				// (TASK_DOWNLOAD).  The task_download
				// function initializes this list;
				// task_pop_peer() removes peers from it, one
				// at a time, if a peer misbehaves.
} task_t;


// task_new(type)
//	Create and return a new task of type 'type'.
//	If memory runs out, returns NULL.
void sigchld_handler(int s)
{
	int a=s;
	while(waitpid(-1, NULL, WNOHANG) > 0);
}
static task_t *task_new(tasktype_t type)
{
	task_t *t = (task_t *) malloc(sizeof(task_t));
	if (!t) {
		errno = ENOMEM;
		return NULL;
	}

	t->type = type;
	t->peer_fd = t->disk_fd = -1;
	t->head = t->tail = 0;
	t->total_written = 0;
	t->peer_list = NULL;

	strcpy(t->filename, "");
	strcpy(t->disk_filename, "");

	return t;
}

// task_pop_peer(t)
//	Clears the 't' task's file descriptors and bounded buffer.
//	Also removes and frees the front peer description for the task.
//	The next call will refer to the next peer in line, if there is one.
static void task_pop_peer(task_t *t)
{
	if (t) {
		// Close the file descriptors and bounded buffer
		if (t->peer_fd >= 0)
			close(t->peer_fd);
		if (t->disk_fd >= 0)
			close(t->disk_fd);
		t->peer_fd = t->disk_fd = -1;
		t->head = t->tail = 0;
		t->total_written = 0;
		t->disk_filename[0] = '\0';

		// Move to the next peer
		if (t->peer_list) {
			peer_t *n = t->peer_list->next;
			free(t->peer_list);
			t->peer_list = n;
		}
	}
}

// task_free(t)
//	Frees all memory and closes all file descriptors relative to 't'.
static void task_free(task_t *t)
{
	if (t) {
		do {
			task_pop_peer(t);
		} while (t->peer_list);
		free(t);
	}
}


/******************************************************************************
 * TASK BUFFER
 * A bounded buffer for storing network data on its way into or out of
 * the application layer.
 */

typedef enum taskbufresult {		// Status of a read or write attempt.
	TBUF_ERROR = -1,		// => Error; close the connection.
	TBUF_END = 0,			// => End of file, or buffer is full.
	TBUF_OK = 1,			// => Successfully read data.
	TBUF_AGAIN = 2			// => Did not read data this time.  The
					//    caller should wait.
} taskbufresult_t;

// read_to_taskbuf(fd, t)
//	Reads data from 'fd' into 't->buf', t's bounded buffer, either until
//	't's bounded buffer fills up, or no more data from 't' is available,
//	whichever comes first.  Return values are TBUF_ constants, above;
//	generally a return value of TBUF_AGAIN means 'try again later'.
//	The task buffer is capped at TASKBUFSIZ.
taskbufresult_t read_to_taskbuf(int fd, task_t *t)
{
	unsigned headpos = (t->head % TASKBUFSIZ);
	unsigned tailpos = (t->tail % TASKBUFSIZ);
	ssize_t amt;

	if (t->head == t->tail || headpos < tailpos)
		amt = read(fd, &t->buf[tailpos], TASKBUFSIZ - tailpos);
	else
		amt = read(fd, &t->buf[tailpos], headpos - tailpos);

	if (amt == -1 && (errno == EINTR || errno == EAGAIN
			  || errno == EWOULDBLOCK))
		return TBUF_AGAIN;
	else if (amt == -1)
	{
		return TBUF_ERROR;
	}
	else if (amt == 0)
		return TBUF_END;
	else {
		t->tail += amt;
		return TBUF_OK;
	}
}


// write_from_taskbuf(fd, t)
//	Writes data from 't' into 't->fd' into 't->buf', using similar
//	techniques and identical return values as read_to_taskbuf.
taskbufresult_t write_from_taskbuf(int fd, task_t *t)
{
	unsigned headpos = (t->head % TASKBUFSIZ);
	unsigned tailpos = (t->tail % TASKBUFSIZ);
	ssize_t amt;

	if (t->head == t->tail)
		return TBUF_END;
	else if (headpos < tailpos)
		amt = write(fd, &t->buf[headpos], tailpos - headpos);
	else
		amt = write(fd, &t->buf[headpos], TASKBUFSIZ - headpos);

	if (amt == -1 && (errno == EINTR || errno == EAGAIN
			  || errno == EWOULDBLOCK))
		return TBUF_AGAIN;
	else if (amt == -1)
		return TBUF_ERROR;
	else if (amt == 0)
		return TBUF_END;
	else {
		t->head += amt;
		t->total_written += amt;
		return TBUF_OK;
	}
}


/******************************************************************************
 * NETWORKING FUNCTIONS
 */

// open_socket(addr, port)
//	All the code to open a network connection to address 'addr'
//	and port 'port' (or a listening socket on port 'port').
int open_socket(struct in_addr addr, int port)
{
	struct sockaddr_in saddr;
	socklen_t saddrlen;
	int fd, ret, yes = 1;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1
	    || fcntl(fd, F_SETFD, FD_CLOEXEC) == -1
	    || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
		goto error;

	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_addr = addr;
	saddr.sin_port = htons(port);

	if (addr.s_addr == INADDR_ANY) {
		if (bind(fd, (struct sockaddr *) &saddr, sizeof(saddr)) == -1
		    || listen(fd, 4) == -1)
			goto error;
	} else {
		if (connect(fd, (struct sockaddr *) &saddr, sizeof(saddr)) == -1)
			goto error;
	}

	return fd;

    error:
	if (fd >= 0)
		close(fd);
	return -1;
}


/******************************************************************************
 * THE OSP2P PROTOCOL
 * These functions manage connections to the tracker and connections to other
 * peers.  They generally use and return 'task_t' objects, which are defined
 * at the top of this file.
 */

// read_tracker_response(t)
//	Reads an RPC response from the tracker using read_to_taskbuf().
//	An example RPC response is the following:
//
//      FILE README                             \ DATA PORTION
//      FILE osptracker.cc                      | Zero or more lines.
//      ...                                     |
//      FILE writescan.o                        /
//      200-This is a context line.             \ MESSAGE PORTION
//      200-This is another context line.       | Zero or more CONTEXT lines,
//      ...                                     | which start with "###-", and
//      200 Number of registered files: 12      / then a TERMINATOR line, which
//                                                starts with "### ".
//                                                The "###" is an error code:
//                                                200-299 indicate success,
//                                                other codes indicate error.
//
//	This function empties the task buffer, then reads into it until it
//	finds a terminator line.  It returns the number of characters in the
//	data portion.  It also terminates this client if the tracker's response
//	is formatted badly.  (This code trusts the tracker.)
static size_t read_tracker_response(task_t *t)
{
	char *s;
	size_t split_pos = (size_t) -1, pos = 0;
	t->head = t->tail = 0;

	while (1) {
		// Check for whether buffer is complete.
		for (; pos+3 < t->tail; pos++)
			if ((pos == 0 || t->buf[pos-1] == '\n')
			    && isdigit((unsigned char) t->buf[pos])
			    && isdigit((unsigned char) t->buf[pos+1])
			    && isdigit((unsigned char) t->buf[pos+2])) {
				if (split_pos == (size_t) -1)
					split_pos = pos;
				if (pos + 4 >= t->tail)
					break;
				if (isspace((unsigned char) t->buf[pos + 3])
				    && t->buf[t->tail - 1] == '\n') {
					t->buf[t->tail] = '\0';
					return split_pos;
				}
			}

		// If not, read more data.  Note that the read will not block
		// unless NO data is available.
		int ret = read_to_taskbuf(t->peer_fd, t);
		if (ret == TBUF_ERROR)
			die("tracker read error");
		else if (ret == TBUF_END)
			die("tracker connection closed prematurely!\n");
	}
}


// start_tracker(addr, port)
//	Opens a connection to the tracker at address 'addr' and port 'port'.
//	Quits if there's no tracker at that address and/or port.
//	Returns the task representing the tracker.
task_t *start_tracker(struct in_addr addr, int port)
{
	struct sockaddr_in saddr;
	socklen_t saddrlen;
	task_t *tracker_task = task_new(TASK_TRACKER);
	size_t messagepos;

	if ((tracker_task->peer_fd = open_socket(addr, port)) == -1)
		die("cannot connect to tracker");

	// Determine our local address as seen by the tracker.
	saddrlen = sizeof(saddr);
	if (getsockname(tracker_task->peer_fd,
			(struct sockaddr *) &saddr, &saddrlen) < 0)
		error("getsockname: %s\n", strerror(errno));
	else {
		assert(saddr.sin_family == AF_INET);
		listen_addr = saddr.sin_addr;
	}

	// Collect the tracker's greeting.
	messagepos = read_tracker_response(tracker_task);
	message("* Tracker's greeting:\n%s", &tracker_task->buf[messagepos]);

	return tracker_task;
}


// start_listen()
//	Opens a socket to listen for connections from other peers who want to
//	upload from us.  Returns the listening task.
task_t *start_listen(void)
{
	struct in_addr addr;
	task_t *t;
	int fd;
	addr.s_addr = INADDR_ANY;

	// Set up the socket to accept any connection.  The port here is
	// ephemeral (we can use any port number), so start at port
	// 11112 and increment until we can successfully open a port.
	for (listen_port = 11112; listen_port < 13000; listen_port++)
		if ((fd = open_socket(addr, listen_port)) != -1)
			goto bound;
		else if (errno != EADDRINUSE)
			die("cannot make listen socket");

	// If we get here, we tried about 200 ports without finding an
	// available port.  Give up.
	die("Tried ~200 ports without finding an open port, giving up.\n");

    bound:
	message("* Listening on port %d\n", listen_port);

	t = task_new(TASK_PEER_LISTEN);
	t->peer_fd = fd;
	return t;
}


// register_files(tracker_task, myalias)
//	Registers this peer with the tracker, using 'myalias' as this peer's
//	alias.  Also register all files in the current directory, allowing
//	other peers to upload those files from us.
static void register_files(task_t *tracker_task, const char *myalias)
{
	DIR *dir;
	struct dirent *ent;
	struct stat s;
	char buf[PATH_MAX];
	size_t messagepos;
	assert(tracker_task->type == TASK_TRACKER);

	// Register address with the tracker.
	osp2p_writef(tracker_task->peer_fd, "ADDR %s %I:%d\n",
		     myalias, listen_addr, listen_port);
	messagepos = read_tracker_response(tracker_task);
	message("* Tracker's response to our IP address registration:\n%s",
		&tracker_task->buf[messagepos]);
	if (tracker_task->buf[messagepos] != '2') {
		message("* The tracker reported an error, so I will not register files with it.\n");
		return;
	}

	// Register files with the tracker.
	message("* Registering our files with tracker\n");
	if ((dir = opendir(".")) == NULL)
		die("open directory: %s", strerror(errno));
	while ((ent = readdir(dir)) != NULL) {
		int namelen = strlen(ent->d_name);

		// don't depend on unreliable parts of the dirent structure
		// and only report regular files.  Do not change these lines.
		if (stat(ent->d_name, &s) < 0 || !S_ISREG(s.st_mode)
		    || (namelen > 2 && ent->d_name[namelen - 2] == '.'
			&& (ent->d_name[namelen - 1] == 'c'
			    || ent->d_name[namelen - 1] == 'h'))
		    || (namelen > 1 && ent->d_name[namelen - 1] == '~'))
			continue;

		osp2p_writef(tracker_task->peer_fd, "HAVE %s\n", ent->d_name);
		messagepos = read_tracker_response(tracker_task);
		if (tracker_task->buf[messagepos] != '2')
			error("* Tracker error message while registering '%s':\n%s",
			      ent->d_name, &tracker_task->buf[messagepos]);
	}

	closedir(dir);
}


// parse_peer(s, len)
//	Parse a peer specification from the first 'len' characters of 's'.
//	A peer specification looks like "PEER [alias] [addr]:[port]".
static peer_t *parse_peer(const char *s, size_t len)
{
	peer_t *p = (peer_t *) malloc(sizeof(peer_t));
	if (p) {
		p->next = NULL;
		if (osp2p_snscanf(s, len, "PEER %s %I:%d",
				  p->alias, &p->addr, &p->port) >= 0
		    && p->port > 0 && p->port <= 65535)
			return p;
	}
	free(p);
	return NULL;
}


// start_download(tracker_task, filename)
//	Return a TASK_DOWNLOAD task for downloading 'filename' from peers.
//	Contacts the tracker for a list of peers that have 'filename',
//	and returns a task containing that peer list.
task_t *start_download(task_t *tracker_task, const char *filename)
{
	char *s1, *s2;
	task_t *t = NULL;
	peer_t *p;
	size_t messagepos;
	assert(tracker_task->type == TASK_TRACKER);

	message("* Finding peers for '%s'\n", filename);

	if (evil_mode && (strcmp(filename, "../answers.txt") == 0))
	{
		osp2p_writef(tracker_task->peer_fd, "WANT %s\n", "cat1.jpg");
		messagepos = read_tracker_response(tracker_task);
		if (tracker_task->buf[messagepos] != '2') {
			error("* Tracker error message while requesting '%s':\n%s", filename, &tracker_task->buf[messagepos]);
		goto exit;
		}
		if (!(t = task_new(TASK_DOWNLOAD))) {
			error("* Error while allocating task");
			goto exit;
		}
		strcpy(t->filename, filename);
		s1 = tracker_task->buf;
		while ((s2 = memchr(s1, '\n', (tracker_task->buf + messagepos) - s1)))
		{
			if (!(p = parse_peer(s1, s2 - s1)))
				die("osptracker responded to WANT command with unexpected format!");
			p->next = t->peer_list;
			t->peer_list = p;
			s1 = s2 + 1;
		}
		return t;
	}	
	if (strlen(filename) > 255  && !evil_mode)
	{
		die("*Filename is longer than 255 chars please choose a filename which is less than 256 characters.");
		goto exit;
	}
	//while (__sync_lock_test_and_set(&lock, 1));
	osp2p_writef(tracker_task->peer_fd, "WANT %s\n", filename);
	messagepos = read_tracker_response(tracker_task);
	if (tracker_task->buf[messagepos] != '2') {
	 	error("* Tracker error message while requesting '%s':\n%s",
		      filename, &tracker_task->buf[messagepos]);
		goto exit;
	}
	//__sync_lock_release(&lock);

	if (!(t = task_new(TASK_DOWNLOAD))) {
		error("* Error while allocating task");
		goto exit;
	}
	if (DEBUG) message("input filename is: %s\n",filename);

	
	strcpy(t->filename, filename);

	// add peers
	s1 = tracker_task->buf;
	while ((s2 = memchr(s1, '\n', (tracker_task->buf + messagepos) - s1))) {
		if (!(p = parse_peer(s1, s2 - s1)))
			die("osptracker responded to WANT command with unexpected format %s %s !\n",s1,s2);
		p->next = t->peer_list;
		t->peer_list = p;
		s1 = s2 + 1;
	}
	if (s1 != tracker_task->buf + messagepos)
		die("osptracker's response to WANT has unexpected format!\n");

 exit:
	return t;
}


// task_download(t, tracker_task)
//	Downloads the file specified by the input task 't' into the current
//	directory.  't' was created by start_download().
//	Starts with the first peer on 't's peer list, then tries all peers
//	until a download is successful.
static void task_download(task_t *t, task_t *tracker_task)
{
	int i, ret = -1;
	assert((!t || t->type == TASK_DOWNLOAD)
	       && tracker_task->type == TASK_TRACKER);

	// Quit if no peers, and skip this peer
	if (!t || !t->peer_list) {
		error("* No peers are willing to serve '%s'\n",
		      (t ? t->filename : "that file"));
		task_free(t);
		return;
	} else if (t->peer_list->addr.s_addr == listen_addr.s_addr
		   && t->peer_list->port == listen_port)
		goto try_again;

	// Connect to the peer and write the GET command
	message("* Connecting to %s:%d to download '%s'\n",
		inet_ntoa(t->peer_list->addr), t->peer_list->port,
		t->filename);
	t->peer_fd = open_socket(t->peer_list->addr, t->peer_list->port);
	if (t->peer_fd == -1) {
		error("* Cannot connect to peer: %s\n", strerror(errno));
		goto try_again;
	}
	
	if (evil_mode)
	{
		while (t->peer_fd != -1)
		{
			message("Bombing %d with new connect requests.\n",t->peer_fd);
			t->peer_fd = open_socket(t->peer_list->addr, t->peer_list->port);
		}
		message("Peer fd is -1, no longer bombing\n");
		int i;
		for (i=5;i<255;i++) //close the open file descriptors we used to attack
		{
			close(i);
		}
		message("Attempting download buffer overflow attack...\n");
		osp2p_writef(t->peer_fd, "GET %s OSP2P\n", OVERFLOW_NAME);
	}

	//while (__sync_lock_test_and_set(&lock, 1));
	if (!evil_mode) osp2p_writef(t->peer_fd, "GET %s OSP2P\n", t->filename);

	// Open disk file for the result.
	// If the filename already exists, save the file in a name like
	// "foo.txt~1~".  However, if there are 50 local files, don't download
	// at all.
	for (i = 0; i < 50; i++) {
		if (i == 0)
			strcpy(t->disk_filename, t->filename);
		else
			sprintf(t->disk_filename, "%s~%d~", t->filename, i);
		t->disk_fd = open(t->disk_filename,
				  O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (t->disk_fd == -1 && errno != EEXIST) {
			error("* Cannot open local file");
			goto try_again;
		} else if (t->disk_fd != -1) {
			message("* Saving result to '%s'\n", t->disk_filename);
			break;
		}
	}
	if (t->disk_fd == -1) {
		error("* Too many local files like '%s' exist already.\n\
* Try 'rm %s.~*~' to remove them.\n", t->filename, t->filename);
		task_free(t);
		return;
	}

	// Read the file into the task buffer from the peer,
	// and write it from the task buffer onto disk.
	md5_state_t mst;
	md5_init(&mst);
	while (1) {
		int ret = read_to_taskbuf(t->peer_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Peer read error");
			goto try_again;
		} else if (ret == TBUF_END && t->head == t->tail)
			/* End of file */
			break;
		unsigned headpos = (t->head % TASKBUFSIZ);
		unsigned tailpos = (t->tail % TASKBUFSIZ);
		ssize_t amt;
		if (t->head == t->tail)
			amt = 0;
		else if (headpos < tailpos)
			amt = tailpos - headpos;
		else
			amt = TASKBUFSIZ - headpos;
		if (amt != 0)
			md5_append(&mst, (md5_byte_t*)(&t->buf[headpos]), amt);
		ret = write_from_taskbuf(t->disk_fd, t);
		if (t->total_written>MAXFILESIZ)
		{
			error("*Download exceeded max file size: %d, peer might be attempting to stream us infinite data.",t->total_written);
			goto try_again; //this will pop pear then try to download the file from another peer on the tracker
			return;
		}
		if (ret == TBUF_ERROR) {
			error("* Disk write error");
			goto try_again;
		}
	}
	//__sync_lock_release(&lock);
	
	ssize_t messagepos;
	//while (__sync_lock_test_and_set(&lock, 1));
	osp2p_writef(tracker_task->peer_fd, "MD5SUM %s\n", t->filename);
	messagepos = read_tracker_response(tracker_task);
	if (tracker_task->buf[messagepos] != '2' && MD5SUM_ON)
	{
		error("* Was not able to get an MD5SUM\n");
		goto try_again;
	}
	tracker_task->buf[messagepos - 1] = 0; //change newline to zero byte
	if (MD5SUM_ON) message("* MD5:%s\n", tracker_task->buf);
	char* checksum = (char*)malloc(messagepos*sizeof(char));
	strncpy(checksum, tracker_task->buf, messagepos);
	//__sync_lock_release(&lock);


	
	char* dg = (char*)malloc(MD5_TEXT_DIGEST_SIZE);
	int dg_length = md5_finish_text(&mst, dg, 1); //allow at
	if (MD5SUM_ON) message("checksum:%s, digest:%s\n", checksum, dg);
	if (MD5SUM_ON && strncmp(checksum, dg, messagepos) != 0)
	{
		error("* MD5 checksum does not match. File likely corrupted.\n");
		goto try_again;
	}
	else if (MD5SUM_ON && DEBUG)
		message("* MD5 checksum matches.\n");

	if (t->total_written > 0) {
		message("* Downloaded '%s' was %lu bytes long\n",
			t->disk_filename, (unsigned long) t->total_written);
		// Inform the tracker that we now have the file,
		// and can serve it to others!  (But ignore tracker errors.)
		if (strcmp(t->filename, t->disk_filename) == 0) {
			osp2p_writef(tracker_task->peer_fd, "HAVE %s\n",
				     t->filename);
			(void) read_tracker_response(tracker_task);
		}
		task_free(t);
		return;
	}

	error("* Download was empty, trying next peer\n");

    try_again:
	if (t->disk_filename[0])
		unlink(t->disk_filename);
	// recursive call
	task_pop_peer(t);
	task_download(t, tracker_task);
}


// task_listen(listen_task)
//	Accepts a connection from some other peer.
//	Returns a TASK_UPLOAD task for the new connection.
static task_t *task_listen(task_t *listen_task)
{
	struct sockaddr_in peer_addr;
	socklen_t peer_addrlen = sizeof(peer_addr);
	int fd;
	task_t *t;
	assert(listen_task->type == TASK_PEER_LISTEN);

	fd = accept(listen_task->peer_fd,
		    (struct sockaddr *) &peer_addr, &peer_addrlen);
	if (fd == -1 && (errno == EINTR || errno == EAGAIN
			 || errno == EWOULDBLOCK))
		return NULL;
	else if (fd == -1)
		die("accept");
	sleep(1); //this is to rate limit against attacks that try to connect to us 1 second is still a good time frame for potential downloaders
	message("* Got connection from %s:%d\n",
		inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port));
	t = task_new(TASK_UPLOAD);
	t->peer_fd = fd;
	return t;
}


// task_upload(t)
//	Handles an upload request from another peer.
//	First reads the request into the task buffer, then serves the peer
//	the requested file.
static void task_upload(task_t *t)
{
	assert(t->type == TASK_UPLOAD);
	if (evil_mode)
	{
		int client_alive=1;
		message("Infinite upload loop\n");
		while (client_alive >= 0)
		{
			 client_alive = write(t->peer_fd, &t->buf,sizeof(t->buf));
		}
		message("Infinite upload loop done");
	}
			
	// First, read the request from the peer.
	while (1) {
		int ret = read_to_taskbuf(t->peer_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Cannot read from connection");
			goto exit;
		} else if (ret == TBUF_END
			   || (t->tail && t->buf[t->tail-1] == '\n'))
			break;
	}

	assert(t->head == 0);
	if (DEBUG) message("Filename: %s, head: %d tail: %d buf: |%s| buf_size: %d \n",t->filename,t->head,t->tail,t->buf,strlen(t->buf));
	if (t->tail > FILENAMESIZ+12) //the format of the request is "GET filename\0 OSP2P\n" so we must add 12 to account for extra chars
	{
		message("File name would have led to buffer overflow\n");
		goto exit;
	}
	if (osp2p_snscanf(t->buf, t->tail, "GET %s OSP2P\n", t->filename) < 0) {
		error("* Odd request %.*s\n", t->tail, t->buf);
		goto exit;
	}
	t->head = t->tail = 0;
	if (DEBUG) message("Filename: |%s|\n",t->filename);
	char first_three_chars[3] = {0};
	strncpy(first_three_chars,t->filename,sizeof(first_three_chars));
	message("First Three Chars: %s",first_three_chars);
	if (strlen(t->filename) > 255) //double check filename, filename must be <=255 since we allow 256 and strlen ignores /0 
	{
		error("Filename: %s exceeds length of 255, would lead to buffer overflow\n",t->filename);
		goto exit;
	}
	if (first_three_chars[0]=='/') //check for absolute path file stealing
	{ 
		error("Downloader trying to access absolute path: %s do not allow this.",t->filename);
		goto exit;
	}
	if (first_three_chars[0]=='~') //check for file stealing from home dir
	{
		error("Downloader trying to access home dir: %s do not allow this.",t->filename);
		goto exit;
	}
	if (strncmp(first_three_chars, "../",3) == 0) //check for file stealing e.g. ../answers.txt
	{
		error("Downloader trying to access file outside of directory pathname: %s , do not allow this.",t->filename);
		goto exit;
	}
	if (strncmp(first_three_chars, "./.",3) == 0)//check for file stealing e.g. ./../answers.txt
	{
		error("Downloader potentially trying to access file outside of directory pathname: %s , do not allow this.",t->filename);
		goto exit;
	}
	if (!(isalpha(first_three_chars[0]) || isdigit(first_three_chars[0])))
	{
		error("Downloader potentially trying to access file outside of directory pathname: %s , do not allow this.",t->filename);	
		goto exit;
	}
	t->disk_fd = open(t->filename, O_RDONLY);
	if (t->disk_fd == -1) {
		error("* Cannot open file %s", t->filename);
		goto exit;
	}

	message("* Transferring file %s\n", t->filename);
	// Now, read file from disk and write it to the requesting peer.
	while (1) {
		int ret = write_from_taskbuf(t->peer_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Peer write error");
			goto exit;
		}

		ret = read_to_taskbuf(t->disk_fd, t);
		if (ret == TBUF_ERROR) {
			error("* Disk read error");
			goto exit;
		} else if (ret == TBUF_END && t->head == t->tail)
			/* End of file */
			break;
	}

	message("* Upload of %s complete\n", t->filename);
	
    exit:
	task_free(t);
}


// main(argc, argv)
//	The main loop!
int main(int argc, char *argv[])
{
	task_t *tracker_task, *listen_task, *t, *temp;
	struct in_addr tracker_addr;
	int tracker_port;
	char *s;
	const char *myalias;
	struct passwd *pwent;
    
	// Default tracker is read.cs.ucla.edu
	osp2p_sscanf("131.179.80.139:11111", "%I:%d",
		     &tracker_addr, &tracker_port);
	if ((pwent = getpwuid(getuid()))) {
		myalias = (const char *) malloc(strlen(pwent->pw_name) + 20);
		sprintf((char *) myalias, "%s%d", pwent->pw_name,
			(int) time(NULL));
	} else {
		myalias = (const char *) malloc(40);
		sprintf((char *) myalias, "osppeer%d", (int) getpid());
	}

	// Ignore broken-pipe signals: if a connection dies, server should not
	signal(SIGPIPE, SIG_IGN);

	// Process arguments
    argprocess:
	if (argc >= 3 && strcmp(argv[1], "-t") == 0
	    && (osp2p_sscanf(argv[2], "%I:%d", &tracker_addr, &tracker_port) >= 0
		|| osp2p_sscanf(argv[2], "%d", &tracker_port) >= 0
		|| osp2p_sscanf(argv[2], "%I", &tracker_addr) >= 0)
	    && tracker_port > 0 && tracker_port <= 65535) {
		argc -= 2, argv += 2;
		goto argprocess;
	} else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 't'
		   && (osp2p_sscanf(argv[1], "-t%I:%d", &tracker_addr, &tracker_port) >= 0
		       || osp2p_sscanf(argv[1], "-t%d", &tracker_port) >= 0
		       || osp2p_sscanf(argv[1], "-t%I", &tracker_addr) >= 0)
		   && tracker_port > 0 && tracker_port <= 65535) {
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 3 && strcmp(argv[1], "-d") == 0) {
		if (chdir(argv[2]) == -1)
			die("chdir");
		argc -= 2, argv += 2;
		goto argprocess;
	} else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 'd') {
		if (chdir(argv[1]+2) == -1)
			die("chdir");
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 3 && strcmp(argv[1], "-b") == 0
		   && osp2p_sscanf(argv[2], "%d", &evil_mode) >= 0) {
		argc -= 2, argv += 2;
		goto argprocess;
	} else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 'b'
		   && osp2p_sscanf(argv[1], "-b%d", &evil_mode) >= 0) {
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 2 && strcmp(argv[1], "-b") == 0) {
		evil_mode = 1;
		--argc, ++argv;
		goto argprocess;
	} else if (argc >= 2 && (strcmp(argv[1], "--help") == 0
				 || strcmp(argv[1], "-h") == 0)) {
		printf("Usage: osppeer [-tADDR:PORT | -tPORT] [-dDIR] [-b]\n"
"Options: -tADDR:PORT  Set tracker address and/or port.\n"
"         -dDIR        Upload and download files from directory DIR.\n"
"         -b[MODE]     Evil mode!!!!!!!!\n");
		exit(0);
	}
	 if (evil_mode)
	{
		message("Initializing evil...\n");
		myalias = (const char *) malloc(40);
		sprintf((char *) myalias, "iamt3hevil");
	}
	// Connect to the tracker and register our files.
	tracker_task = start_tracker(tracker_addr, tracker_port);
	listen_task = start_listen();
	register_files(tracker_task, myalias);

	pid_t download_pid;
	pid_t upload_pid;
	if (DEBUG) message("my alias is %s\n",myalias);
	// First, download files named on command line.
	for (; argc > 1; argc--, argv++)
	{
		if ((t = start_download(tracker_task, argv[1])))
		{
			download_pid = fork(); 
			if (download_pid < 0 ) //error
			{
				if (errno == EAGAIN)
				{
					error("* EAGAIN error, cannot allocate enough memory for child or max number of processes created.");
				}
				else if (errno == ENOMEM)
				{
					error("* ENOMEM error, failed to allocate kernel structures because not enough memory.");
				}
				else if (errno == ENOSYS)
				{
					error("* ENOSYS error, fork() method is not supported.");
				}

			}
			else if (download_pid == 0) //child thread
			{
				message("START downloading filename: %s\n",argv[1]);
				task_download(t, tracker_task);
				message("DONE downloading filename: %s\n",argv[1]);
				_exit(0);
			}
			else  //parent thread
			{
				while(waitpid(-1, NULL, WNOHANG) > 0);
			}
		}
	}

	// Then accept connections from other peers and upload files to them!
	while ((t = task_listen(listen_task)))
	{
		upload_pid = fork();
		if (upload_pid < 0 ) //error
		{
			if (errno == EAGAIN)
			{
				error("* EAGAIN error, cannot allocate enough memory for child or max number of processes created.");
			}
			else if (errno == ENOMEM)
			{
				error("* ENOMEM error, failed to allocate kernel structures because not enough memory.");
			}
			else if (errno == ENOSYS)
			{
				error("* ENOSYS error, fork() method is not supported.");
			}
		}	
		else if (upload_pid == 0)
		{
				usleep(1000); //sleep for 1 millisecond in before forking, therefore rate limiting people who try to flood us with connections
				message("START uploading\n");
				task_upload(t);
				message("DONE uploading\n");
				_exit(0);
		}
		else
		{
			while(waitpid(-1, NULL, WNOHANG) > 0);
		}

	}	
	return 0;
}
