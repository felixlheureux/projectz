package main

import (
	"bufio"
	"io"
	"log"
	"net"
	"time"
)

const addr = "localhost:8080"

func main() {
	for {
		conn, err := net.Dial("tcp", addr)
		if err != nil {
			log.Printf("Failed to connect to %s: %v, retrying...", addr, err)
			time.Sleep(200 * time.Millisecond)
			continue
		}

		log.Printf("Connected to %s", addr)
		runSession(conn)
		conn.Close()
	}
}

func runSession(conn net.Conn) {
	reader := bufio.NewReader(conn)

	for {
		msg := "Hello from client\n"
		_, err := conn.Write([]byte(msg))
		if err != nil {
			log.Printf("Write failed: %v", err)
			return
		}
		log.Printf("Sent: %s", msg)

		response, err := reader.ReadString('\n')
		if err == io.EOF {
			return
		}
		if err != nil {
			log.Printf("Read failed: %v", err)
			return
		}
		log.Printf("Received: %s", response)

		time.Sleep(200 * time.Millisecond)
	}
}
