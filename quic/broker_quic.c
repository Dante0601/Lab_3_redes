/*
 * broker_quic.c
 * ---------------------------------------------------------------------
 * Broker (intermediario) del sistema publicador-suscriptor, usando
 * QUIC (RFC 9000) a través de la API nativa de OpenSSL 3.5+.
 *
 * Es la versión QUIC de broker_tcp.c y conserva la misma mecánica:
 *   - Un solo hilo atiende a todos los clientes (sin threads ni mutex).
 *   - Donde TCP usaba select(), QUIC usa SSL_poll(): bloquea hasta que
 *     haya una conexión nueva o datos listos en alguna conexión.
 *   - Donde TCP usaba accept()/recv()/send(), QUIC usa
 *     SSL_accept_connection()/SSL_read_ex()/SSL_write_ex().
 *   - El protocolo de aplicación es EXACTAMENTE el mismo (una línea por
 *     mensaje, terminada en '\n'):
 *
 *       SUB|<tema>|                -> un Subscriber se suscribe a <tema> (puede
 *                                     mandar varios SUB para seguir varios partidos)
 *       PUB|<tema>|<contenido>     -> un Publisher publica en <tema>
 *       MSG|<tema>|<contenido>     -> el Broker reenvía a los suscritos
 *
 * Diferencias de fondo frente a TCP:
 *   - QUIC viaja sobre UDP (puerto 5000/udp), no sobre TCP.
 *   - QUIC incluye TLS 1.3 obligatorio: el broker necesita un
 *     certificado y una llave privada (se genera uno autofirmado).
 *   - Cada cliente es una conexión QUIC; usamos el "stream por defecto"
 *     de la conexión como un flujo de bytes bidireccional, igual que
 *     el socket TCP. Como en TCP, es un flujo SIN fronteras de mensaje,
 *     por eso se conserva el acumulador de líneas.
 *
 * Requiere OpenSSL >= 3.5 (openssl version).
 *
 * Generar certificado (una sola vez):
 *   openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
 *       -nodes -keyout key.pem -out cert.pem -days 365 -subj "/CN=localhost"
 *
 * Compilar:
 *   gcc broker_quic.c -o broker_quic -lssl -lcrypto
 *
 * Ejecutar:
 *   ./broker_quic cert.pem key.pem
 * ---------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>        // close()
#include <arpa/inet.h>     // htons()
#include <sys/socket.h>    // socket(), bind()
#include <netinet/in.h>    // struct sockaddr_in

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/quic.h>

#define PORT 5000          // puerto UDP en el que escucha el broker
#define MAX_CLIENTES 50
#define TAM_BUFFER 1024
#define TAM_TEMA 64
#define MAX_TEMAS 10       // máximo de partidos que puede seguir un mismo subscriber

/* ALPN: identificador del protocolo de aplicación que QUIC exige negociar. */
static const unsigned char alpn_pubsub[] = { 6, 'p', 'u', 'b', 's', 'u', 'b' };

/*
 * Representa a un cliente conectado. Igual que en la versión TCP, al usar
 * un solo hilo NO se necesita mutex. La diferencia es que en lugar de un
 * descriptor de socket (int) tenemos un objeto SSL* que representa la
 * conexión QUIC.
 */
typedef struct {
    SSL *conn;                  // NULL si esta posición del arreglo está libre
    char temas[MAX_TEMAS][TAM_TEMA]; // temas a los que está suscrito
    int num_temas;              // cantidad de temas suscritos (0 = no es subscriber)
    char acumulado[TAM_BUFFER]; // bytes recibidos que aún no forman una línea completa
    int largo_acumulado;        // cantidad de bytes válidos en "acumulado"
} Cliente;

Cliente clientes[MAX_CLIENTES];

/* Inicializa el arreglo de clientes: todas las posiciones libres. */
void inicializar_clientes() {
    for (int i = 0; i < MAX_CLIENTES; i++) {
        clientes[i].conn = NULL;
        clientes[i].num_temas = 0;
        clientes[i].largo_acumulado = 0;
    }
}

