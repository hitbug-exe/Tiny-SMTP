#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <time.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <pthread.h>

/*
  MAX_LINE_LEN: defines the maximum length of a line of text that can be read or processed.
  BACKLOG_MAX: sets the maximum length of the backlog of connections waiting to be processed.
  BUF_SIZE: sets the size of the buffer used for reading data from a socket or file.
  STREQU(a,b): compares two strings (a and b) and returns true (1) if they are equal, and false (0) otherwise.
*/

#define MAX_LINE_LEN 256 
#define BACKLOG_MAX	(10) 
#define BUF_SIZE	4096
#define STREQU(a,b)	(strcmp(a, b) == 0)


/* This is the configuration struct. It contains the following parameters:

domain: a character array representing the domain name, with a maximum length of MAX_LINE_LEN characters
port: a character array representing the port number, also with a maximum length of MAX_LINE_LEN characters
This struct is intended to store the configuration information related to domain and port. 

By using MAX_LINE_LEN for both domain and port, we ensure that we have enough space to store a wide range of domain and port names without worrying about buffer overflow errors. */

typedef struct {
  char domain[MAX_LINE_LEN];
  char port[MAX_LINE_LEN];
} Config;


/*
This structure defines a linked list node that contains an integer value "d" and a pointer to the next node "next".

By creating a chain of these nodes, we can create a dynamic data structure that can grow and shrink as needed, making it an invaluable tool for managing data in memory-constrained environments. */

struct int_ll {
	int d;
	struct int_ll *next;
};


/* This is the state struct. It contains the following members:

sockfds: a pointer to a linked list of integers representing socket file descriptors.
sockfd_max: an integer that stores the maximum socket file descriptor value.
domain: a pointer to a character array representing the domain of the socket.
thread: a pthread_t variable that stores the ID of a thread.

This struct is used to store the state of the program and to manage the socket connections. The sockfds member holds a linked list of all the socket file descriptors that the program is currently using. The sockfd_max member keeps track of the maximum socket file descriptor value to optimize the process of selecting sockets for read and write operations. The domain member stores the domain of the socket, and the thread member is used to start and stop the thread that manages the sockets. */

struct {
	struct int_ll *sockfds;
	int sockfd_max;
	char *domain;
	pthread_t thread; 
} state;

void init_socket(void); // initializes a socket for network communication
void *handle_smtp (void *thread_arg); // thread routine that handles Simple Mail Transfer Protocol (SMTP) requests.
void *get_in_addr(struct sockaddr *sa); // returns a pointer to the Internet address of a given socket address structure.
void handle_client_connection(int sock_fd); // handles the client connection for a given socket file descriptor.
Config read_config(const char* filename); // takes a filename as input and returns a configuration object parsed from the file.

/* 
 *  Function: main
 *  ----------------
 *  Entry point of the program
 * 
 *  argc: number of command line arguments
 *  argv: array of command line arguments
 *
 *  returns: 0 on successful exit
 */
int main (int argc, char *argv[]) {

    char strbuf[INET6_ADDRSTRLEN];
    
    // Allocate memory for the syslog buffer and initialize it with the program name
    char *syslog_buf = (char*) malloc(1024);
    sprintf(syslog_buf, "%s", argv[0]);
    
    // Open the syslog connection and associate it with the program name
    openlog(syslog_buf, LOG_PERROR | LOG_PID, LOG_USER);
    
    // Read the configuration from the file "config.config"
    Config config=read_config("config.config");

    // Set the domain in the program state
    state.domain = config.domain;

    // Initialize the sockets and start listening for incoming connections
    init_socket();

    // Loop forever, handling incoming connections
    while (1) {
        
        // Create a set of file descriptors to monitor for incoming data
        fd_set sockets;
        FD_ZERO(&sockets);
        struct int_ll *p;
        for (p = state.sockfds; p != NULL; p = p->next) {
            FD_SET(p->d, &sockets);
        }
        
        // Wait for incoming data on any of the monitored file descriptors
        select (state.sockfd_max+1, &sockets, NULL, NULL, NULL);

        // Handle each socket that has incoming data
        for(p = state.sockfds; p != NULL; p = p->next){
            if(FD_ISSET(p->d, &sockets)){
                handle_client_connection(p->d);
            }
        }
    }

    // Cleanup and exit
    return 0;
}


