#!/usr/bin/env python3
import asyncio
import struct
import sys
import os
import time
import random

sys.path.append(os.path.join(os.path.dirname(__file__), 'proto_build'))

try:
    import telemetry_pb2
except ImportError:
    print("[ERROR] telemetry_pb2 not found. Run 'bash scripts/generate_protos.sh' first.")
    sys.exit(1)

async def test_telemetry_flow(reader, writer):
    print("[Client] Connected. Generating mock TelemetryBatch...")
    
    batch = telemetry_pb2.TelemetryBatch()
    batch.device_id = "mock-device-001"
    batch.timestamp = int(time.time())
    
    metrics = [
        (telemetry_pb2.Metric.CPU_USAGE_PERCENT, random.uniform(0.0, 100.0)),
        (telemetry_pb2.Metric.MEMORY_USAGE_BYTES, random.uniform(1e8, 32e9)),
        (telemetry_pb2.Metric.CPU_TEMPERATURE_CELSIUS, random.uniform(30.0, 95.0))
    ]
    
    for t, v in metrics:
        m = batch.metrics.add()
        m.type = t
        m.value = v
    
    # Serialize and prefix with length
    payload = batch.SerializeToString()
    length = len(payload)
    
    # Send
    print(f"[Client] Sending {length} bytes of serialized Protobuf to proxy...")
    writer.write(struct.pack('>I', length) + payload)
    await writer.drain()
    
    # Await response
    resp_length_bytes = await reader.readexactly(4)
    resp_length = struct.unpack('>I', resp_length_bytes)[0]
    
    resp_data = await reader.readexactly(resp_length)
    resp = telemetry_pb2.IngestResponse()
    resp.ParseFromString(resp_data)
    
    if resp.success:
        print(f"[Client] SUCCESS: Load Balancer forwarded protocol buffers perfectly! Server returned {resp.success}")
    else:
        print(f"[Client] ERROR: Server returned failure: {resp.error_message}")
        
    writer.close()

async def main():
    try:
        reader, writer = await asyncio.open_connection('127.0.0.1', 8080)
        await test_telemetry_flow(reader, writer)
    except Exception as e:
        print(f"[Client] Connection failed: {e}")

if __name__ == '__main__':
    asyncio.run(main())
