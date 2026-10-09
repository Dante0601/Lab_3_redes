/*
 * subscriber_quic.c
 * ---------------------------------------------------------------------
 * Subscriber (suscriptor) del sistema publicador-suscriptor, usando QUIC
 * (API nativa de OpenSSL 3.5+). Es la versión QUIC de subscriber_tcp.c:
 * se conecta al broker, se suscribe a uno o varios temas (partidos) y queda
 * esperando en bucle los mensajes que el broker le reenvíe.
 *
 * Protocolo de aplicación (idéntico al de TCP):
 *   Envía  -> SUB|<tema>|\n                 (uno por cada tema, al conectarse)
 *   Recibe -> MSG|<tema>|<contenido>\n       (cada vez que hay una publicación)
 *
 * Compilar:
 *   gcc subscriber_quic.c -o subscriber_quic -lssl -lcrypto
 *
 * Ejecutar:
 *   ./subscriber_quic <IP_broker> <puerto_broker> <tema1> [tema2 ...]
 *
 * Ejemplo:
 *   ./subscriber_quic 127.0.0.1 5000 EquipoA_vs_EquipoB EquipoC_vs_EquipoD
 * ---------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>        // close()
#include <arpa/inet.h>     // inet_pton(), htons()
#include <sys/socket.h>    // socket(), connect()
#include <sys/time.h>      // struct timeval
#include <netinet/in.h>    // struct sockaddr_in

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/quic.h>

#define TAM_BUFFER 1024
#define MAX_TEMAS 10       // máximo de partidos que se pueden seguir a la vez

/* ALPN: debe coincidir con el que acepta el broker. */
static const unsigned char alpn_pubsub[] = { 6, 'p', 'u', 'b', 's', 'u', 'b' };

/* Se pone en 1 al presionar Ctrl+C para salir limpiamente del bucle. */
static volatile sig_atomic_t terminar = 0;
static void manejar_sigint(int s) { (void)s; terminar = 1; }

/*
 * Crea la conexión QUIC con el broker. Devuelve el SSL* ya conectado
 * (handshake QUIC + TLS 1.3 completado) o NULL si falla.
 */
static SSL *conectar_quic(SSL_CTX *ctx, const char *ip_broker, int puerto_broker) {
    // QUIC va sobre UDP: socket de datagramas.
    int socket_udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_udp < 0) {
        perror("Error al crear el socket UDP");
        return NULL;
    }

    struct sockaddr_in direccion_broker;
    memset(&direccion_broker, 0, sizeof(direccion_broker));
    direccion_broker.sin_family = AF_INET;
    direccion_broker.sin_port = htons(puerto_broker); // htons(): host-to-network short

    if (inet_pton(AF_INET, ip_broker, &direccion_broker.sin_addr) <= 0) {
        fprintf(stderr, "Direccion IP invalida: %s\n", ip_broker);
        close(socket_udp);
        return NULL;
    }

    // connect() sobre UDP no envía nada: solo fija el destino por defecto.
    if (connect(socket_udp, (struct sockaddr *)&direccion_broker, sizeof(direccion_broker)) < 0) {
        perror("Error en connect (UDP)");
        close(socket_udp);
        return NULL;
    }

    // OpenSSL necesita el socket UDP en modo no bloqueante.
    BIO_socket_nbio(socket_udp, 1);

    // Envolvemos el socket en un BIO de datagramas y se lo damos a OpenSSL.
    BIO *bio = BIO_new(BIO_s_datagram());
    if (bio == NULL) {
        close(socket_udp);
        return NULL;
    }
    BIO_set_fd(bio, socket_udp, BIO_CLOSE);

    // Dirección del broker en el formato interno de OpenSSL.
    BIO_ADDR *peer = BIO_ADDR_new();
    if (peer == NULL ||
        !BIO_ADDR_rawmake(peer, AF_INET, &direccion_broker.sin_addr,
                          sizeof(direccion_broker.sin_addr), direccion_broker.sin_port)) {
        BIO_free(bio);
        return NULL;
    }

    SSL *ssl = SSL_new(ctx);
    if (ssl == NULL) {
        BIO_free(bio);
        BIO_ADDR_free(peer);
        return NULL;
    }
    SSL_set_bio(ssl, bio, bio);
    SSL_set_tlsext_host_name(ssl, "localhost");            // SNI
    SSL_set_alpn_protos(ssl, alpn_pubsub, sizeof(alpn_pubsub)); // ALPN
    SSL_set1_initial_peer_addr(ssl, peer);
    BIO_ADDR_free(peer);

    // SSL_connect(): equivalente a connect() de TCP. Realiza el handshake
    // QUIC + TLS 1.3 en un solo paso (menos viajes de ida y vuelta que TCP+TLS).
    if (SSL_connect(ssl) < 1) {
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        return NULL;
    }
    return ssl;
}

