#!/usr/bin/env python3
"""
MICROCORE (MCX) WIFI BRIDGE v7.0 — MULTI-MINER SUPPORT
Detects multiple Arduino/AVR miners on different ports
Onboards all connected miners automatically
Gossip Discovery | Peer Caching | Auto Failover

Run: python3 wifi_bridge.py

Requirements:
  pip install pyserial websockets
"""

import asyncio
import serial
import serial.tools.list_ports
import json
import websockets
import time
import os
import sys
from datetime import datetime
from collections import defaultdict

# ==================== GOSSIP DISCOVERY ====================
BOOTSTRAP_NODES = ["127.0.0.1:8080"]
PEER_CACHE_FILE = "bridge_peers.json"
NODE_PORT = 8080
BAUD_RATE = 115200
MAX_RECONNECT_ATTEMPTS = 10
RECONNECT_DELAY = 5

# ==================== GLOBAL VARIABLES ====================
running = True
websocket = None
message_buffers = {}  # Per-port message buffers
current_node_url = None
node_urls = []
current_node_index = 0
reconnect_attempts = 0
discovered_peers = set()
is_registered = False

# Multi-miner tracking
arduino_ports = {}  # port -> serial object
arduino_data = {}  # port -> last data received
arduino_registered = {}  # port -> registration status
arduino_handlers = {}  # port -> asyncio tasks
miner_info = {}  # port -> miner info (vid, username, wallet)

stats = {
    "messages_sent": 0,
    "messages_received": 0,
    "errors": 0,
    "start_time": time.time(),
    "node_switches": 0,
    "arduino_messages": 0,
    "miners_connected": 0,
    "miners_registered": 0
}

# ==================== UTILITY ====================
def save_peers_to_cache(peers):
    try:
        unique = list(set(peers))
        with open(PEER_CACHE_FILE, 'w') as f:
            json.dump(unique, f, indent=2)
        print(f"[CACHE] Saved {len(unique)} peers")
    except:
        pass

def load_peers_from_cache():
    try:
        with open(PEER_CACHE_FILE, 'r') as f:
            return json.load(f)
    except:
        return []

def get_bootstrap_peers():
    peers = BOOTSTRAP_NODES.copy()
    cached = load_peers_from_cache()
    for p in cached:
        if p not in peers:
            peers.append(p)
    return peers

def get_current_node_url():
    if not node_urls:
        return None
    peer = node_urls[current_node_index]
    if "://" not in peer:
        peer = f"ws://{peer}"
    return peer

def djb2_hash(data):
    h = 5381
    for c in data:
        h = ((h << 5) + h) + ord(c)
    return format(h & 0xFFFFFFFF, '08x')

# ==================== PORT SCANNING ====================
def find_all_arduino_ports():
    """Find all Arduino/Serial ports"""
    ports = serial.tools.list_ports.comports()
    found = []
    print("\n[SCAN] Scanning for Arduino/AVR miners...")
    print("-" * 50)
    
    for port in ports:
        # Check if it's a serial port
        if "COM" in port.device or "tty" in port.device or "cu." in port.device:
            print(f"  Found: {port.device} - {port.description}")
            found.append(port.device)
    
    print("-" * 50)
    print(f"[SCAN] Found {len(found)} potential port(s)")
    
    # Ask user which ports to use
    if found:
        print("\n[SELECT] Which ports have Arduino/AVR miners?")
        print("  Enter port numbers separated by commas (e.g., COM3,COM4)")
        print("  Or press Enter to use all found ports")
        choice = input("  Ports to use: ").strip()
        
        if choice:
            selected = [p.strip() for p in choice.split(',')]
            selected = [p for p in selected if p in found]
            if selected:
                return selected
            else:
                print("[WARN] No valid ports selected. Using all found ports.")
                return found
        else:
            return found
    else:
        print("[WARN] No ports found. Waiting for plug-in...")
        return []

def test_port(port):
    """Test if a port responds to JSON commands"""
    try:
        ser = serial.Serial(port, BAUD_RATE, timeout=2)
        time.sleep(1)
        # Send a test ping
        ser.write(b'{"type":"ping"}\n')
        time.sleep(1)
        
        # Check for response
        response = ser.read(100)
        ser.close()
        
        if response:
            try:
                data = json.loads(response.decode('utf-8', errors='ignore'))
                if data.get('type') == 'pong' or data.get('type') == 'register':
                    print(f"[TEST] ✅ {port} responded to ping")
                    return True
            except:
                pass
        return False
    except Exception as e:
        return False

