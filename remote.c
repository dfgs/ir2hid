#include <stdio.h>
#include <math.h>
#include <string.h>
#include <assert.h>
#include <ela/ela.h>
#include <ela/backend.h>
#include <sys/unistd.h>
# include <termio.h>
#include <foils/hid.h>
#include <foils/hid_device.h>
#include <logger/logger.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <errno.h>
#include "remote.h"


#define MAX_MAPPING_SIZE 128
#define UNICODE 1
#define KEYBOARD 2
#define CONSUMER 3
#define SYSTEM 4
#define TIMER_INTERVAL 150



static int timer_started=0;
static struct timeval lastPressedTime;

static struct key_map *currentPressedKey;
static struct ela_el *eventLoop;
static struct ela_event_source *lircEventSource;
static struct ela_event_source *timerEventSource;
static struct foils_hid *client;

static int fileDescriptor;

static int mapping_count;


struct key_map
{
    char command[32];
    int report_index;
    int code;
    char lirc_key[32];
};

static struct key_map mapping[MAX_MAPPING_SIZE];

static const uint8_t unicode_report_descriptor[] = {
    0x05, 0x01,         /*  Usage Page (Desktop),               */
    0x09, 0x06,         /*  Usage (Keyboard),                   */

    0xA1, 0x01,         /*  Collection (Application),           */
    0x85, 0x01,         /*      Report ID (1),                  */
    0x05, 0x10,         /*      Usage Page (Unicode),           */
    0x08,               /*      Usage (00h),                    */
    0x95, 0x01,         /*      Report Count (1),               */
    0x75, 0x20,         /*      Report Size (32),               */
    0x14,               /*      Logical Minimum (0),            */
    0x27, 0xFF, 0xFF, 0xFF,  /*      Logical Maximum (2**24-1), */
    0x81, 0x62,         /*      Input (Variable, No pref state, No Null Pos),  */
    0xC0,               /*  End Collection                      */

    0xA1, 0x01,         /*  Collection (Application),           */
    0x85, 0x02,         /*      Report ID (2),                  */
    0x95, 0x01,         /*      Report Count (1),               */
    0x75, 0x08,         /*      Report Size (8),                */
    0x15, 0x00,         /*      Logical Minimum (0),            */
    0x26, 0xFF, 0x00,   /*      Logical Maximum (255),          */
    0x05, 0x07,         /*      Usage Page (Keyboard),          */
    0x19, 0x00,         /*      Usage Minimum (None),           */
    0x2A, 0xFF, 0x00,   /*      Usage Maximum (FFh),            */
    0x80,               /*      Input,                          */
    0xC0,               /*  End Collection                      */

    0x05, 0x0C,         /* Usage Page (Consumer),               */
    0x09, 0x01,         /* Usage (Consumer Control),            */
    0xA1, 0x01,         /* Collection (Application),            */
    0x85, 0x03,         /*  Report ID (3),                      */
    0x95, 0x01,         /*  Report Count (1),                   */
    0x75, 0x10,         /*  Report Size (16),                   */
    0x19, 0x00,         /*  Usage Minimum (Consumer Control),   */
    0x2A, 0x8C, 0x02,   /*  Usage Maximum (AC Send),            */
    0x15, 0x00,         /*  Logical Minimum (0),                */
    0x26, 0x8C, 0x02,   /*  Logical Maximum (652),              */
    0x80,               /*  Input,                              */
    0xC0,               /* End Collection,                      */

    0x05, 0x01,         /* Usage Page (Desktop),                */
    0x0a, 0x80, 0x00,   /* Usage (System Control),              */
    0xA1, 0x01,         /* Collection (Application),            */
    0x85, 0x04,         /*  Report ID (4),                      */
    0x75, 0x01,         /*  Report Size (1),                    */
    0x95, 0x04,         /*  Report Count (4),                   */
    0x1a, 0x81, 0x00,   /*  Usage Minimum (System Power Down),  */
    0x2a, 0x84, 0x00,   /*  Usage Maximum (System Context menu),*/
    0x81, 0x02,         /*  Input (Variable),                   */
    0x75, 0x01,         /*  Report Size (1),                    */
    0x95, 0x04,         /*  Report Count (4),                   */
    0x81, 0x01,         /*  Input (Constant),                   */
    0xC0,               /* End Collection,                      */
};


