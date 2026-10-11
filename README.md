# Lab 3 – Sistema publicador-suscriptor con sockets (TCP, UDP y QUIC)

Sistema pub-sub en C para transmitir eventos de partidos. Tiene tres actores:

- **Broker**: recibe suscripciones y publicaciones, y reenvía cada publicación
  a los suscriptores del tema.
- **Publisher**: lee eventos desde la terminal y los publica en un tema.
- **Subscriber**: se suscribe a uno o varios temas e imprime los mensajes que recibe.

Hay tres implementaciones del mismo sistema, cada una en su carpeta:

| Carpeta | Transporte | Dependencias |
|---------|------------|--------------|
| `tcp/`  | TCP (`SOCK_STREAM`), broker con `select()` | solo sockets POSIX |
| `udp/`  | UDP (`SOCK_DGRAM`), broker con `recvfrom()`/`sendto()` | solo sockets POSIX |
| `quic/` | QUIC sobre UDP (bono) | OpenSSL >= 3.5 |

## Protocolo de aplicación

Es el mismo en las tres versiones. Son mensajes de texto con el formato `TIPO|tema|contenido`, terminados en `\n`:

| Mensaje | Dirección | Significado |
|---------|-----------|-------------|
| `SUB\|<tema>\|` | Subscriber → Broker | suscribirse a `<tema>` (un `SUB` por cada tema que se quiere seguir) |
| `PUB\|<tema>\|<contenido>` | Publisher → Broker | publicar en `<tema>` |
| `MSG\|<tema>\|<contenido>` | Broker → Subscriber | reenvío de una publicación |

En TCP y QUIC los mensajes viajan por un flujo de bytes, así que el receptor
acumula bytes y separa por `\n`. En UDP cada mensaje es un datagrama.

En UDP, el publisher antepone un número de secuencia al contenido
(`#1 Gol de A`, `#2 Tarjeta amarilla`, ...). El subscriber lo usa para avisar
cuando detecta un **salto** (mensajes perdidos) o un **retroceso** (mensaje
desordenado o duplicado). La secuencia se lleva por separado para cada tema y la
detección supone un solo publisher por tema.

## Compilar

Con el Makefile, desde la raíz del repositorio:

```sh
make            # compila TCP y UDP
make quic       # compila QUIC (requiere OpenSSL >= 3.5)
make clean      # borra los binarios
```

También se puede compilar cada archivo a mano. Todos compilan sin warnings con `-Wall -Wextra`:

```sh
gcc -Wall -Wextra tcp/broker_tcp.c     -o tcp/broker_tcp
gcc -Wall -Wextra tcp/publisher_tcp.c  -o tcp/publisher_tcp
gcc -Wall -Wextra tcp/subscriber_tcp.c -o tcp/subscriber_tcp

gcc -Wall -Wextra udp/broker_udp.c     -o udp/broker_udp
gcc -Wall -Wextra udp/publisher_udp.c  -o udp/publisher_udp
gcc -Wall -Wextra udp/subscriber_udp.c -o udp/subscriber_udp

gcc -Wall -Wextra quic/broker_quic.c     -o quic/broker_quic     -lssl -lcrypto
gcc -Wall -Wextra quic/publisher_quic.c  -o quic/publisher_quic  -lssl -lcrypto
gcc -Wall -Wextra quic/subscriber_quic.c -o quic/subscriber_quic -lssl -lcrypto
```

## Ejecutar

Se abre una terminal por proceso. El broker se inicia primero y escucha en el puerto
**5000**. En todas las versiones, el publisher recibe `<IP_broker> <puerto_broker> <tema>` y el
subscriber recibe `<IP_broker> <puerto_broker> <tema1> [tema2 ...]` (hasta 10 temas).

Para seguir varios partidos con un solo subscriber:

```sh
./tcp/subscriber_tcp 127.0.0.1 5000 EquipoA_vs_EquipoB EquipoC_vs_EquipoD
```

### TCP

```sh
./tcp/broker_tcp
./tcp/subscriber_tcp 127.0.0.1 5000 EquipoA_vs_EquipoB
./tcp/publisher_tcp  127.0.0.1 5000 EquipoA_vs_EquipoB
```

### UDP

```sh
./udp/broker_udp
./udp/subscriber_udp 127.0.0.1 5000 EquipoA_vs_EquipoB
./udp/publisher_udp  127.0.0.1 5000 EquipoA_vs_EquipoB
```

