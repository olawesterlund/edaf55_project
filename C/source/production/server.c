#include "server_common.h"
#include "camera_monitor.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>  // for memcpy, memset, strlen
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>



// some diagnostic printouts
// #define INFO
#undef INFO
#define DEBUG 1

// more diagnostic printouts
#undef DEBUG

// the strncasecmp function is not in the ISO C standard
// #define USE_POSIX_FUNCTION

#ifndef MOTION_PORT
#define MOTION_PORT 9090
#endif

struct client{
  bool isConnected;
  byte sendBuff[BUFSIZE];
  #ifdef USE_CAMERA
  byte* frame_data;
  #endif
};



struct global_state {
  int listenfd;
  int modefd;
  int running;
  int quit;
  pthread_t main_thread;
  pthread_t client_thread;
  pthread_t send_picture_thread;
  //pthread_t ui_thread;
  pthread_t bg_thread;
  pthread_t update_mode_thread;
  struct pollfd pfd;
  int motionfd;
  #ifdef USE_CAMERA
  camera* cam;
  #endif
};


int client_write_string(struct client* client);
int client_write_n(byte* pic_packet, size_t n);
void* take_picture_task(void *ctxt);
void* send_picture_task(void *ctxt);
void server_quit(struct global_state* s);
void signal_to_bg_task();
int is_running(struct global_state* state);

/////////////// camera stuff

#define ERR_OPEN_STREAM 1
#define ERR_GET_FRAME 2

struct client;

static pthread_mutex_t global_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t global_cond = PTHREAD_COND_INITIALIZER;

#ifdef USE_CAMERA

/* try to open capture interface.
* returns 0 on success
*/
int try_open_camera(struct global_state* state)
{
  state->cam = camera_open();
  if (!state->cam){ // Check if null
    printf("axism3006v: Stream is null, can't connect to camera");
    return ERR_OPEN_STREAM;
  }
  return 0;
}

void close_camera(struct global_state* state)
{
  if(state->cam) {
    camera_close(state->cam);
    state->cam = NULL;
  }
}


/* Sets up the packet structure in client->sendBuff.
* This is a minimal HTTP header for an image/jpeg
*
* sets client->frame_data to the beginning of the frame
* in the packet
*
* returns size of entire packet, or -1 if frame too large
*/
ssize_t setup_packet(byte * pic_packet, uint64_t time_stamp, uint32_t frame_sz, byte * data)
{
  size_t header_size = 10;
  time_stamp = time_stamp / 1000000;
  memcpy(pic_packet, &time_stamp, 8);
  memcpy(pic_packet+8, &frame_sz, 2);
  memcpy(pic_packet+header_size, data, frame_sz);

  return header_size+frame_sz;
}

int client_send_frame(struct client* client)
{
  // this should really be a compile-time check, but that is complicated
  #ifndef DISABLE_SANITY_CHECKS
  if(sizeof(size_t) != sizeof(uint32_t)) {
    printf("sizeof(size_t)=%d, sizeof(uint32_t)=%d\n", sizeof(size_t), sizeof(uint32_t));
    printf("Not sending frame, size sanity check failed\n");
    return 2;
  }
  #endif

  byte pic_packet[BUFSIZE];

  int result;
  get_packet(pic_packet);

  ssize_t packet_sz = get_packet_size();
  //printf("Size of packet sent: %zd\n", packet_sz);


  int written=client_write_n(pic_packet, packet_sz);

  if(written != packet_sz) {
    printf("WARNING! packet_sz=" FORMAT_FOR_SIZE_T ", written=%d\n", packet_sz, written);
    result = 3;
  } else {
    result = 0;
  }
  return result;
}

