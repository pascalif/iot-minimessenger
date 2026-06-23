#!/usr/bin/env python3
"""
MQTT Chat Client
Communicates with ESP32 device via HiveMQ MQTT broker.
"""

import curses
import re
import ssl
import sys
import threading
import time
from datetime import datetime

import paho.mqtt.client as mqtt

from config import (
    DEVICE_ID_ME,
    MQTT_HOST,
    MQTT_PASSWORD,
    MQTT_PORT,
    MQTT_USER,
)

# ──────────────────────────────────────────────
# Paramètres applicatifs (peuvent changer)
# ──────────────────────────────────────────────
SHOW_LIVE_STATUS  = True   # False = masque les lignes status=LIVE
CONNECT_TIMEOUT   = 10     # secondes avant d'abandonner la connexion
LIVENESS_INTERVAL = 120    # secondes entre deux pings LIVE

# ──────────────────────────────────────────────
# Regex
# ──────────────────────────────────────────────
RE_MSG_SPLIT = re.compile(r'^(.*?)(?:\s*#(.*))?$', re.DOTALL)    # separe txt et trailer optionnel
RE_DEVICE_ID = re.compile(r'deviceId:(\d+)')                       # extrait deviceId du trailer
RE_LIVENESS  = re.compile(r'^(\S+)\s+(\S+)$')                     # <status> <ts>

# ──────────────────────────────────────────────
# Shared state
# ──────────────────────────────────────────────
messages       = []   # list of (color_name, text)
messages_lock  = threading.Lock()
new_msg_event  = threading.Event()
stop_event     = threading.Event()
mqtt_client    = None

# Connexion : signaux entre callbacks et main thread
_connected_event    = threading.Event()
_connect_error_code = None   # rc reçu par on_connect si échec


# ──────────────────────────────────────────────
# Helpers
# ──────────────────────────────────────────────
def now_str():
    return datetime.now().strftime("%H:%M:%S")


def parse_msg(payload: str):
    """Extrait (txt, device_id) depuis le payload.
    Le trailer # et le deviceId sont tous deux facultatifs.
    Retourne "unk" si le deviceId est absent ou non décodable.
    """
    m = RE_MSG_SPLIT.match(payload)
    if not m:
        return payload.strip(), "unk"
    txt     = m.group(1).strip()
    trailer = m.group(2) or ""
    dev_m   = RE_DEVICE_ID.search(trailer)
    dev_id  = dev_m.group(1) if dev_m else "unk"
    return txt, dev_id


def mqtt_rc_description(rc: int) -> str:
    """Traduit un code de retour MQTT en message lisible."""
    return {
        1: "Protocole MQTT refusé par le broker",
        2: "Client ID rejeté",
        3: "Broker indisponible (réseau ?)",
        4: "Identifiants incorrects (user/password)",
        5: "Connexion non autorisée",
    }.get(rc, f"Erreur inconnue (rc={rc})")


# ──────────────────────────────────────────────
# Callbacks MQTT
# ──────────────────────────────────────────────
def on_connect(client, userdata, flags, reason_code, properties):
    global _connect_error_code
    rc = reason_code if isinstance(reason_code, int) else reason_code.value
    if rc == 0:
        client.subscribe("msg/broadcast")
        client.subscribe(f"msg/unicast/{DEVICE_ID_ME}")
        client.subscribe("admin/liveness/#")
        client.publish(f"admin/liveness/{DEVICE_ID_ME}", payload=f"BOOT {int(__import__('time').time())}", qos=1)
        _connected_event.set()
    else:
        _connect_error_code = rc
        _connected_event.set()   # débloque l'attente même en erreur


def on_disconnect(client, userdata, disconnect_flags, reason_code, properties):
    if not stop_event.is_set():
        with messages_lock:
            messages.append(("pink", f"⚠  Déconnecté du broker (rc={reason_code})"))
        new_msg_event.set()


def on_message(client, userdata, msg):
    topic   = msg.topic
    payload = msg.payload.decode("utf-8", errors="replace").strip()

    with messages_lock:
        if topic == "msg/broadcast":
            txt, dev_id = parse_msg(payload)
            if dev_id == str(DEVICE_ID_ME):
                return   # ignore mes propres messages
            messages.append(("yellow", f"[{dev_id}] - {now_str()} : {txt}"))

        elif topic == f"msg/unicast/{DEVICE_ID_ME}":
            txt, dev_id = parse_msg(payload)
            messages.append(("orange", f"[{dev_id}] - {now_str()} : {txt}"))

        elif topic.startswith("admin/liveness/"):
            dev_id = topic.split("/")[-1]
            m = RE_LIVENESS.match(payload)
            status = m.group(1) if m else payload
            if status == "LIVE" and not SHOW_LIVE_STATUS:
                return
            messages.append(("pink", f"[{dev_id}] - {now_str()} : {status}"))

    new_msg_event.set()