/**
Initializes socket connections for the server to listen to incoming client connections

using configuration parameters in "config.config" file

Parameters: None

Return: None
*/

void init_socket(void) {
int rc, yes = 1;
int sockfd;
struct addrinfo hints, *hostinfo, *p;

// Set hints struct for getaddrinfo()
memset(&hints, 0, sizeof(hints));
hints.ai_family = AF_UNSPEC;
hints.ai_socktype = SOCK_STREAM;
hints.ai_flags = AI_PASSIVE;

// Initialize state variables
state.sockfds = NULL;
state.sockfd_max = 0;

// Read configuration from file
Config config = read_config("config.config");
state.domain = config.domain;

// Get address info for the host
rc = getaddrinfo(NULL, config.port, &hints, &hostinfo);
if (rc != 0) {
syslog(LOG_ERR, "Failed to get host address info");
exit(EXIT_FAILURE);
}

// Iterate through address info and create socket for each valid address
for (p = hostinfo; p != NULL; p = p->ai_next) {
   // Skip if address family does not match hints struct
 if (hints.ai_family != AF_UNSPEC && p->ai_family != hints.ai_family) {
     continue;
 }

 // Convert IP address to string for logging
 char ipstr[INET6_ADDRSTRLEN];
 inet_ntop(p->ai_family, &((struct sockaddr_in*)p->ai_addr)->sin_addr, ipstr, sizeof(ipstr));

 // Create socket for address
 sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
 if (sockfd == -1) {
     syslog(LOG_NOTICE, "Failed to create socket for address %s", ipstr);
     continue;
 }

 // Set socket options
 setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

 // Bind socket to address
 rc = bind(sockfd, p->ai_addr, p->ai_addrlen);
 if (rc == -1) {
     close(sockfd);
     syslog(LOG_NOTICE, "Failed to bind socket to address %s", ipstr);
     continue;
 }

 // Listen to socket
 rc = listen(sockfd, BACKLOG_MAX);
 if (rc == -1) {
     syslog(LOG_NOTICE, "Failed to listen to socket at address %s", ipstr);
     exit(EXIT_FAILURE);
 }

 // Update maximum socket file descriptor
 (sockfd > state.sockfd_max) ? (state.sockfd_max = sockfd) : 1;

 // Create new linked list element for socket file descriptor and add to state
 struct int_ll *new_sockfd = malloc(sizeof(struct int_ll));
 new_sockfd->d = sockfd;
 new_sockfd->next = state.sockfds;
 state.sockfds = new_sockfd;
}

// Free host info
freeaddrinfo(hostinfo);

// Exit if no sockets were bound
if (state.sockfds == NULL) {
syslog(LOG_ERR, "Failed to bind to any sockets");
exit(EXIT_FAILURE);
}
return;
}


/*
handle_smtp -- Handles Simple Mail Transfer Protocol (SMTP) connections
Parameters:
thread_arg: A void pointer to the argument of the thread
Returns:
NULL
Description:
This function handles SMTP connections by creating a thread for a given
socket, receiving messages from the socket, and sending the appropriate
response based on the message received. The function first logs that it
is starting a thread for a given socket, then initializes several variables
and sends the initial message to the client. The function then enters a loop,
waiting for messages to be received from the client. If no message is received
within 120 seconds, the function logs that the socket has timed out and exits.
If a message is received, the function processes the message and sends the
appropriate response back to the client. If the message is part of a message
body, the message data is handled separately.
*/

