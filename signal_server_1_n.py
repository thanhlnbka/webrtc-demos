import asyncio
import websockets
import json
import uuid
from typing import Dict, Set, Optional

# ====== Constants & Data Structures ======

connected_clients: Dict[str, dict] = {}
SENDER_CLIENT: Optional[str] = None
active_sessions: Dict[str, dict] = {}

# ====== Utils ======

def log(msg: str, level: str = "INFO"):
    prefix = {
        "INFO": "\033[94m[INFO]\033[0m",
        "WARN": "\033[93m[WARN]\033[0m",
        "ERROR": "\033[91m[ERROR]\033[0m",
        "DEBUG": "\033[92m[DEBUG]\033[0m"
    }.get(level, "[LOG]")
    print(f"{prefix} {msg}")

def generate_session_id() -> str:
    return str(uuid.uuid4())

# ====== Core WebSocket Handler ======

async def signaling_handler(websocket, path):
    global SENDER_CLIENT

    client_id = str(uuid.uuid4())
    client_ip = websocket.remote_address[0]
    client_type = "unknown"

    log(f"New connection from {client_ip} (ID: {client_id})")

    try:
        first_message = await websocket.recv()
        data = json.loads(first_message)

        if "client_type" in data:
            client_type = data["client_type"]
        elif "sdp" in data and data.get("type") == "offer":
            client_type = "sender"
        else:
            client_type = "viewer"

        connected_clients[client_id] = {
            "websocket": websocket,
            "type": client_type,
            "ip": client_ip
        }

        log(f"{client_type.upper()} {client_id} registered from {client_ip}")

        if client_type == "sender":
            if SENDER_CLIENT:
                log("Replacing existing sender", "WARN")
            SENDER_CLIENT = client_id
            await broadcast_sender_status(True)

        elif client_type == "viewer":
            await notify_sender_of_new_viewer(client_id)

        await websocket.send(json.dumps({
            "type": "registration_successful",
            "client_id": client_id
        }))

        await process_signaling_message(client_id, data)

        async for message in websocket:
            try:
                data = json.loads(message)
                await process_signaling_message(client_id, data)
            except json.JSONDecodeError:
                log(f"Invalid JSON from {client_id}", "WARN")

    except websockets.exceptions.ConnectionClosed as e:
        log(f"Client {client_id} ({client_type}) disconnected: {e}", "WARN")

    except Exception as e:
        log(f"Exception from {client_id}: {e}", "ERROR")

    finally:
        await cleanup_client(client_id, client_type)

# ====== Message Processing ======

async def process_signaling_message(client_id: str, data: dict):
    if client_id not in connected_clients:
        log(f"Unknown client {client_id} tried to send message", "WARN")
        return

    client_type = connected_clients[client_id]["type"]

    if "sdp" in data:
        await handle_sdp(client_id, client_type, data)

    elif "candidate" in data:
        await handle_ice_candidate(client_id, client_type, data)

    elif data.get("type") == "create_new_offer" and client_type == "viewer":
        await request_offer_from_sender(client_id)

    elif data.get("type") == "viewer_joined" and client_type == "viewer":
        await notify_sender_of_new_viewer(client_id)

async def handle_sdp(client_id: str, client_type: str, data: dict):
    sdp_type = data.get("type")
    target_id = data.get("target_id")
    session_id = data.get("session_id", generate_session_id())

    log(f"{client_type.upper()} {client_id} sent SDP ({sdp_type}) to {target_id or 'N/A'}")

    if sdp_type == "offer" and client_type == "sender" and target_id:
        session = active_sessions.setdefault(session_id, {
            "sender": client_id,
            "viewers": set(),
            "offer": data
        })
        session["viewers"].add(target_id)

        await forward_to_client(target_id, data, client_id, session_id, "OFFER")

    elif sdp_type == "answer" and client_type == "viewer":
        target_id = target_id or SENDER_CLIENT
        if target_id:
            active_sessions.setdefault(session_id, {"sender": target_id, "viewers": set()})["viewers"].add(client_id)
            await forward_to_client(target_id, data, client_id, session_id, "ANSWER")
        else:
            log("No sender available to forward answer", "ERROR")

