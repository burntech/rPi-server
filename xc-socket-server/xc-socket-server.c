/* 

   xc-socket-server.c	V5.0
   Copyright - Neil Verplank 2016, 2017 (c)
   neil@capnnemosflamingcarnival.org

   Code is inspired by:
   multi-client-echo-server.c - a multi-client echo server 	
   Copyrights - Guy Keren 1999 (c)

   Oct '16  - updated response to incoming messages for Camera/Flash
   Sep '17  - major updates to handle socket pools, segfaults, clean up code
   Oct '17  - major updates, clean up bad coms, move stuff to subroutines
            - integrated use of hash tables
   Nov '17  - general improvements, added "timed sequences" vs. blocking routines

// Aug 18 - note - need a way to know how many poofers an effect has (i.e. loki)
//  so we can individually adress solenoids?  How was the organ working?


// 0) (771) - uh oh.  not reading messages right in high traffic?

   fundamental issue here is that client needs to add $...% to beginning
   and end of messages, server needs to look through buffer for those 
   characters, and should handle a) multiple msgs in buffer and 
   b) message longer than buffer.  client likely has the same issue.

// 1) kill all signal, free event list (935)
// 2) timed sequence "parser" for passsing in external sequences (425, 1120)
// 3) attempt to add clock pulse and sync 8266's (set up NTP server?)
               
   This is intended as a FAST multi-socket select() server,
   running on a raspberry pi 3 (with wifi), in
   conjunction with the Arduino Adafruit Huzzah (the ESP2866
   wifi enabled board) or similar to create a responsive, wirelessly
   distributed microcontroller network.  We're using it to
   link a series of flame-enabled games, and other effects,
   and trying to limit latency to <2ms throughout, while retaining
   continuous control (e.g. the ability to kill all effects instantly).


   Why a custom server?  Why not an MQTT broker?  First, We want centralized
   control. That gives the capacity to introduce complex behaviors
   (mapping some input to all effects for instance).  Second, it creates
   the basic ability to "shut everything off" - important when dealing
   with fire.  Finally, a custom server  allows for 
   optimization around speed.  Amongst other things, explicitly and 
   always turning off Nalge's algorithm (something Mosquitto doesn't
   currently do), which as of Oct '17 is allowing for <2ms latency on
   all transmissions (below human perceptibility).  It can yet be
   better, but pretty good so far - about 100 microseconds for message in
   to first message out.  Bottom line, the server is not currently
   causing any messaging delays.

   In addition, the clients are presumed to be sending a keep alive
   signal, and the server presumes to keep the sockets open to minimize
   delay.  The various lists and existing collections are kept up to date with
   modification times, so once all the effects are online, far fewer
   "lookups" are required.

   NOTES:

   We're using the RPi 3, and have configured it as
   an access point with a static IP. 

   Start the server on the command line:

       ./xc-socket-server [1]
   
   When the option 1 is added, DEBUG=1, and lots of useful info is dropped 
   into STDOUT.  When DEBUG is 0, the assumption is no output, and it will 
   run as a daemon. 

   There are some (questionable) scripts in initscripts that can be linked
   into /etc/init.d and used to start xc-socket-server at boot, and optionally,
   but-client as well.  The latter presumes you'll have a physical button on
   the pi itself as "The Button".  

   ***

   TO send messages from one socket to another, you must "set a collection"
   to which a given effect will broadcast.  Note that "the button" (currently
   effect: 'B') sends messages to *all* effects, except those with the DS 
   (Don't Send) flag set.  Set a collection by:

      effectname:*:CC:effectname1,effectname2...

   This can be done at any time, whether other effects are present are not, 
   the collection is kept up to date with existing effects.

   ***

   Any effect can block the receipt of messages (and be only a sender):
`
      effectname:*:DS

   ***

   Look in subroutine processMsg() for how and where to 
   insert any custom message handling code.

   Default behavior right now when two identically named things try to get on
   the network is to ignore messages from all but the original.  This keeps both
   sockets open, which is better than thrashing back and forth, or adding
   two things with the same name/hash, as this leads to undefined (bad)
   behavior.  We could mod all names to contain socket, but that gets
   tricky - better to push it back on the originator.

   You will likely either want to also run but-client.c for the button,
   or use an 8266 for the same purpose.

   ***

   To compile:

       gcc -o xc-socket-server xc-socket-server.c -lpthread -lrt -Wall

*/



#include <stdlib.h>		/* Standard Library		*/
#include <stdio.h>		/* Basic I/O routines 		*/
#include <inttypes.h>
#include <time.h>		/* Time in microseconds		*/
#include <math.h>		/* Math!			*/
#include <stdlib.h>
#include <string.h>		
#include <sys/types.h>		/* standard system types 	*/
#include <sys/stat.h>
#include <netinet/in.h>		/* Internet address structures 	*/
#include <arpa/inet.h>
#include <sys/socket.h>		/* socket interface functions 	*/
#include <netdb.h>		/* host to IP resolution 	*/
#include <unistd.h>		/* for table size calculations 	*/
#include <sys/time.h>		/* for timeout values 		*/
#include <signal.h>
#include <netinet/tcp.h>	/* TCP_NODELAY 			*/




int	DEBUG           = 0;    // 0=daemon, no output, 1=command line with output 

#define	PORT		5061	// port for our carnival server   	
#define	BUFLEN		1024	// buffer length 	   	
#define maxclients      40      // max number of newtwork clients, somewhat arbitrary	
#define ROpoofLength	100 	// milliseconds of a poof in roundOrder 
#define ROpoofDelay	40 	// milliseconds delay between poofs roundOrder 	


// PRE-DEFINED OUTGOING MESSAGES  / MESSAGE STRUCTURE
#define MSG_DIV    	":"     // primary internal messaging divider ("EFFECT[:msg1][:msg2][:msg3][:msg4]")
#define MSG_DIV2    	","     // secondary sub-divider (eg msg2 = "EFFECT1,EFFECT2..."
//#define MSG_DIV3    	";"     // sub-divider used by parse_events
//#define MSG_DIV4    	"+"     // sub-divider used by parse_events
//#define MSG_DIV5    	"->"    // sub-divider used by parse_events
//#define MSG_DIV6    	"&"     // sub-divider used by parse_events
#define PoofON 		"$p1%"  // NEED TO MODIFY SEND MESG TO PRE- POST-PEND THE $ AND %
#define PoofOFF		"$p0%"
#define PoofSTM		"$p2%"


// VARIOUS INCOMING MESSAGES & KNOWN EFFECTS
#define KeepAlive       "KA"
#define CONTROL         "*"     // special control message
#define RO       	"RO"    // ctl msg - set round order
#define CC       	"CC"    // ctl msg - set collection
#define DS       	"DS"    // ctl msg - set do_not_send flag
#define XX       	"XX"    // ctl msg - kill all poofers, kill events

#define BUTTON		"B"
#define BIGBETTY        "BIGBETTY"
#define LULU	        "LULU"




/************** OUR DATA & VARIABLES    ******************/


/* hash structure for associative array that stores effects as key / value pairs */
typedef struct _list_t_ {
    char   *effect;                     // name of the effect
    int     socket_num;                 // socket this effect is on
    int     do_not_send;                // true if broadcast-only effect
    long    c_mod;                      // last update of collection, or 0L 
    struct _list_t_ *next;              // next effect on a list, or in the hashtable bucket
    struct _list_t_ *collection;        // list of effects to whom I talk, if any
} list_t;


/* 
    Table structure.  Basic table, with addition of "all" and "ordered",
    where 'all' contains all current nodes (populated and maintained by
    get_all_nodes), and 'ordered' contains an ordered list of nodes.
    By default, 'ordered' is the order in which the sockets came in.  
    You can also pass in a list of ordered nodes that could contain any
    nodes, and would supercede the default order (which is then lost).
*/
typedef struct _hash_table_t_ {
    int      size;                      // buckets in the table
    long     modified;                  // last mod time, in milliseconds
    long     all_set;                   // last time in milliseconds all list set
    long     ordered_set;               // last time in milliseconds ordered list set
    int      numOfElements;		// total number of elements in the table
    list_t **table;                     // the table elements 
    list_t  *all;                       // place to store linked list of all elements in table*
    list_t  *ordered;                   // place to store an ordered list (for Round)
} hash_table_t;
//* See notes above - we retain this list once created, as we think it won't change much.  Faster.
//  Similar for ordered.  We keep track of modification times of the lists, and the table, to be sure.



