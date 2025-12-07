#include "board.h"
#include "display.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>

pthread_mutex_t board_mutex = PTHREAD_MUTEX_INITIALIZER;
board_t *g_board = NULL;

FILE * debugfile;

// Helper private function to find and kill pacman at specific position
static int find_and_kill_pacman(board_t* board, int new_x, int new_y) {
    for (int p = 0; p < board->n_pacmans; p++) {
        pacman_t* pac = &board->pacmans[p];
        if (pac->pos_x == new_x && pac->pos_y == new_y && pac->alive) {
            pac->alive = 0;
            kill_pacman(board, p);
            return DEAD_PACMAN;
        }
    }
    return VALID_MOVE;
}

// Helper private function for getting board position index
static inline int get_board_index(board_t* board, int x, int y) {
    return y * board->width + x;
}

// Helper private function for checking valid position
static inline int is_valid_position(board_t* board, int x, int y) {
    return (x >= 0 && x < board->width) && (y >= 0 && y < board->height); // Inside of the board boundaries
}

void sleep_ms(int milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

int move_pacman(board_t* board, int pacman_index, command_t* command) {
    if (pacman_index < 0 || !board->pacmans[pacman_index].alive) {
        return DEAD_PACMAN; // Invalid or dead pacman
    }

    pacman_t* pac = &board->pacmans[pacman_index];
    int new_x = pac->pos_x;
    int new_y = pac->pos_y;

    // check passo
    if (pac->waiting > 0) {
        pac->waiting -= 1;
        return VALID_MOVE;        
    }
    pac->waiting = pac->passo;

    char direction = command->command;

    if (direction == 'R') {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    // Calculate new position based on direction
    switch (direction) {
        case 'W': // Up
            new_y--;
            break;
        case 'S': // Down
            new_y++;
            break;
        case 'A': // Left
            new_x--;
            break;
        case 'D': // Right
            new_x++;
            break;
        case 'T': // Wait
            if (command->turns_left == 1) {
                pac->current_move += 1; // move on
                command->turns_left = command->turns;
            }
            else command->turns_left -= 1;
            return VALID_MOVE;
        default:
            return INVALID_MOVE; // Invalid direction
    }

    // Logic for the WASD movement
    // Only increment current_move if we have predefined moves
    if (pac->n_moves > 0) {
        pac->current_move += 1;
    }

    // Check boundaries
    if (!is_valid_position(board, new_x, new_y)) {
        return INVALID_MOVE;
    }

    int new_index = get_board_index(board, new_x, new_y);
    int old_index = get_board_index(board, pac->pos_x, pac->pos_y);
    char target_content = board->board[new_index].content;

    if (board->board[new_index].has_portal) {
        board->board[old_index].content = ' ';
        board->board[new_index].content = 'P';
        return REACHED_PORTAL;
    }

    // Check for walls
    if (target_content == 'W') {
        return INVALID_MOVE;
    }

    // Check for ghosts
    if (target_content == 'M') {
        kill_pacman(board, pacman_index);
        return DEAD_PACMAN;
    }

    // Collect points
    if (board->board[new_index].has_dot) {
        pac->points++;
        board->board[new_index].has_dot = 0;
    }

    board->board[old_index].content = ' ';
    pac->pos_x = new_x;
    pac->pos_y = new_y;
    board->board[new_index].content = 'P';

    return VALID_MOVE;
}

// Helper private function for charged ghost movement in one direction
static int move_ghost_charged_direction(board_t* board, ghost_t* ghost, char direction, int* new_x, int* new_y) {
    int x = ghost->pos_x;
    int y = ghost->pos_y;
    *new_x = x;
    *new_y = y;
    
    switch (direction) {
        case 'W': // Up
            if (y == 0) return INVALID_MOVE;
            *new_y = 0; // In case there is no colision
            for (int i = y - 1; i >= 0; i--) {
                char target_content = board->board[get_board_index(board, x, i)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_y = i + 1; // stop before colision
                    return VALID_MOVE;
                }
                else if (target_content == 'P') {
                    *new_y = i;
                    return find_and_kill_pacman(board, *new_x, *new_y);
                }
            }
            break;

        case 'S': // Down
            if (y == board->height - 1) return INVALID_MOVE;
            *new_y = board->height - 1; // In case there is no colision
            for (int i = y + 1; i < board->height; i++) {
                char target_content = board->board[get_board_index(board, x, i)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_y = i - 1; // stop before colision
                    return VALID_MOVE;
                }
                if (target_content == 'P') {
                    *new_y = i;
                    return find_and_kill_pacman(board, *new_x, *new_y);
                }
            }
            break;

        case 'A': // Left
            if (x == 0) return INVALID_MOVE;
            *new_x = 0; // In case there is no colision
            for (int j = x - 1; j >= 0; j--) {
                char target_content = board->board[get_board_index(board, j, y)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_x = j + 1; // stop before colision
                    return VALID_MOVE;
                }
                if (target_content == 'P') {
                    *new_x = j;
                    return find_and_kill_pacman(board, *new_x, *new_y);
                }
            }
            break;

        case 'D': // Right
            if (x == board->width - 1) return INVALID_MOVE;
            *new_x = board->width - 1; // In case there is no colision
            for (int j = x + 1; j < board->width; j++) {
                char target_content = board->board[get_board_index(board, j, y)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_x = j - 1; // stop before colision
                    return VALID_MOVE;
                }
                if (target_content == 'P') {
                    *new_x = j;
                    return find_and_kill_pacman(board, *new_x, *new_y);
                }
            }
            break;
        default:
            debug("DEFAULT CHARGED MOVE - direction = %c\n", direction);
            return INVALID_MOVE;
    }
    return VALID_MOVE;
}   

int move_ghost_charged(board_t* board, int ghost_index, char direction) {
    ghost_t* ghost = &board->ghosts[ghost_index];
    int x = ghost->pos_x;
    int y = ghost->pos_y;
    int new_x = x;
    int new_y = y;

    ghost->charged = 0; //uncharge
    int result = move_ghost_charged_direction(board, ghost, direction, &new_x, &new_y);
    if (result == INVALID_MOVE) {
        debug("DEFAULT CHARGED MOVE - direction = %c\n", direction);
        return INVALID_MOVE;
    }

    // Get board indices
    int old_index = get_board_index(board, ghost->pos_x, ghost->pos_y);
    int new_index = get_board_index(board, new_x, new_y);

    // Update board - clear old position (restore what was there)
    board->board[old_index].content = ' '; // Or restore the dot if ghost was on one
    // Update ghost position
    ghost->pos_x = new_x;
    ghost->pos_y = new_y;
    // Update board - set new position
    board->board[new_index].content = 'M';
    return result;
}

int move_ghost(board_t* board, int ghost_index, command_t* command) {
    ghost_t* ghost = &board->ghosts[ghost_index];
    int new_x = ghost->pos_x;
    int new_y = ghost->pos_y;

    // check passo
    if (ghost->waiting > 0) {
        ghost->waiting -= 1;
        return VALID_MOVE;
    }
    ghost->waiting = ghost->passo;

    char direction = command->command;
    
    if (direction == 'R') {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    // Calculate new position based on direction
    switch (direction) {
        case 'W': // Up
            new_y--;
            break;
        case 'S': // Down
            new_y++;
            break;
        case 'A': // Left
            new_x--;
            break;
        case 'D': // Right
            new_x++;
            break;
        case 'C': // Charge
            ghost->current_move += 1;
            ghost->charged = 1;
            return VALID_MOVE;
        case 'T': // Wait
            if (command->turns_left == 1) {
                ghost->current_move += 1; // move on
                command->turns_left = command->turns;
            }
            else command->turns_left -= 1;
            return VALID_MOVE;
        default:
            return INVALID_MOVE; // Invalid direction
    }

    // Logic for the WASD movement
    if (ghost->charged) {
        int result = move_ghost_charged(board, ghost_index, direction);
        ghost->current_move++;
        return result;
    }

    // Check boundaries
    if (!is_valid_position(board, new_x, new_y)) {
        ghost->current_move++;
        return INVALID_MOVE;
    }

    // Check board position
    int new_index = get_board_index(board, new_x, new_y);
    int old_index = get_board_index(board, ghost->pos_x, ghost->pos_y);
    char target_content = board->board[new_index].content;

    // Check for walls and ghosts
    if (target_content == 'W' || target_content == 'M') {
        ghost->current_move++;
        return INVALID_MOVE;
    }

    int result = VALID_MOVE;
    // Check for pacman
    if (target_content == 'P') {
        result = find_and_kill_pacman(board, new_x, new_y);
    }

    // Update board - clear old position (restore what was there)
    board->board[old_index].content = ' '; // Or restore the dot if ghost was on one

    // Update ghost position
    ghost->pos_x = new_x;
    ghost->pos_y = new_y;

    // Update board - set new position
    board->board[new_index].content = 'M';
    
    ghost->current_move++;
    return result;
}

void *ghost_thread_fn(void *arg) {
    int ghost_index = (int)(long)arg;
 
    while (1) {
        pthread_mutex_lock(&board_mutex);

        if (g_board == NULL || g_board->board == NULL || 
       g_board->pacmans == NULL || !g_board->pacmans[0].alive) {
            pthread_mutex_unlock(&board_mutex);
            break;
        }

        if (g_board->ghosts[ghost_index].n_moves > 0) {
            int current = g_board->ghosts[ghost_index].current_move;
            int n_moves = g_board->ghosts[ghost_index].n_moves;
            int move_index = current % n_moves;
            command_t *cmd = &g_board->ghosts[ghost_index].moves[move_index];
            move_ghost(g_board, ghost_index, cmd);
        }
        pthread_mutex_unlock(&board_mutex);
        sleep_ms(g_board->tempo);
    }

    return NULL;
}

void kill_pacman(board_t* board, int pacman_index) {
    debug("Killing %d pacman\n\n", pacman_index);
    pacman_t* pac = &board->pacmans[pacman_index];
    int index = pac->pos_y * board->width + pac->pos_x;

    // Remove pacman from the board
    board->board[index].content = ' ';

    // Mark pacman as dead
    pac->alive = 0;
}

// Static Loading
int load_pacman(board_t* board, int points) {
    board->board[1 * board->width + 1].content = 'P'; // Pacman
    board->pacmans[0].pos_x = 1;
    board->pacmans[0].pos_y = 1;
    board->pacmans[0].alive = 1;
    board->pacmans[0].points = points;
    board->pacmans[0].n_moves = 0; // Manual control (user input)
    board->pacmans[0].waiting = 0;
    board->pacmans[0].current_move = 0;
    board->pacmans[0].passo = 0;
    return 0;
}

// Static Loading
int load_ghost(board_t* board) {
    // Ghost 0
    board->board[3 * board->width + 1].content = 'M'; // Monster
    board->ghosts[0].pos_x = 1;
    board->ghosts[0].pos_y = 3;
    board->ghosts[0].passo = 0;
    board->ghosts[0].waiting = 0;
    board->ghosts[0].current_move = 0;
    board->ghosts[0].n_moves = 16;
    for (int i = 0; i < 8; i++) {
        board->ghosts[0].moves[i].command = 'D';
        board->ghosts[0].moves[i].turns = 1; 
    }
    for (int i = 8; i < 16; i++) {
        board->ghosts[0].moves[i].command = 'A';
        board->ghosts[0].moves[i].turns = 1; 
    }

    // Ghost 1
    board->board[2 * board->width + 4].content = 'M'; // Monster
    board->ghosts[1].pos_x = 4;
    board->ghosts[1].pos_y = 2;
    board->ghosts[1].passo = 1;
    board->ghosts[1].waiting = 1;
    board->ghosts[1].current_move = 0;
    board->ghosts[1].n_moves = 1;
    board->ghosts[1].moves[0].command = 'R'; // Random
    board->ghosts[1].moves[0].turns = 1; 
    
    return 0;
}

int load_level(board_t* board, const char *level_filepath, int accumulated_points) {
    debug("loading level %s\n", level_filepath);

    int fd = open(level_filepath, O_RDONLY);
    if (fd < 0) {
        perror("open error");
        return 1;
    }
 
    char buffer[8192];
    ssize_t total_read = 0;
    ssize_t bytes_read;
 
    while ((bytes_read = read(fd, buffer + total_read, sizeof(buffer) - total_read - 1)) > 0) {
      total_read += bytes_read;
      if ((size_t)total_read >= sizeof(buffer) - 1) {
        break;
      }
    }

    if (bytes_read < 0) {
      perror("read error");
      close(fd);
      return 1;
    }

    buffer[total_read] = '\0';
    close(fd);

    board->width = 0;
    board->height = 0;
    board->tempo = 10;
    board->n_ghosts = 0;
    board->n_pacmans = 1;
    board->pacman_file[0] = '\0';
 
    char *line = buffer;
    char *line_end;

    while ((line_end = strchr(line, '\n')) != NULL || (line_end = strchr(line, '\0')) != NULL) {
        size_t line_len = line_end - line;
        char current_line[512];
        if (line_len >= sizeof(current_line)) {
            line_len = sizeof(current_line) - 1;
        }
        strncpy(current_line, line, line_len);
        current_line[line_len] = '\0';
 
        while (line_len > 0 && (current_line[line_len - 1] == ' ' || current_line[line_len - 1] == '\r')) {
            current_line[--line_len] = '\0';
        }

        if (line_len == 0) {
            line = line_end + 1;
            continue;
        }

        if (current_line[0] == '#') {
            line = line_end + 1;
            continue;
        }

        if (strncmp(current_line, "DIM", 3) == 0) {
            int width, height;
            if (sscanf(current_line + 3, "%d %d", &width, &height) == 2) {
                board->width = width;
                board->height = height;
            }
        }
        if (strncmp(current_line, "TEMPO", 5) == 0) {
            int tempo;
            if (sscanf(current_line + 5, "%d", &tempo) == 1) {
                board->tempo = tempo;
            }
        }

        if (strncmp(current_line, "PAC", 3) == 0) {
          char pac[256];
          if (sscanf(current_line + 3, " %s", pac) == 1) {
              strncpy(board->pacman_file, pac, sizeof(board->pacman_file) - 1);
              board->pacman_file[sizeof(board->pacman_file) - 1] = '\0';
          }
        }

        if (strncmp(current_line, "MON", 3) == 0) {
          char *mon_start = current_line + 3;
          char mon_file[256];
          board->n_ghosts = 0;

          while (*mon_start != '\0' && board->n_ghosts < MAX_GHOSTS) {
              while (*mon_start == ' ' || *mon_start == '\t') {
                mon_start++;
              }
              if (*mon_start == '\0') break;

              int i = 0;
              while (*mon_start != '\0' && *mon_start != ' ' && *mon_start != '\t' && i < 255) {
                mon_file[i++] = *mon_start++;
              }
              mon_file[i] = '\0';
              if (i > 0) {
                strncpy(board->ghosts_files[board->n_ghosts], mon_file, sizeof(board->ghosts_files[0]) - 1);
                board->ghosts_files[board->n_ghosts][sizeof(board->ghosts_files[0]) - 1] = '\0';
                board->n_ghosts++;
              }
          }
        }
 
        if (*line_end == '\0') break;
        line = line_end + 1;
    }
 
    if (board->width <= 0 || board->height <= 0) {
        return 1;
    }

    board->board = calloc(board->width * board->height, sizeof(board_pos_t));
    board->pacmans = calloc(board->n_pacmans, sizeof(pacman_t));
    board->ghosts = calloc(board->n_ghosts, sizeof(ghost_t));

    if (!board->board || !board->pacmans || !board->ghosts) {
        return 1;
    }

    line = buffer;
    int parsing_matrix = 0;
    int matrix_line = 0;

    while ((line_end = strchr(line, '\n')) != NULL || (line_end = strchr(line, '\0')) != NULL) {
        size_t line_len = line_end - line;
        char current_line[512];
        if (line_len >= sizeof(current_line)) {
            line_len = sizeof(current_line) - 1;
        }
        strncpy(current_line, line, line_len);
        current_line[line_len] = '\0';

        while (line_len > 0 && (current_line[line_len - 1] == ' ')) {
            current_line[--line_len] = '\0';
        }

        if (line_len == 0) {
            line = line_end + 1;
            continue;
        }

        if (!parsing_matrix && current_line[0] != '#' && strncmp(current_line, "DIM", 3) != 0 &&
            strncmp(current_line, "TEMPO", 5) != 0 && strncmp(current_line, "PAC", 3) != 0 &&
            strncmp(current_line, "MON", 3) != 0) {
            parsing_matrix = 1;
        }

        if (parsing_matrix && matrix_line < board->height) {
            for (int col = 0; (size_t)col < line_len && col < board->width; col++) {
                int idx = matrix_line * board->width + col;
                char ch = current_line[col];

                if (ch == 'X') {
                  board->board[idx].content = 'W';
                  board->board[idx].has_dot = 0;
                  board->board[idx].has_portal = 0;
                } else if (ch == 'o') {
                  board->board[idx].content = ' ';
                  board->board[idx].has_dot = 1;
                  board->board[idx].has_portal = 0;
                } else if (ch == '@') {
                  board->board[idx].content = ' ';
                  board->board[idx].has_dot = 0;
                  board->board[idx].has_portal = 1;
                } else {
                  board->board[idx].content = ' ';
                  board->board[idx].has_dot = 0;
                  board->board[idx].has_portal = 0;
                }
            }
          matrix_line++;
        }

      if (*line_end == '\0') break;
      line = line_end + 1;
    }

    strncpy(board->level_name, level_filepath, sizeof(board->level_name) - 1);
    board->level_name[sizeof(board->level_name) - 1] = '\0';

    char level_dir[512];
    const char *last_slash = strrchr(level_filepath, '/');
    if (last_slash) {
        size_t dir_len = last_slash - level_filepath + 1;
        strncpy(level_dir, level_filepath, dir_len);
        level_dir[dir_len] = '\0';
    } else {
        level_dir[0] = '\0';
    }

    if (board->pacman_file[0] == '\0') {
      load_pacman(board, accumulated_points);
    } else {
      char pacman_filepath[1024];
      snprintf(pacman_filepath, sizeof(pacman_filepath), "%s%s", level_dir, board->pacman_file);
      if (load_entity_file(board, 0, pacman_filepath, 1) != 0) {
          load_pacman(board, accumulated_points);
      } else {
          board->pacmans[0].points = accumulated_points;
      }
    }

    for(int i = 0; i < board->n_ghosts; i++){
        char ghost_filepath[1024];
        snprintf(ghost_filepath, sizeof(ghost_filepath), "%s%s", level_dir, board->ghosts_files[i]);
        load_entity_file(board, i, ghost_filepath, 0);
    }

    return 0;
}

void restore_checkpoint(board_t *game_board, board_t *saved_board, pthread_mutex_t *board_mutex, int *accumulated_points){
    unload_level(game_board);
    copy_board_state(game_board, saved_board);
    *accumulated_points = game_board->pacmans[0].points;
    pthread_mutex_lock(board_mutex);
    draw_board(game_board, DRAW_MENU);
    pthread_mutex_unlock(board_mutex);
    refresh_screen();
}

int load_entity_file(board_t* board, int index, const char *filepath, int is_pacman){
    if (is_pacman) {
        if (index < 0 || index >= board->n_pacmans) {
            return 1;
        }
    } else {
        if (index < 0 || index >= board->n_ghosts) {
            return 1;
        }
    }


    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        perror("open error");
        return 1;
    }
 
    char buffer[8192];
    ssize_t total_read = 0;
    ssize_t bytes_read;
 
    while ((bytes_read = read(fd, buffer + total_read, sizeof(buffer) - total_read - 1)) > 0) {
        total_read += bytes_read;
        if ((size_t)total_read >= sizeof(buffer) - 1) {
            break;
        }
    }

    if (bytes_read < 0) {
        perror("read error");
        close(fd);
        return 1;
    }

    buffer[total_read] = '\0';
    close(fd);

    int passo = 0;
    int pos_x = 0;
    int pos_y = 0;
    int n_moves = 0;
    command_t moves[MAX_MOVES];

    if (is_pacman) {
        pacman_t* pac = &board->pacmans[index];
        pac->passo = 0;
        pac->pos_x = 0;
        pac->pos_y = 0;
        pac->waiting = 0;
        pac->current_move = 0;
        pac->n_moves = 0;
        pac->alive = 1;
    } else {
        ghost_t* ghost = &board->ghosts[index];
        ghost->passo = 0;
        ghost->pos_x = 0;
        ghost->pos_y = 0;
        ghost->waiting = 0;
        ghost->current_move = 0;
        ghost->n_moves = 0;
        ghost->charged = 0;
    }
 
    char *line = buffer;
    char *line_end;
    int parsing_commands = 0;

    while ((line_end = strchr(line, '\n')) != NULL || (line_end = strchr(line, '\0')) != NULL) {
        size_t line_len = line_end - line;
        char current_line[512];
        if (line_len >= sizeof(current_line)) {
            line_len = sizeof(current_line) - 1;
        }
        strncpy(current_line, line, line_len);
        current_line[line_len] = '\0';
 
        while (line_len > 0 && (current_line[line_len - 1] == ' ')) {
            current_line[--line_len] = '\0';
        }

        if (line_len == 0) {
            line = line_end + 1;
            continue;
        }

        if (current_line[0] == '#') {
            line = line_end + 1;
            continue;
        }

        if (strncmp(current_line, "PASSO", 5) == 0) {
            if (sscanf(current_line + 5, "%d", &passo) == 1) {
                parsing_commands = 1;
            }
        }
        else if (strncmp(current_line, "POS", 3) == 0) {
            int row, col;
            if (sscanf(current_line + 3, "%d %d", &row, &col) == 2) {
                pos_y = row;
                pos_x = col;
                parsing_commands = 1;
            }
        }
        else if (parsing_commands && n_moves < MAX_MOVES) {
            char cmd = current_line[0];
 
            if (cmd == 'A' || cmd == 'D' || cmd == 'W' || cmd == 'S' || cmd == 'R' || cmd == 'C') {
                moves[n_moves].command = cmd;
                moves[n_moves].turns = 1;
                moves[n_moves].turns_left = 1;
                n_moves++;
            }
            else if (cmd == 'T') {
                int turns;
                if (sscanf(current_line + 1, "%d", &turns) == 1) {
                    moves[n_moves].command = 'T';
                    moves[n_moves].turns = turns;
                    moves[n_moves].turns_left = turns;
                    n_moves++;
                }
            }
        }

        if (*line_end == '\0') break;
        line = line_end + 1;
    }

    if (is_pacman) {
        pacman_t* pac = &board->pacmans[index];
        pac->passo = passo;
        pac->pos_x = pos_x;
        pac->pos_y = pos_y;
        pac->n_moves = n_moves;
        pac->waiting = passo;
        for (int i = 0; i < n_moves; i++) {
            pac->moves[i] = moves[i];
        }

        if (is_valid_position(board, pos_x, pos_y)) {
            int idx = get_board_index(board, pos_x, pos_y);
            if (board->board[idx].content != 'W') {
                board->board[idx].content = 'P';
            }else{
              terminal_cleanup();
              printf("Invalid pacman position");
              exit(1);
            }
        }
    } else {
        ghost_t* ghost = &board->ghosts[index];
        ghost->passo = passo;
        ghost->pos_x = pos_x;
        ghost->pos_y = pos_y;
        ghost->n_moves = n_moves;
        ghost->waiting = passo;
        for (int i = 0; i < n_moves; i++) {
            ghost->moves[i] = moves[i];
        }

        if (is_valid_position(board, pos_x, pos_y)) {
            int idx = get_board_index(board, pos_x, pos_y);
            if (board->board[idx].content != 'W') {
                board->board[idx].content = 'M';
            } else {
                terminal_cleanup();
                printf("Invalid monster position");
                exit(1);
            }
        } else {
            terminal_cleanup();
            printf("Invalid monster position");
            exit(1);
        }
    }

    return 0;
}

void unload_level(board_t * board) {
    if (board->board) {
        free(board->board);
        board->board = NULL;
    }
    if (board->pacmans) {
        free(board->pacmans);
        board->pacmans = NULL;
    }
    if (board->ghosts) {
        free(board->ghosts);
        board->ghosts = NULL;
    }
}

void copy_board_state(board_t *dest, board_t *src) {
    void *old_board = dest->board;
    void *old_pacmans = dest->pacmans;
    void *old_ghosts = dest->ghosts;

    *dest = *src;
 
    int board_size = src->width * src->height;
    dest->board = malloc(board_size * sizeof(board_pos_t));
    if (dest->board) {
        for (int i = 0; i < board_size; i++) {
            dest->board[i] = src->board[i];
        }
    }

    dest->pacmans = malloc(src->n_pacmans * sizeof(pacman_t));
    if (dest->pacmans) {
        for (int i = 0; i < src->n_pacmans; i++) {
            dest->pacmans[i] = src->pacmans[i];
        }
    }
 
    dest->ghosts = malloc(src->n_ghosts * sizeof(ghost_t));
    if (dest->ghosts) {
        for (int i = 0; i < src->n_ghosts; i++) {
            dest->ghosts[i] = src->ghosts[i];
        }
    }
 
    if (old_board && old_board != src->board) {
        free(old_board);
    }
    if (old_pacmans && old_pacmans != src->pacmans) {
        free(old_pacmans);
    }
    if (old_ghosts && old_ghosts != src->ghosts) {
        free(old_ghosts);
    }
}

void reset_board(board_t *saved_board){
  saved_board->width = 0;
  saved_board->height = 0;
  saved_board->board = NULL;
  saved_board->n_pacmans = 0;
  saved_board->pacmans = NULL;
  saved_board->n_ghosts = 0;
  saved_board->ghosts = NULL;
  saved_board->level_name[0] = '\0';
  saved_board->pacman_file[0] = '\0';
  saved_board->tempo = 0;
  for (int i = 0; i < MAX_GHOSTS; i++) {
      saved_board->ghosts_files[i][0] = '\0';
  }
}

void open_debug_file(char *filename) {
    debugfile = fopen(filename, "w");
}

void close_debug_file() {
    fclose(debugfile);
}

void debug(const char * format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(debugfile, format, args);
    va_end(args);

    fflush(debugfile);
}

void print_board(board_t *board) {
    if (!board || !board->board) {
        debug("[%d] Board is empty or not initialized.\n", getpid());
        return;
    }

    // Large buffer to accumulate the whole output
    char buffer[8192];
    size_t offset = 0;

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "=== [%d] LEVEL INFO ===\n"
                       "Dimensions: %d x %d\n"
                       "Tempo: %d\n"
                       "Pacman file: %s\n",
                       getpid(), board->height, board->width, board->tempo, board->pacman_file);

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "Monster files (%d):\n", board->n_ghosts);

    for (int i = 0; i < board->n_ghosts; i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "  - %s\n", board->ghosts_files[i]);
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n=== BOARD ===\n");

    for (int y = 0; y < board->height; y++) {
        for (int x = 0; x < board->width; x++) {
            int idx = y * board->width + x;
            if (offset < sizeof(buffer) - 2) {
                buffer[offset++] = board->board[idx].content;
            }
        }
        if (offset < sizeof(buffer) - 2) {
            buffer[offset++] = '\n';
        }
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "==================\n");

    buffer[offset] = '\0';

    debug("%s", buffer);
}

