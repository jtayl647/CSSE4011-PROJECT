import serial
import serial.tools.list_ports
import re

def find_port():
    ports = serial.tools.list_ports.comports()
    print("Available ports:")
    for p in ports:
        print(f"  {p.device} — {p.description}")
    for p in ports:
        if 'usbmodem' in p.device or 'ACM' in p.device or 'usbserial' in p.device:
            return p.device
    return None

port = find_port()
if not port:
    print("No port found automatically. Set it manually below.")
    port = '/dev/cu.usbmodem2101'
print(f"Connecting to {port}...")

try:
    ser = serial.Serial(port, 115200, timeout=2)
except serial.SerialException as e:
    print(f"[ERROR] {e}")
    exit(1)

print("Listening...")

while True:
    line = ser.readline().decode('utf-8', errors='ignore').strip()
    if not line:
        continue
    # Strip ANSI escape codes
    line = re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', line)
    print(line)