/* 
    Incoming message, parsed.  Message is a string, minimally starting
    with the effect name, sub-divided by ':', and containing up to
    four additional strings.  Escaped characters (returns, line-feeds,
    and etc.) are ignored.

        effect_name[:sub_msg_1:sub_msg_2:sub_msg_3:sub_msg_4]
*/
typedef struct _msg_t_ {
    char   *effectName;                 // name of the effect
    char   *firstMsg;                   // sub messages
    char   *secondMsg;
    char   *thirdMsg;
    char   *fourthMsg;
    int     whosTalking;                // socket this effect is on
    struct _msg_t_ *next;               // next msg on the list, if any
} msg_t;



/* 
    list of timed events.  events can apply to multiple effects, and
    can be "overlapping" (i.e. events can theoretically begin before 
    other events are finished for potentially complex behavior). 

    thus the list isn't necessarily sequential.  currently presuming
    one list at a time...

    "begin" is a time in ms.  if '0', event begins upon receipt of 
    timed sequence, >0 means begin that many ms after initial receipt.
*/
typedef struct _event_t_ {
    char   *action;                     // type of event (e.g. "poof")
    long    begin;                      // relative time from 0 to begin
    long    length;                     // length of event in ms
    int     started;                    // 1 if begun
    struct _event_t_ *next;             // next event on a list
    struct _list_t_  *collection;       // list of effects ?
} event_t;




int     max_socket     = 0;             // will hold the largest possible next socket number
int     firstSocket    = 0;             // socket to which we listen
float   start_time     = 0;             // sys clock when we begin
float   current_time   = 0;             // millisenconds since start_time
int     current_micros = 0;             // microsend component as int
int     start_micros   = 0;             // microsend component as int




/************** SUBROUTINES  ******************/

// socket server routines
void    checkDebug();
void    forkify();
void    getFirstSock();
int     acceptSK();
void    forceCloseSK();
void    closeSK();
int     namedSock();

// messaging routines
msg_t   *new_msg();
char    *copy_str_part();
event_t *readBuffer();
event_t *processMsg();
void    send_msg ();
void    send_all();
event_t *doControl();

// buttons and poofing
event_t *doButton();
void    theButton();
void    poofStorm();
event_t *bigRound();
event_t *roundRobbin();
event_t *lulu_poof();
void    bigbetty_poof();

// events
event_t *new_event();
event_t *check_events();
event_t *concat_events();

// utilities
void    error();
void    cur_time();
long    millis();
int     naive_str2int ();
void    delay();
void    delay_micro();

// lists and collections
void    set_collection();
void    send_to_collection();
void    update_collection();
void    msg_list();
void    set_list_order();
int     not_on_list();    
list_t  *get_ordered_list();
list_t  *copy_list();
list_t  *concat_lists();
list_t  *get_list();

// hash table routines
hash_table_t     *create_hash_table();
unsigned long     hash();

list_t  *new_node();
list_t  *copy_node();
void    update_node();
list_t  *get_all_nodes();
list_t  *lookup_effect();
list_t  *lookup_effect_sk();
int     add_effect();
int     get_socket();
int     set_effect_socket();
int     set_effect_socket_sk();
void    set_effect_ds();
int     hash_table_count();

// freeing memory
void    free_msg();
void    free_table();
void    free_node_list();
void    free_node();
void    free_event_list();
void    free_event();




/* 

    MAIN SETUP AND INFINITE LOOP 


    Go through each socket with incoming data, read message and process.

    NOTE: we range from s+1 to max_socket.  In fact, the first socket allocated
    (typically 3) seems to be just used to bind - we dont send or receive over it.  
    Sockets 0, 1 and 2 normally correspond to STDIN, STDOUT, and STDERR.
	  
    So the first "real" socket is s+1, and the "last" is max_socket-1, and
    max_socket is the next *highest* number available.  Note that it's
    possible for there to be fewer sockets than max_socket-s+1, as sockets can be
    freed up *between* s and max_socket, but we just run through the range for
    sockets available to be read.

    Internally, both an attempt to read from, or to write to an unavailable socket
    forces the socket closed. Arguably we could collect those and close them
    at the end of the message processing.
*/

int main(int argc,char **argv) {

    // socket server variables 
    int		   i;				// index counters for loop operations
    fd_set	   open_sockets;   		// set of open sockets
    fd_set	   readable_sockets; 		// set of sockets available to be read
    fd_set	   writeable_sockets; 		// set of sockets available for writing 
    char           *ipadd;

    hash_table_t   *my_hash_table;		// hash for holding effect names, sockets, properties
    event_t        *my_events  = NULL;		// struct for holding the current timed sequence
    event_t        *new_events = NULL;		// new timed sequences
    long           seq_start   = 0L;            // start time for timed sequence

    int            size_of_table 
                     = (int)maxclients/2;       // theorectically, a max of 2 per bucket, ideal.


    // Make this server a DAEMON if not debugging
    forkify(argc, argv);

    // set initial time, clear socket sets
    millis(); 
    FD_ZERO(&open_sockets);	

    // Create basic hash table to use as associative array for named sockets
    my_hash_table = create_hash_table(size_of_table);

    // get and bind first socket for listening
    getFirstSock(&open_sockets);
   
    // continuously await then process messages
    while (1) {

	readable_sockets   = open_sockets;
	writeable_sockets  = open_sockets;

	// check which sockets are ready for reading.     
        // (null in timeout means wait until incoming data)
        select(max_socket+1, &readable_sockets, &writeable_sockets, NULL, (struct timeval *)NULL);

        // return from select - add incoming sockets to pool 
        ipadd = "";
        acceptSK(firstSocket, readable_sockets, &open_sockets, &ipadd);

        // go through sockets and read buffer from any readable sockets (and internally process messages)
	for (i=firstSocket+1; i<max_socket+1; i++)  {

            // read incoming, set named effect, get socket number
            if (FD_ISSET(i, &readable_sockets)) 
                new_events = readBuffer(i, &readable_sockets, &open_sockets, &writeable_sockets, my_hash_table);
            if (new_events) {
                my_events = concat_events(my_events,new_events);
// kind of presumes there wasn't already a seq_start?
                seq_start = millis();
            }

        }

        // take care of any timed events
        my_events = check_events(my_events, seq_start, &open_sockets, my_hash_table); 

    } 

    free_event_list(my_events);
    free_table(my_hash_table);

    return(0);

} /* end main	*/















/*      DAEMON SUBROUTINES     */


/* check if DEBUG from command line */
void checkDebug(int argc, char **argv) {
    if (argc == 2) {
        DEBUG = (int)argv[1][0]-'0';
    }
}



/* Make this server a DAEMON if not debugging.	*/
void forkify(int argc, char **argv) {

//    INIT SCRIPTS NOT RIGHT!!

    /* 

    I note this isn't really working correctly when launched at boot.  
    In particular, stopping the daemon causes it to re-spawn, several 
    times in the case of the button.

    The problem is in the init scripts, not in this code, but.

    It does however launch on boot, which was the basic goal.

    */


    checkDebug(argc, argv);

    if (!DEBUG) {

        pid_t process_id = 0;
        pid_t sid = 0;
    
        process_id = fork();  	// Create child process

        if (process_id < 0) {
            printf("fork failed!\n");
            exit(1);		// Return failure in exit status
        }
      
        if (process_id > 0) {	// KILL PARENT PROCESS
            exit(0);		// return success in exit status
        }
      
        umask(0);		// unmask the file mode
        sid = setsid();		// set new session
        if(sid < 0) {
            printf("couldn't setsid\n");
            exit(1);		// Return failure
        }
      
        chdir("/");		// Change the current working directory to root.

    } else {
        printf("open for debugging!\n");fflush(stdout);
    } 

    signal(SIGPIPE, SIG_IGN);	// ignore sigpipe (use MSG_NOSIGNAL as well...)
}






