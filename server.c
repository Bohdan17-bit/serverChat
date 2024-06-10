#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <ifaddrs.h>
#include <pthread.h>

#define MAX_CLIENTS_CHAT   8
#define MAX_QUEUE_LENGTH   16
#define MAX_SIZE_USERNAME  32
#define SMALL_BUFFER_SIZE  64
#define MAX_SIZE_MESSAGE   512

#define PORT 7777

struct thread_start_data {
   int *sockets;
   char usernames[MAX_CLIENTS_CHAT][MAX_SIZE_USERNAME];
   int current_socket;
   pthread_mutex_t *users_mutex;
};

enum fsm_states_user {
   fsm_name,
   fsm_message,
   fsm_error,
   fsm_finish,
};

struct session {
   char username[MAX_SIZE_USERNAME];
   char message[MAX_SIZE_MESSAGE];
   enum fsm_states_user state;
   int unique_socket;
};

void session_send_message(struct thread_start_data *data, struct session *sess, char *response);
void session_login(struct thread_start_data *data, struct session *sess, char *response);
void error_message() {
    printf("Something was wrong! %s\n", strerror(errno));
}
// needed for implementation of finite-state machine
void session_fsm_step(struct thread_start_data *users, struct session *sess, char *answer) {
   switch(sess->state) {
      case fsm_name:
         session_login(users, sess, answer);
         break;
      case fsm_message:
         session_send_message(users, sess, answer);
         break;
      case fsm_finish:
      case fsm_error:
   }
}

void session_send_message(struct thread_start_data *data, struct session *sess, char *response) {
   pthread_mutex_lock(data->users_mutex);
   int reserve = 3; // additional place for str ' : '
   char response_with_sender_username[MAX_SIZE_MESSAGE + MAX_SIZE_USERNAME + reserve];
   memset(response_with_sender_username, 0, sizeof(response_with_sender_username));
   // searching for nickname of sender using his socket to create complex answer
   for(int i = 0; i < MAX_CLIENTS_CHAT; i++) {
      if(data->sockets[i] == sess->unique_socket) {
         strcpy(response_with_sender_username,data->usernames[i]);
         strcat(response_with_sender_username, " : ");
         strcat(response_with_sender_username, response);
         break; // here we add nickname sender in full message
      }
   }
   // case when user want to send message to particular participant
   if(response[0] == '!') { // check if it command from user
      response++; // move on 1 symbol right to cut off first symbol '!'
      char empty[] = " ";
      char *who_get_message = strtok(response, empty); // cut off receiver name
      // we should find receiver here 
      for(int i = 0; i < MAX_CLIENTS_CHAT; i++) {
         if(strcmp(data->usernames[i], who_get_message) == 0) {
            send(sess->unique_socket, response_with_sender_username, strlen(response_with_sender_username), 0);
            send(data->sockets[i], response_with_sender_username, strlen(response_with_sender_username), 0);
            // return to sender and send to receiver in one time
            break;
         }
      }
   }
   // if not particular user is receiver - sendinf to all users (except zero sockets)
   else {
      for(int i  = 0; i < MAX_CLIENTS_CHAT; i++) {
         if(data->sockets[i] != 0) {
            send(data->sockets[i], response_with_sender_username, strlen(response_with_sender_username), 0);
         }
      }
   }
   pthread_mutex_unlock(data->users_mutex);
}

void session_login(struct thread_start_data *data, struct session *sess, char *response) {
   pthread_mutex_lock(data->users_mutex);
   sess->state = fsm_message; 
   // move on next step after login
   for(int i = 0; i < MAX_CLIENTS_CHAT; i++) {
      if(strcmp(data->usernames[i], response) == 0) {
         sess->state = fsm_name;
         char error_login[] = "Server:This login is already exist!";
         send(sess->unique_socket, error_login, strlen(error_login), 0);
         pthread_mutex_unlock(data->users_mutex);
         return;
      }
   }
   if(sess->state == fsm_message) { // if we didn't find this login, can save username
      for(int i = 0; i < MAX_CLIENTS_CHAT; i++) {
         if(data->usernames[i][0] == 0) { 
            // if first symbol is zero, - we can save username into data
            strcpy(data->usernames[i], response); 
            data->sockets[i] = sess->unique_socket;
            // also saving socket for user into data
            const char *success_login = "Server:You were successfully login! Your name: ";
            int full_response_size = strlen(success_login) + strlen(response) + 1;
            char *full_response = malloc(full_response_size);
            strcpy(full_response, success_login);
            strcat(full_response, response);
            // make complex answer with name of user and send it
            send(sess->unique_socket, full_response, strlen(full_response), 0);
            free(full_response);
            break;
         }
      }
   }
   pthread_mutex_unlock(data->users_mutex);
}

