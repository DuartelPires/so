#include "board.h"
#include "display.h"
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdbool.h>
#include <semaphore.h>
#include <signal.h>

#define MAX_PIPE_PATH_LENGTH 40

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

enum {
  OP_CODE_CONNECT = 1,
  OP_CODE_DISCONNECT = 2,
  OP_CODE_PLAY = 3,
  OP_CODE_BOARD = 4,
};

typedef struct {
    int req_pipe_fd;
    int notif_pipe_fd;
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
    board_t *board;
    bool active;
    bool disconnected; 
    pthread_mutex_t session_mutex;
} session_t;

typedef struct {
    board_t *board;
    int ghost_index;
} ghost_thread_arg_t;

typedef struct {
    board_t *board;
    session_t *session;
} pacman_thread_arg_t;


typedef struct{
    char id[MAX_PIPE_PATH_LENGTH];
    int score;
}player_score_t;

session_t *current_session = NULL;
pthread_mutex_t session_management_mutex = PTHREAD_MUTEX_INITIALIZER;
int server_max_games = 1;
sem_t max_sessions_sem; 

#define MAX_SESSIONS_BUFFER 1000 
session_t *session_buffer[MAX_SESSIONS_BUFFER]; 
int buffer_head = 0;
int buffer_tail = 0;
sem_t buffer_empty;  
sem_t buffer_full;   
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

char* global_level_dir = NULL;


static volatile sig_atomic_t got_sigusr1 = 0;
session_t *active_sessions[MAX_SESSIONS_BUFFER] = {NULL};
pthread_mutex_t active_sessions_mutex = PTHREAD_MUTEX_INITIALIZER;

int write_all(int fd, const void *buffer, size_t len) {
    size_t written = 0;
    const char *ptr = buffer;
    while (written < len) {
        ssize_t ret = write(fd, ptr + written, len - written);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += ret;
    }
    return 0;
}

static char* board_to_string(board_t* board) {
    size_t buffer_size = board->width * board->height;
    char* output = malloc(buffer_size);
    size_t pos = 0;
    
    for (int y = 0; y < board->height; y++) {
        for (int x = 0; x < board->width; x++) {
            int index = y * board->width + x;
            char ch = board->board[index].content;
            int ghost_charged = 0;

            for (int g = 0; g < board->n_ghosts; g++) {
                ghost_t* ghost = &board->ghosts[g];
                if (ghost->pos_x == x && ghost->pos_y == y) {
                    if (ghost->charged) ghost_charged = 1;
                    break;
                }
            }

            switch (ch) {
                case 'W': output[pos++] = '#'; break;
                case 'P': output[pos++] = 'C'; break;
                case 'M': output[pos++] = ghost_charged ? 'G' : 'M'; break;
                case ' ': 
                    if (board->board[index].has_portal) output[pos++] = '@';
                    else if (board->board[index].has_dot) output[pos++] = '.';
                    else output[pos++] = ' ';
                    break;
                default: output[pos++] = ch; break;
            }
        }
    }
    return output;
}

int create_backup() {
    terminal_cleanup();
    pid_t child = fork();
    if(child != 0) {
        if (child < 0) return -1;
        return child;
    } else {
        debug("[%d] Created\n", getpid());
        return 0;
    }
}

void screen_refresh(board_t * game_board, int mode) {
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();     
}