/* Registra la nueva conexión en la primera posición libre. */
void agregar_cliente(SSL *conn) {
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (clientes[i].conn == NULL) {
            clientes[i].conn = conn;
            clientes[i].num_temas = 0;
            clientes[i].largo_acumulado = 0;
            printf("[Broker] Nueva conexion QUIC (posicion=%d)\n", i);
            return;
        }
    }
    printf("[Broker] Limite de clientes alcanzado, se rechaza conexion.\n");
    SSL_free(conn);
}

/* Cierra y libera la conexión de la posición i. */
void eliminar_cliente(int i) {
    if (clientes[i].conn != NULL) {
        // SSL_free() cierra la conexión QUIC y libera sus recursos.
        SSL_free(clientes[i].conn);
        clientes[i].conn = NULL;
    }
    clientes[i].num_temas = 0;
    clientes[i].largo_acumulado = 0;
}

/* Devuelve 1 si el cliente está suscrito al tema dado. */
int esta_suscrito(const Cliente *c, const char *tema) {
    for (int t = 0; t < c->num_temas; t++) {
        if (strcmp(c->temas[t], tema) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * Agrega un tema a la lista de suscripciones de un cliente ya conectado
 * (comando SUB). Cada SUB suma un tema; un SUB repetido se ignora.
 */
void registrar_suscripcion(int indice, const char *tema) {
    Cliente *c = &clientes[indice];
    if (esta_suscrito(c, tema)) {
        return;
    }
    if (c->num_temas == MAX_TEMAS) {
        printf("[Broker] Cliente (posicion=%d) ya sigue %d temas, se ignora '%s'\n",
               indice, MAX_TEMAS, tema);
        return;
    }
    strncpy(c->temas[c->num_temas], tema, TAM_TEMA - 1);
    c->temas[c->num_temas][TAM_TEMA - 1] = '\0';
    c->num_temas++;
    printf("[Broker] Cliente (posicion=%d) suscrito al tema '%s'\n", indice, tema);
}

/*
 * Reenvía el mensaje publicado a todos los subscribers que sigan ese tema.
 * Es el corazón del patrón pub-sub (idéntico al de la versión TCP).
 */
void difundir_mensaje(const char *tema, const char *contenido) {
    char mensaje_salida[TAM_BUFFER];
    snprintf(mensaje_salida, TAM_BUFFER, "MSG|%s|%s\n", tema, contenido);

    int enviados = 0;
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (clientes[i].conn != NULL && esta_suscrito(&clientes[i], tema)) {

            // SSL_write_ex(): QUIC garantiza entrega completa y en orden
            // dentro del stream (igual que TCP), con retransmisión propia.
            size_t escritos = 0;
            if (SSL_write_ex(clientes[i].conn, mensaje_salida,
                             strlen(mensaje_salida), &escritos)) {
                enviados++;
            }
        }
    }

    printf("[Broker] Mensaje del tema '%s' difundido a %d suscriptor(es)\n", tema, enviados);
}

/*
 * Procesa una línea completa ya extraída: la parsea según el protocolo
 * "TIPO|TEMA|CONTENIDO" y actúa en consecuencia.
 */
void procesar_mensaje(int indice, char *buffer) {
    buffer[strcspn(buffer, "\n")] = '\0'; // quitar salto de línea final si vino

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
        registrar_suscripcion(indice, tema);
    } else if (strcmp(tipo, "PUB") == 0) {
        printf("[Broker] PUB recibido -> tema='%s' contenido='%s'\n", tema, contenido);
        difundir_mensaje(tema, contenido);
    } else {
        printf("[Broker] Mensaje con formato desconocido: %s\n", buffer);
    }
}

/*
 * Lee lo que haya disponible del cliente i y procesa cada línea completa.
 * Devuelve 1 si el cliente sigue conectado, 0 si hay que cerrarlo.
 */
int atender_cliente(int i) {
    Cliente *c = &clientes[i];
    char buffer[TAM_BUFFER];
    size_t bytes_leidos = 0;

    // SSL_read_ex(): equivalente a recv(). Devuelve 1 si leyó datos.
    if (!SSL_read_ex(c->conn, buffer, TAM_BUFFER - 1, &bytes_leidos)) {
        int err = SSL_get_error(c->conn, 0);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            return 1; // no había nada útil todavía; seguimos conectados
        }
        // SSL_ERROR_ZERO_RETURN: el cliente terminó su flujo (FIN de stream).
        // Cualquier otro error: la conexión se cerró o falló.
        return 0;
    }

    // Igual que en TCP: el stream es un FLUJO de bytes sin fronteras de
    // mensaje. Acumulamos y extraemos una línea completa ('\n') a la vez.
    if (c->largo_acumulado + (int)bytes_leidos >= TAM_BUFFER) {
        c->largo_acumulado = 0; // línea demasiado larga: se descarta
    }
    memcpy(c->acumulado + c->largo_acumulado, buffer, bytes_leidos);
    c->largo_acumulado += (int)bytes_leidos;

    char *fin_linea;
    while ((fin_linea = memchr(c->acumulado, '\n', c->largo_acumulado)) != NULL) {
        int largo_linea = fin_linea - c->acumulado;

        char linea[TAM_BUFFER];
        memcpy(linea, c->acumulado, largo_linea);
        linea[largo_linea] = '\0';

        procesar_mensaje(i, linea);

        // Descartar la línea ya procesada (+1 por el '\n')
        int restante = c->largo_acumulado - (largo_linea + 1);
        memmove(c->acumulado, fin_linea + 1, restante);
        c->largo_acumulado = restante;
    }
    return 1;
}