/*  SOCKET SUBROUTINES  */


/* get first socket and bind our listener it it */
void getFirstSock(fd_set *open_sockets) {

    struct sockaddr_in	sa; 		 		/* Internet address struct 			*/

    memset(&sa, 0, sizeof(sa)); 			/* first clear out the struct, to avoid garbage	*/
    sa.sin_family = AF_INET;				/* Using Internet address family 		*/
    sa.sin_port = htons(PORT);				/* copy port number in network byte order 	*/
    sa.sin_addr.s_addr = INADDR_ANY;		  	/* accept connections through any host IP 	*/
    firstSocket = socket(AF_INET, SOCK_STREAM, 0);	/* allocate a free socket 			*/

    if (firstSocket < 0) {
	error("socket: allocation failed");
    }

    // bind the socket to the newly formed address 
    int rc = bind(firstSocket, (struct sockaddr *)&sa, sizeof(sa));

    /* check there was no error */
    if (rc) {
	error("bind");
    }

    /* ask the system to listen for incoming connections	*/
    /* to the address we just bound. specify that up to		*/
    /* 5 pending connection requests will be queued by the	*/
    /* system, if we are not directly awaiting them using	*/
    /* the accept() system call, when they arrive.		*/
    rc = listen(firstSocket, 5);

    /* check there was no error */
    if (rc) {
	error("listen");
    }
    
    max_socket = getdtablesize();	/* calculate size of file descriptors table */
    FD_SET(firstSocket, open_sockets);		/* we initially have only one socket open */
    max_socket = firstSocket;  		/* reset max socket number */

}



/* Accept an incoming socket */
int acceptSK(int s, fd_set readable_sockets, fd_set *open_sockets, char **ipout ) {

    /* accept incoming connections, if any, add to array */
 
    int  cs     = 0;
    int  result = 0;
    int  flag   = 1;	  	 	/* for TCP_NODELAY				*/

    struct sockaddr_storage	csa; 	/* client's address struct 			*/
    char ipstr[INET6_ADDRSTRLEN];
    socklen_t size_csa; 		/* size of client's address struct 		*/
    size_csa = sizeof(csa);		/* remember size for later usage */

    /* if the 's' socket is ready for reading, it	*/
    /* means that a new connection request arrived.	*/

    if (FD_ISSET(s, &readable_sockets)) {

        // accept the incoming connection 
        cs = accept(s, (struct sockaddr *)&csa, &size_csa);

        // check for errors. if any, ignore new connection 
       	if (cs < 0) {
       	    return 0;
        }

        // Turn off Nagle's algorithm for less delay 
        result = setsockopt(
	      cs,
              IPPROTO_TCP,    
              TCP_NODELAY,   
              (char *) &flag,
              sizeof(int));

        if (result < 0) {
            // note, error doesn't preclude continuing 
            if (DEBUG) { printf("TCP_NODEAY failed.\n");fflush(stdout); }
        }

        FD_SET(cs, open_sockets);		        /* add socket to set of open sockets */
        if (cs > max_socket) { max_socket = cs; }  	/* reset max */
	  

        if (csa.ss_family == AF_INET) {
            struct sockaddr_in *ss = (struct sockaddr_in *)&csa;
            inet_ntop(AF_INET, &ss->sin_addr, ipstr, sizeof ipstr);
        } else if (csa.ss_family == AF_INET6) {
            struct sockaddr_in6 *ss = (struct sockaddr_in6 *)&csa;
            inet_ntop(AF_INET6, &ss->sin6_addr, ipstr, sizeof ipstr);
        } 

        if (DEBUG) { cur_time(); printf("->->new socket#:%02d ipaddr:%s\n", cs, ipstr);fflush(stdout); }

        *ipout = strdup(ipstr);
    }

    return cs;
}



/* close socket and clear named socket arrays */
void closeSK(int socket, fd_set *open_sockets, hash_table_t *hashtable) {
    forceCloseSK(socket, open_sockets);
    set_effect_socket_sk(hashtable, socket, 0); 
}


/* just close socket */
void forceCloseSK(int fd, fd_set *open_sockets) {
    close(fd);
    FD_CLR(fd, open_sockets);
}






/*  MESSAGING SUBROUTINES  */

/* parse an incoming messaging into 5 strings */
msg_t *new_msg(char *str, int sock) {

    msg_t *newmsg; 

    // allocate memory for new node
    if ((newmsg = (msg_t *)malloc(sizeof(msg_t))) == NULL) return NULL;

    char *part;
   
 
    // Populate data
    part = copy_str_part(strtok(str,MSG_DIV));
    newmsg->effectName = part;

    if (part != NULL) 
      newmsg->firstMsg = copy_str_part(strtok(NULL,MSG_DIV));

    if (newmsg->firstMsg != NULL) 
      newmsg->secondMsg = copy_str_part(strtok(NULL,MSG_DIV));
    else 
      newmsg->secondMsg = NULL;

    if (newmsg->secondMsg!= NULL) 
      newmsg->thirdMsg = copy_str_part(strtok(NULL,MSG_DIV));
    else 
      newmsg->thirdMsg = NULL;

    if (newmsg->thirdMsg != NULL) 
      newmsg->fourthMsg = copy_str_part(strtok(NULL,MSG_DIV));
    else 
      newmsg->fourthMsg = NULL;

    newmsg->whosTalking = sock;

    newmsg->next = NULL;

    return newmsg;
}




/*     remove trailing escape escape characters    */
char *clean_str_part(char *str) {

    if      (str == NULL) return NULL;
    else if (!strlen(str)) return NULL;

    int i = 0;
    int j = strlen(str);

    while (i<j) {
        if(str[i]<32) 
            str[i] = '\0';
        i++;
    }

    return str;
}




/*     return a "clean" copy of the string  */
char *copy_str_part(char *str) {

    if      (str == NULL) return NULL;
    else if (!strlen(str)) return NULL;

    str = clean_str_part(str);

    return strdup(str);
}







msg_t *makeMsg(int socket, char *line, fd_set *open_sockets, fd_set *readable_sockets, fd_set *writeable_sockets,  hash_table_t *hashtable) {


    msg_t *tmp    = NULL;
    msg_t *my_msg = NULL;

    tmp           = new_msg(line, socket);

    // get name of effect, associate socket with that effect as needed

    if (!namedSock(tmp, open_sockets, readable_sockets, writeable_sockets, hashtable)) 
        free_msg(tmp);
    else 
        my_msg = tmp;

    return my_msg;
}








