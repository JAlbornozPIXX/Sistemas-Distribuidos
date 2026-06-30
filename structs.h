#ifndef STRUCTS_H
#define STRUCTS_H

#define MAX_PLAYERS 8
#define MAX_NAME    32
#define HP_INITIAL  10
#define ROUND_SECS  5
#define PORT_DEFAULT 9090

/* Indicador especial de empate (todos en 0 HP) */
#define DRAW_ID     -2

typedef enum {
    ACT_ATACAR      = 0,
    ACT_ESQUIVAR    = 1,
    ACT_SUPERATAQUE = 2,
    ACT_CURAR       = 3,
    ACT_TIMEOUT     = 4   /* sin respuesta en tiempo */
} ActionType;

/* Mensaje: cliente -> servidor (una accion por ronda) */
typedef struct {
    int       player_id;
    ActionType action;
    int       target_id;   /* -1 si no aplica */
    long      lamport_ts;
} Action;

/* Estado del juego (servidor -> clientes, broadcast cada ronda) */
typedef struct {
    int  round;
    int  hp[MAX_PLAYERS];
    int  alive[MAX_PLAYERS];
    char names[MAX_PLAYERS][MAX_NAME];
    int  n_players;
    int  fury_active;   /* 1 si Furia colectiva activa */
    int  winner_id;     /* -1 continua, -2 empate, >= 0 ganador */
} GameState;

/* Handshake inicial: servidor -> cliente */
typedef struct {
    int  player_id;
    int  n_players;
    char names[MAX_PLAYERS][MAX_NAME];
} Handshake;

#endif /* STRUCTS_H */
