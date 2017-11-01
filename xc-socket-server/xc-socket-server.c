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

// 1) attempt to add clock pulse and sync 8266's
               
   This is intended as a FAST multi-socket select() server,
   running on a raspberry pi 3 (with wifi), in
   conjunction with the Arduino Adafruit Huzzah (the ESP2866
   wifi enabled board) to create a responsive, wirelessly
   distributed microcontroller network.  We're using it to
   link a series of flame-enabled games, and other effects,
   and trying to limit latency to <2ms throughout.


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
   causing any messaging delays (it's in the huzzah code).

   In addition, the clients are presumed to be sending a keep alive
   signal, and the server presumes to keep the sockets open to minimize
   delay.  The various lists and collections are kept up to date with
   modification times, so once all the effects are online, far fewer
   "lookups" are required.

   NOTES:

   TO send messages from one socket to another, you must "set a collection"
   to which a given effect will broadcast.  Note that "the button" (currently
   effect: 'B') sends messages to *all* effects.  Set a collection by:

      effectname:*:CC:effectname1,effectname2...

   This can be done at any time, whether other effects are present are not, 
   the collection is kept up to date with existing effects.

   We're using the RPi 3, and have configured it as
   an access point with a static IP. 

   When DEBUG=1, the idea is that you would run this at the command
   prompt, and all debugging info is dropped into STDOUT.  When DEBUG is 0,
   the assumption is no output, and it will run as a daemon. 

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


// PRE-DEFINED OUTGOING MESSAGES
#define MSG_DIV    	":"     // basic messaging divider
#define MSG_DIV2    	","     // basic messaging sub-divider
#define PoofON 		"$p1%"
#define PoofOFF		"$p0%"
#define PoofSTM		"$p2%"


// VARIOUS INCOMING MESSAGES & KNOWN EFFECTS
#define KeepAlive       "KA"
#define CONTROL         "*"     // special control message
#define RO       	"RO"    // ctl msg - set round order
#define CC       	"CC"    // ctl msg - set collection
#define DS       	"DS"    // ctl msg - set do_not_send flag

#define BUTTON		"B"
#define BIGBETTY        "BIGBETTY"
#define LULU	        "LULU"




/************** OUR VARIABLES    ******************/


// hash structure for associative array that will store key / value pairs
typedef struct _list_t_ {
    char   *effect;                     // name of the effect
    int     socket_num;                 // socket this effect is on
    int     do_not_send;                // true if broadcast-only effect
    long    c_mod;                      // last update of collection, or 0L 
    struct _list_t_ *next;              // next effect in the hashtable bucket
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
} msg_t;



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
msg_t  *new_msg();
char   *test_part();
msg_t  *readMsg();
void    processMsg();
void    send_msg ();
void    send_all();
void    doControl();

// buttons and poofing
void    doButton();
void    theButton();
void    bigRound();
void    roundRobbin();
void    poofStorm();
void    lulu_poof();
void    bigbetty_poof();

// utilities
void    error();
void    cur_time();
long    millis();
int     naive_str2int ();
void    delay();

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
    int            rc;
    int		   i;				// index counters for loop operations
    fd_set	   open_sockets;   		// set of open sockets
    fd_set	   readable_sockets; 		// set of sockets available to be read
    fd_set	   writeable_sockets; 		// set of sockets available for writing 
    char           *ipadd;

    hash_table_t   *my_hash_table;		// hash for holding effect names, sockets, properties

    msg_t          *my_msg;	 		// struct for holding incoming message components

    int            size_of_table 
                     = (int)maxclients/2;       // theorectically, a max of 2 per bucket, ideal.

// NOT right, sockeet numbers aren't 0..maxclients!!
// also, plug unplug, still @#$@#$ seg-faulting.
//    char          *ips[maxclients];

//    for(i=0; i<maxclients; i++) ips[i] = NULL;  

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
        rc = select(max_socket+1, &readable_sockets, &writeable_sockets, NULL, (struct timeval *)NULL);

        // return from select - add incoming sockets to pool 
        ipadd = "";
        //int sk = acceptSK(firstSocket, readable_sockets, &open_sockets, &ipadd);
        acceptSK(firstSocket, readable_sockets, &open_sockets, &ipadd);

        // seemingly no way to determine *which* socket is readable -
        // we go through all of them until we've processed on as
        // many sockets as select() told us were available.
	for (i=firstSocket+1; i<max_socket+1 && rc; i++)  {

            // read incoming, set named effect, get socket number
            if (FD_ISSET(i, &readable_sockets)) {
                my_msg = readMsg(i, &readable_sockets, &open_sockets, &writeable_sockets, my_hash_table);
                rc--;  // found a socket, decrement the count
            } else
                continue;

            // skip mal-formed messages (no message at all/no identifier, no socket?)
            if (!my_msg || !my_msg->whosTalking)
                continue;

            // show message if debugging
            if (DEBUG) {
                cur_time();
                printf("->  %s on socket#:%02d firstMsg:'%s' secondMsg:'%s'\n",
		my_msg->effectName,i,my_msg->firstMsg,my_msg->secondMsg);
		fflush(stdout); }

            // no need to process empty messages or keep alive signals
            if (my_msg->firstMsg == NULL || (my_msg->firstMsg && strcmp(my_msg->firstMsg,KeepAlive)==0))  
                continue; 

            // do something with a meaningful message
            processMsg(my_msg, &open_sockets, my_hash_table);

            free_msg(my_msg);

            // only read from as many sockets as select says have a message
            rc--;
            if (rc == 0) i=max_socket+1;
        } 
    } 

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
    part = test_part(strtok(str,MSG_DIV));
    newmsg->effectName = part;

    if (part != NULL) 
      newmsg->firstMsg = test_part(strtok(NULL,MSG_DIV));

    if (newmsg->firstMsg != NULL) 
      newmsg->secondMsg = test_part(strtok(NULL,MSG_DIV));
    else 
      newmsg->secondMsg = NULL;

    if (newmsg->secondMsg!= NULL) 
      newmsg->thirdMsg = test_part(strtok(NULL,MSG_DIV));
    else 
      newmsg->thirdMsg = NULL;

    if (newmsg->thirdMsg != NULL) 
      newmsg->fourthMsg = test_part(strtok(NULL,MSG_DIV));
    else 
      newmsg->fourthMsg = NULL;

    newmsg->whosTalking = sock;

    return newmsg;
}




