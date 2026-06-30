#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "structs.h"

/* ---- Prototipos ---- */
void run_gateway(int n_players);
void run_player_manager(int rank, int n_players);

static int         recv_all(int sock, void *buf, size_t len);
static int         send_all(int sock, const void *buf, size_t len);
static int         compare_actions(const void *a, const void *b);
static const char *action_name(ActionType a);
static void        log_round(int round, int n_players,
                              Action *actions_log, GameState *gs,
                              int *prev_hp, int total_hp);
static void        sanitize_actions(Action *actions, int n_players, GameState *gs);

/* ================================================================
   main
   ================================================================ */
int main(int argc, char *argv[]) {
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (argc < 2) {
        if (rank == 0) printf("Uso: mpirun -np N+1 %s N\n", argv[0]);
        MPI_Finalize();
        return 1;
    }

    int n_players = atoi(argv[1]);
    if (n_players < 2 || n_players > MAX_PLAYERS || size != n_players + 1) {
        if (rank == 0)
            printf("[ERROR] Se requieren %d ranks (n+1).\n", n_players + 1);
        MPI_Finalize();
        return 1;
    }

    if (rank == 0)
        run_gateway(n_players);
    else
        run_player_manager(rank, n_players);

    MPI_Finalize();
    return 0;
}

/* ================================================================
   Gateway TCP  (Rank 0)
   ================================================================ */
