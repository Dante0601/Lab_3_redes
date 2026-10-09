/*
 * publicador del sistema, usando UDP. Envía al broker mensajes de eventos de un
 * partido, escritos desde la terminal. No hay conexión: cada evento viaja
 * en un datagrama independiente.
 *
 * Protocolo de aplicación:
 *   PUB|<tema>|#<N> <contenido>\n
 *
 * El número de secuencia #N (1, 2, 3, ...) se antepone al contenido para que
 * el subscriber pueda evidenciar pérdida (saltos) y desorden (retrocesos),
 * ya que UDP no garantiza ninguna de las dos cosas.
 *
 * Compilar:
 *   gcc -Wall -Wextra publisher_udp.c -o publisher_udp
 *
 * Ejecutar:
 *   ./publisher_udp <IP_broker> <puerto_broker> <tema>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>       // close()
#include <arpa/inet.h>      // inet_pton(), htons()
#include <sys/socket.h>     // socket(), sendto()
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

    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0); // crea el descriptor del socket.
    //   AF_INET    -> IPv4
    //   SOCK_DGRAM -> UDP (sin conexión, orientado a datagramas)
    if (socket_fd < 0) {
        perror("Error al crear el socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in direccion_broker;
    memset(&direccion_broker, 0, sizeof(direccion_broker));
    direccion_broker.sin_family = AF_INET;
    direccion_broker.sin_port = htons(puerto_broker); // host-to-network short

    // convierte la IP en formato texto a su representación binaria dentro de la estructura sockaddr_in.
    if (inet_pton(AF_INET, ip_broker, &direccion_broker.sin_addr) <= 0) {
        fprintf(stderr, "Direccion IP invalida: %s\n", ip_broker);
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    // No hay connect() ni handshake: el destino se indica en cada sendto().
    // Por eso el publisher no sabe si el broker está corriendo.
    printf("[Publisher] Enviando al broker %s:%d (UDP)\n", ip_broker, puerto_broker);
    printf("[Publisher] Publicando en el tema: %s\n", tema);
    printf("[Publisher] Escriba cada evento y presione Enter para publicarlo.\n");
    printf("[Publisher] Escriba 'salir' para terminar.\n\n");

    char linea[TAM_BUFFER];
    char mensaje[TAM_BUFFER];
    int contador = 0;

    while (1) {
        printf("Evento #%d > ", contador + 1);
        fflush(stdout);

        // lee una línea completa escrita por el usuario.
        if (fgets(linea, sizeof(linea), stdin) == NULL) {
            break; // EOF (ej. Ctrl+D)
        }

        // Quitar el salto de línea que fgets() deja al final.
        linea[strcspn(linea, "\n")] = '\0';

        if (strlen(linea) == 0) {
            continue; // ignorar líneas vacías
        }

        if (strcmp(linea, "salir") == 0) {
            break;
        }

        // Construir el mensaje según el protocolo: PUB|tema|#N contenido
        // Se usa TAM_BUFFER*2 para que el compilador no advierta sobre una posible truncación.
        char mensaje_grande[TAM_BUFFER * 2];
        snprintf(mensaje_grande, sizeof(mensaje_grande), "PUB|%s|#%d %s\n", tema, contador + 1, linea);
        strncpy(mensaje, mensaje_grande, TAM_BUFFER - 1);
        mensaje[TAM_BUFFER - 1] = '\0';

        // sendto(): envía el mensaje como UN datagrama a la dirección del broker.
        // Que retorne sin error solo significa que salió de esta máquina,
        // no que el broker lo haya recibido (UDP no tiene ACKs).
        ssize_t bytes_enviados = sendto(socket_fd, mensaje, strlen(mensaje), 0,
                                        (struct sockaddr *)&direccion_broker, sizeof(direccion_broker));

        if (bytes_enviados < 0) {
            perror("Error al enviar mensaje");
            break;
        }

        contador++;
        printf("[Publisher] Mensaje #%d publicado: \"%s\"\n\n", contador, linea);
    }

    printf("[Publisher] Se publicaron %d mensaje(s) en total.\n", contador);

    // close(): libera el socket. No hay cierre de conexión que notificar al broker.
    close(socket_fd);

    return 0;
}