static const struct foils_hid_device_descriptor deviceDescriptors[] =
{
    {
        .name="Unicode",
        .version=0x0100,
        .descriptor=(void*)unicode_report_descriptor,
        .descriptor_size=sizeof(unicode_report_descriptor),
        .physical=NULL,
        .physical_size=0,
        .strings=NULL,
        .strings_size=0
    }
};

static void hid_status_callback(struct foils_hid *client, enum foils_hid_state state)
{
    log_enter("hid_status_callback");

    switch (state)
    {
        case FOILS_HID_IDLE:
            log_write(DEBUG,"HID status: idle");
            break;
        case FOILS_HID_CONNECTING:
            log_write(DEBUG,"HID status: connecting");
            break;
        case FOILS_HID_CONNECTED:
            log_write(DEBUG,"HID status: connected");
            break;
        case FOILS_HID_RESOLVE_FAILED:
            log_write(DEBUG,"HID status: failed");
            break;
        case FOILS_HID_DROPPED:
            log_write(DEBUG,"HID status: dropped");
            break;
        }
}

static void  hid_feature_report_callback(struct foils_hid *client, uint32_t device_id, uint8_t report_id,const void *data, size_t datalen)
{
    log_enter("hid_feature_report_callback");
}

static void hid_output_report_callback(struct foils_hid *client,uint32_t device_id, uint8_t report_id,const void *data, size_t datalen)
{
    log_enter("hid_output_report_callback");
}

static void hid_feature_report_sollicit_callback(struct foils_hid *client,uint32_t device_id, uint8_t report_id)
{
    log_enter("hid_feature_report_sollicit_callback");
}

static const struct foils_hid_handler hidEventHandler =
{
    .status = hid_status_callback,
    .feature_report = hid_feature_report_callback,
    .output_report = hid_output_report_callback,
    .feature_report_sollicit = hid_feature_report_sollicit_callback,
};

/*struct unicode_state;

typedef void code_sender_t(struct unicode_state *ks, uint32_t code);

struct unicode_state
{
    struct term_input_state input_state;
    struct ela_el *el;
    struct foils_hid *client;
    struct ela_event_source *release;
    code_sender_t *release_sender;
    uint32_t current_code;
    uint32_t release_code;
};

static void release_send(struct unicode_state *ks)
{
    log_enter("release_send");
    if (ks->release_sender)
    {
        log_write(DEBUG,"change release sender");
        ks->release_sender(ks, ks->release_code);
        ks->release_sender = NULL;
    }
    log_write(DEBUG,"quit");
}

static void release_cb(struct ela_event_source *source, int fd, uint32_t mask, void *data)
{
    log_enter("release_cb");
    struct unicode_state *ks = data;
    release_send(ks);
}

static void release_post(struct unicode_state *ks,code_sender_t *release_sender,uint32_t release_code)
{
    log_enter("release_post");
    if (ks->release_sender)
    {
        release_send(ks);
        ela_remove(ks->el, ks->release);
    }

    ks->release_sender = release_sender;
    ks->release_code = release_code;

    struct timeval tv = {0, 100000};
    ela_set_timeout(ks->el, ks->release, &tv, ELA_EVENT_ONCE);
    ela_add(ks->el, ks->release);
}



static void input_handler(struct term_input_state *input,uint8_t is_unicode, uint32_t code)
{
    log_enter("input_handler");

    struct unicode_state *ks = (void*)input;

    if (is_unicode) return send_unicode(ks, code);

    mapping_dump_target(code);

    const struct target_code *target = mapping_get(code);
    code_sender_t *sender;
    uint8_t repeatable = 0;

    switch (target->report)
    {
        case TARGET_UNICODE:
            sender = send_unicode;
            break;
        case TARGET_KEYBOARD:
            sender = send_kbd;
            repeatable = 1;
            break;
        case TARGET_CONSUMER:
            sender = send_cons;
            break;
        case TARGET_DESKTOP:
            sender = send_sysctl;
            break;
        default:
            return;
    }

    if (repeatable
        && target->usage == ks->current_code
        && sender == ks->release_sender) {
        ela_remove(ks->el, ks->release);
        ela_add(ks->el, ks->release);
    } else {
        sender(ks, target->usage);
        release_post(ks, sender, 0);
        ks->current_code = target->usage;
    }
}*/


