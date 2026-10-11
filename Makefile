# Makefile del Lab 3: pub-sub sobre TCP, UDP y QUIC.
#
#   make            compila TCP y UDP
#   make quic       compila QUIC (requiere OpenSSL >= 3.5)
#   make cert       genera quic/cert.pem y quic/key.pem (autofirmado)
#   make test       corre test.sh (TCP y UDP)
#   make clean      borra binarios

CC      = gcc
CFLAGS  = -Wall -Wextra
SSL_LIBS = -lssl -lcrypto

TCP_BIN  = tcp/broker_tcp tcp/publisher_tcp tcp/subscriber_tcp
UDP_BIN  = udp/broker_udp udp/publisher_udp udp/subscriber_udp
QUIC_BIN = quic/broker_quic quic/publisher_quic quic/subscriber_quic

.PHONY: all tcp udp quic cert test clean

all: tcp udp

tcp: $(TCP_BIN)
udp: $(UDP_BIN)
quic: $(QUIC_BIN)

# Cada binario X se compila a partir de X.c
$(TCP_BIN) $(UDP_BIN): %: %.c
	$(CC) $(CFLAGS) $< -o $@

$(QUIC_BIN): %: %.c
	$(CC) $(CFLAGS) $< -o $@ $(SSL_LIBS)

cert: quic/cert.pem

quic/cert.pem quic/key.pem:
	openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
	    -nodes -keyout quic/key.pem -out quic/cert.pem -days 365 -subj "/CN=localhost"

test:
	./test.sh

clean:
	rm -f $(TCP_BIN) $(UDP_BIN) $(QUIC_BIN)
