#include "board.h"
#include "display.h"
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/wait.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4
#define QUIT 5

void screen_refresh(board_t * game_board, int mode) {
    debug("REFRESH\n");
    pthread_mutex_lock(&board_mutex);
    draw_board(game_board, mode);
    pthread_mutex_unlock(&board_mutex);
    refresh_screen();
    if(game_board->tempo != 0)
        sleep_ms(game_board->tempo);       
}

int compare_filenames(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

void create_ghost_threads(board_t *game_board, pthread_t *ghost_threads, int *ghost_thread_num) {
    *ghost_thread_num = 0;
    for (int g = 0; g < game_board->n_ghosts; g++) {
        if (pthread_create(&ghost_threads[g], NULL, ghost_thread_fn, (void*)(long)g) != 0) {
            break;
        }
        (*ghost_thread_num)++;
    }
    sleep_ms(game_board->tempo);
    screen_refresh(game_board, DRAW_MENU);
}

int play_board(board_t * game_board) {
    pacman_t* pacman = &game_board->pacmans[0];
    command_t* play;

    char user_input = get_input();
    if(user_input == 'Q') {
        return QUIT;
    }
  /*
    if(user_input == 'G') {
        debug("create backup\n");
        return CREATE_BACKUP;
    }
 */   
    if(pacman->n_moves == 0) { // if is user input
        command_t c; 
        c.command = user_input;

        if(c.command == '\0')
            return CONTINUE_PLAY;

        c.turns = 1;
        play = &c;
    }
   else { // else if the moves are pre-defined in the file
        // avoid buffer overflow wrapping around with modulo of n_moves
        // this ensures that we always access a valid move for the pacman
        play = &pacman->moves[pacman->current_move%pacman->n_moves];
    }

    debug("KEY %c\n", play->command);

    if(play->command == 'Q') {
        return QUIT;
    }

    if(play->command == 'G') {
        debug("create backup\n");
        return CREATE_BACKUP;
    }

    pthread_mutex_lock(&board_mutex);
    int result = move_pacman(game_board, 0, play);
    if(result == REACHED_PORTAL) {
        pthread_mutex_unlock(&board_mutex);
        return NEXT_LEVEL;
    }

    if(result == DEAD_PACMAN) {
        pthread_mutex_unlock(&board_mutex);
        return QUIT_GAME;
    }

    if(!game_board->pacmans[0].alive) {
        pthread_mutex_unlock(&board_mutex);
        return QUIT_GAME;
    }
    pthread_mutex_unlock(&board_mutex);

    return CONTINUE_PLAY;
}

int main(int argc, char** argv) {
    if(argc != 2) {
        printf("Usage: %s <level_directory>\n", argv[0]);
        return 1;
    }

    char *level_dir_path = argv[1];

    char level_files[MAX_LEVELS][MAX_FILENAME];
    int num_levels = 0;
 
    DIR *dir;
    struct dirent *entry;

    dir = opendir(level_dir_path);
    if(dir == NULL) {
        perror("Error opening level directory");
        return 1;
    }

    while ((entry = readdir(dir)) != NULL && num_levels < MAX_LEVELS) {
        char *filename = entry->d_name;
        char *ext = strrchr(filename, '.');
 
        if(ext && strcmp(ext, ".lvl") == 0) {
            strncpy(level_files[num_levels], filename, MAX_FILENAME - 1);
            level_files[num_levels][MAX_FILENAME - 1] = '\0';
            num_levels++;
        }
    }
    closedir(dir);

    if(num_levels == 0) {
        printf("No .lvl files found: %s\n", level_dir_path);
        return 1;
    }

    qsort(level_files, num_levels, MAX_FILENAME, compare_filenames);

    srand((unsigned int)time(NULL));

    open_debug_file("debug.log");

    terminal_init();
 
    int accumulated_points = 0;
    bool end_game = false;
    bool is_checkpoint_child = false;
    board_t game_board;
    board_t saved_board;
    reset_board(&saved_board);
    bool has_save = false;
    int saved_level = -1;
    bool should_restore = false;

    for (int i = 0; i < num_levels; i++) {
 
        char full_level_path[512];
        snprintf(full_level_path, sizeof(full_level_path), "%s/%s", level_dir_path, level_files[i]);

        if(should_restore && has_save && saved_level >= 0) {
            if(game_board.board) {
                unload_level(&game_board);
            }
            copy_board_state(&game_board, &saved_board);
            accumulated_points = game_board.pacmans[0].points;
            g_board = &game_board;
            should_restore = false;
        }else {
            load_level(&game_board, full_level_path, accumulated_points);
            g_board = &game_board;
        }

        pthread_mutex_lock(&board_mutex);
        draw_board(&game_board, DRAW_MENU);
        pthread_mutex_unlock(&board_mutex);
        refresh_screen();

        static pthread_t ghost_threads[MAX_GHOSTS];
        static int ghost_thread_num = 0;
 
        if (ghost_thread_num == 0) {
            create_ghost_threads(&game_board, ghost_threads, &ghost_thread_num);
        }

        pid_t save_pid = -1;
        bool level_completed = false;
 
        while(true) {
            int result = play_board(&game_board);
 
            if(result == CONTINUE_PLAY) {
              screen_refresh(&game_board, DRAW_MENU);
              pthread_mutex_lock(&board_mutex);
              bool pacman_dead = !game_board.pacmans[0].alive;
              pthread_mutex_unlock(&board_mutex);

              if(pacman_dead) {
                  result = QUIT_GAME;
              }
            }

            if(result == NEXT_LEVEL) {
                if(i == num_levels - 1) {
                    screen_refresh(&game_board, DRAW_WIN);
                    sleep_ms(game_board.tempo);
                }

                if(is_checkpoint_child) {
                    exit(NEXT_LEVEL);
                }

                if(!has_save) {
                    if(saved_board.board) {
                        unload_level(&saved_board);
                        reset_board(&saved_board);
                    }
                    saved_level = -1;
                }
                break;
            }

            if(result == QUIT) {
                screen_refresh(&game_board, DRAW_GAME_OVER);
                sleep_ms(game_board.tempo);

                if(is_checkpoint_child) {
                    exit(QUIT);
                }else {
                    end_game = true;
                    break;
                }
            }

            if(result == CREATE_BACKUP) {
                if(save_pid == -1 && !has_save) {
                    if(saved_board.board) {
                        unload_level(&saved_board);
                    }
                    copy_board_state(&saved_board, &game_board);
                    has_save = true;
                    saved_level = i;

                    pid_t pid = fork();

                    if(pid == -1) {
                        perror("fork error");
                        end_game = true;
                        break;
                    }
 
                    if(pid > 0) {
                        save_pid = pid;
                        while(true) {
                            int status;
                            waitpid(save_pid, &status, 0);

                            if(WIFEXITED(status)) {
                                int exit_code = WEXITSTATUS(status);
                                if(exit_code == QUIT){
                                    end_game = true;
                                    save_pid = -1;
                                    break;
                                }
                                if(exit_code == QUIT_GAME) {
                                    restore_checkpoint(&game_board, &saved_board, &board_mutex, &accumulated_points);
                                    g_board = &game_board;
                                    pid_t new_pid = fork();
                                    if(new_pid == -1) {
                                        perror("fork error");
                                        end_game = true;
                                        save_pid = -1;
                                        break;
                                    }
                                    if(new_pid > 0) {
                                        save_pid = new_pid;
                                        continue;
                                    }else {
                                        save_pid = -1;
                                        is_checkpoint_child = true;
                                        g_board = &game_board;
                                        create_ghost_threads(&game_board, ghost_threads, &ghost_thread_num);
                                        i = saved_level - 1;
                                        should_restore = true;
                                        break;
                                    }
                                } else if(exit_code == CREATE_BACKUP) {
                                    save_pid = -1;
                                    break;
                                } else if(exit_code == NEXT_LEVEL) {
                                    accumulated_points = game_board.pacmans[0].points;
                                    if(i == num_levels - 1){
                                        end_game = true;
                                        save_pid = -1;
                                        break;
                                    }
                                    if(!has_save) {
                                        if(saved_board.board) {
                                            unload_level(&saved_board);
                                            reset_board(&saved_board);
                                        }
                                        saved_level = -1;
                                    }
                                    save_pid = -1;
                                    level_completed = true;
                                    break;
                                }
                            }
                        }

                        if(level_completed) {
                            break;
                        } else if(save_pid == -1 && !end_game) {
                            continue;
                        } else if(end_game) {
                            break;
                        }
                    }else {
                        save_pid = -1;
                        is_checkpoint_child = true;
                        g_board = &game_board;
                        create_ghost_threads(&game_board, ghost_threads, &ghost_thread_num);
                        continue;
                    }
                }else {
                    screen_refresh(&game_board, DRAW_MENU);
                    continue;
                }
            }
 
            if(result == QUIT_GAME) {
                screen_refresh(&game_board, DRAW_GAME_OVER);
                sleep_ms(game_board.tempo);

                if(is_checkpoint_child) {
                    exit(QUIT_GAME);
                } else if(has_save && saved_level >= 0) {
                    restore_checkpoint(&game_board, &saved_board, &board_mutex, &accumulated_points);
                    g_board = &game_board;
                    i = saved_level - 1;
                    should_restore = true;
                    break;
                }else {
                    end_game = true;
                    break;
                }
            }
            if(end_game) {
              break;
            }

            accumulated_points = game_board.pacmans[0].points;
        }

        g_board = NULL;
        for (int g = 0; g < ghost_thread_num; g++) {
            pthread_join(ghost_threads[g], NULL);
        }
        ghost_thread_num = 0;

        print_board(&game_board);
        unload_level(&game_board);

        if(end_game) {
            break;
        }
    }
    if(has_save) {
        unload_level(&saved_board);
    }
    close_debug_file();
    terminal_cleanup();
    return 0;
}