/* 

  Continusouly read socket into a 1K buffer, process through buffer lookinng for messages. 
  Finds messages that are terminated with CR LF (13 10), or with null (0), or possiblrye both (13 10 0).  
  Message my cross buffers, (ie start at the end of one and end at the begginingn fo the next) - we
  handle that, but only to the length to two buffers (no overruns!).

  Process each incoming message, dividing it up into its components, and either do something in response
  to the message, or collecting timed events from the message, or just... ignoring it (has no purpose or
  destination).

  Typical message looks like:  "B:1:0"
  
    effectName = "B"
    firstMsg   = "1"
    secondMsg  = "0"

    whosTalking will equal the socket that sent the message.
  
*/
event_t *readBuffer(int socket, fd_set *readable_sockets, fd_set *open_sockets, fd_set *writeable_sockets,  hash_table_t *hashtable) {

    event_t *my_events  = NULL;                // list of outgoing events
    msg_t   *my_msg     = NULL;                // pointer to a potential message structure
    int      cur_pos    = 0;                   // current position in the buffer
    int      cur_begin  = 0;                   // position of beginnig of current line
    int      rc         = 1;
    int      partial    = 0;                   // 0 if none, else size of last string

    char buf[BUFLEN+1];                        // +1 guarantees a zero at the end
    char last_string[2*BUFLEN+1];              // +1 guarantees a zero at the end

    memset(last_string,0,2*BUFLEN+1);	       // zero out buffer


    while (rc > 0 || partial) {

        cur_pos       = 0;                  // reset position
        cur_begin     = 0;                  // reset position

        memset(buf,0,BUFLEN+1);	            // clear buffer 

        rc = read(socket, buf, BUFLEN);     // read to length of buffer

        if (rc < 1) { // nothing meaningful to read, closing socket.

            if (DEBUG) { cur_time(); printf("x\tclosing socket:%d, can't read\n",socket);fflush(stdout); }
            closeSK(socket, open_sockets, hashtable);
         
            // at this point fall out of the loop and return my_events

        } 
 
        if (rc > 0 || partial) {  // there's a message of at least 1 char, or a partial

            // Go through buffer and get all messages, assuming null termination

            // NOTE - it seems plausible (but I think unlikely?) that a particular
            // socket could put out so much traffic that it could "lock" the server
            // with incoming traffic.  Probably we should spawn a separate thread
            // for each readable socket?   Not sure we're ever going to see that 
            // level of volume....

            while (cur_pos<BUFLEN) {     // go through buffer

                int str_found = 0;

                // look for a line (ends in CR LF or 13 10)
                while (buf[cur_pos] != 0  && buf[cur_pos] != 10 &&  cur_pos<BUFLEN) {   // look for non-null chars
                    str_found = 1;
                    cur_pos++;
                }

                if (str_found || partial) {    // message found

                    int y;

                    if (cur_pos==BUFLEN && buf[cur_pos-1] > 13) {

                        // non-null and non-return final character of string, eg we're at end of buffer (and no CR LF, so it *may* be a partial message).

                        partial      = cur_pos-cur_begin;                        // save length of partial string

                        for (y=0; y<=partial; y++)
                            last_string[y] = buf[cur_begin+y];

                    } else {

                        // we found a string of character(s), ending either in 0, 10, or 13 (null / end of buffer, CR or LF)
                        // or there's a leftover partial

                        if (cur_pos) {
                            if (buf[cur_pos] <= 13) 
                                buf[cur_pos] = '\0';    // get rid of CR/LF
                            if (buf[cur_pos-1]<=13)     // string may end with CR LF (two characters, both non null) 
                                buf[cur_pos-1] = '\0';  // [cur_pos can't be 0 here if str_found is 1]
                        }


                        if (partial) {             // there's part of string left over from last buffer

                            // HM - I guess in theory last_string could contain a full BUFFLEN, and this could be an nth buffer of data.
                            // In fact - we're not incrementing partial with each buffer, so last_string can only be 2 buffers (and thus message is limited to 2K)

                            // copy string found (if any) to end of last_string and then process message
                            for (y=0; y<cur_pos; y++)
                                last_string[partial+y] = buf[y];

                            partial   = 0;

			} else {
                            for (y=0; y<cur_pos-cur_begin; y++)
                                last_string[y] = buf[cur_begin+y];

                        }

                        if (last_string[0] != 0) {

                            // Process message here....
                            my_msg = makeMsg(socket, last_string, open_sockets, readable_sockets, writeable_sockets, hashtable);

                            // do something with a meaningful message.
                            // could return a timed sequence, which we set along with the start time
                            event_t *new_events;
                            new_events = processMsg(socket, my_msg, open_sockets, hashtable);
                            my_events  = concat_events(my_events,new_events);
                            
                            free_msg(my_msg);
                        }

                        memset(last_string,0,2*BUFLEN+1);	  

                        cur_pos++;                // advance to next position 
                        cur_begin = cur_pos;      // start of next line

                    }

                } else {                          // else buffer is empty, jumpt to end
                    cur_pos = BUFLEN;
                    rc      = 0;
                }

            } // end while < BUFLEN

        } // end if rc or partial

    } // end while there's a buffer or a partial

    return my_events;

} // end readBuffer







/* given an incoming message, do the right thing with that message */
event_t *processMsg(int socket, msg_t *my_msg, fd_set *open_sockets, hash_table_t *hashtable) {

    event_t *timed_sequence;
    timed_sequence = NULL;


    // skip mal-formed messages (no message at all/no identifier, no socket?)
    if (!my_msg || !my_msg->whosTalking)
        return NULL;

    // show message if debugging
    if (DEBUG) {
        cur_time();
        printf("->  EFFECT:%s socket#:%02d msg1:'%s' msg2:'%s' msg3:'%s' msg4:'%s' \n",
	my_msg->effectName,socket,my_msg->firstMsg,my_msg->secondMsg,my_msg->thirdMsg,my_msg->fourthMsg);
        fflush(stdout); 
    }

    // no need to process empty messages or keep alive signals
    if (my_msg->firstMsg == NULL || (my_msg->firstMsg && strcmp(my_msg->firstMsg,KeepAlive)==0))  
        return NULL; 


    if (strcmp(my_msg->firstMsg,CONTROL)==0) {

        // Handle a "control" message.
        timed_sequence = doControl(my_msg, open_sockets, hashtable);

    } else if (strcmp(my_msg->effectName,BUTTON)==0) {

       // Handle a message from the button
        timed_sequence = doButton(my_msg, open_sockets, hashtable);

    } else if (my_msg->firstMsg != NULL) {
    
        /*
           All other messages currently wind up here.
           If the incoming message is from an effect that has a
           collection, we send the firstMsg on to that collection.

           Otherwise, we currently do nothing.
        */

        list_t *self = lookup_effect(hashtable, my_msg->effectName);

        if (self->collection != NULL) 
            send_to_collection(my_msg, self, open_sockets, hashtable);
            
    } // end there's a message


    return timed_sequence;

} // end processMsg




/* get ip address of a socket */
char *getip(int sn) {
    struct sockaddr_in addr;
    socklen_t csa = sizeof(struct sockaddr_in);
    getpeername(sn, (struct sockaddr *)&addr, &csa);
    char *ipstr = "";
    ipstr = strdup(inet_ntoa(addr.sin_addr));
    return ipstr;
}




/* 
    Set the name of the effect from the message, (re)-associate 
    with a particular socket using a hash table as an associative
    array.

    Force close any socket no longer associated with a particular
    effect.
*/
int namedSock (msg_t *my_msg, fd_set *open_sockets, fd_set *readable_sockets, fd_set *writeable_sockets, hash_table_t *hashtable) {

    if (!my_msg || !my_msg->effectName || !my_msg->whosTalking) return 0;

    list_t *anEffect = lookup_effect(hashtable, my_msg->effectName);

    int aSock = 0;
  
    if (anEffect != NULL)
         aSock = anEffect->socket_num;

    if (aSock) { 
 
        // already have a socket for this effect
        
        if (aSock != my_msg->whosTalking) {
 
            // TWO sockets for the same effect.  Either it went offline and came back, or there's
            // a duplicate (two same-named effects on different sockets)
            if (strcmp(getip(aSock), getip(my_msg->whosTalking))==0) {
                forceCloseSK(aSock, open_sockets);
                set_effect_socket(hashtable, my_msg->effectName, my_msg->whosTalking);
                if (DEBUG) {cur_time();  printf("x   RE-CONNECT - FORCE close old socket on name:%s socket:%d, new socket:%d\n", 
                   my_msg->effectName, aSock, my_msg->whosTalking);fflush(stdout); }
            } else {
                // different ips - it's a duplicate effect.
// NOTE - I think we can't have two same name, as hash table lookup craps out.  Could probably append IP to name for unique name.....
                if (DEBUG) {cur_time();  printf("xx  DUPLICATE EFFECT!! name:%s is already on socket:%d. IGNORING THIS EFFECT!\n", 
                   my_msg->effectName, aSock);fflush(stdout); }
                return 0;
            }
        } 
    } else { 
   
        // no socket found for this effect

        // existing effect found, update its socket
        if (anEffect != NULL) {
            set_effect_socket(hashtable,anEffect->effect,my_msg->whosTalking);  // update effect with new socket number
            if (DEBUG) {cur_time();  printf("updating known effect with now known socket#:%d\n",anEffect->socket_num); fflush(stdout); }

        // brand new effect, store it
        } else {
            if (my_msg) 
                add_effect(hashtable, my_msg);    // new effect, add to hash table
        }
    }

    return 1;

} // end namedSock