void run_gateway(int n_players) {
    int       server_fd;
    int       client_socks[MAX_PLAYERS];
    GameState gs;
    Action    actions[MAX_PLAYERS];
    long      lamport = 0;

    /* --- Crear socket TCP --- */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("[ERROR] socket"); MPI_Abort(MPI_COMM_WORLD, 1); }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT_DEFAULT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[ERROR] bind"); MPI_Abort(MPI_COMM_WORLD, 1);
    }
    listen(server_fd, MAX_PLAYERS);

    printf("[Servidor] Escuchando en puerto %d — esperando %d jugadores...\n",
           PORT_DEFAULT, n_players);

    /* --- Inicializar GameState (round 0) --- */
    memset(&gs, 0, sizeof(gs));
    gs.n_players  = n_players;
    gs.winner_id  = -1;
    gs.fury_active = 0;
    gs.round      = 0;
    for (int i = 0; i < n_players; i++) {
        gs.hp[i]    = HP_INITIAL;
        gs.alive[i] = 1;
    }

    /* --- Aceptar N conexiones --- */
    for (int i = 0; i < n_players; i++) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        client_socks[i] = accept(server_fd, (struct sockaddr *)&cli, &cli_len);
        if (client_socks[i] < 0) { perror("[ERROR] accept"); MPI_Abort(MPI_COMM_WORLD, 1); }

        /* Recibir nombre */
        char buf[MAX_NAME] = {0};
        if (recv_all(client_socks[i], buf, MAX_NAME) != 0) {
            printf("[ERROR] No se recibio nombre del jugador %d\n", i);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        buf[MAX_NAME - 1] = '\0';
        strncpy(gs.names[i], buf, MAX_NAME - 1);
        printf("[Servidor] Jugador %d conectado: %s\n", i, gs.names[i]);
    }

    printf("[Servidor] Todos conectados. ¡Comienza PULSO!\n\n");

    /* --- Enviar Handshake a cada cliente --- */
    for (int i = 0; i < n_players; i++) {
        Handshake hs;
        hs.player_id = i;
        hs.n_players = n_players;
        memcpy(hs.names, gs.names, sizeof(gs.names));
        send_all(client_socks[i], &hs, sizeof(Handshake));
    }

    /* --- Broadcast estado inicial (round 0) a todos los ranks MPI --- */
    MPI_Bcast(&gs, sizeof(GameState), MPI_BYTE, 0, MPI_COMM_WORLD);

    /* --- Enviar GameState inicial (round 0) a clientes TCP
           CRITICO: el cliente necesita esto para mostrar el HUD inicial
           y enviar su primera accion                                    --- */
    printf("[Servidor] Enviando estado inicial a clientes...\n");
    for (int i = 0; i < n_players; i++)
        send_all(client_socks[i], &gs, sizeof(GameState));

    /* ================================================================
       CICLO DE RONDAS
       ================================================================ */
    while (gs.winner_id == -1) {
        gs.round++;

        /* === Inicializar acciones por defecto === */
        for (int i = 0; i < n_players; i++) {
            actions[i].player_id  = i;
            actions[i].action     = ACT_ESQUIVAR;
            actions[i].target_id  = -1;
            actions[i].lamport_ts = 0;
        }

        /* === Recolectar acciones de clientes vivos con timeout === */
        int responded[MAX_PLAYERS] = {0};
        for (int i = 0; i < n_players; i++)
            if (!gs.alive[i]) responded[i] = 1; /* muertos no envian */

        /* Deadline = ahora + ROUND_SECS */
        struct timeval deadline;
        gettimeofday(&deadline, NULL);
        deadline.tv_sec += ROUND_SECS;

        int n_pending = 0;
        for (int i = 0; i < n_players; i++)
            if (!responded[i]) n_pending++;

        while (n_pending > 0) {
            struct timeval now;
            gettimeofday(&now, NULL);
            long rem_us = (long)(deadline.tv_sec  - now.tv_sec)  * 1000000L
                        + (long)(deadline.tv_usec - now.tv_usec);
            if (rem_us <= 0) break;

            fd_set rfds;
            FD_ZERO(&rfds);
            int max_fd = 0;
            for (int i = 0; i < n_players; i++) {
                if (!responded[i] && client_socks[i] >= 0) {
                    FD_SET(client_socks[i], &rfds);
                    if (client_socks[i] > max_fd) max_fd = client_socks[i];
                }
            }

            struct timeval tv;
            tv.tv_sec  = rem_us / 1000000L;
            tv.tv_usec = rem_us % 1000000L;

            int ret = select(max_fd + 1, &rfds, NULL, NULL, &tv);
            if (ret <= 0) break;

            for (int i = 0; i < n_players; i++) {
                if (!responded[i] && client_socks[i] >= 0 &&
                    FD_ISSET(client_socks[i], &rfds)) {
                    Action recv_act;
                    if (recv_all(client_socks[i], &recv_act, sizeof(Action)) == 0) {
                        actions[i] = recv_act;
                        /* Actualizar reloj de Lamport: max(local, recibido) + 1 */
                        lamport = (recv_act.lamport_ts >= lamport)
                                  ? recv_act.lamport_ts + 1
                                  : lamport + 1;
                    }
                    /* desconectado o timeout: queda ESQUIVAR por defecto */
                    responded[i] = 1;
                    n_pending--;
                }
            }
        }

        /* Asignar Lamport a acciones que no llegaron a tiempo */
        for (int i = 0; i < n_players; i++) {
            if (gs.alive[i] && !responded[i]) {
                actions[i].action     = ACT_ESQUIVAR;
                actions[i].lamport_ts = ++lamport;
                responded[i] = 1;
            }
        }

        /* Validar acciones: rechazar objetivos invalidos */
        sanitize_actions(actions, n_players, &gs);

        /* Guardar copia por player_id para el log (antes de ordenar) */
        Action actions_log[MAX_PLAYERS];
        memcpy(actions_log, actions, sizeof(actions));

        /* === Ordenar acciones por Lamport (menor ts golpea primero) === */
        qsort(actions, n_players, sizeof(Action), compare_actions);

        /* === Broadcast acciones a todos los ranks === */
        MPI_Bcast(actions, sizeof(Action) * n_players, MPI_BYTE, 0, MPI_COMM_WORLD);
        MPI_Barrier(MPI_COMM_WORLD);

        /* Rank 0 no tiene jugador propio: local_hp = 0 */
        int local_hp = 0;
        int all_hp[MAX_PLAYERS + 1];
        memset(all_hp, 0, sizeof(all_hp));

        /* === Recolectar HP actualizado de cada rank === */
        MPI_Gather(&local_hp, 1, MPI_INT,
                   all_hp,    1, MPI_INT, 0, MPI_COMM_WORLD);

        /* === Calcular HP total para Furia (todos los ranks) === */
        int total_hp = 0;
        MPI_Allreduce(&local_hp, &total_hp, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

        /* Guardar HP previo para el log de daños */
        int prev_hp[MAX_PLAYERS];
        for (int i = 0; i < n_players; i++) prev_hp[i] = gs.hp[i];

        /* === Actualizar GameState con HP recibidos === */
        for (int i = 0; i < n_players; i++) {
            gs.hp[i]    = all_hp[i + 1];
            gs.alive[i] = (gs.hp[i] > 0) ? 1 : 0;
        }

        /* === Furia Colectiva para la ronda siguiente === */
        gs.fury_active = (total_hp < (int)(0.30 * n_players * HP_INITIAL)) ? 1 : 0;

        /* === Detectar ganador o empate === */
        int alive_count = 0, last_alive = -1;
        for (int i = 0; i < n_players; i++) {
            if (gs.alive[i]) { alive_count++; last_alive = i; }
        }
        if      (alive_count == 1) gs.winner_id = last_alive;
        else if (alive_count == 0) gs.winner_id = DRAW_ID;

        /* === Log historial de ronda en consola del servidor === */
        log_round(gs.round, n_players, actions_log, &gs, prev_hp, total_hp);

        /* === Enviar GameState actualizado a todos los clientes TCP === */
        for (int i = 0; i < n_players; i++) {
            if (client_socks[i] >= 0) {
                if (send_all(client_socks[i], &gs, sizeof(GameState)) != 0) {
                    close(client_socks[i]);
                    client_socks[i] = -1;
                }
            }
        }

        /* === Broadcast GameState actualizado a ranks MPI === */
        MPI_Bcast(&gs, sizeof(GameState), MPI_BYTE, 0, MPI_COMM_WORLD);
        MPI_Barrier(MPI_COMM_WORLD);
    }

    /* === Cerrar sockets === */
    for (int i = 0; i < n_players; i++)
        if (client_socks[i] >= 0) close(client_socks[i]);
    close(server_fd);
}

