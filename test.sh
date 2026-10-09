#!/usr/bin/env bash
#
# Prueba de extremo a extremo del pub-sub: 1 broker, 2 suscriptores en temas
# distintos, 1 suscriptor que sigue ambos temas y 2 publicadores que envían
# 10 mensajes cada uno. Verifica que cada suscriptor de un tema recibe los 10
# mensajes de su tema y ninguno del otro, y que el de ambos temas recibe los 20.
#
# Uso:
#   ./test.sh              prueba TCP y UDP, y QUIC si openssl es >= 3.5
#   ./test.sh tcp udp      prueba solo los protocolos indicados

set -u
cd "$(dirname "$0")"

PUERTO=5000
IP=127.0.0.1
N=10
TEMA_A=EquipoA_vs_EquipoB
TEMA_B=EquipoC_vs_EquipoD

# Devuelve 0 si "openssl version" es OpenSSL >= 3.5.
openssl_soporta_quic() {
    local v
    v=$(openssl version 2>/dev/null) || return 1
    [[ "$v" =~ ^OpenSSL\ ([0-9]+)\.([0-9]+) ]] || return 1
    local mayor=${BASH_REMATCH[1]} menor=${BASH_REMATCH[2]}
    [ "$mayor" -gt 3 ] || { [ "$mayor" -eq 3 ] && [ "$menor" -ge 5 ]; }
}

# Cuenta cuántas líneas ">> [tema] ..." de un archivo corresponden a un tema.
contar_tema() {
    grep -c "^>> \[$2\]" "$1" 2>/dev/null || true
}

# Revisa la salida de un suscriptor. Imprime el resultado y devuelve 0/1.
verificar() {
    local archivo=$1 tema_propio=$2 tema_ajeno=$3 etiqueta=$4
    local propios ajenos faltan=""
    propios=$(contar_tema "$archivo" "$tema_propio")
    ajenos=$(contar_tema "$archivo" "$tema_ajeno")
    for i in $(seq 1 $N); do
        grep -q "^>> \[$tema_propio\] .*evento $tema_propio $i\$" "$archivo" || faltan="$faltan $i"
    done
    if [ "$propios" -eq "$N" ] && [ "$ajenos" -eq 0 ] && [ -z "$faltan" ]; then
        echo "  OK   $etiqueta: $propios/$N de '$tema_propio', $ajenos de '$tema_ajeno'"
        return 0
    fi
    echo "  FALLA $etiqueta: $propios/$N de '$tema_propio', $ajenos de '$tema_ajeno'${faltan:+, faltan:$faltan}"
    echo "  --- salida de $etiqueta ---"
    sed 's/^/  | /' "$archivo"
    return 1
}

# Revisa la salida del suscriptor que sigue los dos temas: debe tener los N
# mensajes de cada uno. Imprime el resultado y devuelve 0/1.
verificar_ambos() {
    local archivo=$1 etiqueta=$2
    local a b faltan=""
    a=$(contar_tema "$archivo" "$TEMA_A")
    b=$(contar_tema "$archivo" "$TEMA_B")
    for tema in $TEMA_A $TEMA_B; do
        for i in $(seq 1 $N); do
            grep -q "^>> \[$tema\] .*evento $tema $i\$" "$archivo" || faltan="$faltan $tema#$i"
        done
    done
    if [ "$a" -eq "$N" ] && [ "$b" -eq "$N" ] && [ -z "$faltan" ]; then
        echo "  OK   $etiqueta: $a/$N de '$TEMA_A', $b/$N de '$TEMA_B'"
        return 0
    fi
    echo "  FALLA $etiqueta: $a/$N de '$TEMA_A', $b/$N de '$TEMA_B'${faltan:+, faltan:$faltan}"
    echo "  --- salida de $etiqueta ---"
    sed 's/^/  | /' "$archivo"
    return 1
}

# Mata un proceso si sigue vivo y espera a que termine.
detener() {
    for pid in "$@"; do
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    done
}