/* 
    Send messages out.  Confirm socket is open and ready.
    Note that we just close any socket that can't accept messages.
*/
void send_msg (list_t *my_element, fd_set *open_sockets, char *msg, hash_table_t *hashtable) {

    int   sock   = my_element->socket_num;
    char *effect = my_element->effect;

    if (!sock) { return; }
 
    if (FD_ISSET(sock, open_sockets)) { 
        int nBytes = strlen(msg) + 1;
        if (!my_element->do_not_send) {
            int bsent = send(sock, msg, nBytes, MSG_NOSIGNAL);
            if (DEBUG) { cur_time(); printf("<-  sent message:'%s' to %10s on socket:%02d  bytes sent:%d\n",msg,effect,sock,bsent); fflush(stdout); }
        } else {
            if (DEBUG) { cur_time(); printf("xx  skipped message:'%s' to %10s on socket:%02d (dns set)\n",msg,effect,sock); fflush(stdout); }
        }
    } else { // close any unavailable socket
        closeSK(sock, open_sockets, hashtable);
        if (DEBUG) { cur_time(); printf("x   closing socket:%d, can't write\n",sock);fflush(stdout); }
    }
}




/*     Send message to all active sockets (self is ignored).   */
void send_all(int whosTalking, fd_set *open_sockets, hash_table_t *hashtable, char *my_msg) {
    msg_list(whosTalking, get_all_nodes(hashtable), open_sockets, hashtable, my_msg);
}




/* process incoming control message */
event_t *doControl(msg_t *my_msg, fd_set *open_sockets, hash_table_t *hashtable) {

    event_t *timed_sequence;
    timed_sequence = NULL;

    if (strcmp(my_msg->secondMsg,XX)==0) {         // kill all!
// need to set / free kill state?
// send kill all message
// free event list if any (here?  or in main loop better?
    } else if (strcmp(my_msg->secondMsg,RO)==0) {  // setting round order
        set_list_order(hashtable, my_msg);
    } else if (strcmp(my_msg->secondMsg,CC)==0) {  // creating a collection
        set_collection(hashtable, my_msg);
    } else if (strcmp(my_msg->secondMsg,DS)==0) {  // don't-send flag
        set_effect_ds(hashtable, my_msg->effectName, my_msg->thirdMsg);
    } else {
        // more eventually....

        // look for incoming timed sequence and parse here?
    }

    return timed_sequence;
}







/* EVENT ROUTINES  */

// I NOTE there are event routines in the client which are similar, but not the same.
// Seems like it could be one library.

/*     create a new single-event list  */
event_t *new_event(char *str, long begin, int length, list_t *collection) {
    event_t *new_event; 

    // allocate memory 
    if ((new_event  = (event_t *)malloc(sizeof(event_t))) == NULL) return NULL;

    // Populate data
    new_event->action       = strdup(str);          // explicity copy original into memory 
    new_event->begin        = begin;
    new_event->length       = length;
    new_event->started      = 0;

    // linked lists
    new_event->next         = NULL;
    new_event->collection   = collection;

    return new_event;
}




/* check all events in the current timed sequence */
event_t *check_events(event_t *events, long seq_start, fd_set *open_sockets, hash_table_t *hashtable) {

    if (events == NULL) return NULL;

    long     now = millis();

    list_t  *this_node;
    event_t *this_event, *last_event, *drop_event;

    this_event = events;
    last_event = NULL;
    drop_event = NULL;

    while (this_event!= NULL && this_event->action) {

        if ( now > (seq_start + this_event->begin) && !this_event->started ) {

            // begin action
            this_node = this_event->collection;
            while (this_node != NULL) {
                if (strcmp(this_event->action,"poof") == 0) {
                    send_msg(this_node, open_sockets, PoofON, hashtable);
                }
                this_node = this_node->next;
            }
            this_event->started = 1;

        } else if ( now > (seq_start + this_event->begin + this_event->length) ) {

            // complete action
            this_node = this_event->collection;
            while (this_node != NULL) {
                if (strcmp(this_event->action,"poof") == 0) {
                    send_msg(this_node, open_sockets, PoofOFF, hashtable);
                }
                this_node = this_node->next;
            }

            // drop this event from the list
            if (last_event != NULL) {
                last_event->next = this_event->next;
            } else {
                events = this_event->next;
            }
            drop_event = this_event;
        }


        // drop completed events from the list
        if (drop_event != NULL) {
            this_event = this_event->next;
            drop_event = NULL;
            free_event(drop_event);
        } else {
            last_event = this_event;
            this_event = this_event->next;
        }
 
    }
    
    // returns an ever smaller list, eventually null
    return events;
}




/*
    appends second eventlist to first by iterating through the first.
    thus, faster if the first event list is the short one.
*/
event_t *concat_events(event_t *first_event, event_t *second_event) { 

    event_t *current = NULL;

    if (!first_event)  return second_event;
    if (!second_event) return first_event;
    if (first_event == second_event) return first_event;

    for (current=first_event; current->next !=NULL; current=current->next);

    current->next = second_event;

    return first_event;
}



/*

not actually working....

event_t *parse_events(char *str){

  

    event_t *events;
    events = NULL;
    char *action;
    char *effect[12];
    long begin, length;
    int  sub_effect[24];


//#define MSG_DIV3    	";"     // basic messaging sub-divider
//#define MSG_DIV4    	"+"     // basic messaging sub-divider
//#define MSG_DIV5    	"->"    // basic messaging sub-divider
//#define MSG_DIV6    	"&"    // basic messaging sub-divider

    //  CONTROL:*:EV:poof,0,2000,DRAGON;poof,1000,2000,ENTRYWAY&ENTRYWAY2
    //  str = poof,0,2000,DRAGON;poof,1000,2000,ENTRYWAY&ENTRYWAY2
    //  str = poof,400,800,ORGAN->3+4;poof,800,800,ORGAN->5+6;poof,1200,800,ORGAN->7+8;poof,1600,800,ORGAN->9+10;poof,2000,800,ORGAN->11+12;
    char *part = clean_str_part(strtok(str,MSG_DIV3));

    while (part != NULL) {

    //  part = "poof,0,800,ORGAN->1+2;"
    //  part = "poof,0,800,ENTRYWAY&AERIAL;"
    //  part = "poof,0,800,ORGAN->3&AERIAL;"

        char *action = clean_str_part(strtok(part,MSG_DIV2));

        // HM.  need naive_str2long...

        begin  = (long)naive_str2int(clean_str_part(strtok(NULL,MSG_DIV2)));
        length = (long)naive_str2int(clean_str_part(strtok(NULL,MSG_DIV2)));

        char *collection = clean_str_part(strtok(NULL,MSG_DIV2));
    
        //  collection = "ORGAN->1+2&ENTRYWAY"

        char *effect = clean_str_part(strtok(collection,MSG_DIV6));
        while (effect != NULL) {

            //  effect = "ORGAN->1+2"
            char *name = clean_str_part(strtok(effect,MSG_DIV5));
            char *subs = clean_str_part(strtok(NULL,MSG_DIV5));

            effect = clean_str_part(strtok(NULL,MSG_DIV6));
        }

// NOT sure this is still valid?  in fact, I seriously doubt it.
        part = clean_str_part(strtok(NULL,MSG_DIV3));
    }


    return events;
}
*/








/* BUTTON ROUTINES  */


