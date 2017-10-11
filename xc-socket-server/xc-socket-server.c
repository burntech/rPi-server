
/*
  want EFFECT NAME: TYPE: UNIQUE #: MSG: TIMESTAMP:
  avoid forcing names of all things here
  "provide" available effects, create patterns (save?)
 
*/

/* 

   xc-socket-server.c	V3.0
   Copyrights - Neil Verplank 2016 (c)
   neil@capnnemosflamingcarnival.org

   Code is very loosely based on:
   multi-client-echo-server.c - a multi-client echo server 	
   Copyrights - Guy Keren 1999 (c)

   Oct 12, 16 - updated response to incoming messages for Camera/Flash
   Sep 2, 17  - major updates to handle socket pools, segfaults, clean up code
   Oct 17     - major updates, clean up bad coms, move stuff to subroutines
               

   This is intended as a multi-socket select() server,
   running on a raspberry pi 3 (with wifi), in
   conjunction with the Arduino Adafruit Huzzah (the ESP2866
   wifi enabled board) to create a responsive, wirelessly
   distributed microcontroller network.  We're using it to
   link a series of flame-enabled games. 

   NOTE: We're using the RPi 3, and have configured it as
   an access point with a static IP. Except for wiringPi,
   all libraries are standard.  WiringPi makes the raspberry
   "look like" an Arduino.

       http://wiringpi.com/download-and-install/

   Also you'll need but-client.c for the button!

   To compile:

       gcc -o xc-socket-server xc-socket-server.c -lpthread -lrt -Wall


   NOTE: When DEBUG=1, the idea is that you would run this at the command
   prompt, and all debugging info is dropped into STDOUT.  When DEBUG is 0,
   the assumption is no output, and it will run as a daemon. 

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





/* 
   THESE THINGS ARE WHAT YOU SHOULD SET AS A USER 

   The port this server runs on (fairly arbitrary),

   Also, maxclients is really a limit on how many "named"
   poofers we're going to index in an array.  It isn't really
   a limit on how many sockets can connect.

*/


// look into a config file! 

int	DEBUG           =0;     // 0=daemon, no output, 1=command line with output 
#define	PORT		5061	// port of our carnival server   	

#define	BUFLEN		1024	// buffer length 	   	
#define maxclients      20      // max number of poofer clients	


/* 
    Rather than !@#$ing with an *actual* associative array, 
    we store the "names" of the possible poofers as strings, and
    create an enum that corresponds to the same order.  

    Be sure and add / subtract to all THREE of the following
    lists in the same way.  Or fix it.
*/

#define BUTTON		"B"		// 0
#define ENTRY   	"ENTRY"		// 1
#define SMOKESTACK      "SMOKESTACK"	// 2
#define SKEEBALL	"SKEEBALL"	// 3
#define LOKI      	"LOKI"		// 4
#define DRAGON		"DRAGON"	// 5
#define ORGAN		"ORGAN"		// 6
#define POPCORN		"POPCORN"	// 7
#define STRIKER		"STRIKER"	// 8
#define SIDESHOW	"SIDESHOW"	// 9
#define TICTAC		"TICTAC"	// 10
#define CAMERA 		"CAMERA"	// 11
#define FPBUTTON        "FPBUTTON"      // 12
#define FLASH1		"FLASH1" 	// 13
#define FLASH2		"FLASH2" 	// 14
#define SHAKE		"SHAKE"		// 15
#define NEO 		"NEO"           // 16
#define AERIAL 		"AERIAL"        // 17
#define BIGBETTY        "BIGBETTY"      // 18
#define LULU		"LULU"		// 19


#define PoofON 		"$p1%"
#define PoofOFF		"$p0%"
#define PoofSTM		"$p2%"
#define KeepAlive       "$KA%"

enum POOFER 
{
  button,
  entry,
  smokestack,
  skeeball,
  loki,
  dragon,
  organ,
  popcorn,
  striker,
  sideshow,
  tictac,
  camera,
  fpbutton,
  flash1,
  flash2,
  neo,
  shake,
  aerial,
  bigbetty,
  lulu	
} mypoofers;

