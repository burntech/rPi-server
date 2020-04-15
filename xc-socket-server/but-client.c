/*


   but-client.c	V4.0
   Copyright - Neil Verplank 2016, 2017 (c)
   neil@capnnemosflamingcarnival.org

  
	This is a "simple" networked button detection client, part of 
        the custom client - server software that runs the Carnival, and
        specifically provides for up to 3 onboard buttons on the raspberry pi
        (which is in turn runing the xc-socket server as the Carnival). .
        Alternatively, you can use the huzzahClient, with WHOAMI = "B".
        
        It can be run at startup as a daemon when not in DEBUG mode.  

        It attaches to localhost:PORT (provided by xc-socket-server) and 
        sends a message when it detects a button being pushed.  

	Button's 1-3 are "special" - button 1 is The Button, #2 is 
	Round Order (poof in a circle), and #3 is
        currently Poofstorm.  
	
        NOTE that all this logic is in xc-socket-server.  

	FWIW, the buttons (#1 on up) correspond to one of the Raspberry
	Pi's "column" of pins, which provides for 12 buttons total.  I'm
	vaguely envisioning the other column as being for a wholly separate
	king of input - a band-pass filter for instance, or say hooking up
	a drum kit (or keyboard, etc.) for different types of live musical
	control.  Obviously, you can double the number of inputs in either 
	feature by monopolizing the Pi's inputs...

	To compile (you will need to install the wiringPi Library):

	    gcc -o but-client but-client.c -lpthread -lwiringPi -Wall

	NOTE that you can run xc-socket-server on more or less any machine
	in normal user mode, and connect with Telnet (or an ESP8266).
	However, butclient has to be run as root, and won't run except on the
	Pi itself (because it's looking for the GPIO pins and button pushes).

	As of now, the code lives on the Pi in ~code/xc-socket-server/, where
	you would compile it, then symlink it in order to run at boot:

	    sudo ln -s /home/pi/code/xc-socket-server/but-client /usr/sbin/

	NOTE the init scripts aren't right, so you currently have to reboot
	if you want the script running as a daemon.  

        To run in debug mode append a '1'(otherwise it's a daemon process): 

	    sudo ./but-client 1

*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>         /* Internet address structures  */
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>             /* for table size calculations  */
#include <wiringPi.h>           /* Include WiringPi library! 	*/


int	DEBUG      = 0;	        /* 0=daemon, 1=command line	*/
#define PORT       5061    	/* port of our xc-socket-server */
#define ON         LOW
#define OFF        HIGH 
#define stdDelay   1            /* loop round trip (ms)		*/
#define butDelay   2000     	/* delay after round,etc (ms)	*/
#define butPin1    14      	/* third pin down on the right  */
#define butPin2    15       	/* fourth pin down on the right */          
#define butPin3    18       	/* fifth pin down on the right  */ 


// String messages we want to send regularly.  Basically, button is on or off, and might be Button 1, 2, 3....
// So, push[0] is the message for Button 1 = 1 / is pushed, release[0] is when a release is pushed.

// General form of message is EFECT NAME[:Msg1][:Msg2][:Msg3][:Msg4]
//char*   HELLO      = "B:HELLO:0:*:0";
char*   KEEPALIVE  = "B:KA:0";
char*   NO_SEND    = "B:*:DS:1";
char*   push[3]    = {"B:1:1", "B:2:1", "B:3:1"};
char*   release[3] = {"B:1:0", "B:2:0", "B:3:0"};
#define DEBOUNCE   30           // minimum milliseconds between state change (non-blocking)
long    idleTime   = 0L;        // current idle time
long    maxIdle    = 300000L;   // milliseconds network idle before keep alive signal (5 minutes)


/* you may wish to change these...  */
#define numbtns 3
int     buttons[numbtns] = {butPin1,butPin2,butPin3};



// arrays to hold button states and last state changes

int     onboardPush[numbtns];   // physical button is currently pushed (not wireless)
long    lastButsChgd[numbtns];  // most recently allowed state change for given button
int     butspushed[numbtns];    // state of a given button



/*      OUR SUBROUTINES     */

void    setupPi();
void    checkDebug(int argc, char **argv);
void    forkify(int argc, char **argv);
int     getSock();
int     acceptSK(int fd, fd_set c_rfd, fd_set *rfd);
void    send_msg (int sock, char *msg);
int     checkButtons(int sockfd);
void    keepAlive(int sockfd);







/*      MAIN LOOP           */