async def handle_ice_candidate(client_id: str, client_type: str, data: dict):
    session_id = data.get("session_id")
    target_id = data.get("target_id")

    log(f"{client_type.upper()} {client_id} sent ICE candidate")

    if target_id:
        await forward_to_client(target_id, data, client_id, session_id, "ICE")
    elif session_id in active_sessions:
        targets = (
            active_sessions[session_id]["viewers"]
            if client_type == "sender"
            else [active_sessions[session_id]["sender"]]
        )
        for tid in targets:
            await forward_to_client(tid, data, client_id, session_id, "ICE")
    elif client_type == "viewer" and SENDER_CLIENT:
        await forward_to_client(SENDER_CLIENT, data, client_id, session_id, "ICE")

async def forward_to_client(target_id: str, data: dict, sender_id: str, session_id: str, label: str):
    if target_id not in connected_clients:
        log(f"Target {target_id} not connected", "WARN")
        return
    try:
        message = data.copy()
        message["from"] = sender_id
        message["session_id"] = session_id
        await connected_clients[target_id]["websocket"].send(json.dumps(message))
        log(f"[{label}] {sender_id} -> {target_id}")
    except Exception as e:
        log(f"Failed to forward {label} to {target_id}: {e}", "ERROR")

# ====== Sender/Viewer Communication ======

async def notify_sender_of_new_viewer(viewer_id: str):
    if SENDER_CLIENT and SENDER_CLIENT in connected_clients:
        try:
            await connected_clients[SENDER_CLIENT]["websocket"].send(json.dumps({
                "type": "viewer_joined",
                "viewer_id": viewer_id,
                "session_id": generate_session_id()
            }))
            log(f"Notified sender of new viewer {viewer_id}")
        except Exception as e:
            log(f"Failed to notify sender: {e}", "ERROR")

async def request_offer_from_sender(viewer_id: str):
    if SENDER_CLIENT and SENDER_CLIENT in connected_clients:
        try:
            await connected_clients[SENDER_CLIENT]["websocket"].send(json.dumps({
                "type": "create_new_offer",
                "viewer_id": viewer_id,
                "session_id": generate_session_id()
            }))
            log(f"{viewer_id} requested new offer from sender")
        except Exception as e:
            log(f"Request offer error: {e}", "ERROR")

async def broadcast_sender_status(available: bool):
    message = {
        "type": "sender_status",
        "available": available,
        "sender_id": SENDER_CLIENT if available else None
    }

    for cid, info in connected_clients.items():
        if info["type"] == "viewer":
            try:
                await info["websocket"].send(json.dumps(message))
            except Exception as e:
                log(f"Failed to send sender status to {cid}: {e}", "ERROR")

# ====== Cleanup ======

async def cleanup_client(client_id: str, client_type: str):
    global SENDER_CLIENT

    connected_clients.pop(client_id, None)

    if client_type == "sender" and SENDER_CLIENT == client_id:
        SENDER_CLIENT = None
        await broadcast_sender_status(False)

    # Remove sessions
    sessions_to_remove = [
        sid for sid, sess in active_sessions.items()
        if sess["sender"] == client_id or client_id in sess["viewers"]
    ]
    for sid in sessions_to_remove:
        del active_sessions[sid]

    log(f"Cleaned up client {client_id} ({client_type}). Remaining: {len(connected_clients)}")

# ====== Server Startup ======

async def main():
    host = "0.0.0.0"
    port = 8765
    log(f"Starting signaling server on ws://{host}:{port}")
    async with websockets.serve(signaling_handler, host, port):
        await asyncio.Future()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log("Server shutting down...", "WARN")