UDP no tiene conexión. Si el broker no está corriendo cuando el subscriber envía su
`SUB`, ese datagrama se pierde sin error y el subscriber no recibirá nada. En ese caso hay que
reiniciar el subscriber después de iniciar el broker.

### QUIC (bono)

Requiere OpenSSL **3.5 o superior** y sus headers de desarrollo (en Ubuntu,
`sudo apt install libssl-dev`). Para revisar la versión instalada:

```sh
openssl version
```

#### Generar el certificado

QUIC incluye TLS 1.3 obligatorio, así que el broker necesita un certificado y una
llave. Para el laboratorio se usa un certificado autofirmado, que se genera una sola vez:

```sh
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -nodes -keyout quic/key.pem -out quic/cert.pem -days 365 -subj "/CN=localhost"
```

`make cert` ejecuta el mismo comando. `key.pem` es una llave privada y no se debe
subir al repositorio.

#### Ejecutar

```sh
./quic/broker_quic quic/cert.pem quic/key.pem
./quic/subscriber_quic 127.0.0.1 5000 EquipoA_vs_EquipoB
./quic/publisher_quic  127.0.0.1 5000 EquipoA_vs_EquipoB
```

En los publishers, `salir` (o Ctrl+D) termina el programa. En los subscribers se sale con Ctrl+C.

## Prueba automática

`test.sh` levanta 1 broker, 2 suscriptores (temas `EquipoA_vs_EquipoB` y
`EquipoC_vs_EquipoD`), 1 suscriptor que sigue ambos temas y 2 publicadores que
envían 10 mensajes cada uno en paralelo. Luego verifica que cada suscriptor de un
tema recibió los 10 mensajes de su tema y ninguno del otro, y que el suscriptor de
ambos temas recibió los 20.

```sh
./test.sh                 # TCP y UDP
./test.sh udp             # solo un protocolo
make test                 # equivalente a ./test.sh
```

Si alguna prueba falla, el script muestra la salida del suscriptor y deja los logs
en un directorio temporal.

### Capturas para Wireshark e imágenes del informe

Con `CAPTURA=1`, los scripts graban el tráfico del puerto 5000 con `dumpcap`
(viene con Wireshark; el usuario debe estar en el grupo `wireshark`, no hace falta sudo):

```sh
CAPTURA=1 ./test.sh                  # capturas/tcp.pcapng y udp.pcapng
CAPTURA=1 ./test.sh udp              # solo un protocolo
CAPTURA=1 PAUSA=1 N=5 ./test.sh tcp  # menos mensajes y más separados
CAPTURA=1 ./test_red_mala.sh         # capturas/redmala_tcp.pcapng y redmala_udp.pcapng
```

En este modo los logs quedan en `capturas/<proto>/`, se muestra la salida de cada
suscriptor y los publicadores esperan `PAUSA` segundos entre eventos (0.3 por
defecto), para que cada mensaje se distinga en la línea de tiempo.

Al terminar, `test.sh` imprime el comando para abrir cada captura
(`wireshark -r capturas/tcp.pcapng`).

Filtros útiles: `tcp.port == 5000` (handshake, `PSH`, `FIN`), `udp.port == 5000`
(un datagrama por mensaje). En TCP y UDP el contenido (`SUB|...`, `PUB|...`,
`MSG|...`) se lee en texto plano en el panel de bytes.

## Funciones de OpenSSL usadas en la versión QUIC

El enunciado pide documentar las funciones de cualquier librería externa. La versión QUIC usa la API
QUIC nativa de OpenSSL (`<openssl/ssl.h>`, `<openssl/quic.h>`, `<openssl/err.h>`),
disponible desde OpenSSL 3.2. La parte de servidor (`SSL_new_listener`, `SSL_poll`, etc.) existe desde la 3.5.
Las llamadas de sockets POSIX (`socket`, `bind`, `connect`) siguen siendo las del
sistema operativo. Las funciones se presentan agrupadas por propósito.

### Contexto (configuración común a todas las conexiones)