void* pacman_thread(void *arg) {
    pacman_thread_arg_t *pacman_arg = (pacman_thread_arg_t *) arg;
    board_t *board = pacman_arg->board;
    session_t *session = pacman_arg->session;
    
    free(pacman_arg);

    pacman_t* pacman = &board->pacmans[0];
    int *retval = malloc(sizeof(int));

    while (true) {
        if(!pacman->alive) {
            *retval = LOAD_BACKUP;
            return (void*) retval;
        }

        sleep_ms(board->tempo * (1 + pacman->passo));

        command_t* play;
        command_t c;
        
        pthread_mutex_lock(&session->session_mutex);
        bool has_client = (session->active && !session->disconnected);
        int client_fd = has_client ? session->req_pipe_fd : -1;
        pthread_mutex_unlock(&session->session_mutex);

        if (pacman->n_moves == 0) {
            if (has_client && client_fd != -1) {
                char op_code;
                ssize_t bytes_read = read(client_fd, &op_code, 1);
                
                if (bytes_read <= 0) {
                   pthread_mutex_lock(&session->session_mutex);
                   session->disconnected = true;
                   pthread_mutex_unlock(&session->session_mutex);
                   *retval = QUIT_GAME;
                   return (void*) retval;
                }
                
                if (op_code == OP_CODE_PLAY) {
                    char command_char;
                    ssize_t cmd_read = read(client_fd, &command_char, 1);
                    if (cmd_read <= 0) {
                        pthread_mutex_lock(&session->session_mutex);
                        session->disconnected = true;
                        pthread_mutex_unlock(&session->session_mutex);
                        *retval = QUIT_GAME;
                        return (void*) retval;
                    }
                    c.command = command_char;
                    c.turns = 1;
                    play = &c;
                } else if (op_code == OP_CODE_DISCONNECT) {
                     pthread_mutex_lock(&session->session_mutex);
                     session->disconnected = true;
                     pthread_mutex_unlock(&session->session_mutex);
                     *retval = QUIT_GAME;
                     return (void*) retval;
                } else {
                    continue; 
                }
            } else {
                // Modo sem cliente (teclado local)
                c.command = get_input();
                if(c.command == '\0') continue;
                c.turns = 1;
                play = &c;
            }
        } else {
            play = &pacman->moves[pacman->current_move % pacman->n_moves];
        }

        if (play->command == 'Q') {
            *retval = QUIT_GAME;
            return (void*) retval;
        }

        pthread_rwlock_rdlock(&board->state_lock);
        int result = move_pacman(board, 0, play);
        if (result == REACHED_PORTAL) {
            *retval = NEXT_LEVEL;
            pthread_rwlock_unlock(&board->state_lock); 
            break;
        }
        if(result == DEAD_PACMAN) {
            *retval = LOAD_BACKUP;
            pthread_rwlock_unlock(&board->state_lock); 
            break;
        }
        pthread_rwlock_unlock(&board->state_lock);
    }
    return (void*) retval;
}

void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;
    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_rdlock(&board->state_lock);
        if (board->thread_shutdown) { 
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move % ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

void* board_update_thread(void *arg) {
    session_t *session = (session_t*) arg;
    board_t *board = session->board;

    while (true) {
        sleep_ms(board->tempo); 

        pthread_mutex_lock(&session->session_mutex);
        int fd = session->notif_pipe_fd;
        bool active = session->active && !session->disconnected;
        pthread_mutex_unlock(&session->session_mutex);

        if (!active || fd == -1) break; 

        pthread_rwlock_rdlock(&board->state_lock);
        if (board->thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            break;
        }

        char op_code = OP_CODE_BOARD;
        int header[6];
        header[0] = board->width;
        header[1] = board->height;
        header[2] = board->tempo;
        header[3] = 0;
        if(!board->pacmans[0].alive){
          header[4] = 1;
        }else{
          header[4] = 0;
        }
        header[5] = board->pacmans[0].points;

        char *map_data = board_to_string(board);
        int map_size = header[0] * header[1];
        
        pthread_rwlock_unlock(&board->state_lock);

        pthread_mutex_lock(&session->session_mutex);
        if (write_all(fd, &op_code, 1) == 0) {
            if (write_all(fd, header, sizeof(header)) == 0) {
                write_all(fd, map_data, map_size);
            }
        } else {
                session->disconnected = true;
        }
        pthread_mutex_unlock(&session->session_mutex);

        free(map_data);
        
        if (header[4]) break;
    }
    return NULL;
}

void *game_session(void *arg){
    session_t *my_session = (session_t *) arg;

    int accumulated_points = 0;
    bool end_game = false;
    board_t game_board;
    game_board.thread_shutdown = 0;

    int index = -1;

    pthread_mutex_lock(&active_sessions_mutex);
    for(int i = 0; i < MAX_SESSIONS_BUFFER; i++){
        if(!active_sessions[i]){
            index = i;
            active_sessions[i] = my_session;
            break;
        }
    }

    pthread_mutex_unlock(&active_sessions_mutex);

    DIR* level_dir = opendir(global_level_dir);
    
    if (level_dir == NULL) {
        pthread_mutex_lock(&my_session->session_mutex);
        my_session->active = false;
        my_session->disconnected = true;
        pthread_mutex_unlock(&my_session->session_mutex);
        
        sem_post(&max_sessions_sem);
        return NULL;
    }

    struct dirent* entry;
    while ((entry = readdir(level_dir)) != NULL && !end_game) {
        if (entry->d_name[0] == '.') continue;
        char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".lvl") != 0) continue;

        load_level(&game_board, entry->d_name, global_level_dir, accumulated_points);
        
        pthread_mutex_lock(&my_session->session_mutex);
        my_session->board = &game_board;
        pthread_mutex_unlock(&my_session->session_mutex);

        while(true) {
            pthread_t pacman_tid;
            pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));
            pthread_t board_update_tid = 0;

            game_board.thread_shutdown = 0;
            
            pthread_mutex_lock(&my_session->session_mutex);
            bool has_client = (my_session->active && !my_session->disconnected);
            if (has_client) {
                pthread_create(&board_update_tid, NULL, board_update_thread, my_session);
            }
            pthread_mutex_unlock(&my_session->session_mutex);

            pacman_thread_arg_t *pacman_arg = malloc(sizeof(pacman_thread_arg_t));
            pacman_arg->board = &game_board;
            pacman_arg->session = my_session;
            
            pthread_create(&pacman_tid, NULL, pacman_thread, (void*) pacman_arg);
            
            for (int i = 0; i < game_board.n_ghosts; i++) {
                ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                arg->board = &game_board;
                arg->ghost_index = i;
                pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) arg);
            }

            int *retval;
            pthread_join(pacman_tid, (void**)&retval);

            pthread_rwlock_wrlock(&game_board.state_lock);
            game_board.thread_shutdown = 1; 
            pthread_rwlock_unlock(&game_board.state_lock);

            if (board_update_tid != 0) {
                pthread_join(board_update_tid, NULL);
            }

            for (int i = 0; i < game_board.n_ghosts; i++) pthread_join(ghost_tids[i], NULL);
            free(ghost_tids);

            int result = *retval;
            free(retval);

            pthread_mutex_lock(&my_session->session_mutex);
            if(my_session->disconnected) end_game = true;
            pthread_mutex_unlock(&my_session->session_mutex);

            if(result == NEXT_LEVEL && !end_game) {
                accumulated_points = game_board.pacmans[0].points;      
                break; 
            } else {
                end_game = true; 
                break;
            }
            accumulated_points = game_board.pacmans[0].points;      
        }
        print_board(&game_board);
        unload_level(&game_board);
        if(end_game) break;
    }
    
    closedir(level_dir); 

    pthread_mutex_lock(&active_sessions_mutex);
    active_sessions[index] = NULL;
    pthread_mutex_unlock(&active_sessions_mutex);

    pthread_mutex_lock(&my_session->session_mutex);
    if (my_session->req_pipe_fd != -1) close(my_session->req_pipe_fd);
    if (my_session->notif_pipe_fd != -1) close(my_session->notif_pipe_fd);
    my_session->active = false;
    my_session->disconnected = true;
    pthread_mutex_unlock(&my_session->session_mutex);
    
    sem_post(&max_sessions_sem);

    return NULL;
}    