/* ================================================================
   Manager de jugador  (Rank 1..N)
   ================================================================ */
void run_player_manager(int rank, int n_players) {
    int       my_idx = rank - 1;
    int       my_hp  = HP_INITIAL;
    Action    actions[MAX_PLAYERS];
    GameState gs;

    /* --- Recibir estado inicial desde Rank 0 --- */
    MPI_Bcast(&gs, sizeof(GameState), MPI_BYTE, 0, MPI_COMM_WORLD);

    while (1) {
        /* === Recibir acciones del broadcast === */
        MPI_Bcast(actions, sizeof(Action) * n_players, MPI_BYTE, 0, MPI_COMM_WORLD);
        MPI_Barrier(MPI_COMM_WORLD);

        /* === Aplicar efectos sobre my_hp segun actions[] === */
        if (gs.alive[my_idx] && my_hp > 0) {

            int hp_before  = my_hp;
            int is_dodging = 0;

            /* Buscar mi accion en el arreglo (ya ordenado por Lamport) */
            Action my_action = { my_idx, ACT_ESQUIVAR, -1, 0 };
            for (int i = 0; i < n_players; i++) {
                if (actions[i].player_id == my_idx) {
                    my_action = actions[i];
                    break;
                }
            }

            is_dodging = (my_action.action == ACT_ESQUIVAR ||
                          my_action.action == ACT_TIMEOUT);

            /* --- Aplicar ataques entrantes en orden Lamport --- */
            for (int i = 0; i < n_players; i++) {
                if (my_hp <= 0) break;                          /* ya muerto */
                if (actions[i].target_id  != my_idx)  continue;
                if (actions[i].player_id  == my_idx)  continue; /* no autoataque */
                if (!gs.alive[actions[i].player_id])  continue; /* atacante muerto */

                if (actions[i].action == ACT_ATACAR && !is_dodging) {
                    my_hp -= gs.fury_active ? 4 : 3;
                } else if (actions[i].action == ACT_SUPERATAQUE && !is_dodging) {
                    my_hp -= gs.fury_active ? 7 : 6;
                }
            }
            if (my_hp < 0) my_hp = 0;

            /* --- Curar (solo si HP inicial > 2 y sigo vivo) --- */
            if (my_action.action == ACT_CURAR && hp_before > 2 && my_hp > 0) {
                my_hp += 2;
                if (my_hp > HP_INITIAL) my_hp = HP_INITIAL;
            }

            /* --- Riesgo del Superataque: objetivo me ataca → -2 HP --- */
            if (my_action.action == ACT_SUPERATAQUE && my_action.target_id >= 0) {
                int tgt = my_action.target_id;
                for (int i = 0; i < n_players; i++) {
                    if (actions[i].player_id == tgt) {
                        if (actions[i].target_id == my_idx &&
                            (actions[i].action == ACT_ATACAR ||
                             actions[i].action == ACT_SUPERATAQUE)) {
                            my_hp -= 2;
                            if (my_hp < 0) my_hp = 0;
                        }
                        break;
                    }
                }
            }
        }
        if (my_hp < 0) my_hp = 0;

        /* === Enviar HP actualizado a Rank 0 === */
        MPI_Gather(&my_hp, 1, MPI_INT,
                   NULL,   0, MPI_INT, 0, MPI_COMM_WORLD);

        /* === Participar en Allreduce de HP total === */
        int total_hp = 0;
        MPI_Allreduce(&my_hp, &total_hp, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

        /* === Recibir GameState actualizado (fury, alive, winner) === */
        MPI_Bcast(&gs, sizeof(GameState), MPI_BYTE, 0, MPI_COMM_WORLD);
        MPI_Barrier(MPI_COMM_WORLD);

        /* === Verificar condicion de fin de partida === */
        if (gs.winner_id != -1) break;
    }
}

/* ================================================================
   Funciones auxiliares
   ================================================================ */

static int recv_all(int sock, void *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t r = recv(sock, (char *)buf + total, len - total, 0);
        if (r <= 0) return -1;
        total += (size_t)r;
    }
    return 0;
}

