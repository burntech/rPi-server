/*

THIS IS might be DEPRECATED IN FAVOR OF but-client.ino (the arduino-like
version of the code for the ESP8266).

NEED NON-BLOCKING DEBOUNCE - only accept a single button push within (say) 100 ms (for a given button)


   but-client.c	V3.0
   Copyrights - Neil Verplank 2016 (c)
   neil@capnnemosflamingcarnival.org

  
	This is a simple button detection client, it runs at startup as
	a daemon when not in DEBUG mode.  It attaches to localhost:PORT 
	(provided by xc-socket-server) and sends a message when it detects 
	a button being pushed.  

	Button's 1-3 are "special" - button 1 is The Button, #2 is 
	Round Order (poof in a circle ending with Lulu), and #3 is
        currently Poofstorm.  Button 4 on up currently attach to 
	individual games in a (currently) random fashion.  NOTE that
 	all this logic is in xc-socket-server.

	FWIW, the buttons (#1 on up) correspond to one of the Raspberry
	Pi's "column" of pins, which provides for 12 buttons total.  I'm
	vaguely envisioning the other column as being for a wholly separate
	king of input - a band-pass filter for instance, or say hooking up
	a drum kit (or keyboard, etc.) for different types of live musical
	control.  Obviously, you can double the number of inputs in either 
	feature by monopolizing the Pi's inputs...

	Versions 1 and 2 presumed the buttons would repeatedly send a 
	"pushed" signal, effectively making it a keep-alive scenario.
	Version 3, we're attempting to dramaticaly improve responsiveness
	by minimizing traffic, and simply sending an ON and subsequently an
	OFF signal for a given button.


	To compile (you will need to install the wiringPi Library):

	    gcc -o but-client but-client.c -lpthread -lwiringPi -Wall

	NOTE that you can run xc-socket-server on more or less any machine
	in normal user mode, and connect with Telnet (or the Huzzah).
	However, butclient has to be run as root, and won't run except on the
	Pi itself.

	As of now, the code lives on the Pi in ~code/xc-socket-server/, where
	you would compile it, then symlink it in order to run at boot:

	    sudo ln -s /home/pi/code/xc-socket-server/but-client /usr/sbin/

	NOTE the init scripts aren't right, so you currently have to reboot
	if you want the script running as a daemon.  To run in debug mode
	append a one (otherwise it's a daemon process): 

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

int	DEBUG = 0;	        /* 0=daemon, 1=command line	*/

#define PORT      5061    	/* port of our xc-socket-server */
#define ON        LOW
#define OFF       HIGH 
#define stdDelay  1             /* loop round trip (ms)		*/
#define butDelay  2000    	/* delay after round,etc (ms)	*/
#define msgSize   16 

/* GPIO pinouts being used.  This is the order they occur	*/
/* on the board, so theoretically a flat cable will work	*/
/* Buttons are active-low					*/


//#define numbtns   12
#define numbtns   2

// make an array of actual buttons we need to go through....

#define butPin1   14      	
#define butPin2   15  
#define butPin3   18  
#define butPin4   23  
#define butPin5   24  
#define butPin6   25  
#define butPin7   8   
#define butPin8   7   
#define butPin9   12  
#define butPin10  16  
#define butPin11  20  
#define butPin12  21  

//int buttons[numbtns] = {butPin1,butPin2,butPin3,butPin4,butPin5,butPin6,butPin7,butPin8,butPin9,butPin10,butPin11,butPin12};
int buttons[numbtns] = {butPin1,butPin2};


const char* MSG = "B";
const char* DIV = ":";
const char* NL  = '\0';

char* RELEASE = "B:1:0";
char* PUSH    = "B:1:1";


/* Set up raspberry pi for buttons */
void setupPi() {

    wiringPiSetupGpio();
    int i;
    for (i=0; i<numbtns; i++) {
        pinMode(buttons[i], INPUT);             /* Set button as INPUT */
        pullUpDnControl(buttons[i], PUD_UP);    /* Enable pull-up resistor on button */
    }

}