void* consumer_thread(void* arg){
    (void)arg; 
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    while(true){
        sem_wait(&buffer_full);
        pthread_mutex_lock(&buffer_mutex);
        
        session_t *session = session_buffer[buffer_tail];
        buffer_tail = (buffer_tail + 1) % MAX_SESSIONS_BUFFER;

        pthread_mutex_unlock(&buffer_mutex);
        sem_post(&buffer_empty); 

        if(session != NULL){
            game_session(session);
            free(session); 
        }
    }
    return NULL;
}

int compare_scores(const void *a, const void *b){
    player_score_t *player1 = (player_score_t *)a;
    player_score_t *player2 = (player_score_t *)b;
    return (player2->score - player1->score);
}

int get_points(player_score_t *scores_list){
    int count = 0;

    pthread_mutex_lock(&active_sessions_mutex);

    for(int i = 0; i < MAX_SESSIONS_BUFFER; i++){
        if(active_sessions[i] && active_sessions[i]->active && active_sessions[i]->board){
            char *start = active_sessions[i]->notif_pipe_path + 5;
            char *underscore = strchr(start, '_');
            size_t length;
            length = underscore - start;
            strncpy(scores_list[count].id, start, length);
            scores_list[count].id[length] = '\0';

            scores_list[count].score = active_sessions[i]->board->pacmans[0].points;
            count++;
        }
    }

    pthread_mutex_unlock(&active_sessions_mutex);

    if(count == 0){
        return 0;
    }

    int index = 5;

    if(count < 5){
        index = count;
    }

    qsort(scores_list, count, sizeof(player_score_t), compare_scores);

    //for(int i = 0; i < index; i++){
    //    printf("%s, %d\n", scores_list[i].id, scores_list[i].score);
    //}

    return index;
}