# ──────────────────────────────────────────────
# MQTT setup
# ──────────────────────────────────────────────
def setup_mqtt():
    """
    Crée et connecte le client MQTT.
    Lève une RuntimeError avec un message clair en cas d'échec.
    """
    global mqtt_client

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
    client.tls_set(tls_version=ssl.PROTOCOL_TLS_CLIENT)
    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_message    = on_message

    liveness_topic = f"admin/liveness/{DEVICE_ID_ME}"
    client.will_set(liveness_topic, payload="DEAD", qos=1, retain=False)

    try:
        client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    except OSError as e:
        raise RuntimeError(f"Impossible de joindre le broker ({MQTT_HOST}:{MQTT_PORT}) : {e}") from e

    client.loop_start()

    # Attendre la confirmation de connexion (ou l'erreur)
    if not _connected_event.wait(timeout=CONNECT_TIMEOUT):
        client.loop_stop()
        raise RuntimeError(
            f"Timeout : pas de réponse du broker après {CONNECT_TIMEOUT}s "
            f"({MQTT_HOST}:{MQTT_PORT})"
        )

    if _connect_error_code is not None:
        client.loop_stop()
        raise RuntimeError(
            f"Connexion refusée par le broker : {mqtt_rc_description(_connect_error_code)}"
        )

    mqtt_client = client
    return client


# ──────────────────────────────────────────────
# Publication
# ──────────────────────────────────────────────
def publish_message(text: str):
    ts = int(time.time())
    payload = f"{text} # deviceId:{DEVICE_ID_ME} ts:{ts}"
    mqtt_client.publish("msg/broadcast", payload)


def publish_liveness():
    """Publie LIVE toutes les LIVENESS_INTERVAL secondes."""
    while not stop_event.wait(timeout=LIVENESS_INTERVAL):
        ts = int(time.time())
        mqtt_client.publish(f"admin/liveness/{DEVICE_ID_ME}", payload=f"LIVE {ts}", qos=1)


# ──────────────────────────────────────────────
# TUI (curses)
# ──────────────────────────────────────────────
def init_colors():
    curses.start_color()
    curses.use_default_colors()
    curses.init_pair(1, curses.COLOR_YELLOW,  -1)   # broadcast reçu
    curses.init_pair(2, curses.COLOR_RED,     -1)   # unicast reçu (orange≈red+bold)
    curses.init_pair(3, curses.COLOR_MAGENTA, -1)   # liveness / system
    curses.init_pair(4, curses.COLOR_WHITE,   -1)   # mes messages envoyés
    curses.init_pair(5, curses.COLOR_CYAN,    -1)   # prompt / séparateur


COLOR_MAP = {"yellow": 1, "orange": 2, "pink": 3, "white": 4, "cyan": 5}


def run_tui(stdscr):
    curses.curs_set(1)
    stdscr.nodelay(True)
    stdscr.keypad(True)
    init_colors()

    input_buf = ""

    while not stop_event.is_set():
        height, width = stdscr.getmaxyx()
        msg_area_h = height - 2
        sep_y      = height - 2
        input_y    = height - 1

        # ── Messages ─────────────────────────────────
        stdscr.erase()
        with messages_lock:
            visible = messages[-msg_area_h:]

        for i, (color_name, text) in enumerate(visible):
            if i >= msg_area_h:
                break
            pair = COLOR_MAP.get(color_name, 4)
            attr = curses.color_pair(pair)
            if color_name == "orange":
                attr |= curses.A_BOLD
            try:
                stdscr.addnstr(i, 0, text, width - 1, attr)
            except curses.error:
                pass

        # ── Séparateur ───────────────────────────────
        try:
            stdscr.addnstr(sep_y, 0, "─" * (width - 1), width - 1,
                           curses.color_pair(5))
        except curses.error:
            pass

        # ── Ligne d'entrée ───────────────────────────
        prompt     = f"[{DEVICE_ID_ME}]> "
        input_line = prompt + input_buf
        try:
            stdscr.addnstr(input_y, 0, input_line, width - 1,
                           curses.color_pair(5))
        except curses.error:
            pass

        try:
            stdscr.move(input_y, min(len(input_line), width - 1))
        except curses.error:
            pass

        stdscr.refresh()

        # ── Clavier ──────────────────────────────────
        new_msg_event.clear()
        try:
            key = stdscr.get_wch()
        except curses.error:
            key = None

        if key is not None:
            if key in (curses.KEY_ENTER, '\n', '\r'):
                if input_buf.strip():
                    with messages_lock:
                        messages.append(("white", f"[{DEVICE_ID_ME}] - {now_str()} : {input_buf}"))
                    publish_message(input_buf)
                    input_buf = ""

            elif key in (curses.KEY_BACKSPACE, '\x7f', '\b'):
                input_buf = input_buf[:-1]

            elif key == '\x03':   # Ctrl-C
                stop_event.set()
                break

            elif isinstance(key, str) and key.isprintable():
                input_buf += key

        new_msg_event.wait(timeout=0.05)


# ──────────────────────────────────────────────
# Point d'entrée
# ──────────────────────────────────────────────
def main():
    print(f"Connexion à {MQTT_HOST}:{MQTT_PORT}…")
    try:
        client = setup_mqtt()
    except RuntimeError as e:
        print(f"\n❌  {e}", file=sys.stderr)
        sys.exit(1)

    print(f"✓  Connecté — device ID : {DEVICE_ID_ME}")
    liveness_thread = threading.Thread(target=publish_liveness, daemon=True)
    liveness_thread.start()
    time.sleep(0.3)

    try:
        curses.wrapper(run_tui)
    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        client.loop_stop()
        client.disconnect()
        print("Déconnecté.")


if __name__ == "__main__":
    main()