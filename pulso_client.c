#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "structs.h"

static int recv_all(int sock, void *buf, size_t len);
static int send_all(int sock, const void *buf, size_t len);
static void print_separator(char c, int width);
static void print_hp_bar(int hp, int max_hp);

/* ================================================================
   main
   ================================================================ */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Uso: %s <IP> <PUERTO>\n", argv[0]);
        return 1;
    }

    /* ---- Conexion TCP ---- */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("[ERROR] socket"); return 1; }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_port        = htons(atoi(argv[2]));
    serv_addr.sin_addr.s_addr = inet_addr(argv[1]);

    printf("Conectando al servidor %s:%s...\n", argv[1], argv[2]);
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("[ERROR] connect");
        close(sock);
        return 1;
    }
    printf("Conectado al servidor %s:%s\n", argv[1], argv[2]);

    /* ---- Handshake ---- */
    printf("Ingresa tu nombre (max 31 chars): ");
    fflush(stdout);

    char name[MAX_NAME] = {0};
    if (fgets(name, MAX_NAME, stdin) != NULL)
        name[strcspn(name, "\n")] = '\0';
    if (name[0] == '\0') strncpy(name, "Jugador", MAX_NAME - 1);

    char name_buf[MAX_NAME] = {0};
    strncpy(name_buf, name, MAX_NAME - 1);
    if (send_all(sock, name_buf, MAX_NAME) != 0) {
        printf("[ERROR] No se pudo enviar el nombre\n");
        close(sock);
        return 1;
    }

    Handshake hs;
    if (recv_all(sock, &hs, sizeof(Handshake)) != 0) {
        printf("[ERROR] No se recibio handshake\n");
        close(sock);
        return 1;
    }

    /* Banner de bienvenida */
    print_separator('=', 40);
    printf("  Bienvenido, %-25s\n", name);
    printf("  Eres el Jugador #%-22d\n", hs.player_id);
    printf("  Partida con %d jugador(es)             \n", hs.n_players);
    print_separator('=', 40);

    printf("\nJugadores en la arena:\n");
    for (int i = 0; i < hs.n_players; i++)
        printf("  [%d] %s%s\n", i, hs.names[i],
               (i == hs.player_id) ? " <- TU" : "");

    printf("\nEsperando inicio de partida...\n");

    long      lamport  = 0;
    GameState prev_gs;
    int       first    = 1;
    memset(&prev_gs, 0, sizeof(prev_gs));

    /* ================================================================
       BUCLE DE JUEGO
       ================================================================ */
    while (1) {
        /* Recibir estado del servidor */
        GameState gs;
        if (recv_all(sock, &gs, sizeof(GameState)) != 0) {
            printf("[ERROR] Conexion perdida con el servidor\n");
            break;
        }

        /* ---- Fin de partida ---- */
        if (gs.winner_id != -1) {
            print_separator('=', 40);
            printf("        FIN DE LA PARTIDA              \n");
            print_separator('=', 40);
            for (int i = 0; i < gs.n_players; i++)
                printf("  [%d] %-20s HP: %d\n", i, gs.names[i], gs.hp[i]);
            print_separator('-', 40);

            if (gs.winner_id == DRAW_ID) {
                printf("\n  EMPATE — todos los jugadores eliminados\n\n");
            } else if (gs.winner_id == hs.player_id) {
                printf("\n  *** GANASTE! Eres el ultimo en pie. ***\n\n");
            } else {
                printf("\n  Ganador: [%d] %s\n\n",
                       gs.winner_id, gs.names[gs.winner_id]);
            }
            break;
        }

        /* ---- Mostrar resultado de la ronda anterior (si no es la inicial) ---- */
        if (!first) {
            int my_delta = gs.hp[hs.player_id] - prev_gs.hp[hs.player_id];
            if (my_delta < 0)
                printf("\n  [!] Recibiste %d de dano! HP: %d → %d\n",
                       -my_delta, prev_gs.hp[hs.player_id], gs.hp[hs.player_id]);
            else if (my_delta > 0)
                printf("\n  [+] Te curaste %d HP! HP: %d → %d\n",
                       my_delta, prev_gs.hp[hs.player_id], gs.hp[hs.player_id]);
        }

        /* ---- Mostrar HUD ---- */
        printf("\n");
        if (gs.fury_active) {
            print_separator('!', 40);
            printf("  *** FURIA COLECTIVA ACTIVA ***         \n");
            printf("  Ataques causan +1 dano extra           \n");
            print_separator('!', 40);
        }

        printf("---- Ronda %d ----\n", gs.round);
        for (int i = 0; i < gs.n_players; i++) {
            printf("  [%d] %-18s ", i, gs.names[i]);
            print_hp_bar(gs.hp[i], HP_INITIAL);
            printf(" HP:%2d%s%s\n",
                   gs.hp[i],
                   (i == hs.player_id) ? " <- TU" : "",
                   gs.alive[i]         ? ""        : " (eliminado)");
        }

        /* ---- Si estoy eliminado, solo observo ---- */
        if (!gs.alive[hs.player_id]) {
            printf("\nEstas eliminado. Observando la partida...\n");
            prev_gs = gs;
            first   = 0;
            continue;
        }

        /* ---- Menu de acciones ---- */
        printf("\n");
        print_separator('-', 40);
        printf("  A <id>  Atacar      (-3 HP)%s\n",
               gs.fury_active ? " [FURIA: -4]" : "");
        printf("  E       Esquivar    (inmune)\n");
        printf("  S <id>  Superataque (-6 HP)%s\n",
               gs.fury_active ? " [FURIA: -7]" : "");
        printf("  C       Curar       (+2 HP)%s\n",
               gs.hp[hs.player_id] <= 2 ? " [NO DISPONIBLE]" : "");
        print_separator('-', 40);

        /* Listar objetivos validos */
        printf("  Objetivos: ");
        for (int i = 0; i < gs.n_players; i++)
            if (gs.alive[i] && i != hs.player_id)
                printf("[%d]%s ", i, gs.names[i]);
        printf("\n");

        /* ---- Leer accion con timeout ---- */
        Action act;
        act.player_id  = hs.player_id;
        act.lamport_ts = ++lamport;
        act.target_id  = -1;
        act.action     = ACT_ESQUIVAR;

        printf("\nAccion [%d s]: ", ROUND_SECS);
        fflush(stdout);

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv = { ROUND_SECS, 0 };

        int ret = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
        if (ret > 0) {
            char input[64] = {0};
            if (fgets(input, sizeof(input), stdin) != NULL) {
                char cmd[4] = {0};
                int  target = -1;
                sscanf(input, " %3s %d", cmd, &target);
                char c = (char)toupper((unsigned char)cmd[0]);

                if (c == 'A') {
                    if (target < 0 || target >= gs.n_players)
                        printf("  [!] ID invalido. Aplicando ESQUIVAR.\n");
                    else if (target == hs.player_id)
                        printf("  [!] No puedes atacarte a ti mismo. ESQUIVAR.\n");
                    else if (!gs.alive[target])
                        printf("  [!] Ese jugador ya esta eliminado. ESQUIVAR.\n");
                    else {
                        act.action    = ACT_ATACAR;
                        act.target_id = target;
                        printf("  -> ATACAR a [%d] %s (-3 HP%s)\n",
                               target, gs.names[target],
                               gs.fury_active ? "  [FURIA: -4]" : "");
                    }
                } else if (c == 'E') {
                    act.action = ACT_ESQUIVAR;
                    printf("  -> ESQUIVAR (inmune este turno)\n");
                } else if (c == 'S') {
                    if (target < 0 || target >= gs.n_players)
                        printf("  [!] ID invalido. Aplicando ESQUIVAR.\n");
                    else if (target == hs.player_id)
                        printf("  [!] No puedes atacarte a ti mismo. ESQUIVAR.\n");
                    else if (!gs.alive[target])
                        printf("  [!] Ese jugador ya esta eliminado. ESQUIVAR.\n");
                    else {
                        act.action    = ACT_SUPERATAQUE;
                        act.target_id = target;
                        printf("  -> SUPERATAQUE a [%d] %s (-6 HP%s) [RIESGO: -2 si contraataca]\n",
                               target, gs.names[target],
                               gs.fury_active ? " [FURIA: -7]" : "");
                    }
                } else if (c == 'C') {
                    if (gs.hp[hs.player_id] <= 2)
                        printf("  [!] HP <= 2, no puedes curar. ESQUIVAR.\n");
                    else {
                        act.action = ACT_CURAR;
                        printf("  -> CURAR (+2 HP, maximo %d)\n", HP_INITIAL);
                    }
                } else {
                    printf("  [!] Accion '%s' desconocida. ESQUIVAR.\n", cmd);
                }
            }
        } else {
            printf("[TIMEOUT] Sin respuesta — accion automatica: ESQUIVAR\n");
            act.action = ACT_ESQUIVAR;
        }

        /* Enviar accion al servidor */
        if (send_all(sock, &act, sizeof(Action)) != 0) {
            printf("[ERROR] No se pudo enviar la accion\n");
            break;
        }

        prev_gs = gs;
        first   = 0;
    }

    close(sock);
    return 0;
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

static void print_separator(char c, int width) {
    for (int i = 0; i < width; i++) putchar(c);
    putchar('\n');
}

static void print_hp_bar(int hp, int max_hp) {
    putchar('[');
    for (int i = 0; i < max_hp; i++)
        putchar(i < hp ? '#' : ' ');
    putchar(']');
}