int client_save_frame(struct client* client, frame* fr)
{
  // this should really be a compile-time check, but that is complicated
  #ifndef DISABLE_SANITY_CHECKS
  if(sizeof(size_t) != sizeof(uint32_t)) {
    printf("sizeof(size_t)=%d, sizeof(uint32_t)=%d\n", sizeof(size_t), sizeof(uint32_t));
    printf("Not sending frame, size sanity check failed\n");
    return 2;
  }
  #endif

  size_t frame_sz = get_frame_size(fr);
  capture_time time_stamp = get_frame_timestamp(fr);
  printf("timestamp is: %llu\n",time_stamp);
  //printf("%" PRIu64 "\n", time_stamp);
  byte * data = get_frame_bytes(fr);

  byte pic_packet[BUFSIZE];

  ssize_t packet_sz = setup_packet(pic_packet, time_stamp, frame_sz, data);


  //FILE *fp;
  //fp = fopen("./test.txt", "w");
  //int i;
  //for(i = 0; i < frame_sz; i++) {
  //   fprintf(fp, "%c", data[i]);
  //   fprintf(stdout, "%c", data[i]);
  //}
  //fclose(fp);

  save_packet_size(packet_sz);

  save_packet(pic_packet);

  int result;

  if(packet_sz < 0) {
    printf("Frame too big for send buffer(" FORMAT_FOR_SIZE_T " > " FORMAT_FOR_SIZE_T "), skipping.\n", frame_sz, sizeof(client->sendBuff));
    result = 1;
  } else {
    result = 0;

    #ifdef DEBUG
    printf("encode size:" FORMAT_FOR_SIZE_T "\n",  frame_sz);
    printf("sizeof(size_t)=" FORMAT_FOR_SIZE_T ", sizeof(uint32_t)=" FORMAT_FOR_SIZE_T "\n", sizeof(size_t), sizeof(uint32_t));
    #endif
  }
  return result;
}

/* get frame from camera and send to client
* returns zero on success
*/
int try_send_frame(struct client* client)
{
  int result=-1;

  if((result = client_send_frame(client))) {
    printf("Warning: client_save_frame returned %d\n", result);
  }
  return result;
}
#endif


int try_get_frame(struct client* client)
{
  int result=-1;
  frame *fr = fr = camera_get_frame(cam_mon->cam);


  if(fr) {
    if((result = client_save_frame(client, fr))) {
      printf("Warning: client_save_frame returned %d\n", result);
    }

    frame_free(fr);
  } else {
    return ERR_GET_FRAME;
  }
  return result;
}


int client_write_string(struct client* client)
{
  return write_string(cam_mon->connfd, client->sendBuff);
}

/*
* Returns number of bytes written.
* Returns -1 if an error occurs and sets errno.
* Note: an error occurs if the web browser closes the connection
*   (might happen when reloading too frequently)
*/
int client_write_n(byte * send_data, size_t n)
{
  return write_n(cam_mon->connfd, send_data,n);
}

/* The function for a thread that processes a single client request.
* The client struct pointer is passed as void* due to the pthreads API
*/
void* take_picture_task(void *ctxt)
{

  struct client* client = ctxt;
  while(client->isConnected)
  {
    //mutex logic
    pthread_mutex_lock(&global_mutex);
    while(get_pic_take_send() != TAKE_PIC_STATE)
    {
      pthread_cond_wait(&global_cond, &global_mutex);
    }
    //mutex logic

    memset(client->sendBuff, 0, sizeof(client->sendBuff));
    int mode = get_mode();
    if (mode == IDLE_MODE)
    sleep(5);



    int cres=0;
    if( !cam_mon->cam || (cres=try_get_frame(client))) {
      printf("ERROR getting frame from camera: %d\n",cres);
    }

    //mutex logic
    flip_pic_take_send();
    pthread_mutex_unlock(&global_mutex);
    pthread_cond_broadcast(&global_cond);
    //mutex logic

  }

  return (void*) (intptr_t) close(cam_mon->connfd);
}


void* send_picture_task(void *ctxt)
{
  struct client* client = ctxt;
  while(client->isConnected)
  {
    //mutex logic
    pthread_mutex_lock(&global_mutex);
    while(get_pic_take_send() != SEND_PIC_STATE)
    {
      pthread_cond_wait(&global_cond, &global_mutex);
    }
    //mutex logic

    int cres=0;
    if( !cam_mon->cam || (cres=try_send_frame(client))) {
      printf("ERROR getting frame from camera: %d\n",cres);
    }

    //mutex logic
    flip_pic_take_send();
    pthread_mutex_unlock(&global_mutex);
    pthread_cond_broadcast(&global_cond);
    //mutex logic

  }

  return (void*) (intptr_t) close(cam_mon->connfd);
}