int main(int argc, char *argv[]) {

    if (argc < 4 || argc - 3 > MAX_TEMAS) {
        fprintf(stderr, "Uso: %s <IP_broker> <puerto_broker> <tema1> [tema2 ...] (max %d temas)\n",
                argv[0], MAX_TEMAS);
        fprintf(stderr, "Ejemplo: %s 127.0.0.1 5000 EquipoA_vs_EquipoB EquipoC_vs_EquipoD\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *ip_broker = argv[1];
    int puerto_broker = atoi(argv[2]);
    char **temas = &argv[3];   // temas a seguir: argv[3], argv[4], ...
    int num_temas = argc - 3;

    // OSSL_QUIC_client_method(): este contexto habla QUIC.
    SSL_CTX *ctx = SSL_CTX_new(OSSL_QUIC_client_method());
    if (ctx == NULL) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    // Laboratorio: el broker usa un certificado AUTOFIRMADO, así que no se
    // valida la cadena. En producción se usaría SSL_VERIFY_PEER con una CA.
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    SSL *ssl = conectar_quic(ctx, ip_broker, puerto_broker);
    if (ssl == NULL) {
        fprintf(stderr, "Error al conectar con el broker\n");
        SSL_CTX_free(ctx);
        exit(EXIT_FAILURE);
    }

    printf("[Subscriber] Conectado al broker %s:%d (QUIC)\n", ip_broker, puerto_broker);

    // Enviar un mensaje de suscripción SUB|tema| por cada tema, todos por el
    // mismo stream; el broker los agrega a la lista de esta conexión.
    for (int t = 0; t < num_temas; t++) {
        char mensaje_sub[TAM_BUFFER];
        snprintf(mensaje_sub, sizeof(mensaje_sub), "SUB|%s|\n", temas[t]);

        // SSL_write_ex(): equivalente a send(). El primer write abre el stream
        // bidireccional; el broker registra esta conexión como interesada en el tema.
        size_t escritos = 0;
        if (!SSL_write_ex(ssl, mensaje_sub, strlen(mensaje_sub), &escritos)) {
            fprintf(stderr, "Error al enviar la suscripcion\n");
            ERR_print_errors_fp(stderr);
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            exit(EXIT_FAILURE);
        }

        printf("[Subscriber] Suscrito al tema: %s\n", temas[t]);
    }
    printf("[Subscriber] Esperando mensajes... (Ctrl+C para salir)\n\n");

    signal(SIGINT, manejar_sigint);

    // A partir de aquí la conexión pasa a modo no bloqueante para poder
    // revisar la bandera de Ctrl+C periódicamente con SSL_poll().
    SSL_set_blocking_mode(ssl, 0);

    char buffer[TAM_BUFFER];
    char acumulado[TAM_BUFFER]; // bytes recibidos que aún no forman una línea completa
    int largo_acumulado = 0;

    // Bucle de recepción: el subscriber no envía más mensajes propios,
    // solo se queda esperando lo que el broker le reenvíe.
    while (!terminar) {
        // SSL_poll(): espera hasta 500 ms a que haya datos (R) o la conexión
        // se cierre (EC). Es el análogo de select() con timeout.
        SSL_POLL_ITEM item;
        item.desc = SSL_as_poll_descriptor(ssl);
        item.events = SSL_POLL_EVENT_R | SSL_POLL_EVENT_EC;
        item.revents = 0;
        struct timeval espera = { 0, 500000 };
        size_t listos = 0;
        if (!SSL_poll(&item, 1, sizeof(item), &espera, 0, &listos)) {
            break;
        }
        if (item.revents == 0) {
            continue; // nada nuevo; volver a revisar la bandera de salida
        }

        size_t bytes_leidos = 0;

        // SSL_read_ex(): equivalente a recv(). Devuelve 1 si leyó datos.
        if (!SSL_read_ex(ssl, buffer, TAM_BUFFER - 1, &bytes_leidos)) {
            int err = SSL_get_error(ssl, 0);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            printf("\n[Subscriber] El broker cerro la conexion.\n");
            break;
        }

        // QUIC, igual que TCP, entrega un FLUJO de bytes: un solo read puede
        // traer varios mensajes pegados o solo parte de uno. Acumulamos los
        // bytes y procesamos una línea completa (terminada en '\n') a la vez.
        if (largo_acumulado + (int)bytes_leidos >= TAM_BUFFER) {
            largo_acumulado = 0; // línea demasiado larga: se descarta
        }
        memcpy(acumulado + largo_acumulado, buffer, bytes_leidos);
        largo_acumulado += (int)bytes_leidos;

        char *fin_linea;
        while ((fin_linea = memchr(acumulado, '\n', largo_acumulado)) != NULL) {
            int largo_linea = fin_linea - acumulado;

            char linea[TAM_BUFFER];
            memcpy(linea, acumulado, largo_linea);
            linea[largo_linea] = '\0';

            // Descartar la línea ya extraída (+1 por el '\n')
            int restante = largo_acumulado - (largo_linea + 1);
            memmove(acumulado, fin_linea + 1, restante);
            largo_acumulado = restante;

            // --- Parseo del mensaje recibido: MSG|tema|contenido ---
            char tipo[8] = {0};
            char tema_recibido[64] = {0};
            char contenido[TAM_BUFFER] = {0};

            char copia[TAM_BUFFER];
            strncpy(copia, linea, TAM_BUFFER - 1);
            copia[TAM_BUFFER - 1] = '\0';

            char *token = strtok(copia, "|");
            if (token != NULL) strncpy(tipo, token, sizeof(tipo) - 1);

            token = strtok(NULL, "|");
            if (token != NULL) strncpy(tema_recibido, token, sizeof(tema_recibido) - 1);

            token = strtok(NULL, "|");
            if (token != NULL) strncpy(contenido, token, sizeof(contenido) - 1);

            if (strcmp(tipo, "MSG") == 0) {
                printf(">> [%s] %s\n", tema_recibido, contenido);
            } else {
                printf(">> (mensaje sin formato esperado): %s\n", linea);
            }
        }
        fflush(stdout);
    }

    // Cierre de la conexión QUIC (equivalente al FIN/ACK de TCP). El
    // subscriber no tiene datos pendientes por enviar, así que usamos el
    // cierre RÁPIDO: envía CONNECTION_CLOSE de inmediato sin esperar a que
    // el broker termine su lado del stream (si no, puede tardar hasta que
    // expire el idle timeout de QUIC, ~30 s).
    SSL_set_blocking_mode(ssl, 1);
    SSL_shutdown_ex(ssl, SSL_SHUTDOWN_FLAG_RAPID, NULL, 0);

    SSL_free(ssl);
    SSL_CTX_free(ctx);
    return 0;
}