static void send_unicode(uint32_t code)
{
    log_enter("send_unicode");
    foils_hid_input_report_send(client, 0,UNICODE, 1, &code, sizeof(code));
}

static void send_keyboard(uint32_t _code)
{
    log_enter("send_keyboard");
    uint8_t code = _code;
    foils_hid_input_report_send(client, 0, KEYBOARD, 1, &code, sizeof(code));
}

static void send_consumer(uint32_t _code)
{
    log_enter("send_consumer");
    uint16_t code = _code;
    foils_hid_input_report_send(client, 0, CONSUMER, 1, &code, sizeof(code));
}

static void send_sysctl(uint32_t _code)
{
    log_enter("send_sysctl");
    uint8_t code = _code;
    foils_hid_input_report_send(client, 0, SYSTEM, 1, &code, sizeof(code));
}//*/


static struct key_map *get_key_map(const char* key)
{
    for(int t=0;t<mapping_count;t++)
    {
        if (strcmp(mapping[t].lirc_key,key)==0) return &mapping[t];
    }

    return NULL;
}

static void add_timer_event()
{
    int error;

    log_write(DEBUG,"Adding timer event source");
    error=ela_add(eventLoop, timerEventSource);
    if (error)
    {
        log_write(ERROR,"Failed to add timer event source");
        return ;
    }
}

static void remove_timer_event()
{
    int error;

    log_write(DEBUG,"Removing timer event source");
    error=ela_remove(eventLoop, timerEventSource);
    if (error)
    {
        log_write(ERROR,"Failed to remove timer event source");
        return ;
    }

}



static void on_key_pressed(struct key_map* key)
{


    log_enter("on_key_pressed");

    log_write(INFO,"Key pressed: %s",key->lirc_key);

    switch(key->report_index)
    {
        case UNICODE:
            send_unicode(key->code);
            break;
        case KEYBOARD:
            send_keyboard(key->code);
            break;
        case CONSUMER:
            send_consumer(key->code);
            break;
        case SYSTEM:
            send_sysctl(key->code);
            break;

    }


}
static void on_key_released(struct key_map* key)
{
    log_enter("on_key_released");

    log_write(INFO,"Key released: %s",key->lirc_key);

    switch(key->report_index)
    {
        case UNICODE:
            send_unicode(0);
            break;//*/
        case KEYBOARD:
            send_keyboard(0);
            break;
        case CONSUMER:
            send_consumer(0);
            break;//*/
        case SYSTEM:
            send_sysctl(0);
            break;//*/

    }

}


static long timevaldiff(struct timeval *starttime, struct timeval *finishtime)
{
    long msec;
    msec=(finishtime->tv_sec-starttime->tv_sec)*1000;
    msec+=(finishtime->tv_usec-starttime->tv_usec)/1000;
    return msec;
}