/* The function for a thread that processes a single client request.
* The client struct pointer is passed as void* due to the pthreads API
*/
void* update_mode_task(void *ctxt)
{
  struct client* client = ctxt;
  char buf[1] = {};
  while(client->isConnected)
  {
    int rres = read(cam_mon->connmodefd, buf, 1);
    //printf("received mode update: %d\n", buf[0]);
    printf("rres is: %d\nbuf is: %d\n", rres, buf[0]);
    if(rres == 0) {
      printf("client disconnected!");
      client->isConnected = false;
    }
    if(rres < 0) {
      perror("update_mode_task: read");
      return (void*) (intptr_t) errno;
    }
    #ifdef DEBUG
    printf("read: rres = %d\n", rres);
    printf("buf[%s]\n", buf);
    #endif

    set_mode(buf[0]);
  }
  return (void*) (intptr_t) close(cam_mon->connmodefd);
}

/* signal the global condition variable
*/
void signal_to_bg_task()
{
  pthread_mutex_lock(&global_mutex);
  pthread_cond_broadcast(&global_cond);
  pthread_mutex_unlock(&global_mutex);
}
/* set flags to signal shutdown and
* signal global condition variable
*/
void server_quit(struct global_state* s)
{
  printf("Quitting\n");
  pthread_mutex_lock(&global_mutex);
  s->quit=1;
  s->running=0;
  pthread_cond_broadcast(&global_cond);
  pthread_mutex_unlock(&global_mutex);
}

void* bg_task(void *ctxt)
{
  struct global_state* s = ctxt;

  pthread_mutex_lock(&global_mutex);
  while(s->running){
    pthread_cond_wait(&global_cond, &global_mutex);
    #ifdef INFO
    //printf("bg_task: global_cond was signalled\n");
    #endif
  }
  pthread_mutex_unlock(&global_mutex);
  return 0;
}

int serve_clients(struct global_state* state);

void* main_task(void *ctxt)
{
  struct global_state* state = ctxt;
  return (void*) (intptr_t) serve_clients(state);
}

int try_accept(struct global_state* state, struct client* client)
{
  int result = 0;
  if( !state->cam && try_open_camera(state)) {
    printf("Error opening camera\n");
    return 1;
  }
  cam_mon->cam = state->cam;
  cam_mon->connfd = accept(state->listenfd, (struct sockaddr*)NULL, NULL);
  cam_mon->connmodefd = accept(state->modefd, (struct sockaddr*)NULL, NULL);

  client->isConnected = true;

  if(cam_mon->connfd < 0) {
    result = errno;
  } else {
    #ifdef INFO
    printf("accepted connection\n");
    #endif

    // serve clients in separate thread, for four reasons
    // 1. Illustrate threading
    // 2. Not blocking the user interface while serving client
    // 3. Prepare for serving multiple clients concurrently
    // 4. The AXIS software requires capture to be run outside the main thread
    if (pthread_create(&state->client_thread, 0, take_picture_task, client) ||
    pthread_create(&state->send_picture_thread, 0, send_picture_task, client) ||
    pthread_create(&state->update_mode_thread, 0, update_mode_task, client)) {
      printf("Error pthread_create()\n");
      perror("creating");
      result = errno;
    } else {
      void* status;
      if(pthread_join(state->client_thread, &status) ||
      pthread_join(state->send_picture_thread, &status) ||
      pthread_join(state->update_mode_thread, &status)){
        perror("join");
        result = errno;
      } else {
        #ifdef DEBUG
        printf("Join: status = %d\n", (int)(intptr_t) status);
        #endif
      }
    }
  }
  return result;
}


static int create_threads(struct global_state* state)
{
  pthread_mutex_lock(&global_mutex);
  int result = 0;
  if (pthread_create(&state->bg_thread, 0, bg_task, state)) {
    printf("Error pthread_create()\n");
    perror("creating bg thread");
    result = errno;
    state->running=0;
    goto failed_to_start_bg_thread;
  }
  if (pthread_create(&state->main_thread, 0, main_task, state)) {
    printf("Error pthread_create()\n");
    perror("creating main thread");
    result = errno;
    state->running=0;
    goto failed_to_start_main_thread;
  }

  failed_to_start_main_thread:
  failed_to_start_bg_thread:
  pthread_mutex_unlock(&global_mutex);
  return result;
}
static void join_bg_thread(pthread_t* bg_thread, const char* msg)
{
  void* status;
  if(pthread_join(*bg_thread, &status)){
    perror("join bg_tread");
  } else {
    printf("Join bg thread (%s): status = " FORMAT_FOR_SIZE_T "\n", msg, (intptr_t) status);
  }
}