/* send messages out.  confirm socket is ready for writing, and
isn't on the "no send" list (we haven't created that yet) */
void send_msg (int sock, char *msg) {

    int nBytes = strlen(msg) + 1;
    send(sock, msg, nBytes, MSG_NOSIGNAL);
    if (DEBUG) { printf("message:%s, i:%d, size:%d\n",msg,sock,sizeof(msg)); fflush(stdout); }
}


int main(int argc,char **argv) {

    int 	i;
    int 	sockfd;
    struct 	sockaddr_in servaddr;
    int       	pushed; 
    int		thedelay;
    int         butspushed[numbtns];
    char 	message[msgSize];
    char* 	ONE;
    char* 	ZERO;

    ONE  = "1";
    ZERO = "0";

    if (argc == 2) { 
        DEBUG = (int)argv[1][0]-'0';
    }


    if (!DEBUG) {
        /* Set us up as a daemon */
        pid_t process_id = 0;
        pid_t sid = 0;
        // Create child process
        process_id = fork();
        // Indication of fork() failure
        if (process_id < 0)
        {
            printf("fork failed!\n");
            // Return failure in exit status
            exit(1);
        }
        // PARENT PROCESS. Need to kill it.
        if (process_id > 0)
        {
            // return success in exit status
            exit(0);
        }
        //unmask the file mode
        umask(0);
        //set new session
        sid = setsid();
        if(sid < 0)
        {
            // Return failure
            exit(1);
        }
        // Change the current working directory to root.
        chdir("/");

    }
 
    sockfd=socket(AF_INET,SOCK_STREAM,0);
    bzero(&servaddr,sizeof servaddr);
 
    servaddr.sin_family=AF_INET;
    servaddr.sin_port=htons(PORT);
 
    inet_pton(AF_INET,"127.0.0.1",&(servaddr.sin_addr));
 
    while (connect(sockfd,(struct sockaddr *)&servaddr,sizeof(servaddr)) < 0) { delay(10); }

    setupPi();

    /* announce ourself as the button client ('B:')	*/
/*    memset(message,'\0',msgSize);
    strcpy(message, MSG);
    strcat(message, DIV);*/
    send_msg(sockfd,RELEASE);

    /* zero out our buttons	*/
    for (i=0; i<numbtns; i++) { butspushed[i]=0; }

    while (1)
    {


        /* we allow for the possibility that more than one button
	could have changed state since the last loop.	*/

        thedelay = stdDelay;
	for (i=0; i<numbtns; i++) {


            pushed = i+1; 
	    /* "B:dd:d:" ('B'utton:2 digit identifier:1 or 0) */
            memset(message,'\0',msgSize);

	    if (digitalRead(buttons[i])==ON) {
                if (!butspushed[i]) {
                    /* button is on, but wasn't before */
                    butspushed[i]=1;

	            // button 1 is "active" for as long as you push it.
	     	    // buttons 2 and 3 send a signal, wait 2 seconds.
                    if (i==2 || i==3) { thedelay=butDelay; }
/*
		    strcpy(message, MSG);
		    strcat(message, DIV);
		    char tmp[10];
		    snprintf(tmp,10,"%d",pushed);
		    strcat(message, tmp);
		    strcat(message, DIV);
		    strcat(message, ONE);
	    	    strcat(message, DIV);*/
                    send_msg(sockfd,PUSH);
                }
            } else {
                if (butspushed[i]) { 

                    /* button is off, but was on before */
                    butspushed[i]=0;
/*
		    strcpy(message, MSG);
		    strcat(message, DIV);
		    char tmp[10];
		    snprintf(tmp,10,"%d",pushed);
		    strcat(message, tmp);
		    strcat(message, DIV);
		    strcat(message, ZERO);
	    	    strcat(message, DIV); */
                    send_msg(sockfd,RELEASE);
                }
            }
        }


        delay(thedelay);  

    }

    return (0);

}