static void timer_callback(struct ela_event_source *source, int fd, uint32_t mask, void *data)
{
    struct timeval currentTime;
    long elapsedTime;

    log_enter("Trace: timer_callback");

    gettimeofday (&currentTime, NULL);

    elapsedTime=timevaldiff(&lastPressedTime,&currentTime);
    if (elapsedTime>TIMER_INTERVAL)
    {
        remove_timer_event();
        timer_started=0;
        on_key_released(currentPressedKey);
    }

}

static void lirc_input_callback(struct ela_event_source *source, int fd, uint32_t mask, void *data)
{
    int readCount;
    char* buffer;
    long int code;
    int count;
    char* key;
    char* remote;
    int error;
    struct key_map* kmap;

    log_enter("lirc_input_callback");

    gettimeofday (&lastPressedTime, NULL);

    buffer=malloc(255*sizeof(char));
    key=malloc(255*sizeof(char));
    remote=malloc(255*sizeof(char));

    log_write(DEBUG,"Reading from file descriptor");
    readCount=read(fd, buffer, 255*sizeof(char));
    if (readCount==-1)
    {
        log_write(ERROR,"Failed to read from file descriptor");
        goto end;
    }
    else
    {
        log_write(DEBUG,"Read %i chars from file descriptor",readCount);
        //log_write(DEBUG,"Received from lirc: %s",buffer);
    }

    error=sscanf(buffer, "%lx %x %s %s",&code,&count,key,remote);

    if (error!=4)
    {
        log_write(ERROR,"Invalid data received");
        goto end;
    }
    else
    {
        log_write(DEBUG,"Received key %s from remote %s",key,remote);

        if (timer_started) goto end;

        log_write(DEBUG,"Searching for lirc key mapping");

        kmap=get_key_map(key);
        if (kmap==NULL)
        {
            log_write(DEBUG,"No key mapping found");
            goto end;
        }
        else
        {
            log_write(DEBUG,"Found key mapping to function %s",kmap->command);
            currentPressedKey=kmap;
        }

        timer_started=1;
        on_key_pressed(kmap);
        add_timer_event();

    }

    end:
    free(buffer);
    free(key);
    free(remote);
}

int remote_stop()
{
    log_enter("remote_stop");

    if (eventLoop) ela_exit(eventLoop);

    if (lircEventSource)
    {
        log_write(DEBUG,"Removing lirc event source");
        ela_remove(eventLoop, lircEventSource);
        log_write(DEBUG,"Disposing lirc event source");
        ela_source_free(eventLoop,lircEventSource);
        lircEventSource=NULL;
    }

    if (timerEventSource)
    {
        log_write(DEBUG,"Removing timer event source");
        ela_remove(eventLoop, timerEventSource);
        log_write(DEBUG,"Disposing timer event source");
        ela_source_free(eventLoop,timerEventSource);
        timerEventSource=NULL;
    }

    if (fileDescriptor>=0)
    {
        log_write(DEBUG,"Closing file descriptor");
        close(fileDescriptor);
        fileDescriptor=-1;
    }

    if (client)
    {
        log_write(DEBUG,"Disposing hid client");
        foils_hid_deinit(client);
        free(client);
        client=NULL;
    }

    if (eventLoop!=NULL)
    {
        log_write(DEBUG,"Disposing event loop");
        ela_close(eventLoop);
        eventLoop=NULL;
    }



    return 0;
}

static void read_mapping_file()
{
    FILE* file;
    int result;


    log_enter("read_mapping_file");

    log_write(DEBUG,"Opening mapping file");
    file=fopen("/etc/ir2hid/mapping.csv", "r");
    if (file==NULL)
    {
        log_write(ERROR,"Failed to open mapping file: %s",strerror(errno));
        return;
    }

    mapping_count=0;
    do
    {
        result=fscanf(file, "%[^\t]\t%i\t%i\t%[^\n]\n", mapping[mapping_count].command,&mapping[mapping_count].report_index,&mapping[mapping_count].code,mapping[mapping_count].lirc_key);
        if (result==4)
        {
            log_write(DEBUG,"Read key map for command %s: %s",mapping[mapping_count].command,mapping[mapping_count].lirc_key);
        }
        else if (result==EOF)
        {
            log_write(DEBUG,"Reached end of file");
        }
        else
        {
            log_write(ERROR,"Failed to read key map: %i",result);
        }
        mapping_count++;
    }while( (result==4) &&  (mapping_count<MAX_MAPPING_SIZE) );

    fclose(file);



}


