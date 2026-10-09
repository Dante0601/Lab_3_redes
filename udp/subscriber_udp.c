/*
 * suscriptor del sistema, usando UDP. Envía al broker un datagrama de
 * suscripción a un partido y luego se queda esperando los datagramas que
 * el broker le reenvíe, imprimiéndolos en pantalla en tiempo real.
 *
 * Protocolo de aplicación:
 *   Envía  -> SUB|<tema>|\n                     (una sola vez, al iniciar)
 *   Recibe -> MSG|<tema>|#<N> <contenido>\n      (cada vez que hay una publicación)
 *
 * Usa el número de secuencia #N que pone el publisher para avisar si
 * detecta pérdida (salto hacia adelante) o desorden/duplicado (retroceso).
 * Supone un solo publisher por tema; con varios, sus secuencias se mezclan.
 *
 * Compilar:
 *   gcc -Wall -Wextra subscriber_udp.c -o subscriber_udp
 *
 * Ejecutar:
 *   ./subscriber_udp <IP_broker> <puerto_broker> <tema>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>       // close()
#include <arpa/inet.h>      // inet_pton(), htons()
#include <sys/socket.h>     // socket(), sendto(), recvfrom()
#include <netinet/in.h>     // struct sockaddr_in


#define TAM_BUFFER 1024

int main(int argc, char *argv[]) {

    if (argc != 4) {
        fprintf(stderr, "Uso: %s <IP_broker> <puerto_broker> <tema>\n", argv[0]);
        fprintf(stderr, "Ejemplo: %s 127.0.0.1 5000 EquipoA_vs_EquipoB\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *ip_broker = argv[1];
    int puerto_broker = atoi(argv[2]);
    const char *tema = argv[3];

    // socket(): crea el descriptor del socket.
    //   AF_INET    -> IPv4
    //   SOCK_DGRAM -> UDP (sin conexión, orientado a datagramas)
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        perror("Error al crear el socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in direccion_broker;
    memset(&direccion_broker, 0, sizeof(direccion_broker));
    direccion_broker.sin_family = AF_INET;
    direccion_broker.sin_port = htons(puerto_broker); // htons(): host-to-network short

    // inet_pton(): convierte la IP en formato texto a su representación
    // binaria dentro de la estructura sockaddr_in.
    if (inet_pton(AF_INET, ip_broker, &direccion_broker.sin_addr) <= 0) {
        fprintf(stderr, "Direccion IP invalida: %s\n", ip_broker);
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    // Construir y enviar el mensaje de suscripción: SUB|tema|
    char mensaje_sub[TAM_BUFFER];
    snprintf(mensaje_sub, sizeof(mensaje_sub), "SUB|%s|\n", tema);

    // sendto(): envía la suscripción como un datagrama. Al no haber bind()
    // explícito, el sistema operativo asigna aquí un puerto local efímero;
    // el broker guardará esa IP:puerto de origen y enviará los MSG a ella.
    // Si el broker no está corriendo, este datagrama se pierde sin error.
    if (sendto(socket_fd, mensaje_sub, strlen(mensaje_sub), 0,
               (struct sockaddr *)&direccion_broker, sizeof(direccion_broker)) < 0) {
        perror("Error al enviar la suscripcion");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    printf("[Subscriber] Suscripcion enviada al broker %s:%d (UDP)\n", ip_broker, puerto_broker);
    printf("[Subscriber] Suscrito al tema: %s\n", tema);
    printf("[Subscriber] Esperando mensajes... (Ctrl+C para salir)\n\n");
    fflush(stdout);

    char buffer[TAM_BUFFER];
    int ultima_secuencia = 0; // último #N recibido (0 = ninguno todavía)

    // Bucle de recepción: el subscriber no envía más mensajes propios,
    // solo se queda esperando lo que el broker le reenvíe.
    while (1) {
        struct sockaddr_in direccion_origen;
        socklen_t tam_direccion = sizeof(direccion_origen);

        // recvfrom(): bloquea hasta que llegue UN datagrama. Cada llamada
        // devuelve un mensaje completo, así que no hace falta acumular bytes
        // ni buscar '\n' como en TCP.
        ssize_t bytes_leidos = recvfrom(socket_fd, buffer, TAM_BUFFER - 1, 0,
                                        (struct sockaddr *)&direccion_origen, &tam_direccion);

        if (bytes_leidos < 0) {
            perror("Error en recvfrom");
            break;
        }

        buffer[bytes_leidos] = '\0';
        buffer[strcspn(buffer, "\n")] = '\0';

        // --- Parseo del mensaje recibido: MSG|tema|contenido ---
        char tipo[8] = {0};
        char tema_recibido[64] = {0};
        char contenido[TAM_BUFFER] = {0};

        char copia[TAM_BUFFER];
        strncpy(copia, buffer, TAM_BUFFER - 1);
        copia[TAM_BUFFER - 1] = '\0';

        char *token = strtok(copia, "|");
        if (token != NULL) strncpy(tipo, token, sizeof(tipo) - 1);

        token = strtok(NULL, "|");
        if (token != NULL) strncpy(tema_recibido, token, sizeof(tema_recibido) - 1);

        token = strtok(NULL, "|");
        if (token != NULL) strncpy(contenido, token, sizeof(contenido) - 1);

        if (strcmp(tipo, "MSG") != 0) {
            printf(">> (mensaje sin formato esperado): %s\n", buffer);
            fflush(stdout);
            continue;
        }

        // Revisar el número de secuencia "#N" para evidenciar pérdida o desorden.
        int secuencia;
        if (sscanf(contenido, "#%d", &secuencia) == 1) {
            if (ultima_secuencia != 0 && secuencia > ultima_secuencia + 1) {
                printf("[Subscriber] AVISO: salto de secuencia %d -> %d (se perdieron %d mensaje(s))\n",
                       ultima_secuencia, secuencia, secuencia - ultima_secuencia - 1);
            } else if (ultima_secuencia != 0 && secuencia <= ultima_secuencia) {
                printf("[Subscriber] AVISO: retroceso de secuencia %d -> %d (mensaje desordenado o duplicado)\n",
                       ultima_secuencia, secuencia);
            }
            if (secuencia > ultima_secuencia) {
                ultima_secuencia = secuencia;
            }
        }

        printf(">> [%s] %s\n", tema_recibido, contenido);
        fflush(stdout);
    }

    // close(): libera el socket. El broker no se entera (no hay conexión).
    close(socket_fd);

    return 0;
}