/* process incoming message from the button */
event_t *doButton(msg_t *my_msg, fd_set *open_sockets, hash_table_t *hashtable) {

    event_t *timed_sequence;
    timed_sequence = NULL;

    int	which_but  = 0; 	// number of button currently pushed/released
    int butstate   = 0;		// 1 if pressed, 0 if released

    if (my_msg->firstMsg != NULL) {
        which_but = naive_str2int(my_msg->firstMsg);
        if (my_msg->secondMsg != NULL) {
            butstate = naive_str2int(my_msg->secondMsg);
        }
    }

    if (which_but) {
        if (which_but==1) {                         
            // THE button
            char *p_msg = PoofOFF;
            if (butstate==1) p_msg=PoofON;
            send_all(my_msg->whosTalking, open_sockets, hashtable, p_msg);
        } else if (which_but==2 && butstate==1) {
            // do a round
            timed_sequence = bigRound(my_msg->whosTalking, open_sockets, hashtable);
        } else if (which_but==3 && butstate==1) {   
            // send poofstorm!
            send_all(my_msg->whosTalking, open_sockets, hashtable, PoofSTM);
        }
    }

    return timed_sequence;
}




/* The following all generate (and return) timed sequences, and are non-blocking */


/*  Go around in a "circle" several times, faster, then big finish */
event_t *bigRound(int whosTalking, fd_set *open_sockets, hash_table_t *hashtable) {

    if (DEBUG) { cur_time(); printf("\tLet's Have a big ROUND!!\n");  fflush(stdout); }

    list_t *all_elements =  get_ordered_list(hashtable);
    int poof_time = ROpoofLength;

    event_t *round, *big_round;
    long  cur_start = 0L;

    round     = roundRobbin(whosTalking, all_elements, poof_time, 240, &cur_start);
    big_round = round; 
    cur_start += 130;
    round     = roundRobbin(whosTalking, all_elements, poof_time, 130, &cur_start);
    big_round = concat_events(big_round,round); 
    cur_start += 80;
    round     = roundRobbin(whosTalking, all_elements, poof_time, 80, &cur_start);
    big_round = concat_events(big_round,round); 
    cur_start += 50;
    round     = roundRobbin(whosTalking, all_elements, poof_time, 50, &cur_start);
    big_round = concat_events(big_round,round); 
    cur_start += 40;
    round     = roundRobbin(whosTalking, all_elements, poof_time, 40, &cur_start);
    big_round = concat_events(big_round,round); 
    cur_start += 30;
    round     = roundRobbin(whosTalking, all_elements, poof_time, 30, &cur_start);
    big_round = concat_events(big_round,round); 
    cur_start += 30;
    round     = roundRobbin(whosTalking, all_elements, poof_time, 30, &cur_start);
    big_round = concat_events(big_round,round); 
    cur_start += 30;

    list_t *eff;
    eff = lookup_effect(hashtable, LULU);

    if (eff != NULL) {
        round     = lulu_poof(eff, &cur_start);
        big_round = concat_events(big_round,round); 
        cur_start += 30;
    }

    eff = lookup_effect(hashtable, BIGBETTY);
    if (eff != NULL) {
        round = new_event("poof", cur_start, 5000L, copy_node(eff) );
        big_round = concat_events(big_round,round); 
    }

    return big_round;
}




/* 
    DO A ROUND

    poof the effects in a given order, for a length of time, with a particular delay 
*/
event_t *roundRobbin(int whosTalking, list_t *all_elements, int the_poof, int the_wait, long *start_time) {

    event_t *my_events, *cur_event;

    int      my_sock   = 0;
    long     my_start  = *start_time;

    my_events          = NULL;

    // send message to everyone on the list expcept the originating socket
    while (all_elements!= NULL) {

        my_sock = all_elements->socket_num;
        if (my_sock != whosTalking) {
            event_t *event = new_event("poof", my_start, (long)the_poof, copy_node(all_elements) );
            if (my_events ==  NULL) {
                my_events = event;
                cur_event = my_events;
            } else {
                cur_event->next = event;
                cur_event = event;
            }
            my_start = my_start + (long)the_poof + (long)the_wait;
        }

        all_elements = all_elements->next;
    }

    *start_time = my_start;
    return my_events;
}




event_t *lulu_poof(list_t *my_element, long *start_time) {

    if (my_element == NULL) return NULL;

    event_t   *my_events, *cur_event;
    my_events  = NULL;
    long     my_start  = *start_time;


    int mySock = my_element->socket_num;

    if (!mySock) { return NULL; }

    int xx;
    for (xx=0; xx<4; xx++) {

        event_t *event = new_event("poof", my_start, 80L, copy_node(my_element) );
        if (my_events ==  NULL) {
            my_events = event;
            cur_event = my_events;
        } else {
            cur_event->next = event;
            cur_event = event;
        }
        my_start += 80L + 50L;

    }

    event_t *event = new_event("poof", my_start+500L, 1600L, copy_node(my_element) );
    cur_event->next = event;
    cur_event = event;
    my_start += 500L + 1600L;

    *start_time = my_start;
    return my_events;
}








/******  UTILITIES **********/


/*  error - wrapper for perror */
void error(char *msg) {
	perror(msg);
	exit(1);
}




/* set and keep the current time since process start, in millisenconds */
long millis(void) {

    float           ms; // Milliseconds
    time_t          s;  // Seconds
    struct timespec spec;

    clock_gettime(CLOCK_REALTIME, &spec);

    s  = spec.tv_sec;
    ms = spec.tv_nsec / 1.0e6; // Convert nanoseconds to milliseconds
  
    current_micros = (ms-(int)ms)*1000;

    /*printf("Current time: %"PRIdMAX".%03ld seconds since the Epoch.  ms:%ld\n",
           (intmax_t)s, ms, ms);*/
    if (!start_time) {
        start_time   = ms + s*1000;
        start_micros = current_micros;
        return 0L;
    } 

    current_time = ms + s*1000 + current_micros/1000 - start_time;
       
    return current_time;
}




/* prints the time , mostly for debugging*/
void cur_time (void) {

    millis();
    int h  = current_time/(3600000);
    int m  = (current_time - h*3600000)/60000;
    int s  = (current_time - h*3600000 - m*60000)/1000;
    int ms = (current_time - h*3600000 - m*60000 - s*1000);
    
    printf("%02d:%02d:%02d.%03d%03d  ", h,m,s,ms,current_micros);
}




/* delays some number of milliseconds  */
void delay (unsigned int howLong) {

  struct timespec sleeper, dummy ;

  sleeper.tv_sec  = (time_t)(howLong / 1000) ;
  sleeper.tv_nsec = (long)(howLong % 1000) * 1000000 ;

  nanosleep (&sleeper, &dummy) ;
}



/* delays some number of microseconds  */
void delay_micro (unsigned int howLong) {

  struct timespec sleeper, dummy ;

  sleeper.tv_sec  = (time_t)(howLong / 1000000) ;
  sleeper.tv_nsec = (long)(howLong % 1000000) * 1000000 ;

  nanosleep (&sleeper, &dummy) ;
}



/* 
    Turns a string into an integer by ASSUMING it's 

	- non-negative
	- contains only integer characters
	- does not exceed integer range
*/
int naive_str2int (const char* snum) {

    const int NUMLEN = (int)strlen(snum);
    int i,accum=0;
    for(i=0; i<NUMLEN; i++) {
        accum = 10*accum;
	accum += (snum[i]-0x30);
    }
    return accum;
}






/************** LISTS & COLLECTIONS ROUTINES ************/

/*

    Various routines for dealing with linked lists.  In all cases
    we're using lists of list_t pointers, i.e. lists of effects.

    A collection is a "broadcast list" -- when a particular effect
    has a collection, and sends a message to the server, it's 
    passed on to that collection.  Collections are also linked lists
    of effects, but have a modification time, and so are kept up to
    date with respect to the primary list of effects. 

*/




/* Send out *msg to a given socket list, skip the sending socket */
void msg_list(int whosTalking, list_t *all_effects, fd_set *open_sockets, hash_table_t *hashtable, char *msg) {

    while (all_effects != NULL) {

        if (all_effects->socket_num != whosTalking) 
            send_msg(all_effects, open_sockets, msg, hashtable);
        
        all_effects = all_effects->next;
    }
}