| Función | Qué hace | Uso en el programa |
|---------|----------|--------------------|
| `OSSL_QUIC_server_method()` | Devuelve el "método" de un servidor QUIC. | El broker crea su `SSL_CTX` con este método para hablar QUIC y no TLS sobre TCP. |
| `OSSL_QUIC_client_method()` | Devuelve el método de un cliente QUIC. El programa debe llamar a OpenSSL con frecuencia para que la conexión avance. | El subscriber lo usa. Su bucle llama a `SSL_poll()` cada 500 ms, lo que mantiene viva la conexión. |
| `OSSL_QUIC_client_thread_method()` | Igual que el anterior, pero OpenSSL crea un hilo interno que atiende la conexión (ACKs, temporizadores, keep-alive) aunque el programa no llame a OpenSSL. | El publisher lo usa porque pasa la mayor parte del tiempo bloqueado en `fgets()` esperando al usuario. Con el método sin hilo, la conexión moría por *idle timeout* (~30 s) y los mensajes siguientes se perdían. |
| `SSL_CTX_new()` | Crea un contexto (`SSL_CTX`) a partir de un método. | Cada programa crea un contexto al iniciar. |
| `SSL_CTX_use_certificate_chain_file()` | Carga el certificado (y su cadena) desde un archivo PEM. | El broker carga `cert.pem`, que presenta a los clientes en el handshake TLS 1.3. |
| `SSL_CTX_use_PrivateKey_file()` | Carga la llave privada que corresponde al certificado. | El broker carga `key.pem`. |
| `SSL_CTX_set_verify()` | Define si se verifica el certificado del otro extremo. | Todos usan `SSL_VERIFY_NONE`: el broker no pide certificado a los clientes, y los clientes no validan el certificado autofirmado del broker. En producción se usaría `SSL_VERIFY_PEER` con una CA. |
| `SSL_CTX_set_alpn_select_cb()` | Registra la función que elige el protocolo de aplicación (ALPN) entre los que ofrece el cliente. | El broker registra `seleccionar_alpn()`. QUIC exige negociar ALPN. |
| `SSL_select_next_proto()` | Busca un protocolo en común entre la lista del servidor y la del cliente. | `seleccionar_alpn()` la usa para aceptar `"pubsub"` o rechazar la conexión si el cliente no lo ofrece. |
| `SSL_CTX_free()` | Libera el contexto. | Se llama al terminar o cuando falla la inicialización. |

### Sockets y direcciones (BIO)

| Función | Qué hace | Uso en el programa |
|---------|----------|--------------------|
| `BIO_socket_nbio()` | Pone un descriptor de socket en modo no bloqueante. | Broker y clientes ponen su socket UDP en modo no bloqueante, porque OpenSSL lo requiere para manejar QUIC. |
| `BIO_new()` + `BIO_s_datagram()` | Crea un BIO (abstracción de E/S de OpenSSL) de tipo datagrama. | Los clientes envuelven su socket UDP en este BIO. |
| `BIO_set_fd()` | Asocia un descriptor de socket existente a un BIO. Con `BIO_CLOSE`, el socket se cierra cuando se libera el BIO. | Los clientes asocian el socket UDP al BIO. |
| `BIO_free()` | Libera un BIO. | Limpieza si falla la creación de la conexión. |
| `BIO_ADDR_new()` | Crea una dirección en el formato interno de OpenSSL. | Los clientes la usan para la dirección del broker. |
| `BIO_ADDR_rawmake()` | Llena un `BIO_ADDR` a partir de familia, IP binaria y puerto. | Se copia ahí la IP y el puerto del `struct sockaddr_in` del broker. |
| `BIO_ADDR_free()` | Libera un `BIO_ADDR`. | Se libera después de entregárselo a `SSL_set1_initial_peer_addr()`, que guarda su propia copia. |

### Conexión del cliente

| Función | Qué hace | Uso en el programa |
|---------|----------|--------------------|
| `SSL_new()` | Crea un objeto `SSL`, que representa una conexión QUIC, a partir del contexto. | Cada cliente crea su conexión. |
| `SSL_set_bio()` | Asocia el BIO de lectura/escritura a la conexión. | Se asocia el BIO de datagramas con el socket UDP. |
| `SSL_set_tlsext_host_name()` | Fija el nombre de servidor (SNI) que se envía en el handshake. | Se envía `"localhost"`, que coincide con el CN del certificado. |
| `SSL_set_alpn_protos()` | Fija la lista de protocolos ALPN que ofrece el cliente. | Se ofrece `"pubsub"`, el único que acepta el broker. |
| `SSL_set1_initial_peer_addr()` | Indica a QUIC la dirección del servidor para el primer paquete. | Se le pasa la dirección del broker. |
| `SSL_connect()` | Hace el handshake QUIC + TLS 1.3. Es el equivalente a `connect()` en TCP. | Los clientes se conectan al broker. Si falla, se imprime el error y se sale. |