/* Callback que elige el protocolo ALPN "pubsub" ofrecido por el cliente. */
static int seleccionar_alpn(SSL *ssl, const unsigned char **out,
                            unsigned char *out_len, const unsigned char *in,
                            unsigned int in_len, void *arg) {
    (void)ssl;
    (void)arg;
    if (SSL_select_next_proto((unsigned char **)out, out_len, alpn_pubsub,
                              sizeof(alpn_pubsub), in, in_len) == OPENSSL_NPN_NEGOTIATED)
        return SSL_TLSEXT_ERR_OK;
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

/* Crea el contexto TLS/QUIC del servidor con su certificado y llave. */
SSL_CTX *crear_contexto(const char *cert, const char *llave) {
    // OSSL_QUIC_server_method(): este contexto habla QUIC (no TLS sobre TCP).
    SSL_CTX *ctx = SSL_CTX_new(OSSL_QUIC_server_method());
    if (ctx == NULL) return NULL;

    if (SSL_CTX_use_certificate_chain_file(ctx, cert) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, llave, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "No se pudo cargar el certificado o la llave\n");
        SSL_CTX_free(ctx);
        return NULL;
    }

    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL); // no pedimos certificado al cliente
    SSL_CTX_set_alpn_select_cb(ctx, seleccionar_alpn, NULL);
    return ctx;
}