/*    Send a message to a collection - a set of effects to which a given effect broadcasts */
void send_to_collection(msg_t *my_msg, list_t *self, fd_set *open_sockets, hash_table_t *hashtable) {

    if (self->c_mod != hashtable->modified)
        update_collection(hashtable, self);

    msg_list(self->socket_num, self->collection, open_sockets, hashtable, my_msg->firstMsg);
}




/* set the broadcast collection for an effect */
void set_collection(hash_table_t *hashtable, msg_t *my_msg) {

    list_t  *self;
    list_t  *collection;
    char    *str = my_msg->thirdMsg;

    collection       = get_list(hashtable, str); 
    self             = lookup_effect(hashtable, my_msg->effectName);
    self->collection = collection;

    self->c_mod      = hashtable->modified;
}




/* Send out msg to a given socket list */
void update_collection(hash_table_t *hashtable, list_t *self) {
    list_t *position, *tmp;
    position = self->collection;

    // send message to everyone on the list expcept the originating socket
    while (position != NULL) {
        tmp = lookup_effect(hashtable,position->effect);
        if (tmp != NULL) // copy all data if existing effect
            update_node(tmp,position);
        position = position->next;
    }

    self->c_mod = hashtable->modified;
}




/*     search for element on linked list.  */
int not_on_list(list_t *in_list, list_t *element) { 

    list_t *list; 

    for (list = in_list; list != NULL; list = list->next)  
        if (strcmp(list->effect,element->effect) == 0) 
            return 0; // it's on the list 
    
    return 1; // it's not on the list 
}




/*    allocates memory and copies a linked list, node by node   */
list_t *copy_list(list_t *list) { 

    list_t *new_element = NULL;
    list_t *new_list    = NULL;
    list_t *current     = NULL;

    while (list != NULL) {
        new_element = copy_node(list);

        if (!new_list) {
            new_list = new_element;
            current  = new_list;
        } else {
            current->next = new_element;
            current       = current->next;
        }

        list = list->next;
    }

    return new_list;
}


/*
    appends second list to first by iterating through the first.
    thus, faster if the first list is the short one.
*/
list_t *concat_lists(list_t *first_list, list_t *second_list) { 

    list_t *current = NULL;

    if (!first_list)  return second_list;
    if (!second_list) return first_list;
    if (first_list == second_list) return first_list;

    for (current=first_list; current->next !=NULL; current=current->next);

    current->next = second_list;

    return first_list;
}




/* given a message 'effect:*:RO:[an ordered list of effects]'
   set the internal ordered list to this order.  use existing
   socket numbers where found.  */
void set_list_order(hash_table_t *hashtable, msg_t *input) {

    list_t *ordered_list   = get_list(hashtable, input->thirdMsg); 
    if (hashtable->ordered != NULL) 
        free_node_list(hashtable->ordered);  // free up old list, if any
    hashtable->ordered     = ordered_list;
    hashtable->modified    = millis();
    hashtable->ordered_set = hashtable->modified;
}



/* given a string "a,b,c", return ordered list of nodes */
list_t *get_list(hash_table_t *hashtable, char *input) {

    list_t *ordered_list = NULL;
    list_t *position     = NULL;
    list_t *effect;
    
    char *part = copy_str_part(strtok(input,MSG_DIV2));

    while (part != NULL) {

        effect = lookup_effect(hashtable, part);

        list_t *new_list;

        if (effect != NULL)
            new_list = copy_node(effect);
        else
            new_list = new_node(part, 0); 

        if (position == NULL) {
            position     = new_list;
            ordered_list = position;
        } else {
            position->next = new_list;
            position = position->next;
        }
        part = copy_str_part(strtok(NULL,MSG_DIV2));
    }

    return ordered_list; 
}




/*
   return ordered list for round.  by default, it will
   be the order in which they connected. 
   if for some reason the default list is empty,
   we get the list in an arbitrary order.  
   order can also be set through a control message above.

   if the odered list modification date is different
   than the table modification date, we re-set the
   socket numbers in the ordered list.
*/
list_t *get_ordered_list(hash_table_t *hashtable) { 

    list_t *position, *tmp;

    if (hashtable==NULL) return NULL;  
    if (hash_table_count(hashtable) < 1) return NULL;  

    if (hashtable->ordered == NULL) {
        hashtable->ordered     = get_all_nodes(hashtable);
        hashtable->ordered_set = hashtable->all_set;
    }

    if (hashtable->ordered_set != hashtable->modified) {
        for (position = hashtable->ordered; position != NULL; position = position->next) {
            tmp = lookup_effect(hashtable,position->effect);
            if (tmp != NULL) // copy all data if existing effect
                update_node(lookup_effect(hashtable,position->effect),position);
        }

        hashtable->ordered_set = hashtable->modified;
    }

    return hashtable->ordered;
}







/*********    HASH ROUTINES *************/

/*

    These routines implement a "simple" hash table.  Why not just
    use glibhash?  Primarily to optimize around expected functionality.
    We basically want an associative array for fast lookup, but don't
    really expect that array to change very often after starting up,
    nor to be especially large (currently imagine dozens of entries at 
    most). 
 
    Essentially, we expect some number of "effects" to connect to the
    server, and in an ideal world, to stay "static" - i.e. to remain
    on the same socket, open and available.  Using a hash gives us
    fast access when needed.

    FWIW, I thought the simplest example I found on the interwebs
    would be sufficient, and then learned way more than I wanted to
    know, and, well, here we are.  - NV    

*/



/*   allocate memory and set initial values for primary array of hash table */
hash_table_t *create_hash_table(int size) { 

    int i;

    hash_table_t *new_table; 
 
    if (size<1) return NULL; 		// invalid size for table

    /* Attempt to allocate memory for the table structure */ 
    if ((new_table          = malloc(sizeof(hash_table_t))) == NULL) { return NULL; }  
    if ((new_table->table   = malloc(sizeof(list_t *) * size)) == NULL) { return NULL; }
    if ((new_table->all     = malloc(sizeof(list_t *))) == NULL) { return NULL; }
    if ((new_table->ordered = malloc(sizeof(list_t *))) == NULL) { return NULL; }

    /* Initialize the elements of the table */ 
    for(i=0; i<size; i++) new_table->table[i] = NULL;  

    /* Set the table's size, number of elements, mod time */ 
    new_table->size          = size;  
    new_table->numOfElements = 0;  
    new_table->all           = NULL;
    new_table->ordered       = NULL;
    new_table->modified      = millis();  
    new_table->all_set       = 0L;  
    new_table->ordered_set   = 0L;  

    return new_table; 
} 




/*

    djb2 hash with the mysterious 33
    we multiply initial prime by 33 (we shift left 5 times and add once,
    more efficient than multiplication), then turn the string into
    an integer by adding its char values.  No one knows why '33'
    seems to work so well.

*/
unsigned long hash (hash_table_t *hashtable, char *str) { 
    unsigned long hash = 5381;
    int c;
    while ((c = *str++) != '\0')
        hash = ((hash<<5) + hash) + c; /* hash * 33 + c */

    return hash % hashtable->size; 
}




/*     find effect given name, using hash.  */
list_t *lookup_effect(hash_table_t *hashtable, char *str) { 

    list_t *list; 

    unsigned long hashval = hash(hashtable, str);  

    /* Go to the correct list based on the hash value and see if str is 
    * in the list. If it is, return return a pointer to the list element. 
    * If it isn't, the item isn't in the table, so return NULL. */ 

    list = hashtable->table[hashval]; 
    while (list && list->effect) {
        if (strcmp(str, list->effect) == 0) 
            return list; 
        list = list->next;
    }
     
    return NULL; 
}




