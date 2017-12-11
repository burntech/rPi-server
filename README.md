# rPi-server

   Initial motivation for this is as a very fast control server for
   flame effects, lighting effects, and the like.  It's the basis
   for the wireless control of Capn Nemo's Flaming Carnival, and
   is provided in hopes it can help you too.

   By default, this server is located in the /home/pi/code/ directory.  
   You need to compile of couurse, typically 

      gcc -o xc-socket-server xc-socket-server.c -lpthread -lrt -Wall
 
   then add it to user-executables: 

      sudo ln -s /home/pi/code/xc-socket-server/xc-socket-server /usr/sbin/

   and set up /etc/init.d scripts to have the server, and possibly
   the button run as daemons at boot:

      sudo ln -s /home/pi/code/xc-socket-server/initscripts/* /etc/init.d/
      sudo ln -s /etc/init.d/xc-socket-serverd /etc/rc3.d/S02xc-socket-serverd

   ###

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
   delay.  The various lists and collections are kept up to date with
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

   ###

   TO send messages from one socket to another, you must "set a collection"
   to which a given effect will broadcast.  Note that "the button" (currently
   effect: 'B') sends messages to *all* effects, except those with the DS 
   (Don't Send) flag set.  Set a collection by:

      effectname:*:CC:effectname1,effectname2...

   This can be done at any time, whether other effects are present are not, 
   the collection is kept up to date with existing effects.

   ###

   Any effect can block the receipt of messages (and be only a sender):
`
      effectname:*:DS

   ###

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
 