int main(int argc, char *argv[]) {

    if (argc != 3) {
        fprintf(stderr, "Uso: %s <cert.pem> <key.pem>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    inicializar_clientes();

    SSL_CTX *ctx = crear_contexto(argv[1], argv[2]);
    if (ctx == NULL) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    // QUIC va sobre UDP: socket de datagramas (SOCK_DGRAM), no SOCK_STREAM.
    int socket_udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_udp < 0) {
        perror("Error al crear el socket UDP");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in direccion_servidor;
    memset(&direccion_servidor, 0, sizeof(direccion_servidor));
    direccion_servidor.sin_family = AF_INET;
    direccion_servidor.sin_addr.s_addr = INADDR_ANY;
    direccion_servidor.sin_port = htons(PORT);

    // bind(): asocia el socket UDP al puerto. No hay listen() ni accept() de
    // socket: el "listener" QUIC de OpenSSL hace ese trabajo sobre este socket.
    if (bind(socket_udp, (struct sockaddr *)&direccion_servidor, sizeof(direccion_servidor)) < 0) {
        perror("Error en bind");
        close(socket_udp);
        exit(EXIT_FAILURE);
    }

    // OpenSSL necesita el socket en modo no bloqueante.
    if (BIO_socket_nbio(socket_udp, 1) <= 0) {
        fprintf(stderr, "No se pudo poner el socket en modo no bloqueante\n");
        exit(EXIT_FAILURE);
    }

    // El listener QUIC reemplaza a listen()/accept() de TCP.
    SSL *listener = SSL_new_listener(ctx, 0);
    if (listener == NULL || !SSL_set_fd(listener, socket_udp) ||
        !SSL_set_blocking_mode(listener, 0) || !SSL_listen(listener)) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    printf("[Broker] Escuchando QUIC (UDP) en el puerto %d...\n", PORT);

    while (1) {
        // Construimos el conjunto de objetos a vigilar (equivalente al
        // fd_set de select()). El elemento 0 es el listener: avisa de
        // conexiones entrantes (SSL_POLL_EVENT_IC). Los demás son las
        // conexiones de los clientes: avisan de datos legibles (R) o de
        // conexión cerrada/en error (EC).
        SSL_POLL_ITEM items[MAX_CLIENTES + 1];
        int indice_cliente[MAX_CLIENTES + 1];
        size_t n_items = 0;

        items[n_items].desc = SSL_as_poll_descriptor(listener);
        items[n_items].events = SSL_POLL_EVENT_IC;
        items[n_items].revents = 0;
        indice_cliente[n_items] = -1;
        n_items++;

        for (int i = 0; i < MAX_CLIENTES; i++) {
            if (clientes[i].conn != NULL) {
                items[n_items].desc = SSL_as_poll_descriptor(clientes[i].conn);
                // ISB (stream bidireccional entrante): el cliente abre su stream
                // al enviar el primer mensaje. Hasta que el broker lo asocia
                // con la primera lectura, el evento R todavía no existe, así
                // que ISB es lo que despierta al broker la primera vez.
                items[n_items].events = SSL_POLL_EVENT_R | SSL_POLL_EVENT_ISB | SSL_POLL_EVENT_EC;
                items[n_items].revents = 0;
                indice_cliente[n_items] = i;
                n_items++;
            }
        }

        // SSL_poll(): bloquea (timeout NULL) hasta que ALGUNO de los objetos
        // tenga actividad. Además procesa internamente los temporizadores y
        // paquetes de QUIC (handshake, ACKs, retransmisiones).
        size_t listos = 0;
        if (!SSL_poll(items, n_items, sizeof(SSL_POLL_ITEM), NULL, 0, &listos)) {
            ERR_print_errors_fp(stderr);
            continue;
        }

        // ¿Hay conexiones nuevas esperando?
        if (items[0].revents & SSL_POLL_EVENT_IC) {
            SSL *nueva;
            // SSL_accept_connection(): equivalente a accept().
            while ((nueva = SSL_accept_connection(listener, SSL_ACCEPT_CONNECTION_NO_BLOCK)) != NULL) {
                agregar_cliente(nueva);
            }
        }

        // Revisamos cada cliente con actividad.
        for (size_t k = 1; k < n_items; k++) {
            int i = indice_cliente[k];
            if (clientes[i].conn == NULL) continue;

            int sigue = 1;
            if (items[k].revents & (SSL_POLL_EVENT_R | SSL_POLL_EVENT_ISB)) {
                sigue = atender_cliente(i);
            }
            if (sigue && (items[k].revents & (SSL_POLL_EVENT_EC | SSL_POLL_EVENT_ECD))) {
                sigue = 0;
            }
            if (!sigue) {
                printf("[Broker] Cliente (posicion=%d) desconectado.\n", i);
                eliminar_cliente(i);
            }
        }
    }

    SSL_free(listener);
    SSL_CTX_free(ctx);
    close(socket_udp);
    return 0;
}
