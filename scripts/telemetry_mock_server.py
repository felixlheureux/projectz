#!/usr/bin/env python3
import asyncio
import struct
import sys
import os

# Ensure the generated proto files are in the python path
sys.path.append(os.path.join(os.path.dirname(__file__), 'proto_build'))

try:
    import telemetry_pb2
except ImportError:
    print("[ERROR] telemetry_pb2 not found. Run 'bash scripts/generate_protos.sh' first.")
    sys.exit(1)

async def handle_client(reader, writer):
    addr = writer.get_extra_info('peername')
    print(f"[Server] Connection from {addr}")
    
    try:
        while True:
            # We assume a simple length-prefixed framing: 4 bytes (network byte order) = length
            length_bytes = await reader.readexactly(4)
            if not length_bytes:
                break
            length = struct.unpack('>I', length_bytes)[0]
            
            data = await reader.readexactly(length)
            
            # Parse the batch
            batch = telemetry_pb2.TelemetryBatch()
            batch.ParseFromString(data)
            
            # Send the response
            response = telemetry_pb2.IngestResponse()
            response.success = True
            response_data = response.SerializeToString()
            
            writer.write(struct.pack('>I', len(response_data)) + response_data)
            await writer.drain()
            
    except asyncio.IncompleteReadError:
        pass
    except Exception as e:
        print(f"[Server] Error processing data: {e}")
        
    print(f"[Server] Connection closed from {addr}")
    writer.close()

async def main():
    server = await asyncio.start_server(handle_client, '127.0.0.1', 8081)
    addr = server.sockets[0].getsockname()
    print(f"[Server] Telemetry Mock Ingester serving on {addr}")
    async with server:
        await server.serve_forever()

if __name__ == '__main__':
    asyncio.run(main())
