#include "browser.h"
#include <logger.h>
#include <stdio.h>
#include <stdlib.h>
#include "remote.h"
#include <unistd.h>
#include  <signal.h>

static int quitting=0;


static void  signalhandler(int sig)
{
    log_write(INFO,"Received signal %i",sig);
    quitting=1;
    remote_stop();
    browser_stop();


}


//AVAHI_GCC_UNUSED
int main(int argc, char*argv[])
{
    int error;
    int port;
    char* hostName;


    error=log_init("/var/log/ir2hid.log",100,0);
    if (error)
    {
        fprintf(stderr, "Cannot create log files\n");
    }


    log_write(DEBUG,"Initialize signal handlers");
    signal(SIGINT, signalhandler);
    signal(SIGTERM, signalhandler);

    hostName=malloc(255*sizeof(char));
    do
    {
        log_write(DEBUG,"Trying to resolve player address");
        error=find_player(&hostName,&port);
        if (error)
        {
            log_write(ERROR,"Failed to resolve player address, retrying in 5 seconds...");
            sleep(5);
        }
        else
        {
            log_write(INFO,"Player address successfully resolved");
        }
    }while(error && !quitting);

    if (!error && !quitting) remote_start(hostName,port);

    log_write(DEBUG,"Disposing resources");
    free(hostName);
    log_dispose();
    //free(hostName);


    return error;
}