void *worker_thread(void *data) {
   struct thread_start_data *clients = data;
   char buffer[MAX_SIZE_MESSAGE];

   pthread_mutex_t * users_mutex = clients->users_mutex;
   // create a user session to save active client socket
   struct session *user_session = malloc(sizeof(struct session));
   memset(user_session, 0, sizeof(struct session));
   user_session->unique_socket = clients->current_socket;

   char message_greeetings[] = "Server:Welcome!";
   send(user_session->unique_socket, message_greeetings, strlen(message_greeetings), 0);

   char message_request_login[] = "Server:Please, input your login...";
   send(user_session->unique_socket, message_request_login, strlen(message_request_login), 0);
   
   int bytes_read = 0;
   while((bytes_read = recv(user_session->unique_socket, buffer, sizeof(buffer) - 1, 0)) > 0) {
      buffer[bytes_read] = '\0';
      printf("Received message from socket: %d : %s\n", user_session->unique_socket, buffer);
      session_fsm_step(data, user_session, buffer);
   }
   
   close(user_session->unique_socket);
   user_session->state = fsm_name;
   pthread_mutex_lock(users_mutex);
   for(int i = 0; i < MAX_CLIENTS_CHAT; i++) {
      if(user_session->unique_socket == clients->sockets[i]) {
         clients->sockets[i] = 0;
         memset(clients->usernames[i], 0, sizeof(MAX_SIZE_USERNAME));
         break;
      }
   }
   pthread_mutex_unlock(users_mutex);
   printf("Client on socket %d disconnected\n", user_session->unique_socket);

   if(bytes_read == -1) {
      error_message();
   }   
   
   free(user_session);
   return NULL;
}

int main() {
   int listening_socket = socket(AF_INET, SOCK_STREAM, 0);
   if (listening_socket == -1) {
        fprintf(stderr, "Can't create a socket! exit!\n");
        return -1;
   }

   struct sockaddr_in server_addr;
   struct ifaddrs *addresses;

   if (getifaddrs(&addresses) == -1) {
      printf("getifaddrs call failed!\n");
      return -1;
   }

   struct ifaddrs *adrs = addresses;

   while (adrs) {
      if (adrs->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *server = (struct sockaddr_in *)adrs->ifa_addr;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(server->sin_addr), ip, INET_ADDRSTRLEN);
            if (strcmp(ip, "localhost") != 0 && strcmp(ip, "0.0.0.0") != 0 &&
                strcmp(ip, "127.0.0.1") != 0) {
                server_addr.sin_addr = server->sin_addr;
                break;
            }
        }
        adrs = adrs->ifa_next;
    }

   freeifaddrs(addresses);

   server_addr.sin_family = AF_INET;
   server_addr.sin_port = htons(PORT);

   int opt = 1;
   setsockopt(listening_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

   int res = bind(listening_socket, (struct sockaddr *)&server_addr, sizeof(server_addr));

   if (res == -1) {
      error_message();
      return -1;
   }
   
   int listen_ok = listen(listening_socket, MAX_QUEUE_LENGTH);

   if (listen_ok == -1) {
      error_message();
      return -1;
   }

   char server_ip[SMALL_BUFFER_SIZE];
   inet_ntop(AF_INET, &(server_addr.sin_addr), server_ip, SMALL_BUFFER_SIZE);
   printf("Server working with IP: %s and port : %d\n", server_ip, PORT);

   struct sockaddr_in client_addr;

   struct thread_start_data data;
   int sockets[MAX_CLIENTS_CHAT] = {0};
   pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

   data.sockets = sockets;
   data.users_mutex = &clients_mutex;
   
   while(1) {
      int socket_client;
      socklen_t slen = sizeof(client_addr);
      socket_client = accept(listening_socket, (struct sockaddr*) &client_addr, &slen);
      if(socket_client < 0) {
         error_message();
         continue;
      }

      printf("New connection was established with socket : %d\n", socket_client);

      pthread_mutex_lock(&clients_mutex);
      int added = 0;
      for(int i = 0; i < MAX_CLIENTS_CHAT; i++) {
         if(sockets[i] == 0) {
            sockets[i] = socket_client;
            added = 1;
            break;
         } 
      }
      pthread_mutex_unlock(&clients_mutex);

      if(!added) {
         char message_error[] = "Too much users in chat! Connection refused on socket!";
         printf("%s with socket : %d\n", message_error, socket_client);
         send(socket_client, message_error, strlen(message_error), 0);
         char message_warning[] = "Your socket was reset.";
         send(socket_client, message_warning, strlen(message_warning), 0);
         close(socket_client);
         continue;
      }

      data.current_socket = socket_client;
      pthread_t client_thread;
      if(pthread_create(&client_thread, NULL, worker_thread, &data) != 0) {
         error_message();
         close(socket_client);
         continue;
      }

      pthread_detach(client_thread);
   }

   close(listening_socket);
   return 0;
}