/* 
These are the strings each effect (including the button) will
initially use to identify themselves.  It's important this list
correspond with the enumeration above (same order, same number)
*/

const char* game[maxclients] = {
BUTTON,
ENTRY,
SMOKESTACK,
SKEEBALL,
LOKI,
DRAGON,
ORGAN,
POPCORN,
STRIKER,
SIDESHOW,
TICTAC,
CAMERA,
FPBUTTON,
FLASH1,
FLASH2,
NEO,
SHAKE,
AERIAL,
BIGBETTY,
LULU
};




/* 
   magical array of integers, where key is implied
   number above, value is sock fd descriptor.

   This is a way of finding the socket for a known game,
   and doesn't necessarily have all sockets in the array.
*/

int gameSocket[maxclients];


/* order in which to "poof a round" of the above" */

int roundOrder[maxclients] = {popcorn,smokestack,dragon,bigbetty,striker,organ,skeeball,aerial,loki};

int max_socket = 0;

#define ROpoofLength	30 	/* milliseconds of a poof in roundOrder 	*/
#define ROpoofDelay	30 	/* milliseconds delay between poofs roundOrder 	*/



/************** OUR SUBROUTINES  ******************/

int     rc; 			/* system calls return value storage 		*/
char    *effectName = ""; 
char    *firstMsg   = "";
char    *secondMsg  = "";
int     whosTalking;		/* which socket sent a message 			*/




void    checkDebug(int argc, char **argv);
void    forkify(int argc, char **argv);
void    getFirstSock();
int     acceptSK(int fd, fd_set c_rfd, fd_set *rfd);
void    forceCloseSK(int fd, fd_set *rfd);
void    closeSK(int fd, fd_set *rfd);
char*   namedSock(int sock, char *mesg, fd_set *sockets);
void    send_msg (int sock, fd_set *wr_sock, char *msg);
int     readMsg(int socket, fd_set *sockets, fd_set *allSocks);
void    processMsg(fd_set *sockets, fd_set *allSocks);




void    doButton(fd_set *sockets);
void    theButton(int butstate, fd_set *sockets);
void    bigRound(fd_set *sockets);
void    roundRobbin(fd_set *sockets,int delay);
void    poofStorm(fd_set *sockets);
void    lulu_poof(fd_set *sockets, int s);
void    bigbetty_poof(fd_set *sockets, int s);

void    error(char *msg);
void    cur_time (void);
int     naive_str2int (const char* snum);
void    delay (unsigned int howLong);



int     firstSocket; 			/* socket descriptor 				*/
time_t  start_time;

/*  MAIN SETUP AND INFINITE LOOP */

int main(int argc,char **argv) {

    /* socket server variables */

    int		i;			/* index counters for loop operations 		*/
    fd_set	rfd;     		/* set of open sockets 				*/
    fd_set	c_rfd; 			/* set of sockets available to be read 		*/
    fd_set	w_rfd; 			/* set of sockets available to be written to 	*/


    /* Make this server a DAEMON if not debugging.	*/
    forkify(argc, argv);

    /* clear sets and memory */
    FD_ZERO(&rfd);	
    FD_ZERO(&c_rfd);
    FD_ZERO(&w_rfd);
    start_time = 0L;

    /* Set up socket server */
    getFirstSock(&rfd);
    


    /* enter an accept-write-close infinite loop */

    while (1) {

	c_rfd = rfd;
	w_rfd = rfd;

	/* check which sockets are ready for reading.     */
        /* null in timeout means wait until incoming data */
        rc = select(max_socket+1, &c_rfd, NULL, NULL, (struct timeval *)NULL);

        /* return from select - add incoming sockets to pool */
        acceptSK(firstSocket, c_rfd, &rfd);

        /* 
           Go through each socket with incoming data, read message and process.

	   NOTE: we range from s+1 to max_socket.  In fact, the first socket allocated
	   seems to be just used to bind - we dont send or receive over it.  Also,
           sockets 0, 1 and 2 correspond to STDIN, STDOUT, and STDERR, so "s" is likely
           to be 3 (presuming nothing else is running).
	  
	   So the first "real" socket is s+1, and the "last" is max_socket-1.  Note that it's
	   possible for there to be fewer sockets than max_socket-s+1, as sockets can be
	   freed up *between* s and max_socket, but we just run through the range for
	   sockets available to be read.
	*/


	for (i=firstSocket+1; i<max_socket+1; i++)  {

            if (!readMsg(i, &c_rfd, &rfd)) { continue; }

            /* Incoming  Message is of the form "effectName":"firstMsg":"secondMsg", which should now be set*/
            if (DEBUG) { printf("%s on socket#:%d  firstMsg:%s secondMsg:%s\n",effectName,i,firstMsg,secondMsg);cur_time();fflush(stdout); }

            if (firstMsg && strcmp(firstMsg,"KA")==0) { continue; } // Keep alive signal
            processMsg(&w_rfd, &rfd);


        } /* end IF is member of readable set */ 


    } /* end while(1) */

    return(0);

} /* end main	*/