static void client_init(struct client* client)
{
  cam_mon->cam=NULL;
  cam_mon->connfd=-1;
}
int serve_clients(struct global_state* state)
{
  init();
  pthread_mutex_init(&camera_mutex, NULL);

  int result=0;
  state->pfd.fd = state->listenfd;
  state->pfd.events=POLLIN;
while(1) {
  //while(is_running(state)) {
    int ret; // result of poll

    struct client* client = malloc(sizeof(*client));
    if(client == NULL) {
      perror("malloc client");
      result = errno;
      goto failed_to_alloc_client;
    }
    client_init(client);
    #ifdef DEBUG
    printf("serve_clients: listening\n");
    #endif

    do {
      ret = poll(&state->pfd, POLLIN, 2000);
      #ifdef DEBUG_VERBOSE
      printf("client poll returns %d\n", ret);
      #endif
    } while(ret==0 && is_running(state));

    if(ret <0) {
      perror("poll");
      result = errno;
      server_quit(state);
    } else if (is_running(state)) {
      if(try_accept(state, client)) {
        perror("try_accept");
        result = errno;
        server_quit(state);
      };
    }
    client->isConnected = false;
    free(client);
  }
  failed_to_alloc_client:
  close_camera(state);
  return result;
}

void init_global_state(struct global_state* state)
{
  pthread_mutex_lock(&global_mutex);
  state->running=0;
  state->quit=0;
  state->cam=NULL;
  pthread_mutex_unlock(&global_mutex);
}

int is_running(struct global_state* state)
{
  int result=0;
  pthread_mutex_lock(&global_mutex);
  result =  state->running;
  pthread_mutex_unlock(&global_mutex);
  return result;
}
int create_socket(struct global_state* state)
{
  int reuse;
  state->listenfd = socket(AF_INET, SOCK_STREAM, 0);
  state->motionfd = socket(AF_INET, SOCK_STREAM, 0);
  state->modefd   = socket(AF_INET, SOCK_STREAM, 0);

  if(state->listenfd < 0 || state->motionfd < 0 || state->modefd < 0) {
    perror("socket");
    return errno;
  }

  reuse = 1;
  set_socket_sigpipe_option(state->listenfd);
  if(setsockopt(state->listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))){
    perror("setsockopt listenfd");
    return errno;
  }

  return 0;
}

int bind_and_listen(int * member, int port)
{
  struct sockaddr_in serv_addr;

  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  serv_addr.sin_port = htons(port);

  if( bind(*member, (struct sockaddr*)&serv_addr, sizeof(serv_addr))) {
    perror("bind fd");
    return errno;
  }

  if(listen(*member, 10)){
    perror("listen fd");
    return errno;
  }

  return 0;
}


int main(int argc, char *argv[])
{
  int port1;
  int port2;
  struct global_state state;
  int result=0;

  if(argc==2) {
    printf("interpreting %s as port number for image\n", argv[1]);
    port1 = atoi(argv[1]);
    port2 = 9998;
  } else if(argc==3) {
    printf("interpreting %s as port number for image\n", argv[1]);
    printf("interpreting %s as port number for motion\n", argv[2]);
    port1 = atoi(argv[1]);
    port2 = atoi(argv[2]);
  } else {
    port1 = 9999;
    port2 = 9998;
  }
  printf("starting on port %d and port %d\n", port1, port2);

  init_global_state(&state);

  if(create_socket(&state)) {
    goto no_server_socket;
  }

  if(bind_and_listen(&(state.listenfd), port1)) {
    goto failed_to_listen;
  }

  if(bind_and_listen(&(state.modefd), port2)) {
    goto failed_to_listen_mode;
  }

  pthread_mutex_lock(&global_mutex);
  state.running = 1;
  pthread_mutex_unlock(&global_mutex);

  if(create_threads(&state)) {
    goto failed_to_start_threads;
  }

  join_bg_thread(&state.bg_thread, "bg_thread");
  join_bg_thread(&state.main_thread, "main_thread");
  join_bg_thread(&state.update_mode_thread, "update_mode_thread");

  failed_to_start_threads:
  failed_to_listen:
  failed_to_listen_mode:
  if(close(state.listenfd)) perror("closing listenfd");
  no_server_socket:
  return result;
}
