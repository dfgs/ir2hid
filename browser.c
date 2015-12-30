#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <logger.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>

#include <avahi-common/simple-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>
#include "browser.h"

#include "remote.h"

static int* resolvedPort;
static char** resolvedHostname;
static AvahiSimplePoll *poller;
static AvahiClient *client = NULL;
static AvahiServiceBrowser *browser = NULL;
static int resolved;

static void resolve_callback(AvahiServiceResolver *resolver,AVAHI_GCC_UNUSED AvahiIfIndex interface,AVAHI_GCC_UNUSED AvahiProtocol protocol,AvahiResolverEvent event,
                            const char *name,const char *type,const char *domain,  const char *host_name,  const AvahiAddress *address,   uint16_t port,   AvahiStringList *txt,
                            AvahiLookupResultFlags flags,   AVAHI_GCC_UNUSED void* userdata)
{

    log_enter("resolve_callback");
    if (resolver==NULL)
    {
        log_write(ERROR,"Resolver parameter is null");
        return;
    }

    switch (event)
    {
        case AVAHI_RESOLVER_FAILURE:
            log_write(ERROR,"Failed to resolve service '%s' of type '%s' in domain '%s': %s",name, type, domain, avahi_strerror(avahi_client_errno(avahi_service_resolver_get_client(resolver))));
            break;

        case AVAHI_RESOLVER_FOUND:
        {
            log_write(INFO,"Resolved service '%s' of type '%s' in domain '%s'", name, type, domain);
            log_write(INFO,"Found new player at %s:%i",host_name,port);
            *resolvedPort=port;
            //*resolvedHostname=host_name;
            strcpy(*resolvedHostname,host_name);
            //*resolvedHostname=host_name;
            resolved=0;
        }
    }
    avahi_simple_poll_quit(poller);

    log_write(DEBUG,"Disposing service resolver");
    avahi_service_resolver_free(resolver);
}

static void browse_callback(AvahiServiceBrowser *browser,AvahiIfIndex interface,AvahiProtocol protocol,AvahiBrowserEvent event,const char *name,const char *type,
                            const char *domain,AVAHI_GCC_UNUSED AvahiLookupResultFlags flags, void* userdata)
{
    AvahiServiceResolver* resolver;

    log_enter("browse_callback");

    if (browser==NULL)
    {
        log_write(ERROR,"Browser parameter is null");
        return;
    }

    AvahiClient *client = userdata;

    switch (event)
    {
        case AVAHI_BROWSER_FAILURE:
            log_write(ERROR,"Browser failure: %s", avahi_strerror(avahi_client_errno(avahi_service_browser_get_client(browser))));
            avahi_simple_poll_quit(poller);
            return;

        case AVAHI_BROWSER_NEW:
            log_write(INFO,"New service '%s' of type '%s' in domain '%s' found", name, type, domain);

            /* We ignore the returned resolver object. In the callback
               function we free it. If the server is terminated before
               the callback function is called the server will free
               the resolver for us. */
            log_write(DEBUG,"Creating new service resolver");
            resolver=avahi_service_resolver_new(client, interface, protocol, name, type, domain, AVAHI_PROTO_UNSPEC, 0, resolve_callback, client);
            if (resolver==NULL)
            {
               log_write(ERROR, "Failed to create service resolver for service '%s': %s", name, avahi_strerror(avahi_client_errno(client)));
            }
            break;

        case AVAHI_BROWSER_REMOVE:
            log_write(INFO, "Service '%s' of type '%s' in domain '%s' has been removed", name, type, domain);

            break;

        case AVAHI_BROWSER_ALL_FOR_NOW:
            log_write(DEBUG,"Browser, all for now...");
            break;
        case AVAHI_BROWSER_CACHE_EXHAUSTED:
            log_write(DEBUG,"Browser, cache exhausted");
            break;
    }
}

static void client_callback(AvahiClient *client, AvahiClientState state, AVAHI_GCC_UNUSED void * userdata)
{
   log_enter("client_callback");

    if (client==NULL)
    {
        log_write(ERROR,"Client parameter is null");
        return;
    }

    /* Called whenever the client or server state changes */

    if (state == AVAHI_CLIENT_FAILURE)
    {
        log_write(ERROR, "Server connection failure, quit application: %s", avahi_strerror(avahi_client_errno(client)));
        avahi_simple_poll_quit(poller);
    }
}

void browser_stop()
{
/* Cleanup things */
    if (browser)
    {
        log_write(DEBUG,"Disposing browser");
        avahi_service_browser_free(browser);
        browser=NULL;
    }

    if (client)
    {
        log_write(DEBUG,"Disposing client");
        avahi_client_free(client);
        client=NULL;
    }

    if (poller)
    {
        log_write(DEBUG,"Disposing poller");
        avahi_simple_poll_free(poller);
        poller=NULL;
    }
}

int find_player(char** hostname,int *port)
{
    int error;

    log_enter("start_browse");

    resolvedPort=port;
    resolvedHostname=hostname;

    resolved=1;

    log_write(DEBUG,"Creating poller");
    poller = avahi_simple_poll_new();

    if (poller==NULL)
    {
        log_write(ERROR, "Failed to create poller");
        goto end;
    }


    log_write(DEBUG,"Creating client");
    client = avahi_client_new(avahi_simple_poll_get(poller), 0, client_callback, NULL, &error);

    if (client==NULL)
    {
        log_write(ERROR, "Failed to create client: %s", avahi_strerror(error));
        goto end;
    }

    log_write(DEBUG,"Creating browser");
    browser = avahi_service_browser_new(client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, "_hid._udp", NULL, 0, browse_callback, client);
    if (browser==NULL)
    {
        log_write(ERROR, "Failed to create browser: %s", avahi_strerror(avahi_client_errno(client)));
        goto end;
    }

    log_write(INFO,"Running poller loop");
    avahi_simple_poll_loop(poller);
    log_write(DEBUG,"Poller loop terminated");


end:

    browser_stop();

    return resolved;
}