static int send_all(int sock, const void *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t s = send(sock, (const char *)buf + total, len - total, 0);
        if (s <= 0) return -1;
        total += (size_t)s;
    }
    return 0;
}

static int compare_actions(const void *a, const void *b) {
    const Action *aa = (const Action *)a;
    const Action *bb = (const Action *)b;
    if (aa->lamport_ts != bb->lamport_ts)
        return (aa->lamport_ts < bb->lamport_ts) ? -1 : 1;
    return aa->player_id - bb->player_id; /* desempate por player_id */
}

static const char *action_name(ActionType a) {
    switch (a) {
        case ACT_ATACAR:      return "ATACAR";
        case ACT_ESQUIVAR:    return "ESQUIVA";
        case ACT_SUPERATAQUE: return "SUPERATAQUE";
        case ACT_CURAR:       return "CURAR";
        case ACT_TIMEOUT:     return "TIMEOUT->ESQUIVA";
        default:              return "?";
    }
}

/* Validar acciones en Rank 0 antes del broadcast */
static void sanitize_actions(Action *actions, int n_players, GameState *gs) {
    for (int i = 0; i < n_players; i++) {
        if (!gs->alive[i]) continue; /* muertos ya tienen ESQUIVAR */

        ActionType a = actions[i].action;
        int        t = actions[i].target_id;

        if (a == ACT_ATACAR || a == ACT_SUPERATAQUE) {
            /* Objetivo invalido, muerto, o autoataque → ESQUIVAR */
            if (t < 0 || t >= n_players || t == i || !gs->alive[t]) {
                actions[i].action    = ACT_ESQUIVAR;
                actions[i].target_id = -1;
            }
        }
        /* CURAR con HP <= 2 → ESQUIVAR (sin efecto, spec lo prohíbe) */
        if (a == ACT_CURAR && gs->hp[i] <= 2) {
            actions[i].action = ACT_ESQUIVAR;
        }
    }
}

/* Imprimir historial de ronda en consola del servidor */
static void log_round(int round, int n_players,
                       Action *actions_log, GameState *gs,
                       int *prev_hp, int total_hp) {

    int fury_thresh = (int)(0.30 * n_players * HP_INITIAL);

    printf("\n=== Ronda %d %s===\n",
           round, gs->fury_active ? "[FURIA ACTIVA PROXIMA RONDA] " : "");

    for (int i = 0; i < n_players; i++) {
        char act_desc[48] = {0};
        ActionType a = actions_log[i].action;

        if ((a == ACT_ATACAR || a == ACT_SUPERATAQUE) &&
             actions_log[i].target_id >= 0) {
            snprintf(act_desc, sizeof(act_desc), "%s->[%d]%s",
                     action_name(a),
                     actions_log[i].target_id,
                     gs->names[actions_log[i].target_id]);
        } else {
            snprintf(act_desc, sizeof(act_desc), "%s", action_name(a));
        }

        int delta = gs->hp[i] - prev_hp[i];

        printf("  [%d] %-18s HP:%2d -> %2d (%+3d)  %-30s (Lamport:%ld)%s\n",
               i,
               gs->names[i],
               prev_hp[i],
               gs->hp[i],
               delta,
               act_desc,
               actions_log[i].lamport_ts,
               gs->alive[i] ? "" : " [ELIMINADO]");
    }

    printf("  HP total vivos: %d / %d  |  Umbral Furia: %d%s\n",
           total_hp, n_players * HP_INITIAL, fury_thresh,
           gs->fury_active ? "  *** FURIA ACTIVA ***" : "");

    if (gs->winner_id >= 0)
        printf("\n  *** GANADOR: [%d] %s ***\n",
               gs->winner_id, gs->names[gs->winner_id]);
    else if (gs->winner_id == DRAW_ID)
        printf("\n  *** EMPATE — todos eliminados ***\n");
}