int remote_start(const char* hostname,int port)
{
    int error;
    struct sockaddr_un addr;

    log_enter("remote_start");

    error=1;

    if (eventLoop!=NULL)
    {
        log_write(ERROR,"Event loop is already running");
        goto fin;
    }

    log_write(DEBUG,"Reading mapping file");
    read_mapping_file();

    log_write(DEBUG,"Creating event loop");
    eventLoop = ela_create(NULL);
    if (eventLoop==NULL)
    {
        log_write(ERROR,"Failed to create event loop");
        goto fin;
    }


    log_write(DEBUG,"Creating hid client");
    client=malloc(sizeof(struct foils_hid));
    error = foils_hid_init(client, eventLoop, &hidEventHandler, deviceDescriptors, 1);
    if (error)
    {
        log_write(ERROR,"Failed to create hid client: %s", strerror(error));
        goto fin;
    }


    log_write(DEBUG,"Connecting hid client");
    foils_hid_client_connect_hostname(client, hostname, port, 0);

    log_write(DEBUG,"Enabling hid client");
    foils_hid_device_enable(client, 0);

    log_write(DEBUG,"Allocating lirc event source");
    error=ela_source_alloc(eventLoop,lirc_input_callback,NULL,&lircEventSource);
    if (error)
    {
        lircEventSource=NULL;
        log_write(ERROR,"Failed to allocate lirc event source");
        goto fin;
    }


    log_write(DEBUG,"Setting up lirc fd");

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path,"/var/run/lirc/lircd");

    log_write(DEBUG,"Opening file descriptor");
    fileDescriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    if(fileDescriptor == -1)
    {
        log_write(ERROR,"Failed to open file descriptor");
        goto fin;
    }
    else
    {
        error=connect(fileDescriptor, (struct sockaddr *)&addr, sizeof(addr));
        if (error!=0)
        {
            log_write(ERROR,"Failed to open lirc socket %s: %s",addr.sun_path, strerror (errno));
            goto fin;
        }
    };

    log_write(DEBUG,"Setting lirc event source fd");
    error=ela_set_fd(eventLoop, lircEventSource, fileDescriptor, ELA_EVENT_READABLE); // 0 = stdin
    if (error)
    {
        log_write(ERROR,"Failed to set lirc event source fd");
        goto fin;
    }
    log_write(DEBUG,"Adding lirc event source");
    error=ela_add(eventLoop, lircEventSource);
    if (error)
    {
        log_write(ERROR,"Failed to add lirc event source");
        goto fin;
    }



    log_write(DEBUG,"Allocating timer event source");
    error=ela_source_alloc(eventLoop,timer_callback,NULL,&timerEventSource);
    if (error)
    {
        timerEventSource=NULL;
        log_write(ERROR,"Failed to allocate timer event source");
        goto fin;
    }
    log_write(DEBUG,"Setting timer event source timeout");

    struct timeval timer={0, TIMER_INTERVAL*1000};;

    error=ela_set_timeout(eventLoop, timerEventSource, &timer, ELA_EVENT_ONCE);
    if (error)
    {
        log_write(ERROR,"Failed set timer event source timeout");
        goto fin;
    }





    log_write(DEBUG,"Running event loop");
    ela_run(eventLoop);
    log_write(DEBUG,"Event loop terminated");

    error=0;
    //term_input_deinit(&ks.input_state);

    fin:

    remote_stop();

    return error;
}
