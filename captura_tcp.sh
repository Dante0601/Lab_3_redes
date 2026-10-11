#!/usr/bin/env bash
#
# Repite la prueba TCP de la sección 1 del informe y la graba en
# capturas/tcp_pubsub.pcap, lista para abrir en Wireshark.
#
# Escenario: 1 broker, 2 suscriptores y 2 publicadores, todos en el tema
# EquipoA_vs_EquipoB. Cada publicador envía 10 mensajes (20 en total) y cada
# uno se reenvía a los 2 suscriptores. Son 4 conexiones TCP hacia 127.0.0.1:5000.
#
# Uso (desde la raíz del repositorio):
#   ./captura_tcp.sh
#
# Variables:
#   PAUSA=s    segundos entre mensajes (por defecto 0.5)
#   MEDIR=1    mide CPU y memoria de los 5 procesos justo antes del cierre y
#              guarda la tabla en mediciones/recursos_captura_tcp.csv
#
# Requiere dumpcap (paquete wireshark) y que el usuario pueda capturar
# (grupo wireshark). Tarda unos 15 segundos.

set -u
cd "$(dirname "$0")"

PUERTO=5000
IP=127.0.0.1
TEMA=EquipoA_vs_EquipoB
PAUSA=${PAUSA:-0.5}
DIR=capturas
ARCHIVO=$DIR/tcp_pubsub.pcap
LOGS=$DIR/tcp_pubsub
SIN_CAPTURA=${SIN_CAPTURA:-0}     # 1 = correr la prueba sin grabar (para probar el script)
MEDIR=${MEDIR:-0}
CSV_MEDICION=mediciones/recursos_captura_tcp.csv
TICKS=$(getconf CLK_TCK)

# Mide un proceso vivo leyendo /proc: CPU de usuario y de sistema acumulada,
# memoria residente máxima (VmHWM) y tiempo de vida. Agrega una línea al CSV.
medir_vivo() {
    local pid=$1 etiqueta=$2
    [ -r "/proc/$pid/stat" ] || { echo "$etiqueta,,,,,," >>"$CSV_MEDICION"; return; }
    local utime stime inicio uptime rss
    read -r utime stime inicio < <(awk '{print $14, $15, $22}' "/proc/$pid/stat")
    uptime=$(cut -d' ' -f1 /proc/uptime)
    rss=$(awk '/^VmHWM:/ {print $2}' "/proc/$pid/status")
    awk -v e="$etiqueta" -v u="$utime" -v s="$stime" -v i="$inicio" -v up="$uptime" \
        -v r="$rss" -v t="$TICKS" 'BEGIN {
            us = u / t; ss = s / t; vida = up - i / t
            printf "%s,%.2f,%.2f,%.2f,%d,%.2f,%.1f\n", e, us, ss, us + ss, r, vida, 100 * (us + ss) / vida
        }' >>"$CSV_MEDICION"
}

# Mensajes de cada publicador (10 y 10), se envían intercalados.
PUB1=(
  "inicia el primer tiempo del partido"
  "El arbitro registra un fuera de lugar para el equipo A por un pase hacia Montero"
  "Fuera de lugar de Montero que la recibe para el equipo A"
  "se cobra falta y tarjeta amarilla para Velazquez del equipo B"
  "Penalty a favor del equipo A. La jugada la cobra el capitan"
  "el primer tiempo finaliza despues de 2 minutos de tiempo complementario"
  "fin del primer tiempo"
  "el segundo tiempo inicia con el balon en manos del equipo B"
  "Despues de que el var revisara una jugada con posible falta, se declara tarjeta roja y tiro libre a favor del equipo B"
  "El arbitro decreta tarjeta roja y tiro libre a favor del equipo B"
)
PUB2=(
  "el partido da inicio"
  "tiro libre por parte de Lopez del equipo A"
  "Rianio saca la pelota y la tira hacia el lado del equipo contrario"
  "El arbitro pita una falta a favor del equipo A por parte de Velazquez"
  "se cobra el penalty a favor del equipo A"
  "el capitan cobra penalty a favor del equipo A y marca un gol"
  "comienza el segundo tiempo con saque de porteria del equipo B"
  "Se evalua una posible falta por parte del equipo A al equipo B"
  "se llama al var para verificar la falta"
  "Morantes cobra tiro libre y saca el balon del estadio. Les toco ir a comprar otro"
)

PIDS=()
limpiar() {
    for pid in "${PIDS[@]:-}"; do kill "$pid" 2>/dev/null; done
    [ -n "${PID_CAPTURA:-}" ] && kill -INT "$PID_CAPTURA" 2>/dev/null
}
trap limpiar EXIT

make -s tcp || { echo "FALLA: no se pudo compilar la version TCP"; exit 1; }
mkdir -p "$LOGS"

# --- captura ---
PID_CAPTURA=""
if [ "$SIN_CAPTURA" != 1 ]; then
    command -v dumpcap >/dev/null || { echo "Falta dumpcap (sudo apt install wireshark)"; exit 1; }
    rm -f "$ARCHIVO"
    # -P guarda en formato pcap clásico (no pcapng), que es el que pide la guía.
    dumpcap -q -P -i lo -f "tcp port $PUERTO" -w "$ARCHIVO" >"$LOGS/dumpcap.log" 2>&1 &
    PID_CAPTURA=$!
    for _ in $(seq 1 50); do
        [ -s "$ARCHIVO" ] && break
        kill -0 "$PID_CAPTURA" 2>/dev/null || break
        sleep 0.1
    done
    if ! kill -0 "$PID_CAPTURA" 2>/dev/null; then
        echo "FALLA: dumpcap no pudo capturar (¿usuario en el grupo wireshark?)"
        cat "$LOGS/dumpcap.log"; exit 1
    fi