/*     return a copy of the string, if any, less trailing escape characters    */
char *test_part(char *str) {
    if      (str == NULL) return NULL;
    else if (!strlen(str)) return NULL;

    int i = 0;
    int j = strlen(str);

    while (i<j) {
        if(str[i]<32) 
            str[i] = '\0';
        i++;
    }

    return strdup(str);
}




/* 
  read incoming message into approriate strings.  

  message looks like:  "B:1:0"
  
    effectName = "B"
    firstMsg   = "1"
    secondMsg  = "0"

    whosTalking will equal the socket that sent the message.
  
*/
msg_t *readMsg(int socket, fd_set *readable_sockets, fd_set *open_sockets, fd_set *writeable_sockets,  hash_table_t *hashtable) {

    msg_t *my_msg = NULL;
    int    received  = 0;
    char   buf[BUFLEN+1]; 

    memset(buf,0,BUFLEN);	        // clear buffer 
    int rc = read(socket, buf, BUFLEN);	// read from the socket 

    if (rc < 1) {
        if (DEBUG) { cur_time(); printf("x\tclosing socket:%d, can't read\n",socket);fflush(stdout); }
        closeSK(socket, open_sockets, hashtable);
    } else if (buf[0] < 32) {
        // skip escaped characters 
        return NULL;
    } else {
        received = socket;
        my_msg   = new_msg(buf, received);

        // get name of effect, associate socket with that effect as needed
        if (!namedSock(my_msg, open_sockets, readable_sockets, writeable_sockets, hashtable)) 
           return NULL;
    }

    return my_msg;
}




/* given an incoming message, do the right thing with that message */
void processMsg(msg_t *my_msg, fd_set *open_sockets, hash_table_t *hashtable) {

    if (strcmp(my_msg->firstMsg,CONTROL)==0) {

        // Handle a "control" message.
        doControl(my_msg, open_sockets, hashtable);

    } else if (strcmp(my_msg->effectName,BUTTON)==0) {

       // Handle a message from the button
        doButton(my_msg, open_sockets, hashtable);

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
}




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

    if (!my_msg->effectName || !my_msg->whosTalking) return 0;

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
                if (DEBUG) {cur_time();  printf("xx  DUPLICATE EFFECT!! name:%s is already on socket:%d. IGNORING THIS EFFECT!\n", 
                   my_msg->effectName, aSock);fflush(stdout); }
                return 0;
            }
        } 
    } else { 
   
        // no socket found for this effect

        // effect found, update its socket
        if (anEffect != NULL) {
            set_effect_socket(hashtable,anEffect->effect,my_msg->whosTalking);  // update effect with new socket number
            if (DEBUG) {cur_time();  printf("updating known effect with now known socket#:%d\n",anEffect->socket_num); fflush(stdout); }

        // brand new effect, store it
        } else {
            add_effect(hashtable, my_msg);    // new effect, add to hash table
            if (DEBUG) {cur_time();  printf("adding new effect with new socket.\n"); fflush(stdout); }
        }
    }

    return 1;
}




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
void doControl(msg_t *my_msg, fd_set *open_sockets, hash_table_t *hashtable) {

    if (strcmp(my_msg->secondMsg,RO)==0) {         // setting round order
        set_list_order(hashtable, my_msg);
    } else if (strcmp(my_msg->secondMsg,CC)==0) {  // creatiang a collection
        set_collection(hashtable, my_msg);
    } else if (strcmp(my_msg->secondMsg,DS)==0) {  // don't-send flag
        set_effect_ds(hashtable, my_msg->effectName, my_msg->thirdMsg);
    } else {
        // more eventually....
    }
}







