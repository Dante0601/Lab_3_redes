/*
 * publisher_quic.c
 * ---------------------------------------------------------------------
 * Publisher (publicador) del sistema publicador-suscriptor, usando QUIC
 * (API nativa de OpenSSL 3.5+). Es la versión QUIC de publisher_tcp.c:
 * se conecta al broker y envía mensajes de eventos de un partido (tema),
 * tecleados interactivamente desde la terminal.
 *
 * Protocolo de aplicación (idéntico al de TCP):
 *   PUB|<tema>|<contenido>\n
 *
 * Compilar:
 *   gcc publisher_quic.c -o publisher_quic -lssl -lcrypto
 *
 * Ejecutar:
 *   ./publisher_quic <IP_broker> <puerto_broker> <tema>
 *
 * Ejemplo:
 *   ./publisher_quic 127.0.0.1 5000 EquipoA_vs_EquipoB
 * ---------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>        // close(), usleep()
#include <arpa/inet.h>     // inet_pton(), htons()
#include <sys/socket.h>    // socket(), connect()
#include <netinet/in.h>    // struct sockaddr_in

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/quic.h>

#define TAM_BUFFER 1024

/* ALPN: debe coincidir con el que acepta el broker. */
static const unsigned char alpn_pubsub[] = { 6, 'p', 'u', 'b', 's', 'u', 'b' };

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

    if (argc != 4) {
        fprintf(stderr, "Uso: %s <IP_broker> <puerto_broker> <tema>\n", argv[0]);
        fprintf(stderr, "Ejemplo: %s 127.0.0.1 5000 EquipoA_vs_EquipoB\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *ip_broker = argv[1];
    int puerto_broker = atoi(argv[2]);
    const char *tema = argv[3];

    // OSSL_QUIC_client_thread_method(): este contexto habla QUIC y además
    // crea un hilo interno que atiende la conexión (ACKs, temporizadores,
    // PINGs de keep-alive) mientras el programa está bloqueado en fgets().
    // Sin ese hilo, si el usuario tarda más de ~30 s en escribir, la
    // conexión muere por idle timeout y los mensajes siguientes se pierden.
    SSL_CTX *ctx = SSL_CTX_new(OSSL_QUIC_client_thread_method());
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

    printf("[Publisher] Conectado al broker %s:%d (QUIC)\n", ip_broker, puerto_broker);
    printf("[Publisher] Publicando en el tema: %s\n", tema);
    printf("[Publisher] Escriba cada evento y presione Enter para publicarlo.\n");
    printf("[Publisher] Escriba 'salir' para terminar.\n\n");

    char linea[TAM_BUFFER];
    char mensaje[TAM_BUFFER];
    int contador = 0;

    while (1) {
        printf("Evento #%d > ", contador + 1);
        fflush(stdout);

        // fgets(): lee una línea completa escrita por el usuario.
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
        char mensaje_grande[TAM_BUFFER * 2];
        snprintf(mensaje_grande, sizeof(mensaje_grande), "PUB|%s|%s\n", tema, linea);
        strncpy(mensaje, mensaje_grande, TAM_BUFFER - 1);
        mensaje[TAM_BUFFER - 1] = '\0';

        // SSL_write_ex(): equivalente a send(). El primer write abre el
        // stream bidireccional de la conexión QUIC; QUIC garantiza entrega
        // completa y en orden dentro del stream.
        size_t bytes_enviados = 0;
        if (!SSL_write_ex(ssl, mensaje, strlen(mensaje), &bytes_enviados)) {
            fprintf(stderr, "Error al enviar mensaje\n");
            ERR_print_errors_fp(stderr);
            break;
        }

        contador++;
        printf("[Publisher] Mensaje #%d publicado: \"%s\"\n\n", contador, linea);
    }

    printf("[Publisher] Se publicaron %d mensaje(s) en total.\n", contador);

    // Cierre (equivalente al FIN/ACK de TCP): se concluye el stream (FIN)
    // y luego se cierra la conexión QUIC con el cierre RÁPIDO, que envía
    // CONNECTION_CLOSE de inmediato. Con el cierre normal (bucle de
    // SSL_shutdown()) el programa tardaba ~31 s, hasta el idle timeout.
    // Incluso el cierre rápido en modo bloqueante a veces esperaba esos
    // ~30 s (con varios publishers a la vez), así que se usa NO_BLOCK y se
    // espera como máximo 1 s a que el cierre termine.
    SSL_stream_conclude(ssl, 0);
    for (int intento = 0; intento < 100; intento++) {
        if (SSL_shutdown_ex(ssl, SSL_SHUTDOWN_FLAG_RAPID | SSL_SHUTDOWN_FLAG_NO_BLOCK,
                            NULL, 0) == 1)
            break;
        usleep(10000); // 10 ms; el hilo interno de OpenSSL sigue trabajando
    }

    SSL_free(ssl);
    SSL_CTX_free(ctx);
    return 0;
}