/* SUBROUTINES  */


void checkDebug(int argc, char **argv) {
    
    if (argc == 2) {
        DEBUG = (int)argv[1][0]-'0';
    }

}

void forkify(int argc, char **argv) {
//    INIT SCRIPTS NOT RIGHT!!
/* 

    I note this isn't really working correctly when launched at boot.  
    In particular, stopping the daemon causes it to re-spawn, several 
    times in the case of the button.

    The problem is in the init scripts, not in this code, but.

    It does however launch on boot, which was the basic goal.
*/

    /* Make this server a DAEMON if not debugging.	*/

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
          // printf("process_id of child process %d \n", process_id);
          exit(0);		// return success in exit status
      }
      
      umask(0);			// unmask the file mode
      sid = setsid();		// set new session
      if(sid < 0) {
          printf("couldn't setsid\n");
          exit(1);		// Return failure
      }
      
      chdir("/");		// Change the current working directory to root.

   } else {
      printf("open for debugging!\n");fflush(stdout);
   } 


    signal(SIGPIPE, SIG_IGN);	/* ignore sigpipe */
}



void getFirstSock(fd_set *rfd) {

    struct sockaddr_in	sa; 			/* Internet address struct 			*/

    memset(&sa, 0, sizeof(sa)); 			/* first clear out the struct, to avoid garbage	*/
    sa.sin_family = AF_INET;				/* Using Internet address family 		*/
    sa.sin_port = htons(PORT);				/* copy port number in network byte order 	*/
    sa.sin_addr.s_addr = INADDR_ANY;		  	/* accept connections through any host IP 	*/
    firstSocket = socket(AF_INET, SOCK_STREAM, 0);	/* allocate a free socket 			*/

    if (firstSocket < 0) {
	error("socket: allocation failed");
    }

    /* bind the socket to the newly formed address */
    rc = bind(firstSocket, (struct sockaddr *)&sa, sizeof(sa));

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
    FD_SET(firstSocket, rfd);		/* we initially have only one socket open */
    max_socket = firstSocket;  		/* reset max socket number */

}






/*  SOCKET SUBROUTINES  */


int acceptSK(int s, fd_set c_rfd, fd_set *rfd) {

    /* accept incoming connections, if any, add to array */

        int cs     = 0;
        int result = 0;
        int flag   = 1;		/* for TCP_NODELAY				*/
        struct sockaddr_in	csa; 			/* client's address struct 			*/
        socklen_t         	size_csa; 		/* size of client's address struct 		*/
        size_csa = sizeof(csa);		/* remember size for later usage */

	/* if the 's' socket is ready for reading, it	*/
	/* means that a new connection request arrived.	*/
	if (FD_ISSET(s, &c_rfd)) {
	    /* accept the incoming connection */
       	    cs = accept(s, (struct sockaddr *)&csa, &size_csa);

       	    /* check for errors. if any, ignore new connection */
       	    if (cs < 0) {
       		return 0;
            }

            /* Turn off Nagle's algorithm for less delay */
            result = setsockopt(
	      cs,
              IPPROTO_TCP,    
              TCP_NODELAY,   
              (char *) &flag,
              sizeof(int));
            if (result < 0) {
              /* note, error doesn't preclude continuing */
              if (DEBUG) { printf("TCP_NODEAY failed.\n");fflush(stdout); }
            }

	    FD_SET(cs, rfd);			        /* add socket to set of open sockets */
            if (cs > max_socket) { max_socket = cs; }  	/* reset max */
	   
	    if (DEBUG) { printf("socket received. cs:%d s:%d dscize:%d\n",cs,s,max_socket);fflush(stdout); }

	}

     return cs;
}