fi

# --- broker ---
stdbuf -oL ./tcp/broker_tcp >"$LOGS/broker.log" 2>&1 &
PID_BROKER=$!; PIDS+=("$PID_BROKER")
sleep 0.5
kill -0 "$PID_BROKER" 2>/dev/null || { echo "FALLA: el broker no arranco (¿puerto $PUERTO ocupado?)"; cat "$LOGS/broker.log"; exit 1; }

# --- 2 suscriptores al mismo tema ---
stdbuf -oL ./tcp/subscriber_tcp $IP $PUERTO $TEMA >"$LOGS/sub_1.log" 2>&1 &
PID_SUB1=$!; PIDS+=("$PID_SUB1")
sleep 1
stdbuf -oL ./tcp/subscriber_tcp $IP $PUERTO $TEMA >"$LOGS/sub_2.log" 2>&1 &
PID_SUB2=$!; PIDS+=("$PID_SUB2")
sleep 1

# --- 2 publicadores: cada uno lee sus mensajes de una tubería con nombre ---
rm -f "$LOGS/p1.fifo" "$LOGS/p2.fifo"; mkfifo "$LOGS/p1.fifo" "$LOGS/p2.fifo"
./tcp/publisher_tcp $IP $PUERTO $TEMA <"$LOGS/p1.fifo" >"$LOGS/pub_1.log" 2>&1 &
PID_PUB1=$!; PIDS+=("$PID_PUB1")
exec 3>"$LOGS/p1.fifo"
sleep 1
./tcp/publisher_tcp $IP $PUERTO $TEMA <"$LOGS/p2.fifo" >"$LOGS/pub_2.log" 2>&1 &
PID_PUB2=$!; PIDS+=("$PID_PUB2")
exec 4>"$LOGS/p2.fifo"
sleep 1

echo "Publicando 20 mensajes (pausa de ${PAUSA}s)..."
for i in $(seq 0 9); do
    echo "${PUB1[$i]}" >&3; sleep "$PAUSA"
    echo "${PUB2[$i]}" >&4; sleep "$PAUSA"
done
sleep 1

# --- medición de CPU y memoria (todos los procesos siguen vivos) ---
if [ "$MEDIR" = 1 ]; then
    mkdir -p mediciones
    echo "proceso,cpu_usuario_s,cpu_sistema_s,cpu_total_s,memoria_max_kb,tiempo_s,cpu_promedio_pct" >"$CSV_MEDICION"
    medir_vivo "$PID_BROKER" broker
    medir_vivo "$PID_SUB1" suscriptor_1
    medir_vivo "$PID_SUB2" suscriptor_2
    medir_vivo "$PID_PUB1" publicador_1
    medir_vivo "$PID_PUB2" publicador_2
fi

# --- cierre: primero los publicadores, luego los suscriptores, al final el broker ---
echo salir >&4; exec 4>&-; wait "$PID_PUB2" 2>/dev/null; sleep 0.5
echo salir >&3; exec 3>&-; wait "$PID_PUB1" 2>/dev/null; sleep 0.5
kill "$PID_SUB2" 2>/dev/null; wait "$PID_SUB2" 2>/dev/null; sleep 0.5
kill "$PID_SUB1" 2>/dev/null; wait "$PID_SUB1" 2>/dev/null; sleep 0.5
kill "$PID_BROKER" 2>/dev/null; wait "$PID_BROKER" 2>/dev/null
rm -f "$LOGS/p1.fifo" "$LOGS/p2.fifo"

if [ -n "$PID_CAPTURA" ]; then
    sleep 0.5
    kill -INT "$PID_CAPTURA" 2>/dev/null; wait "$PID_CAPTURA" 2>/dev/null
    PID_CAPTURA=""
fi
PIDS=()

# --- verificación ---
r1=$(grep -c "^>> \[$TEMA\]" "$LOGS/sub_1.log" 2>/dev/null || true)
r2=$(grep -c "^>> \[$TEMA\]" "$LOGS/sub_2.log" 2>/dev/null || true)
echo "Suscriptor 1 recibio $r1/20 mensajes; suscriptor 2 recibio $r2/20."
if [ "$r1" -ne 20 ] || [ "$r2" -ne 20 ]; then
    echo "FALLA: no llegaron todos los mensajes. Revise los logs en $LOGS/"
    exit 1
fi
if [ "$SIN_CAPTURA" != 1 ]; then
    echo "Captura guardada en $ARCHIVO"
    command -v capinfos >/dev/null && capinfos -c "$ARCHIVO" | tail -1
    echo "Abrir con: wireshark $ARCHIVO"
fi
if [ "$MEDIR" = 1 ]; then
    echo "CPU y memoria (guardado en $CSV_MEDICION):"
    column -t -s, "$CSV_MEDICION" | sed 's/^/  /'
fi
echo "Logs de la terminal en $LOGS/ (broker.log, sub_1.log, sub_2.log, pub_1.log, pub_2.log)"
