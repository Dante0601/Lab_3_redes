#!/usr/bin/env bash
#
# Prueba del pub-sub con una red mala simulada con tc netem sobre loopback (lo).
# Compara TCP y UDP: 1 broker, 1 suscriptor y 1 publicador que envía N eventos
# mientras la red pierde, retrasa y desordena paquetes.
#
# Uso:
#   ./test_red_mala.sh                 TCP y UDP, 20% de pérdida
#   PERDIDA=40% N=100 ./test_red_mala.sh udp
#
# Requiere sudo (solo para tc). La regla de netem se quita al terminar,
# incluso si el script se interrumpe con Ctrl+C.

set -u
cd "$(dirname "$0")"

IP=127.0.0.1
PUERTO=5000
TEMA=EquipoA_vs_EquipoB
N=${N:-50}
PERDIDA=${PERDIDA:-20%}
RETARDO=${RETARDO:-30ms}
DESORDEN=${DESORDEN:-25%}

quitar_netem() {
    sudo tc qdisc del dev lo root 2>/dev/null
}
trap quitar_netem EXIT

poner_netem() {
    sudo tc qdisc replace dev lo root netem loss "$PERDIDA" delay "$RETARDO" reorder "$DESORDEN"
}

detener() {
    for pid in "$@"; do
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    done
}

probar() {
    local proto=$1
    local logs
    logs=$(mktemp -d "${TMPDIR:-/tmp}/redmala_${proto}.XXXXXX")
    echo "== $proto: pérdida $PERDIDA, retardo $RETARDO, desorden $DESORDEN, $N mensajes =="

    make -s "$proto" || return 1

    # El broker y el suscriptor se conectan con la red sana, para que el SUB
    # (o el handshake TCP) no se pierda antes de empezar.
    "./$proto/broker_$proto" >"$logs/broker.log" 2>&1 &
    local pid_broker=$!
    sleep 0.5
    "./$proto/subscriber_$proto" $IP $PUERTO $TEMA >"$logs/sub.log" 2>&1 &
    local pid_sub=$!
    sleep 1

    poner_netem || { echo "  no se pudo aplicar netem"; detener "$pid_sub" "$pid_broker"; return 1; }

    local inicio=$SECONDS
    { for i in $(seq 1 "$N"); do echo "evento $i"; done; echo salir; } \
        | "./$proto/publisher_$proto" $IP $PUERTO $TEMA >"$logs/pub.log" 2>&1
    # Tiempo para que TCP termine de retransmitir lo pendiente.
    sleep 8
    quitar_netem
    sleep 1

    # Se detiene primero el broker: el suscriptor TCP ve el cierre y vacía su salida.
    detener "$pid_broker"
    sleep 0.3
    detener "$pid_sub"

    local recibidos faltan="" avisos
    recibidos=$(grep -c "^>> \[$TEMA\]" "$logs/sub.log")
    for i in $(seq 1 "$N"); do
        grep -q "^>> \[$TEMA\] .*evento $i\$" "$logs/sub.log" || faltan="$faltan $i"
    done
    avisos=$(grep -c AVISO "$logs/sub.log")

    echo "  recibidos: $recibidos/$N  (tiempo: $((SECONDS - inicio)) s, incluye 8 s de espera)"
    echo "  perdidos:  ${faltan:- ninguno}"
    # Orden de llegada: compara la secuencia recibida con la ordenada.
    local orden
    orden=$(grep "^>> \[$TEMA\]" "$logs/sub.log" | grep -o 'evento [0-9]*$' | awk '{print $2}')
    if [ "$orden" = "$(echo "$orden" | sort -n)" ]; then
        echo "  orden:     correcto"
    else
        echo "  orden:     DESORDENADO"
    fi
    if [ "$avisos" -gt 0 ]; then
        echo "  avisos del suscriptor ($avisos):"
        grep AVISO "$logs/sub.log" | sed 's/^/    | /'
    fi
    echo "  logs en $logs"
    echo
}

sudo -v || exit 1   # pide la contraseña una sola vez al inicio

protocolos=("$@")
[ ${#protocolos[@]} -eq 0 ] && protocolos=(tcp udp)
for p in "${protocolos[@]}"; do
    probar "$p"
done