/* close socket and clear named socket arrays */
void closeSK(int fd, fd_set *rfd) {

    int x;

    /* if client closed the connection, close the socket */
    forceCloseSK(fd, rfd);

    /* also find and reset corresponding associative array element to 0, if any */
    for (x=0; x<maxclients; x++) { 
        if (gameSocket[x] == fd) { 
            if (gameSocket[x] == max_socket) { max_socket--; }
	    gameSocket[x]=0;
            if (DEBUG) { printf("gameSocket:%d closed. x=%d\n",fd,x);fflush(stdout); }
	} 
    }
    if (DEBUG) { printf("closed socket:%d  Max open socket is now:%d\n",fd,max_socket);fflush(stdout); }


}


/* just close socket */
void forceCloseSK(int fd, fd_set *rfd) {
    close(fd);
    FD_CLR(fd, rfd);
}



/* 
  read incoming message into approriate strings.  

  message looks like:  "B:1:0"
  
    effectName = "B"
    firstMsg   = "1"
    secondMsg  = "0"

    whosTalking will equal the socket that sent the message.
  
*/

int readMsg(int socket, fd_set *sockets, fd_set *allSocks) {

    char  buf[BUFLEN+1];  	/* buffer for incoming data 			*/
    int   received = 0;
 
    if (FD_ISSET(socket, sockets)) { 
  	/* 
	NOTE:
	Seems to "take a while" for a disconnected huzzah to register.
	*/

        memset(buf,0,BUFLEN);	/* clear buffer */
	rc = read(socket, buf, BUFLEN);	/* read from the socket */
	if (rc < 1) {
            closeSK(socket, allSocks);
            if (DEBUG) { printf("closing sock:%d, no read?\n",socket);fflush(stdout); }
	} else {
                
            whosTalking = socket;
                
            // get name of effect, associate socket with that effect
	    effectName =  namedSock(socket, buf, allSocks);

            if (effectName) {
                firstMsg  = strtok(NULL,":");
	        secondMsg = strtok(NULL,":");
                received = 1;
            }

	} /* end else data to read from this socket */
    }

    return received;

}




/* given an incoming message, do the right thing with that message */

void processMsg(fd_set *sockets, fd_set *allsockets) {

    // Handle a message from the button
    if (strcmp(effectName,game[button])==0) {
        doButton(allsockets);
    } 


    // all other messages, currently:

    if (firstMsg != NULL) {

        /*
	we know we just got an actual message from some socket other
	than the button.

	who is it, what should we do? Answers vary.
        */

        if (whosTalking == gameSocket[skeeball]) {
	    /* skeeball - high score - message to organ? */
        } else if (whosTalking == gameSocket[flash1]) {
	    /* camera - just send the message on to the flash */
	    send_msg(gameSocket[flash1], allsockets, firstMsg);
        } else if (whosTalking == gameSocket[flash2]) {
	    /* camera - just send the message on to the flash */
            send_msg(gameSocket[flash2], allsockets, firstMsg);
        } else if (whosTalking == gameSocket[camera]) {
	    /* camera - just send the message on to the flash */
            send_msg(gameSocket[camera], allsockets, firstMsg);
        } else if (whosTalking == gameSocket[fpbutton]) {
	    /* camera - just send the message on to the flash */
	    send_msg(gameSocket[fpbutton], allsockets, firstMsg);
        } else if (whosTalking == gameSocket[shake]) {
	    /* sleeping button - send message to neopixels*/
	    send_msg(gameSocket[neo], allsockets, firstMsg);
        }

    } // end there's a message
}














