import serial
import serial.tools.list_ports
import re

import time
from datetime import datetime, timezone, timedelta

BRISBANE_TZ = timezone(timedelta(hours=10))

# Assuming when mobile sees base the current time is fetched instantly.
# travel_time_sensor_to_base = (mobile_sees_base_time - mobile_sees_sensor_time)ms 
# current_dt == (mobile_sees_base_time)ms
# dt_when_mobile_sees_sensor = (sensor_sees_mobile_time)ms
# current_dt - (mobile_sees_base_time - mobile_sees_sensor_time)ms = dt_when_mobile_sees_sensor
# dt_when_mobile_sees_sensor - (sensor_sees_mobile_time - sensor_meas_time)ms = dt_when_measurement_was_taken
def compute_measurement_time(mobile_sees_base_time,
                              mobile_sees_sensor_time,
                              sensor_sees_mobile_time,
                              sensor_meas_time):
    # Fetch real current time (ms) at the moment mobile sees base
    current_dt = int(time.time() * 1000)
    # How long the mobile was travelling from sensor to base
    travel_time = mobile_sees_base_time - mobile_sees_sensor_time
    # Real time when mobile was at the sensor
    real_time_mobile_sees_sensor = current_dt - travel_time
    # How long before the mobile arrived that the measurement was taken (sensor clock)
    sensor_delay = sensor_sees_mobile_time - sensor_meas_time
    # Real time when measurement was actually taken
    dt_when_measurement_was_taken = real_time_mobile_sees_sensor - sensor_delay
    return dt_when_measurement_was_taken

# def to_brisbane_time(unix_ms):
#     dt = datetime.fromtimestamp(unix_ms / 1000, tz=BRISBANE_TZ)
#     return dt.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3] + " AEST"

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