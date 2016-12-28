# rPi-server
The Carnival socket server that works in conjunction with the ESP clients.

Generally, this directory would be located in the /home/pi/code/ directory.  You need to compile of couurse, typically 

    gcc -o xc-socket-server xc-socket-server.c -lpthread -lrt -Wall
 
then add it to user-executables: 

    ln -s /home/pi/code/xc-socket-server/xc-socket-server /usr/sbin/
  