void *handle_smtp(void *thread_arg) {
// Print debug message indicating thread has started and what socket it's working on
syslog(LOG_DEBUG, "Starting thread for socket #%d", *(int *)thread_arg);

// Declare and initialize variables
int rc, i;
char buffer[BUF_SIZE], bufferout[BUF_SIZE];
size_t buffer_offset = 0; // Use size_t for buffer_offset, since it's used as an array index
buffer[BUF_SIZE - 1] = '\0'; // Add null terminator to end of buffer

// Get the socket file descriptor from the thread argument and free the argument
int sockfd = *(int *)thread_arg;
free(thread_arg);

// Set initial state to not in a message
int inmessage = 0;

// Send greeting to client
sprintf(bufferout, "220 %s SMTP CCSMTP\r\n", state.domain);
printf("%s", bufferout);
send(sockfd, bufferout, strlen(bufferout), 0);

// Main loop to receive and process commands from client
while (1) {
// Use select() to wait for data to be available on the socket
fd_set sockset;
struct timeval tv;
FD_ZERO(&sockset);
FD_SET(sockfd, &sockset);
tv.tv_sec = 120;
tv.tv_usec = 0;
select(sockfd + 1, &sockset, NULL, NULL, &tv);
// If no data available on socket within 120 seconds, log and break out of loop
if (!FD_ISSET(sockfd, &sockset)) {
  syslog(LOG_DEBUG, "%d: Socket timed out", sockfd);
  break;
}

// Calculate space left in buffer for receiving data
size_t buffer_left = BUF_SIZE - buffer_offset - 1;

// If no space left in buffer, send error response and reset buffer
if (buffer_left == 0) {
  syslog(LOG_DEBUG, "%d: Command line too long", sockfd);
  sprintf(bufferout, "500 Too long\r\n");
  printf("S%d: %s", sockfd, bufferout);
  send(sockfd, bufferout, strlen(bufferout), 0);
  buffer_offset = 0;
  continue;
}

// Receive data into buffer
rc = recv(sockfd, buffer + buffer_offset, buffer_left, 0);

// If remote host closed socket, log and break out of loop
if (rc == 0) {
  syslog(LOG_DEBUG, "%d: Remote host closed socket", sockfd);
  break;
}

// If there was an error on the socket, log and break out of loop
if (rc == -1) {
  syslog(LOG_DEBUG, "%d: Error on socket", sockfd);
  break;
}

// Update buffer_offset to reflect amount of data received
buffer_offset += rc;

// Find the end of the line in the buffer
char *eol;
processline:
eol = strstr(buffer, "\r\n");
// If end of line not found yet, continue waiting for more data
if (eol == NULL) {
  syslog(LOG_DEBUG, "%d: Haven't found EOL yet", sockfd);
  continue;
}

// Replace end of line with null terminator
eol[0] = '\0';
/* check if message is part of email or not */
	if (!inmessage) {

		/* convert first 4 characters to uppercase if they are lowercase */
		for (i = 0; i < 4; i++) {
			if (islower(buffer[i])) {
				buffer[i] += 'A' - 'a';
			}
		}

		/* check recognized commands and send appropriate response back to client */
		if (STREQU(buffer, "HELO")) { 
			sprintf(bufferout, "250 Ok\r\n");
			printf("S%d: %s", sockfd, bufferout);
			send(sockfd, bufferout, strlen(bufferout), 0);
		} else if (STREQU(buffer, "MAIL")) { 
			sprintf(bufferout, "250 Ok\r\n");
			printf("S%d: %s", sockfd, bufferout);
			send(sockfd, bufferout, strlen(bufferout), 0);
		} else if (STREQU(buffer, "RCPT")) { 
			sprintf(bufferout, "250 Ok recipient\r\n");
			printf("S%d: %s", sockfd, bufferout);
			send(sockfd, bufferout, strlen(bufferout), 0);
		} else if (STREQU(buffer, "DATA")) { 
			sprintf(bufferout, "354 Continue\r\n");
			printf("S%d: %s", sockfd, bufferout);
			send(sockfd, bufferout, strlen(bufferout), 0);
			inmessage = 1;
		} else if (STREQU(buffer, "RSET")) { 
			sprintf(bufferout, "250 Ok reset\r\n");
			printf("S%d: %s", sockfd, bufferout);
			send(sockfd, bufferout, strlen(bufferout), 0);
		} else if (STREQU(buffer, "NOOP")) { 
			sprintf(bufferout, "250 Ok noop\r\n");
			printf("S%d: %s", sockfd, bufferout);
			send(sockfd, bufferout, strlen(bufferout), 0);
		} else if (STREQU(buffer, "QUIT")) {
			sprintf(bufferout, "221 Ok\r\n");
			printf("S%d: %s", sockfd, bufferout);
			send(sockfd, bufferout, strlen(bufferout), 0);
			break;
		} else { 
			sprintf(bufferout, "502 Command Not Implemented\r\n");
			printf("S%d: %s", sockfd, bufferout);
			send(sockfd, bufferout, strlen(bufferout), 0);
			}
		} else {
      printf("C%d: %s\n", sockfd, buffer);
      if (STREQU(buffer, ".")) { 
    /*
    If the message ends with a ".", a confirmation message is created and sent back to the client.
    */
    sprintf(bufferout, "250 Ok\r\n");
    printf("S%d: %s", sockfd, bufferout);
    send(sockfd, bufferout, strlen(bufferout), 0);
    inmessage = 0;
  }
}
/*
shifts the buffer to remove the processed message and update the buffer_offset accordingly.
*/
memmove(buffer, eol+2, BUF_SIZE - (eol + 2 - buffer));
buffer_offset -= (eol - buffer) + 2;
/*
checks if there are more messages in the buffer, and if so, continues processing them by jumping to the 'processline' label.
*/
if (strstr(buffer, "\r\n"))
goto processline;

/*
When there are no more messages to process, the socket is closed and the pthread is exited.
*/
close(sockfd);
pthread_exit(NULL);
}
}


