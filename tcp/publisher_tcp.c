/*
 * publicador del sistema , usando TCP. Se conecta al broker y envía mensajes de eventos de un
 * partido, escritos desde la terminal.
 *
 * Protocolo de aplicación:
 *   PUB|<tema>|<contenido>\n
 *
 * Compilar:
 *   gcc publisher_tcp.c -o publisher_tcp
 *
 * Ejecutar:
 *   ./publisher_tcp <IP_broker> <puerto_broker> <tema>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>       // close()
#include <arpa/inet.h>      // inet_pton(), htons()
#include <sys/socket.h>     // socket(), connect(), send()
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

    
    
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0); // crea el descriptor del socket.
    //   AF_INET     -> IPv4
    //   SOCK_STREAM -> TCP (orientado a conexión)
    if (socket_fd < 0) {
        perror("Error al crear el socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in direccion_broker;
    direccion_broker.sin_family = AF_INET;
    direccion_broker.sin_port = htons(puerto_broker); // host-to-network short

    // convierte la IP en formato texto a su representación binaria dentro de la estructura sockaddr_in.
    if (inet_pton(AF_INET, ip_broker, &direccion_broker.sin_addr) <= 0) {
        fprintf(stderr, "Direccion IP invalida: %s\n", ip_broker);
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    if (connect(socket_fd, (struct sockaddr *)&direccion_broker, sizeof(direccion_broker)) < 0) { //establece la conexión TCP con el broker. Internamente dispara el three-way handshake

        perror("Error al conectar con el broker");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    printf("[Publisher] Conectado al broker %s:%d\n", ip_broker, puerto_broker);
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

        // Construir el mensaje según el protocolo: PUB|tema|contenido
        // Se usa TAM_BUFFER*2 para que el compilador no advierta sobre una posible truncación al concatenar tema+linea en un buffer del mismo tamaño que cada uno por separado.
        char mensaje_grande[TAM_BUFFER * 2];
        snprintf(mensaje_grande, sizeof(mensaje_grande), "PUB|%s|%s\n", tema, linea);
        strncpy(mensaje, mensaje_grande, TAM_BUFFER - 1);
        mensaje[TAM_BUFFER - 1] = '\0';

        // envía los bytes del mensaje por el socket TCP ya conectado
        ssize_t bytes_enviados = send(socket_fd, mensaje, strlen(mensaje), 0);

        if (bytes_enviados < 0) {
            perror("Error al enviar mensaje");
            break;
        }

        contador++;
        printf("[Publisher] Mensaje #%d publicado: \"%s\"\n\n", contador, linea);
    }

    printf("[Publisher] Se publicaron %d mensaje(s) en total.\n", contador);

    // cierra la conexión TCP de forma ordenada
    close(socket_fd);

    return 0;
}