# ==================== WEBSOCKET CONNECTION ====================
async def connect_to_node():
    global websocket, current_node_url, node_urls, reconnect_attempts, is_registered
    
    while running:
        if not node_urls:
            node_urls = get_bootstrap_peers()
            current_node_index = 0
        
        current_node_url = get_current_node_url()
        if not current_node_url:
            print("[BRIDGE] No nodes available. Waiting...")
            await asyncio.sleep(30)
            node_urls = get_bootstrap_peers()
            continue
        
        try:
            print(f"[BRIDGE] Connecting to node: {current_node_url}")
            async with websockets.connect(
                current_node_url,
                ping_interval=20,
                ping_timeout=10,
                close_timeout=5
            ) as ws:
                websocket = ws
                reconnect_attempts = 0
                is_registered = False
                print(f"[BRIDGE] ✅ Connected to {current_node_url}")
                
                # Request peers
                await ws.send(json.dumps({"type": "get_peers"}))
                
                try:
                    async for message in ws:
                        await handle_node_message(message)
                except websockets.exceptions.ConnectionClosed:
                    print(f"[BRIDGE] Connection closed")
                    
        except Exception as e:
            print(f"[BRIDGE] ❌ Connection failed: {e}")
            await asyncio.sleep(5)
        
        websocket = None

# ==================== NODE MESSAGE HANDLER ====================
async def handle_node_message(message):
    global websocket, stats, is_registered
    try:
        msg = json.loads(message)
        stats["messages_received"] += 1
        print(f"[← NODE] {message[:200]}")
        
        if msg.get("type") == "peers":
            for peer in msg.get("peers", []):
                if peer not in discovered_peers:
                    discovered_peers.add(peer)
                    node_urls.append(peer)
            save_peers_to_cache(list(discovered_peers))
            return
        
        if msg.get("type") == "registered":
            is_registered = True
            stats["miners_registered"] += 1
            # Forward to all Arduino ports
            for port, ser in arduino_ports.items():
                if ser and ser.is_open:
                    try:
                        ser.write((message + "\n").encode())
                    except:
                        pass
            return
        
        # Forward to all Arduino ports
        for port, ser in arduino_ports.items():
            if ser and ser.is_open:
                try:
                    ser.write((message + "\n").encode())
                except:
                    pass
            
    except Exception as e:
        print(f"[ERROR] Node message: {e}")

# ==================== ARDUINO MESSAGE HANDLER ====================
async def handle_arduino_messages(port):
    """Handle messages from a specific Arduino port"""
    global stats, arduino_data, arduino_registered, miner_info
    
    ser = arduino_ports.get(port)
    if not ser or not ser.is_open:
        return
    
    print(f"[ARDUINO:{port}] 📡 Listening for messages...")
    
    while running and ser and ser.is_open:
        try:
            if ser.in_waiting:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                if line:
                    stats["arduino_messages"] += 1
                    arduino_data[port] = time.time()
                    print(f"[ARDUINO:{port}] 📤 {line[:150]}")
                    
                    # Try to parse JSON
                    try:
                        data = json.loads(line)
                        
                        # Track miner info
                        if data.get("type") == "register":
                            username = data.get("username", "unknown")
                            vid = data.get("validator_id", "unknown")
                            miner_info[port] = {
                                "username": username,
                                "validator_id": vid,
                                "wallet": data.get("wallet", ""),
                                "stake": data.get("stake", 0),
                                "level": data.get("level", 1)
                            }
                            print(f"[ARDUINO:{port}] 🔑 Miner '{username}' (ID: {vid[:8]}) registering...")
                            stats["miners_connected"] += 1
                            
                        if data.get("type") == "uptime_ping":
                            vid = data.get("validator_id", "unknown")
                            uptime = data.get("uptime_seconds", 0)
                            print(f"[ARDUINO:{port}] ⏱️ Miner {vid[:8]} uptime: {uptime}s")
                            
                        # Check for registration response
                        if data.get("type") == "debug" and "REGISTERED" in data.get("message", ""):
                            arduino_registered[port] = True
                            print(f"[ARDUINO:{port}] ✅ REGISTERED!")
                            
                    except json.JSONDecodeError:
                        pass
                    
                    # Send to node if connected
                    if websocket and websocket.state == websockets.protocol.State.OPEN:
                        try:
                            await websocket.send(line)
                            stats["messages_sent"] += 1
                        except Exception as e:
                            print(f"[ERROR:{port}] Send failed: {e}")
                            if port not in message_buffers:
                                message_buffers[port] = []
                            message_buffers[port].append(line)
                    else:
                        if port not in message_buffers:
                            message_buffers[port] = []
                        message_buffers[port].append(line)
            
            await asyncio.sleep(0.01)
        except Exception as e:
            print(f"[ERROR:{port}] {e}")
            await asyncio.sleep(1)