char* namedSock (int sock, char *buf, fd_set *sockets) {

                /*
                  split the string on ":" 
                  first part of array is the sender, second/remaining is the message.
                  e.g. SKEEBALL:725:  or DRAGON:1:

                  the button is B:a:b:, where B means button, "a" is the number 
		  representing which button, and "b" is either 1 or 0 (has just 
 		  been pushed, or just been released).
                */

    /* If this is a message from a  socket associated with a
    named effect, (re-)associate it.  If it's from the button,
    get which button, and the button state.  Right now, this
    predominantly collects the "Hi I'm this game" messages and
    keeps track of which socket is which game.  It does not
    track all sockets (the unnamed). */

    int x = 0;

    char *en = strtok(buf,":");

    if (en[0] == 13) { return 0; } // it's a carriage return

    for (x=0; x<maxclients; x++) {
        if (!game[x]) continue;
        if (strcmp(buf,game[x])==0) { 
            // if the socket has changed, close old socket
            if (sock && gameSocket[x]>0 && gameSocket[x] != sock) {
                forceCloseSK(gameSocket[x], sockets);
                if (DEBUG) { printf("FORCE CLOSE gameSocket[x]:%d??  new sock:%d   game[x]:%s\n",gameSocket[x], sock, game[x]);fflush(stdout); }
            }
            // associate current socket, end loop
	    gameSocket[x] = sock;
	    x = maxclients;
	}
    }

    return en;
}






/* 

send messages out.  confirm socket is ready for writing, and
isn't on the "no send" list (we haven't created that yet).

Note that we just close any socket that can't accept messages.

 */

void send_msg (int sock, fd_set *sockets, char *msg) {
 
    /* 
      don't send messages to button, it isn't listening (camera either) 
    */

    if ((gameSocket[button] && sock == gameSocket[button]) ||
        (gameSocket[camera] && sock == gameSocket[camera])) {
        if (DEBUG) { printf("Not sending to button/camera.\n");fflush(stdout); }
        return;
    }

    if (sock != gameSocket[button] && sock != gameSocket[camera]) {
        if (FD_ISSET(sock, sockets)) { 
            int nBytes = strlen(msg) + 1;
            send(sock, msg, nBytes, MSG_NOSIGNAL);
            if (DEBUG) { printf("sent message:'%s' to socket:%d\n",msg,sock);cur_time(); fflush(stdout); }
	} else { // close any unavailable socket
            closeSK(sock, sockets);
            if (DEBUG) { printf("can't write to socket:%d, closing\n",sock);fflush(stdout); }
        }
    }
  
}









/* BUTTON ROUTINES  */



/* process incoming message from the button */

void doButton(fd_set *sockets) {

    int	pushed   = 0; 		/* number of button currently pushed/released 	*/
    int butstate = 0;		/* 1 if pressed, 0 if released			*/

    if (firstMsg) {
        pushed = naive_str2int(firstMsg);
        if (secondMsg) {
            /* 1 is pushed, 0 is released */
            butstate = naive_str2int(secondMsg);
        }
    }

    /* if button pushed, write to appropriate sockets */ 
    if (pushed) {

        /* 
	Now we collect all sockets available for writing. 
	this should return instantly (right?).  Also helps
	clean up when Huzzahs have been uplugged (no
	longer available for writing).
  	*/


        if (pushed==3 && butstate==1) {
            poofStorm(sockets);
        } else if (pushed==2 && butstate==1) {
            bigRound(sockets);
        } else if (pushed==1) {
            theButton(butstate,sockets);
        } 
                

        pushed   = 0;  /* reset each time around */
        butstate = 0;
    }
}


// turn on (or off) all active effects

void theButton(int butstate, fd_set *sockets) {
    int i = 0;
    // first actual game socket is usually 4, max socket
    // is next available slot
    char *mymsg = PoofOFF;
    if (butstate == 1) { mymsg = PoofON; }
    for (i=firstSocket+1; i<max_socket+1; i++) {
        send_msg(i, sockets, mymsg);
    }
    if (DEBUG) { printf("The Button!! butstate:%d, max socket:%d, firstSocket:%d\n",butstate,max_socket,firstSocket); cur_time(); fflush(stdout); }
}



