#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

// op code + req_path + notif_path
#define BUFFER_SIZE 81

struct Session {
    int req_pipe_fd;
    int notif_pipe_fd;
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
};

static struct Session session = { .req_pipe_fd = -1, .notif_pipe_fd = -1 };



int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
    int fserv;
    int op_code = 1;
    char buffer[BUFFER_SIZE] =  {0};

    buffer[0] = op_code;

    strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
    strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);

    strncpy(&buffer[1], req_pipe_path, MAX_PIPE_PATH_LENGTH);
    strncpy(&buffer[41], notif_pipe_path, MAX_PIPE_PATH_LENGTH);

    unlink(req_pipe_path);
    unlink(notif_pipe_path);

    if(mkfifo(req_pipe_path, 0666) == -1){
      return 1;
    }
    if(mkfifo(notif_pipe_path, 0666) == -1){
      return 1;
    }

    if ((fserv = open (server_pipe_path,O_WRONLY)) < 0){
      return 1;
    }

    write(fserv, buffer, BUFFER_SIZE);
    close(fserv);

    if ((session.notif_pipe_fd = open (notif_pipe_path,O_RDONLY)) < 0){
      return 1;
    }
    if ((session.req_pipe_fd = open (req_pipe_path,O_WRONLY)) < 0){
      return 1;
    }

    char response[2];
    read(session.notif_pipe_fd, response, 2);

    if (response[1] != 0) return 1;

    return 0;
}

int pacman_disconnect() {
    char op_code = 2;

    if ((session.req_pipe_fd) != -1){
       write(session.req_pipe_fd, &op_code,1);
       close(session.req_pipe_fd);
    }
    if ((session.notif_pipe_fd) != -1){
        close(session.notif_pipe_fd);
    }

    unlink(session.req_pipe_path);
    unlink(session.notif_pipe_path);

    session.req_pipe_fd = -1;
    return 0;
}

void pacman_play(char command) {
  if(session.req_pipe_fd < 0){
    return;
  }
  char buffer[2];
  buffer[0] = 3;
  buffer[1] = command;

  write(session.req_pipe_fd, buffer, 2);
}

Board receive_board_update(void) {
  Board board = {0};

  if(session.notif_pipe_fd < 0){
    board.game_over = 1;
    return board;
  }

  char op_code;
  int n = read(session.notif_pipe_fd, &op_code, 1);

  if(n <= 0 || op_code != 4){
    board.game_over = 1;
    return board;
  }

  int header[6];
  read(session.notif_pipe_fd, header, sizeof(header));
  board.width = header[0];
  board.height = header[1];
  board.tempo = header[2];
  board.victory = header[3];
  board.game_over = header[4];
  board.accumulated_points = header[5];

  int size = board.width * board.height;
  board.data = malloc(size);

  if(read(session.notif_pipe_fd, board.data, size) < size){
    free(board.data);
    board.data = NULL;
    board.game_over = 1;
  }
  return board;

}

