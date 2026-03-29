package main

import (
	"bufio"
	"bytes"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

const listenPort = 6000

func notify(controlURL, action, ip string, port int) bool {
	body := fmt.Sprintf(`{"ip":%q,"port":%d}`, ip, port)
	resp, err := http.Post(controlURL+"/"+action, "application/json", bytes.NewBufferString(body))
	if err != nil {
		log.Printf("Control %s failed: %v", action, err)
		return false
	}
	resp.Body.Close()
	log.Printf("Control %s: %s:%d", action, ip, port)
	return true
}

// registerWithRetry retries with exponential backoff until success or ctx done.
func registerWithRetry(controlURL, podIP string, port int, stop <-chan struct{}) {
	backoff := time.Second
	for {
		if notify(controlURL, "register", podIP, port) {
			return
		}
		select {
		case <-stop:
			return
		case <-time.After(backoff):
		}
		if backoff < 30*time.Second {
			backoff *= 2
		}
	}
}

func main() {
	podIP := os.Getenv("POD_IP")
	controlURL := os.Getenv("LB_CONTROL_URL") // e.g. http://loadbalancer-control:8080

	ln, err := net.Listen("tcp", fmt.Sprintf(":%d", listenPort))
	if err != nil {
		log.Fatalf("Failed to listen: %v", err)
	}
	defer ln.Close()

	log.Printf("Ingestion server listening on :%d", listenPort)

	stop := make(chan struct{})

	if podIP != "" && controlURL != "" {
		// Initial registration with retry.
		registerWithRetry(controlURL, podIP, listenPort, stop)

		// Periodic re-registration handles LB restarts losing its backend list.
		go func() {
			ticker := time.NewTicker(30 * time.Second)
			defer ticker.Stop()
			for {
				select {
				case <-ticker.C:
					notify(controlURL, "register", podIP, listenPort)
				case <-stop:
					return
				}
			}
		}()

		// Deregister on SIGTERM (k8s preStop sends this).
		go func() {
			ch := make(chan os.Signal, 1)
			signal.Notify(ch, syscall.SIGTERM)
			<-ch
			close(stop)
			notify(controlURL, "deregister", podIP, listenPort)
			os.Exit(0)
		}()
	}

	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Printf("Accept error: %v", err)
			continue
		}
		go handle(conn)
	}
}

func handle(conn net.Conn) {
	defer conn.Close()
	log.Printf("Connection from %s", conn.RemoteAddr())

	scanner := bufio.NewScanner(conn)
	for scanner.Scan() {
		log.Printf("Received: %s", scanner.Text())
		_, err := conn.Write([]byte("Hello from ingestion\n"))
		if err != nil {
			log.Printf("Write error: %v", err)
			return
		}
	}
	if err := scanner.Err(); err != nil {
		log.Printf("Read error: %v", err)
	}
}