# ==================== SERIAL MANAGEMENT ====================
async def manage_serial():
    """Manage multiple serial connections"""
    global arduino_ports, arduino_handlers
    
    while running:
        # Find all ports
        ports = find_all_arduino_ports()
        
        # Connect to new ports
        for port in ports:
            if port not in arduino_ports:
                print(f"[SERIAL] Opening {port}...")
                try:
                    ser = serial.Serial(port, BAUD_RATE, timeout=1, write_timeout=1)
                    arduino_ports[port] = ser
                    arduino_registered[port] = False
                    
                    # Start handler for this port
                    handler = asyncio.create_task(handle_arduino_messages(port))
                    arduino_handlers[port] = handler
                    
                    print(f"[SERIAL] ✅ {port} opened, handler started")
                    
                    # Send initial ping
                    ser.write(b'{"type":"ping"}\n')
                    
                except Exception as e:
                    print(f"[SERIAL] ❌ Failed to open {port}: {e}")
        
        # Check existing ports
        for port in list(arduino_ports.keys()):
            ser = arduino_ports[port]
            if not ser or not ser.is_open:
                # Try to reopen
                try:
                    ser = serial.Serial(port, BAUD_RATE, timeout=1, write_timeout=1)
                    arduino_ports[port] = ser
                    print(f"[SERIAL] ✅ Reopened {port}")
                except:
                    print(f"[SERIAL] ❌ Lost {port}")
                    del arduino_ports[port]
                    if port in arduino_handlers:
                        arduino_handlers[port].cancel()
                        del arduino_handlers[port]
        
        # Show status
        if arduino_ports:
            print(f"\n[STATUS] Miners connected: {len(arduino_ports)}")
            for port, ser in arduino_ports.items():
                status = "✅ Connected"
                if ser and ser.is_open:
                    if arduino_registered.get(port, False):
                        status = "✅ Registered"
                    info = miner_info.get(port, {})
                    username = info.get("username", "unknown")
                    print(f"  - {port}: {status} (User: {username})")
                else:
                    print(f"  - {port}: ❌ Disconnected")
            print()
        
        await asyncio.sleep(5)

# ==================== STATUS REPORTER ====================
async def status_reporter():
    while running:
        await asyncio.sleep(10)
        uptime = int(time.time() - stats["start_time"])
        minutes = uptime // 60
        seconds = uptime % 60
        
        print(f"\n{'='*60}")
        print(f"BRIDGE STATUS — v7.0 Multi-Miner")
        print(f"{'='*60}")
        print(f"Uptime: {minutes}m {seconds}s")
        print(f"Arduino messages: {stats['arduino_messages']}")
        print(f"Messages to node: {stats['messages_sent']}")
        print(f"Messages from node: {stats['messages_received']}")
        print(f"Miners connected: {stats['miners_connected']}")
        print(f"Miners registered: {stats['miners_registered']}")
        print(f"Node: {'✅ Connected' if websocket and websocket.state == websockets.protocol.State.OPEN else '❌ Disconnected'}")
        print(f"Active ports: {len(arduino_ports)}")
        for port, ser in arduino_ports.items():
            status = "❌ Closed"
            if ser and ser.is_open:
                status = "✅ Open"
                if arduino_registered.get(port, False):
                    status = "✅ Registered"
            info = miner_info.get(port, {})
            username = info.get("username", "unknown")
            print(f"  - {port}: {status} ({username})")
        print(f"Buffer: {sum(len(v) for v in message_buffers.values())}")
        print(f"{'='*60}\n")

# ==================== MAIN ====================
async def main():
    print("\n" + "=" * 60)
    print("MICROCORE WIFI BRIDGE v7.0 — MULTI-MINER SUPPORT")
    print("Detects and onboards multiple Arduino/AVR miners")
    print("=" * 60)
    
    global node_urls, discovered_peers
    node_urls = get_bootstrap_peers()
    discovered_peers = set(node_urls)
    
    print(f"[BRIDGE] Bootnodes: {BOOTSTRAP_NODES}")
    print("[BRIDGE] Starting...\n")
    
    # Initial port scan
    ports = find_all_arduino_ports()
    if ports:
        print(f"[BRIDGE] Found {len(ports)} port(s): {', '.join(ports)}")
    
    await asyncio.gather(
        manage_serial(),
        connect_to_node(),
        status_reporter()
    )

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[BRIDGE] Stopped by user")
    finally:
        running = False
        for port, ser in arduino_ports.items():
            if ser and ser.is_open:
                ser.close()
        print("[BRIDGE] Goodbye!")