/*
     look up effect by its socket number.

     finding by socket number is just brute force iteration -
     unless we create *two* hash tables - a lot of overhead.
*/
list_t *lookup_effect_sk(hash_table_t *hashtable, int sock_num) { 

    int     i;
    list_t *list; 

    /* Go to the correct list based on the hash value and see if str is 
    * in the list. If it is, return return a pointer to the list element. 
    * If it isn't, the item isn't in the table, so return NULL. */ 
    for(i=0; i<hashtable->size; i++) 
        for (list = hashtable->table[i]; list != NULL; list = list->next)  
            if (sock_num == list->socket_num)
                return list; 
     
    return NULL; 
}




/*    
      Add a named effect to the hash table. 

      We skip looking it up - we do that before we get here.
 */
int add_effect(hash_table_t *hashtable, msg_t *my_msg) { 

    long   temp;
    list_t *new_element   = NULL; 
    list_t *new_element2  = NULL; 

    unsigned long hashval = hash(hashtable, my_msg->effectName);  

    new_element = new_node(my_msg->effectName, my_msg->whosTalking);
    if (!new_element) return 1;  		// insert failed 
    
    if (DEBUG) {
        printf("\n       >>>>>>>>>> ADDING EFFECT '%s' on socket:%d\n\n",my_msg->effectName,my_msg->whosTalking); 
        fflush(stdout);
    }

    new_element2 = copy_node(new_element);


    // append new element to current ordered list if not there already
    if (!hashtable->ordered)
        hashtable->ordered = new_element2;
    else if (not_on_list(hashtable->ordered,new_element2))
        hashtable->ordered = concat_lists(hashtable->ordered,new_element2); 

    // prepend to current hash table bucket
    new_element->next         = hashtable->table[hashval]; 
    hashtable->table[hashval] = new_element;
 
    temp                      = hashtable->modified;
    hashtable->modified       = millis();
  
    // ordered set is still current if it was previuosly current  
    if (hashtable->ordered_set == temp)
        hashtable->ordered_set = hashtable->modified;
   
    hashtable->numOfElements++;

    return 0; 
} 




/*     create a new single-element list (i.e. a node)   */
list_t *new_node(char* str, int sock) {
    list_t *new_list; 

    // allocate memory for new node
    if ((new_list  = (list_t *)malloc(sizeof(list_t))) == NULL) return NULL;

    // Populate data
    new_list->effect          = strdup(str);          // explicity copy original into memory 
    new_list->socket_num      = sock;
    new_list->do_not_send     = 0;
    new_list->c_mod           = 0L;

    // linked lists
    new_list->next            = NULL;
    new_list->collection      = NULL;

    return new_list;
}




/*
     Copy all the -data- in the node, but not the linked lists.
     So, this node will function the same (same socket number, same
     properties), but be included on a different list.
*/
list_t *copy_node(list_t *a_node) {

    if (a_node == NULL) return NULL;

    list_t *new_list; 

    new_list = new_node(a_node->effect, a_node->socket_num);

    new_list->do_not_send = a_node->do_not_send;

    return new_list;
}




/*   update a node's *data* */
void update_node(list_t *a_node, list_t *new_list) {

    if (new_list->effect != NULL) free(new_list->effect);
    new_list->effect          = strdup(a_node->effect);

    new_list->socket_num      = a_node->socket_num; 
    new_list->do_not_send     = a_node->do_not_send;
 
    // we're going to presume since the *data* is this
    // current, this new node is also this current.                 
    new_list->c_mod           = a_node->c_mod;                 
}




/*    (re)-set the socket number for a given effect (hash fast)  */
int set_effect_socket(hash_table_t *hashtable, char *str, int sock) { 

    list_t *current_list; 

    current_list = lookup_effect(hashtable, str); 

    /* item doesn't exist */
    if (current_list == NULL) return 1; 

    current_list->socket_num = sock;
    hashtable->modified      = millis();

    return 0; 
} 




/*    (re)-set socket number for an effect given original socket number   (linear fast)  */
int set_effect_socket_sk(hash_table_t *hashtable, int sock, int val) {

    list_t *current_list; 

    current_list = lookup_effect_sk(hashtable, sock); 

    /* item doesn't exist */
    if (current_list == NULL) return 1; 

    current_list->socket_num = val;
    hashtable->modified      = millis();

    return 0; 
} 




/*    (re)-set the socket number for a given effect (hash fast)  */
void set_effect_ds(hash_table_t *hashtable, char *str, char *msg) { 

    list_t *current_list; 

    current_list = lookup_effect(hashtable, str); 

    /* item doesn't exist */
    if (current_list == NULL) return; 

    current_list->do_not_send = naive_str2int(msg);
    hashtable->modified       = millis();
} 




/*    get a socket number for a given effect  */
int get_socket(hash_table_t *hashtable, char *str) {

    list_t *effect = lookup_effect(hashtable, str);
    if (effect) 
        return effect->socket_num;

    return 0;
}



int hash_table_count(hash_table_t *hashtable) {
    return hashtable->numOfElements;
}




/*
    gets all the nodes in the hash table in a new linked list.
    order is irrelevant.

    given the infrequency of hash table change, and the frequency
    we'll want this list, we keep it around, and store the table
    and this list's modification times, so we can just return
    the prior collection if no changes have occured since we 
    last created it.  faster given our usage.
*/
list_t *get_all_nodes(hash_table_t *hashtable) { 

    list_t *list     = NULL;   	// points at table position
    list_t *outlist  = NULL;	// points at new output list
    list_t *new_list = NULL;    // new node

    int     i;

    // error check to make sure hashtable exists and has at least one element
    if (hashtable==NULL) return NULL;  
    if (hash_table_count(hashtable) < 1) return NULL;  

    // if we already generated this list and the
    // table hasn't changed in any way, return the list
    if (hashtable->all) 
        if (hashtable->modified == hashtable->all_set) 
            return hashtable->all;

    // go through every index and concatenate all list elements in each index into a new list 
    for(i=0; i<hashtable->size; i++) { 

        list=hashtable->table[i];			// each list from each table 

        if (list != NULL) {				// if there's a list

            new_list = copy_list(list);			// copy the list

            if (outlist == NULL) {
                outlist = new_list;
            } else {
                outlist = concat_lists(new_list, outlist);
            }
        }
    }  

    free_node_list(hashtable->all);

    hashtable->modified = millis();
    hashtable->all_set  = hashtable->modified;
    hashtable->all      = outlist;

    return outlist; 
}







/*      FREE MEMORY ROUTINES  */


/*    frees all memory used by a message   */
void free_msg(msg_t *msg) {

    if (msg == NULL) return;

    free(msg->effectName); 
    free(msg->firstMsg); 
    free(msg->secondMsg); 
    free(msg->thirdMsg); 
    free(msg->fourthMsg); 
    free(msg); 
}




/*    frees all memory taken by the table  */
void free_table(hash_table_t *hashtable) { 

    int i; 
    list_t *list;

    if (hashtable == NULL) return;  

    /* Free the memory for every item in the table, including the  
    * effects themselves. */ 

    for(i=0; i<hashtable->size; i++) { 
        list = hashtable->table[i]; 
        free_node_list(list);
    }  

    /* Free the table itself */ 
    free(hashtable->table); 
    free_node_list(hashtable->all); 
    free_node_list(hashtable->ordered); 
    free(hashtable); 
}




/*    frees all memory used by a linked list of nodes    */
void free_node_list(list_t *head) {
    list_t *pos, *temp;
    pos = head;
    while(pos!=NULL) { 
        temp = pos; 
        pos  = pos->next; 
        free_node(temp);
    } 
}




/*      frees all memory for a given node   */
void free_node(list_t *node) {
    free(node->effect);
    free_node_list(node->collection); 
    free(node); 
}




/*    frees all memory used by a linked list of events    */
void free_event_list(event_t *head) {
    event_t *pos, *temp;
    pos = head;
    while(pos!=NULL) { 
        temp = pos; 
        pos  = pos->next; 
        free_event(temp);
    } 
}




/*      frees all memory for a given event   */
void free_event(event_t *event) {
    free(event->action);
    free_node_list(event->collection); 
    free(event); 
}