/* BUTTON ROUTINES  */


/* process incoming message from the button */
void doButton(msg_t *my_msg, fd_set *open_sockets, hash_table_t *hashtable) {

    int	which_but = 0; 		// number of button currently pushed/released
    int butstate  = 0;		// 1 if pressed, 0 if released

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
            bigRound(my_msg->whosTalking, open_sockets, hashtable);
        } else if (which_but==3 && butstate==1) {   
            // send poofstorm!
            send_all(my_msg->whosTalking, open_sockets, hashtable, PoofSTM);
        }
    }
}




// NOTE THAT ALL THESE ARE BLOCKING ROUTINES!!!! NEED TO FIX THAT 

/*  Go around in a "circle" several times, faster, then big finish */
void bigRound(int whosTalking, fd_set *open_sockets, hash_table_t *hashtable) {

    if (DEBUG) { cur_time(); printf("\tLet's Have a big ROUND!!\n");  fflush(stdout); }

    list_t *all_elements =  get_ordered_list(hashtable);
    int poof_time = ROpoofLength;

    roundRobbin(whosTalking, hashtable,all_elements,open_sockets,poof_time,240);
    roundRobbin(whosTalking, hashtable,all_elements,open_sockets,poof_time,130);
    roundRobbin(whosTalking, hashtable,all_elements,open_sockets,poof_time,80);
    roundRobbin(whosTalking, hashtable,all_elements,open_sockets,poof_time,50);
    roundRobbin(whosTalking, hashtable,all_elements,open_sockets,poof_time,40);
    roundRobbin(whosTalking, hashtable,all_elements,open_sockets,poof_time,30);
    roundRobbin(whosTalking, hashtable,all_elements,open_sockets,poof_time,30);

    lulu_poof(lookup_effect(hashtable, LULU), open_sockets, 1, hashtable);
    bigbetty_poof(lookup_effect(hashtable, BIGBETTY), open_sockets, 5000, hashtable);
}




/* 
    DO A ROUND

    poof the effects in a given order, for a length of time, with a particular delay 
*/
void roundRobbin(int whosTalking, hash_table_t *hashtable, list_t *all_elements, fd_set *open_sockets, int the_poof, int the_wait) {

    int mySock  = 0;

    // send message to everyone on the list expcept the originating socket
    while (all_elements!= NULL) {
        mySock = all_elements->socket_num;

        if (mySock != whosTalking) {
            send_msg(all_elements, open_sockets, PoofON, hashtable);
            delay(the_poof);
            send_msg(all_elements, open_sockets, PoofOFF, hashtable);
        }
        all_elements = all_elements->next;
        delay(the_wait);   
    }
}




void lulu_poof (list_t *my_element, fd_set *open_sockets, int s, hash_table_t *hashtable) {

    if (my_element==NULL) return; 
    int mySock = my_element->socket_num;

    if (!mySock) { return; }

    int xx;
    for (xx=0; xx<4; xx++) {
        send_msg(my_element, open_sockets, PoofON, hashtable);
        delay(80);
        send_msg(my_element, open_sockets, PoofOFF, hashtable);
        delay(50);
    }
    delay(500);
    send_msg(my_element, open_sockets, PoofON, hashtable);
    delay(1600);
    send_msg(my_element, open_sockets, PoofOFF, hashtable);

}




void bigbetty_poof (list_t *my_element, fd_set *open_sockets, int s, hash_table_t *hashtable) {

    if (my_element==NULL) return; 
    int mySock = my_element->socket_num;
    if (!mySock) { return; }

    send_msg(my_element, open_sockets, PoofON, hashtable);
    delay(s);
    send_msg(my_element, open_sockets, PoofOFF, hashtable);

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
    
    char *part = test_part(strtok(input,MSG_DIV2));

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
        part = test_part(strtok(NULL,MSG_DIV2));
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
    for (list = hashtable->table[hashval]; list != NULL; list = list->next)  
        if (strcmp(str, list->effect) == 0) 
            return list; 
     
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
list_t *new_node(char*str, int sock) {
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
    head = NULL;
}




/*      frees all memory for a given node   */
void free_node(list_t *node) {

    free(node->effect);
    free(node); 
}