int main(int argc,char **argv) {

    int 	i;
    int 	sockfd;
    struct 	sockaddr_in servaddr;
    int		thedelay;
   
    // zero memory 
    memset(&servaddr, 0, sizeof(servaddr)); 
    for (i=0; i<numbtns; i++) { butspushed[i]=0; }

    // Make this server a DAEMON if not debugging. 
    forkify(argc, argv);

    // Get our socket
    sockfd = getSock();

    // set up pins and buttons
    setupPi();

    // announce ourself as the button client ('B:')
    send_msg(sockfd,NO_SEND);


    // loop endlessly, detecting pushes
    while (1) {

        thedelay = checkButtons(sockfd);
    
        keepAlive(sockfd);

        delay(thedelay);  

    }

    return (0);
}







/* SUBROUTINES  */



/* Set up raspberry pi for buttons */
void setupPi() {

    wiringPiSetupGpio();
    int i;
    for (i=0; i<numbtns; i++) {
        pinMode(buttons[i], INPUT);             /* Set button as INPUT */
        pullUpDnControl(buttons[i], PUD_UP);    /* Enable pull-up resistor on button */
    }
}




/* check if DEBUG from command line */
void checkDebug(int argc, char **argv) {

    if (argc == 2) {
        DEBUG = (int)argv[1][0]-'0';
    }
}



/* make this a daemon unless passed a '1' at start (for debugging) */

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

        process_id = fork();    // Create child process

        if (process_id < 0) {
            printf("fork failed!\n");
            exit(1);            // Return failure in exit status
        }

        if (process_id > 0) {   // KILL PARENT PROCESS
            // printf("process_id of child process %d \n", process_id);
            exit(0);            // return success in exit status
        }

        umask(0);                       // unmask the file mode
        sid = setsid();         // set new session
        if(sid < 0) {
            printf("couldn't setsid\n");
            exit(1);            // Return failure
        }

        chdir("/");             // Change the current working directory to root.

    } else {
        printf("open for debugging!\n");fflush(stdout);
    }

//    signal(SIGPIPE, SIG_IGN);   /* ignore sigpipe */
}



/* get our socket and connect to server */
int getSock() {

    struct 	sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));   /* first clear out the struct, to avoid garbage */

    int sockfd=socket(AF_INET,SOCK_STREAM,0);
    bzero(&servaddr,sizeof servaddr);

    servaddr.sin_family=AF_INET;
    servaddr.sin_port=htons(PORT);

    inet_pton(AF_INET,"127.0.0.1",&(servaddr.sin_addr));

    // connect to server

    while (connect(sockfd,(struct sockaddr *)&servaddr,sizeof(servaddr)) < 0) { delay(10); }

    return sockfd;
}


void keepAlive(int sockfd) {
  long cTime = millis();
  if ((cTime - idleTime) > maxIdle) {
      send_msg(sockfd,KEEPALIVE);
      idleTime = cTime;
  }
}





/* send messages out.  confirm socket is ready for writing, and
isn't on the "no send" list (we haven't created that yet) */
void send_msg (int sock, char *msg) {

    int nBytes = strlen(msg) + 1;
    send(sock, msg, nBytes, MSG_NOSIGNAL);
    if (DEBUG) { printf("message:%s, i:%d, size:%d\n",msg,sock,sizeof(msg)); fflush(stdout); }
}




/* 
    check each button for a state change if DEBOUNCE milliseconds have passed.
    send push or release message as relevant.
*/

int checkButtons(int sockfd) {

    int x;
    int thedelay = stdDelay;
    
    if (numbtns) {  // allow push if we have button
        for (x = 0; x < numbtns; x++) {

            int     shotValue = digitalRead(buttons[x]);

            if (shotValue == LOW) {                  // button pushed
                if (!onboardPush[x]) {               // first (?) detection of push
                    if ((millis() - lastButsChgd[x]) > DEBOUNCE) {
                        onboardPush[x] = 1;
                        lastButsChgd[x] = millis();
                        if (x>0) {
                            thedelay = butDelay;
                        }
                        send_msg(sockfd,push[x]);
                    }
                }
            } else {                                 // not pushed
                if (onboardPush[x]) {                // button just released
                    if ((millis() - lastButsChgd[x]) > DEBOUNCE) {
                        onboardPush[x]   = 0;
                        lastButsChgd[x] = millis();
                        send_msg(sockfd,release[x]);
                    }
                }
            }
        }
   }

   return thedelay;
} 