probar() {
    local proto=$1
    local dir=$proto
    local logs
    logs=$(mktemp -d "${TMPDIR:-/tmp}/pubsub_${proto}.XXXXXX")

    echo "== $proto (puerto $PUERTO) =="

    if ! make -s "$proto" >"$logs/make.log" 2>&1; then
        echo "  FALLA compilacion:"; sed 's/^/  | /' "$logs/make.log"
        return 1
    fi

    if [ "$proto" = quic ]; then
        make -s cert >/dev/null 2>&1 || { echo "  FALLA generando certificado"; return 1; }
        "./$dir/broker_$proto" quic/cert.pem quic/key.pem >"$logs/broker.log" 2>&1 &
    else
        "./$dir/broker_$proto" >"$logs/broker.log" 2>&1 &
    fi
    local pid_broker=$!
    sleep 0.5
    if ! kill -0 "$pid_broker" 2>/dev/null; then
        echo "  FALLA: el broker no arranco"; sed 's/^/  | /' "$logs/broker.log"
        return 1
    fi

    "./$dir/subscriber_$proto" $IP "$PUERTO" $TEMA_A >"$logs/sub_a.log" 2>&1 &
    local pid_sub_a=$!
    "./$dir/subscriber_$proto" $IP "$PUERTO" $TEMA_B >"$logs/sub_b.log" 2>&1 &
    local pid_sub_b=$!
    "./$dir/subscriber_$proto" $IP "$PUERTO" $TEMA_A $TEMA_B >"$logs/sub_ab.log" 2>&1 &
    local pid_sub_ab=$!
    sleep 1   # dar tiempo a que el broker registre todas las suscripciones

    # Los dos publicadores corren en paralelo; cada uno manda N eventos y "salir".
    local pids_pub=()
    for tema in $TEMA_A $TEMA_B; do
        { for i in $(seq 1 $N); do echo "evento $tema $i"; done; echo salir; } \
            | "./$dir/publisher_$proto" $IP "$PUERTO" "$tema" >"$logs/pub_$tema.log" 2>&1 &
        pids_pub+=($!)
    done
    wait "${pids_pub[@]}"
    sleep 1   # dar tiempo a que lleguen los últimos MSG

    # Se detiene primero el broker: en TCP el suscriptor ve el cierre,
    # sale de su bucle normalmente y vacía su salida estándar.
    detener "$pid_broker"
    sleep 0.3
    detener "$pid_sub_a" "$pid_sub_b" "$pid_sub_ab"

    local r=0
    verificar "$logs/sub_a.log" $TEMA_A $TEMA_B "suscriptor A" || r=1
    verificar "$logs/sub_b.log" $TEMA_B $TEMA_A "suscriptor B" || r=1
    verificar_ambos "$logs/sub_ab.log" "suscriptor A+B" || r=1
    if [ "$proto" = udp ] && grep -q AVISO "$logs"/sub_*.log; then
        echo "  nota: avisos de secuencia en UDP:"; grep -h AVISO "$logs"/sub_*.log | sed 's/^/  | /'
    fi
    if [ $r -ne 0 ]; then
        echo "  logs en $logs"
    else
        rm -rf "$logs"
    fi
    return $r
}

protocolos=("$@")
if [ ${#protocolos[@]} -eq 0 ]; then
    protocolos=(tcp udp)
    if openssl_soporta_quic; then
        protocolos+=(quic)
    else
        echo "QUIC omitido: 'openssl version' = '$(openssl version 2>/dev/null)' (se requiere OpenSSL >= 3.5)"
    fi
fi

fallos=0
for p in "${protocolos[@]}"; do
    probar "$p" || fallos=$((fallos + 1))
done

echo
if [ $fallos -eq 0 ]; then
    echo "Todas las pruebas pasaron (${protocolos[*]})."
else
    echo "$fallos protocolo(s) con fallas."
fi
exit $fallos