void bigRound(fd_set *sockets) {

    roundRobbin(sockets,240);
    roundRobbin(sockets,130);
    roundRobbin(sockets,80);
    roundRobbin(sockets,50);
    roundRobbin(sockets,40);
    roundRobbin(sockets,30);
    roundRobbin(sockets,30);
    roundRobbin(sockets,30);
    roundRobbin(sockets,10);
    lulu_poof(sockets, 1);
    bigbetty_poof(sockets, 5000);
}



void roundRobbin(fd_set *w_rfd,int thewait) {

    /* 
        DO A ROUND
	
        poof the games in roundOrder if the given socket
        is available for writing, delay between poofs
    */

    int mySock  = 0;
    int i       = 0;


    for (i=0; i<maxclients; i++) {

        if (roundOrder[i]) { 			/* if there's a game specified		*/
            mySock = gameSocket[roundOrder[i]];  	/* get the socket associated		*/
            if (FD_ISSET(mySock, w_rfd)) { 	/* if it's available for writing	*/

                if (mySock == gameSocket[lulu]) { 

                   lulu_poof(w_rfd, 1);
 
                } else if (mySock == gameSocket[bigbetty]) { 

                   bigbetty_poof(w_rfd, ROpoofLength);

	        } else { 

                    send_msg(mySock, w_rfd, PoofON);
                    delay(ROpoofLength);
                    send_msg(mySock, w_rfd, PoofOFF);

		    /* delay between poofs if game was hooked up */

		}

                delay(thewait);   

            }
        }
    }

    if (DEBUG) { printf("Let's Have a ROUND!!\n"); cur_time(); fflush(stdout); }
}



void poofStorm(fd_set *sockets) {

    /* POOFSTORM */

    int i = 0;
 
    for (i=firstSocket+1; i<max_socket+1; i++) {
        send_msg(i, sockets, PoofSTM);
    }

}




void lulu_poof (fd_set *sockets,int s) {

    int mySock = gameSocket[lulu];

    int xx;
    for (xx=0; xx<4; xx++) {
        send_msg(mySock, sockets, PoofON);
        delay(80);
        send_msg(mySock, sockets, PoofOFF);
        delay(50);
    }
    delay(500);
    send_msg(mySock, sockets, PoofON);
    delay(1600);
    send_msg(mySock, sockets, PoofOFF);

}





void bigbetty_poof (fd_set *sockets, int s) {

    int mySock = gameSocket[bigbetty];

    send_msg(mySock, sockets, PoofON);
    delay(s);
    send_msg(mySock, sockets, PoofOFF);

}





/* USEFUL TOOLS */


/*  error - wrapper for perror */
void error(char *msg) {
	perror(msg);
	exit(1);
}



/* prints the time in nanosecond precision	*/
void cur_time (void)
{
    long           ms; // Milliseconds
    time_t         s;  // Seconds
    int            m;  // minutes?
    int            secs;
    int            millis;
   
    struct timespec spec;

    clock_gettime(CLOCK_REALTIME, &spec);

    s  = spec.tv_sec;
    /* ms = spec.tv_nsec / 1.0e6; // Convert nanoseconds to milliseconds*/
    ms = spec.tv_nsec;

    if (!start_time) {
        start_time = s;
        return;
    } 

    secs = s - start_time;
    m    = secs/60;
    secs = secs-m*60;
  
    millis = ms / 1000000;
    
    ms = ms - millis*1000000;    

    printf("time: m:%d s:%d ms:%d.%ld\n", m, secs, millis, ms);
    //printf("Current time: m:%d s:%lld ms:%ld\n", m, (intmax_t)s, ms);
          
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








void delay (unsigned int howLong)
{
  struct timespec sleeper, dummy ;

  sleeper.tv_sec  = (time_t)(howLong / 1000) ;
  sleeper.tv_nsec = (long)(howLong % 1000) * 1000000 ;

  nanosleep (&sleeper, &dummy) ;
}