static void write_top5(){


    player_score_t player_scores[MAX_SESSIONS_BUFFER];
    int index = get_points(player_scores);

    int fd = open("top5.txt", O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        return;
    }

    for(int i = 0; i < index; i++){

        int score = player_scores[i].score;
        char *id = player_scores[i].id;

        //calcula tamanho do buffer
        int score_len = snprintf(NULL, 0, "%d", score);
        int id_len = strlen(id);
        int buffer_size = id_len + 1 + score_len + 1 + 1;
        char buffer[buffer_size];


        snprintf(buffer, buffer_size,"%s %d\n", player_scores[i].id, player_scores[i].score);

        int len = sizeof(buffer) - 1;
        int done = 0;

        while (len > done) {
            int bytes_written = write(fd, buffer + done, len - done);

            if (bytes_written < 0) {
                return;
            }

            done += bytes_written;
        }

    }
    close(fd);
    return;
}

void* connection_handler_thread(void *arg) {
    char *registration_fifo = (char*) arg;
    char buffer[81];

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGQUIT);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    while (true) {

        if(got_sigusr1){
            write_top5();
            got_sigusr1 = 0;
        }
        int rx = open(registration_fifo, O_RDONLY);

        if (rx == -1){
            continue;
        } 
 
        int n = read(rx, buffer, 81);
        close(rx);

        if (n != 81 || buffer[0] != 1) continue;

        char req_pipe[41] = {0};
        char notif_pipe[41] = {0};
        strncpy(req_pipe, &buffer[1], 40);
        strncpy(notif_pipe, &buffer[41], 40);

        debug("[INFO] Novo cliente: %s\n", notif_pipe);

        if (sem_wait(&max_sessions_sem) != 0) continue;

        int notif_fd = open(notif_pipe, O_WRONLY);
        int req_fd = open(req_pipe, O_RDONLY);

        if (notif_fd == -1 || req_fd == -1) {
            if (notif_fd != -1) close(notif_fd);
            if (req_fd != -1) close(req_fd);
            sem_post(&max_sessions_sem);
            continue;
        }

        session_t *session = malloc(sizeof(session_t)); 
        session->req_pipe_fd = req_fd;
        session->notif_pipe_fd = notif_fd;
        strncpy(session->notif_pipe_path, notif_pipe, MAX_PIPE_PATH_LENGTH);
        strncpy(session->req_pipe_path, req_pipe, MAX_PIPE_PATH_LENGTH);
        session->active = true;
        session->disconnected = false;
        session->board = NULL;
        pthread_mutex_init(&session->session_mutex, NULL);

        char response[2] = {OP_CODE_CONNECT, 0};
        write(notif_fd, response, 2);

        sem_wait(&buffer_empty);
        pthread_mutex_lock(&buffer_mutex);

        session_buffer[buffer_head] = session;
        buffer_head = (buffer_head + 1) % MAX_SESSIONS_BUFFER;
        
        pthread_mutex_unlock(&buffer_mutex);
        sem_post(&buffer_full); 
    }
    return NULL;
}


static void sig_handler(int sig) {
  if (sig == SIGUSR1) {

    if (signal(SIGUSR1, sig_handler) == SIG_ERR) {
      exit(EXIT_FAILURE);
    }

    got_sigusr1 = 1;
    return;
  }
}



int main(int argc, char** argv) {
    if (argc != 4) {
        printf("Usage: %s <level_directory> <max_games> <registration_fifo_name>\n", argv[0]);
        return -1;
    }

    if (signal(SIGUSR1, sig_handler) == SIG_ERR) {
        exit(EXIT_FAILURE);
    }

    global_level_dir = argv[1]; 
    server_max_games = atoi(argv[2]);
    char *fifo_name = argv[3];
    srand((unsigned int)time(NULL));

    if (unlink(fifo_name) != 0 && errno != ENOENT) return 0;
    if (mkfifo(fifo_name, 0666) != 0) return 0;

    open_debug_file("debug.log");

    sem_init(&max_sessions_sem, 0, server_max_games);
    sem_init(&buffer_empty, 0, MAX_SESSIONS_BUFFER);
    sem_init(&buffer_full, 0, 0);

    pthread_t *consumer_threads = malloc(server_max_games * sizeof(pthread_t));
    for(int i = 0; i < server_max_games; i++){
        pthread_create(&consumer_threads[i], NULL, consumer_thread, NULL);
    }

    pthread_t connection_thread;
    pthread_create(&connection_thread, NULL, connection_handler_thread, fifo_name);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    
    pthread_join(connection_thread, NULL);

    close_debug_file();
    sem_destroy(&max_sessions_sem);
    sem_destroy(&buffer_full);
    sem_destroy(&buffer_empty);
    free(consumer_threads);
    return 0;
}