### Servidor: escuchar y aceptar

| Función | Qué hace | Uso en el programa |
|---------|----------|--------------------|
| `SSL_new_listener()` | Crea un objeto *listener* QUIC, que reemplaza a `listen()`/`accept()` de TCP sobre un socket UDP. | El broker crea un listener a partir de su contexto. |
| `SSL_set_fd()` | Asocia directamente un descriptor de socket a un objeto `SSL`. | Se asocia el socket UDP (ya con `bind()` al puerto 5000) al listener. |
| `SSL_set_blocking_mode()` | Activa o desactiva el modo bloqueante de un objeto `SSL`. | El broker pone el listener en modo no bloqueante. El subscriber pasa a no bloqueante después de suscribirse, para poder revisar Ctrl+C entre esperas, y vuelve a bloqueante antes de cerrar. |
| `SSL_listen()` | Empieza a aceptar conexiones entrantes en el listener. | Lo llama el broker al iniciar. |
| `SSL_accept_connection()` | Devuelve una conexión QUIC entrante ya establecida. Es el equivalente a `accept()`. | Con `SSL_ACCEPT_CONNECTION_NO_BLOCK`, el broker acepta en un bucle todas las conexiones pendientes y las agrega a su arreglo de clientes. |

### Espera de eventos (equivalente a `select()`)

| Función | Qué hace | Uso en el programa |
|---------|----------|--------------------|
| `SSL_as_poll_descriptor()` | Convierte un objeto `SSL` (listener o conexión) en un descriptor que se puede vigilar con `SSL_poll()`. | Se usa para armar el arreglo de `SSL_POLL_ITEM`. |
| `SSL_poll()` | Bloquea hasta que alguno de los objetos tenga un evento: `SSL_POLL_EVENT_IC` (conexión entrante), `R` (datos legibles), `ISB` (stream bidireccional entrante), `EC`/`ECD` (conexión cerrándose o cerrada). Mientras espera, procesa los paquetes y temporizadores de QUIC. | El broker vigila el listener y todas las conexiones, igual que el broker TCP con `select()`. El subscriber vigila su única conexión con un timeout de 500 ms. |

### Envío y recepción

| Función | Qué hace | Uso en el programa |
|---------|----------|--------------------|
| `SSL_write_ex()` | Escribe bytes en el stream por defecto de la conexión. Es el equivalente a `send()`. El primer write abre el stream bidireccional. | Los clientes envían `SUB` y `PUB`. El broker envía `MSG` a cada suscriptor del tema. |
| `SSL_read_ex()` | Lee bytes del stream. Es el equivalente a `recv()`. | El broker lee lo que envía cada cliente y el subscriber lee los `MSG`. Igual que en TCP, el resultado pasa por un acumulador de líneas. |
| `SSL_get_error()` | Indica por qué falló una operación de E/S. | Si es `SSL_ERROR_WANT_READ`/`WANT_WRITE`, todavía no hay datos y se sigue esperando. Con `SSL_ERROR_ZERO_RETURN` o cualquier otro error, el otro extremo cerró y se da por terminada la conexión. |

### Cierre y errores

| Función | Qué hace | Uso en el programa |
|---------|----------|--------------------|
| `SSL_stream_conclude()` | Envía el FIN del stream: indica que este lado no escribirá más. | El publisher la llama después de `salir`. |
| `SSL_shutdown_ex()` | Cierra la conexión QUIC (`CONNECTION_CLOSE`). Con `SSL_SHUTDOWN_FLAG_RAPID`, el cierre es inmediato y no espera la negociación completa con el otro extremo. | Publisher y subscriber cierran así. El cierre normal tardaba ~31 s, porque esperaba hasta el *idle timeout*. |
| `SSL_free()` | Libera un objeto `SSL`. Si es una conexión, también la cierra. | El broker libera las conexiones de clientes desconectados y los clientes liberan la suya al salir. |
| `ERR_print_errors_fp()` | Imprime en `stderr` la cola de errores de OpenSSL. | Se llama después de cualquier fallo de OpenSSL para mostrar la causa. |