/*
Description: A function that extracts IP address from a given sockaddr struct.
Parameters:
sa: A pointer to a sockaddr struct containing the IP address to be extracted.
Returns:
A pointer to the extracted IP address.
Assumptions/Preconditions:
The sa pointer points to a valid sockaddr struct.
The IP address family (AF_INET or AF_INET6) is correctly set in the sockaddr struct.
*/

void * get_in_addr(struct sockaddr *sa){ 
// If the address family is IPv4
if (sa->sa_family == AF_INET) {

    // Return a pointer to the IPv4 address
	return &(((struct sockaddr_in*)sa)->sin_addr);

}
// Otherwise, the address family must be IPv6
else {
    // Return a pointer to the IPv6 address
	return &(((struct sockaddr_in6*)sa)->sin6_addr);
  }
}


/*
Description:
Reads a configuration file specified by the "filename" parameter and returns a structure "Config" containing two fields "domain" and "port". The function parses the configuration file line by line, extracts the key-value pairs, and assigns them to the respective fields of the "Config" structure.

Parameters:

filename: A pointer to a string representing the name of the configuration file to be read.

Returns:

A structure of type "Config" containing two fields "domain" and "port" representing the domain and port values read from the configuration file.
*/

Config read_config(const char* filename){
    // Initialize the "Config" structure with empty strings
    Config config = {"", ""};

    // Open the configuration file for reading
    FILE* fp = fopen(filename, "r");
    if (fp == NULL) {
        fprintf(stderr, "Error: could not open config file\n");
        return config;
    }

    // Read the configuration file line by line
    char line[MAX_LINE_LEN];
    while (fgets(line, MAX_LINE_LEN, fp) != NULL) {
        // Extract the key-value pairs from the current line
        char key[MAX_LINE_LEN], value[MAX_LINE_LEN];
        int count = sscanf(line, " %[^= ] = %s", key, value);
        if (count == 2) {
            // Assign the value to the respective field of the "Config" structure based on the key
            if (strcmp(key, "DOMAIN") == 0) {
                strncpy(config.domain, value, MAX_LINE_LEN);
            } else if (strcmp(key, "PORT") == 0) {
                strncpy(config.port, value, MAX_LINE_LEN);
            }
        }
    }

    // Close the configuration file
    fclose(fp);

    // Return the "Config" structure with the domain and port values read from the configuration file
    return config;
}


/*
Description:
accepts a socket file descriptor as an argument, accepts a connection from a client, logs the connection details, and creates a new thread to handle the connection using the "handle_smtp" function.

Parameters:

sock_fd: An integer representing the socket file descriptor for the listening socket.

Returns:

NULL

*/
void handle_client_connection(int sock_fd) {
    struct sockaddr_storage client_addr;
    socklen_t sin_size = sizeof(client_addr);
    // Accept a new client connection and get a new socket file descriptor for the client
    int new_sock = accept(sock_fd, (struct sockaddr*) &client_addr, &sin_size);
    // If accept() fails, log an error message and return
    if (new_sock == -1) {
        syslog(LOG_ERR, "Accepting client connection failed");
        return;
    }

    char strbuf[INET6_ADDRSTRLEN];
    // Get the client IP address as a void pointer
    void *client_ip = get_in_addr((struct sockaddr *)&client_addr);
    // Convert the client IP address to a string
    inet_ntop(client_addr.ss_family, client_ip, strbuf, sizeof(strbuf));
    // Log the connection details
    syslog(LOG_DEBUG, "Connection from %s", strbuf);

    // Create a new thread to handle the client connection
    int *thread_arg = (int*) malloc(sizeof(int));
    *thread_arg = new_sock;
    pthread_create(&(state.thread), NULL, handle_smtp, thread_arg);
}