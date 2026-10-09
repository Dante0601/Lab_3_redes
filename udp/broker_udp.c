/*
 * Broker (intermediario) del sistema publicador-suscriptor, usando
 * sockets UDP. Un solo socket recibe TODOS los datagramas (de publishers
 * y subscribers) con recvfrom(), sin conexiones, sin accept() y sin select().
 *
 * Protocolo de aplicación (texto plano, un mensaje por datagrama):
 *
 *   SUB|<tema>|                       un Subscriber se suscribe a <tema>
 *   PUB|<tema>|<contenido>            un Publisher publica <contenido> en <tema>
 *   MSG|<tema>|<contenido>            el Broker reenvía el mensaje a cada Subscriber suscrito a <tema>
 *
 * Diferencias frente a TCP:
 *   - No hay conexión: el broker identifica a cada subscriber por su
 *     dirección (IP:puerto) de origen, que recvfrom() entrega con cada datagrama.
 *   - UDP conserva las fronteras de mensaje: un recvfrom() devuelve
 *     exactamente un datagrama, por eso no se necesita acumulador de líneas.
 *   - UDP no garantiza entrega ni orden: un datagrama puede perderse,
 *     duplicarse o llegar desordenado, y nadie lo retransmite.
 *
 * Compilar:
 *   gcc -Wall -Wextra broker_udp.c -o broker_udp
 *
 * Ejecutar:
 *   ./broker_udp
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>       // close()
#include <arpa/inet.h>      // inet_ntoa(), htons(), ntohs()
#include <sys/socket.h>     // socket(), bind(), recvfrom(), sendto()
#include <netinet/in.h>     // struct sockaddr_in

#define PORT 5000
#define MAX_SUSCRIPTORES 50
#define TAM_BUFFER 1024
#define TAM_TEMA 64


// Representa a un subscriber registrado. Como UDP no tiene conexiones,
// lo único que identifica al subscriber es su dirección IP:puerto.
typedef struct {
    int en_uso;                    // 0 si esta posición del arreglo está libre
    struct sockaddr_in direccion;  // IP y puerto desde donde el subscriber mandó el SUB
    char tema[TAM_TEMA];           // tema al que está suscrito
} Suscriptor;

Suscriptor suscriptores[MAX_SUSCRIPTORES];

// Inicializa la tabla de suscriptores: todas las posiciones libres.
void inicializar_suscriptores() {
    for (int i = 0; i < MAX_SUSCRIPTORES; i++) {
        suscriptores[i].en_uso = 0;
        suscriptores[i].tema[0] = '\0';
    }
}

// Compara dos direcciones IPv4 (IP y puerto).
int misma_direccion(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

// Registra (o actualiza) la suscripción de la dirección dada (comando SUB).
// Si la misma IP:puerto ya estaba registrada, solo se actualiza su tema,
// así un SUB repetido no crea una entrada duplicada.
void registrar_suscripcion(const struct sockaddr_in *direccion, const char *tema) {
    int libre = -1;

    for (int i = 0; i < MAX_SUSCRIPTORES; i++) {
        if (suscriptores[i].en_uso && misma_direccion(&suscriptores[i].direccion, direccion)) {
            strncpy(suscriptores[i].tema, tema, TAM_TEMA - 1);
            suscriptores[i].tema[TAM_TEMA - 1] = '\0';
            printf("[Broker] Suscriptor %s:%d actualizado al tema '%s'\n",
                   inet_ntoa(direccion->sin_addr), ntohs(direccion->sin_port), tema);
            return;
        }
        if (!suscriptores[i].en_uso && libre == -1) {
            libre = i;
        }
    }

    if (libre == -1) {
        printf("[Broker] Limite de suscriptores alcanzado, se ignora el SUB.\n");
        return;
    }

    suscriptores[libre].en_uso = 1;
    suscriptores[libre].direccion = *direccion;
    strncpy(suscriptores[libre].tema, tema, TAM_TEMA - 1);
    suscriptores[libre].tema[TAM_TEMA - 1] = '\0';
    printf("[Broker] Suscriptor %s:%d suscrito al tema '%s'\n",
           inet_ntoa(direccion->sin_addr), ntohs(direccion->sin_port), tema);
}

// Reenvía el mensaje publicado a todos los subscribers cuyo tema coincida.
void difundir_mensaje(int socket_fd, const char *tema, const char *contenido) {
    char mensaje_salida[TAM_BUFFER];
    snprintf(mensaje_salida, TAM_BUFFER, "MSG|%s|%s\n", tema, contenido);

    int enviados = 0;
    for (int i = 0; i < MAX_SUSCRIPTORES; i++) {
        if (suscriptores[i].en_uso && strcmp(suscriptores[i].tema, tema) == 0) {

            // sendto(): envía UN datagrama a la dirección indicada. A diferencia
            // de send() en TCP, no hay garantía de que llegue ni de que llegue
            // en orden; si se pierde, nadie lo retransmite.
            if (sendto(socket_fd, mensaje_salida, strlen(mensaje_salida), 0,
                       (struct sockaddr *)&suscriptores[i].direccion,
                       sizeof(suscriptores[i].direccion)) < 0) {
                perror("Error en sendto");
            } else {
                enviados++;
            }
        }
    }

    printf("[Broker] Mensaje del tema '%s' difundido a %d suscriptor(es)\n", tema, enviados);
}

// Procesa un datagrama recibido: lo parsea según el protocolo "TIPO|TEMA|CONTENIDO"
void procesar_mensaje(int socket_fd, const struct sockaddr_in *origen, char *buffer) {
    buffer[strcspn(buffer, "\n")] = '\0'; // quitar salto de línea final si viene

    char tipo[8] = {0};
    char tema[TAM_TEMA] = {0};
    char contenido[TAM_BUFFER] = {0};

    char *token = strtok(buffer, "|");
    if (token != NULL) strncpy(tipo, token, sizeof(tipo) - 1);

    token = strtok(NULL, "|");
    if (token != NULL) strncpy(tema, token, sizeof(tema) - 1);

    token = strtok(NULL, "|");
    if (token != NULL) strncpy(contenido, token, sizeof(contenido) - 1);

    if (strcmp(tipo, "SUB") == 0) {
        registrar_suscripcion(origen, tema);
    } else if (strcmp(tipo, "PUB") == 0) {
        printf("[Broker] PUB recibido -> tema='%s' contenido='%s'\n", tema, contenido);
        difundir_mensaje(socket_fd, tema, contenido);
    } else {
        printf("[Broker] Mensaje con formato desconocido: %s\n", buffer);
    }
}

int main() {
    int socket_servidor;
    struct sockaddr_in direccion_servidor;

    inicializar_suscriptores();

    // AF_INET (IPv4), SOCK_DGRAM (UDP), protocolo por defecto.
    socket_servidor = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_servidor < 0) {
        perror("Error al crear el socket");
        exit(EXIT_FAILURE);
    }

    // Permite reiniciar el broker rápidamente sin el error "Address already in use".
    int opt = 1;
    setsockopt(socket_servidor, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&direccion_servidor, 0, sizeof(direccion_servidor));
    direccion_servidor.sin_family = AF_INET;
    direccion_servidor.sin_addr.s_addr = INADDR_ANY;
    direccion_servidor.sin_port = htons(PORT);

    // bind(): asocia el socket a la IP/puerto locales. En UDP no hay
    // listen() ni accept(): tras el bind el socket ya puede recibir datagramas.
    if (bind(socket_servidor, (struct sockaddr *)&direccion_servidor, sizeof(direccion_servidor)) < 0) {
        perror("Error en bind");
        close(socket_servidor);
        exit(EXIT_FAILURE);
    }

    printf("[Broker] Escuchando UDP en el puerto %d...\n", PORT);
    fflush(stdout);

    while (1) {
        char buffer[TAM_BUFFER];
        struct sockaddr_in direccion_origen;
        socklen_t tam_direccion = sizeof(direccion_origen);

        // recvfrom(): bloquea hasta que llegue UN datagrama de cualquier
        // cliente. Devuelve el datagrama completo (nunca medio ni dos pegados)
        // y llena direccion_origen con la IP:puerto de quien lo envió.
        ssize_t bytes_leidos = recvfrom(socket_servidor, buffer, TAM_BUFFER - 1, 0,
                                        (struct sockaddr *)&direccion_origen, &tam_direccion);

        if (bytes_leidos < 0) {
            perror("Error en recvfrom");
            continue;
        }

        buffer[bytes_leidos] = '\0';
        procesar_mensaje(socket_servidor, &direccion_origen, buffer);
        fflush(stdout);
    }

    close(socket_servidor);
    return 0;
}
