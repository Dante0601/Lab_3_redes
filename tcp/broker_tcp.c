/*
 * Broker (intermediario) del sistema publicador-suscriptor, usando
 * sockets TCP y multiplexación de E/S con select() (un solo hilo
 * atiende a todos los clientes conectados simultáneamente).
 *
 * Protocolo de aplicación (texto plano, un mensaje por línea):
 *
 *   SUB|<tema>|                       un Subscriber se suscribe a <tema>
 *   PUB|<tema>|<contenido>            un Publisher publica <contenido> en <tema>
 *   MSG|<tema>|<contenido>            el Broker reenvía el mensaje a cada Subscriber suscrito a <tema>
 *
 * Compilar:
 *   gcc broker_tcp.c -o broker_tcp
 *
 * Ejecutar:
 *   ./broker_tcp
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>       // close()
#include <arpa/inet.h>      // inet_ntoa(), htons(), ntohs()
#include <sys/socket.h>     // socket(), bind(), listen(), accept(), send(), recv()
#include <netinet/in.h>     // struct sockaddr_in
#include <sys/select.h>     // select(), fd_set, FD_SET, FD_ZERO, FD_ISSET

#define PORT 5000
#define MAX_CLIENTES 50
#define TAM_BUFFER 1024
#define TAM_TEMA 64


 //Representa a un cliente conectado.
typedef struct {
    int socket_fd;             // -1 si esta posición del arreglo está libre
    char tema[TAM_TEMA];       // tema al que está suscrito (vacío si no aplica)
    int es_subscriber;         // 1 si ya mandó un SUB
    char acumulado[TAM_BUFFER]; // bytes recibidos que aún no forman una línea completa
    int largo_acumulado;       // cantidad de bytes válidos en "acumulado"
} Cliente;

Cliente clientes[MAX_CLIENTES];

//Inicializa el arreglo de clientes. 
void inicializar_clientes() {
    for (int i = 0; i < MAX_CLIENTES; i++) {
        clientes[i].socket_fd = -1;
        clientes[i].tema[0] = '\0';
        clientes[i].es_subscriber = 0;
        clientes[i].largo_acumulado = 0;
    }
}

// Busca la primera posición libre del arreglo y registra ahí al nuevo cliente. 
void agregar_cliente(int socket_fd) {
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (clientes[i].socket_fd == -1) {
            clientes[i].socket_fd = socket_fd;
            clientes[i].tema[0] = '\0';
            clientes[i].es_subscriber = 0;
            clientes[i].largo_acumulado = 0;
            return;
        }
    }
    printf("[Broker] Limite de clientes alcanzado, se rechaza conexion.\n");
    close(socket_fd);
}

// Libera la posición del arreglo correspondiente a socket_fd.
void eliminar_cliente(int socket_fd) {
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (clientes[i].socket_fd == socket_fd) {
            clientes[i].socket_fd = -1;
            clientes[i].es_subscriber = 0;
            clientes[i].tema[0] = '\0';
            clientes[i].largo_acumulado = 0;
            return;
        }
    }
}

// Registra el tema de suscripción de un cliente ya conectado (comando SUB).    
void registrar_suscripcion(int socket_fd, const char *tema) {
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (clientes[i].socket_fd == socket_fd) {
            strncpy(clientes[i].tema, tema, TAM_TEMA - 1);
            clientes[i].tema[TAM_TEMA - 1] = '\0';
            clientes[i].es_subscriber = 1;
            printf("[Broker] Cliente (fd=%d) suscrito al tema '%s'\n", socket_fd, tema);
            return;
        }
    }
}


// Reenvía el mensaje publicado a todos los subscribers cuyo temacoincida

void difundir_mensaje(const char *tema, const char *contenido) {
    char mensaje_salida[TAM_BUFFER];
    snprintf(mensaje_salida, TAM_BUFFER, "MSG|%s|%s\n", tema, contenido);

    int enviados = 0;
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (clientes[i].socket_fd != -1 &&
            clientes[i].es_subscriber &&
            strcmp(clientes[i].tema, tema) == 0) {

            // send(): TCP garantiza que estos bytes lleguen completos
            // y en orden al subscriber correspondiente.
            send(clientes[i].socket_fd, mensaje_salida, strlen(mensaje_salida), 0);
            enviados++;
        }
    }

    printf("[Broker] Mensaje del tema '%s' difundido a %d suscriptor(es)\n", tema, enviados);
}

//Procesa un mensaje recién leído de un cliente: lo parsea según el protocolo "TIPO|TEMA|CONTENIDO"
 
void procesar_mensaje(int socket_fd, char *buffer) {
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
        registrar_suscripcion(socket_fd, tema);
    } else if (strcmp(tipo, "PUB") == 0) {
        printf("[Broker] PUB recibido -> tema='%s' contenido='%s'\n", tema, contenido);
        difundir_mensaje(tema, contenido);
    } else {
        printf("[Broker] Mensaje con formato desconocido: %s\n", buffer);
    }
}

int main() {
    int socket_servidor;
    struct sockaddr_in direccion_servidor;
    fd_set read_fds;     // conjunto de descriptores que le pasamos a select() en cada vuelta
    int fd_maximo;       // select() necesita saber cuál es el descriptor numéricamente más alto

    inicializar_clientes();

    //AF_INET (IPv4), SOCK_STREAM (TCP), protocolo por defecto.
    socket_servidor = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_servidor < 0) {
        perror("Error al crear el socket");
        exit(EXIT_FAILURE);
    }

    // Permite reiniciar el broker rápidamente sin el error "Address already in use".
    int opt = 1;
    setsockopt(socket_servidor, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    direccion_servidor.sin_family = AF_INET;
    direccion_servidor.sin_addr.s_addr = INADDR_ANY;
    direccion_servidor.sin_port = htons(PORT);

    // asocia el socket a la IP/puerto locales.
    if (bind(socket_servidor, (struct sockaddr *)&direccion_servidor, sizeof(direccion_servidor)) < 0) {
        perror("Error en bind");
        close(socket_servidor);
        exit(EXIT_FAILURE);
    }

    //  pone el socket en modo escucha pasiva, cola de hasta 10 conexiones pendientes.
    if (listen(socket_servidor, 10) < 0) {
        perror("Error en listen");
        close(socket_servidor);
        exit(EXIT_FAILURE);
    }

    printf("[Broker] Escuchando en el puerto %d...\n", PORT);

    while (1) {
        // limpia el conjunto antes de reconstruirlo en cada vuelta del bucle.
        FD_ZERO(&read_fds);

        // Siempre se escucha el socket servidor, para detectar conexiones nuevas.
        FD_SET(socket_servidor, &read_fds);
        fd_maximo = socket_servidor;

        // se agrega cada socket de cliente ya conectado.
        for (int i = 0; i < MAX_CLIENTES; i++) {
            int fd = clientes[i].socket_fd;
            if (fd != -1) {
                FD_SET(fd, &read_fds);
                if (fd > fd_maximo) {
                    fd_maximo = fd;
                }
            }
        }

        // select(): bloquea hasta que AL MENOS uno de los descriptores en
        // read_fds tenga datos listos para leer (o una conexión nueva
        // esperando, en el caso del socket servidor). Al volver, select()
        // modifica read_fds dejando marcados SOLO los que están listos.
        int actividad = select(fd_maximo + 1, &read_fds, NULL, NULL, NULL);

        if (actividad < 0) {
            perror("Error en select");
            continue;
        }

        // revisa si hay una conexion nueva esperando en el socket servidor.
        if (FD_ISSET(socket_servidor, &read_fds)) {
            struct sockaddr_in direccion_cliente;
            socklen_t tam_direccion = sizeof(direccion_cliente);

            // accept(): acepta la conexión entrante y devuelve un socket nuevo para comunicarse con ese cliente.
            int socket_cliente = accept(socket_servidor, (struct sockaddr *)&direccion_cliente, &tam_direccion);

            if (socket_cliente >= 0) {
                printf("[Broker] Nueva conexion desde %s:%d (fd=%d)\n",
                       inet_ntoa(direccion_cliente.sin_addr),
                       ntohs(direccion_cliente.sin_port),
                       socket_cliente);
                agregar_cliente(socket_cliente);
            }
        }

        // se revisa cada cliente para ver si tiene un mensaje para leer.
        for (int i = 0; i < MAX_CLIENTES; i++) {
            int fd = clientes[i].socket_fd;

            if (fd != -1 && FD_ISSET(fd, &read_fds)) {
                char buffer[TAM_BUFFER];
                memset(buffer, 0, TAM_BUFFER);

                // recv(): como select() ya nos confirmó que hay datos
                // listos, esta llamada NO bloquea en este caso.
                int bytes_leidos = recv(fd, buffer, TAM_BUFFER - 1, 0);

                if (bytes_leidos <= 0) {
                    // El cliente cerró la conexión, o hubo un error.
                    printf("[Broker] Cliente (fd=%d) desconectado.\n", fd);
                    close(fd);
                    eliminar_cliente(fd);
                } else {
                    
                    Cliente *c = &clientes[i];

                    if (c->largo_acumulado + bytes_leidos >= TAM_BUFFER) {
                        c->largo_acumulado = 0; // línea demasiado larga: se descarta
                    }
                    memcpy(c->acumulado + c->largo_acumulado, buffer, bytes_leidos);
                    c->largo_acumulado += bytes_leidos;

                    char *fin_linea;
                    while ((fin_linea = memchr(c->acumulado, '\n', c->largo_acumulado)) != NULL) {
                        int largo_linea = fin_linea - c->acumulado;

                        char linea[TAM_BUFFER];
                        memcpy(linea, c->acumulado, largo_linea);
                        linea[largo_linea] = '\0';

                        procesar_mensaje(fd, linea);

                        // Descartar la línea ya procesada 
                        int restante = c->largo_acumulado - (largo_linea + 1);
                        memmove(c->acumulado, fin_linea + 1, restante);
                        c->largo_acumulado = restante;
                    }
                }
            }
        }
    }

    close(socket_servidor);
    return 0;
}