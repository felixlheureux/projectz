// stress is a TCP load generator for the projectz load balancer.
//
// Usage:
//
//	go run stress/main.go -conns 500 -duration 30s -addr localhost:8080
package main

import (
	"bufio"
	"flag"
	"fmt"
	"net"
	"os"
	"sync"
	"sync/atomic"
	"text/tabwriter"
	"time"
)

func main() {
	addr := flag.String("addr", "localhost:8080", "LB address")
	conns := flag.Int("conns", 100, "concurrent persistent connections")
	duration := flag.Duration("duration", 30*time.Second, "test duration")
	rate := flag.Int("rate", 0, "max messages/sec per connection (0 = unlimited)")
	flag.Parse()

	var (
		sent     atomic.Int64
		received atomic.Int64
		errors   atomic.Int64
		active   atomic.Int64
	)

	stop := make(chan struct{})
	var wg sync.WaitGroup

	fmt.Printf("Stress test: addr=%s conns=%d duration=%s\n\n", *addr, *conns, *duration)

	var throttle <-chan time.Time
	if *rate > 0 {
		ticker := time.NewTicker(time.Second / time.Duration(*rate))
		defer ticker.Stop()
		throttle = ticker.C
	}

	for range *conns {
		wg.Go(func() {
			runConn(*addr, stop, throttle, &sent, &received, &errors, &active)
		})
	}

	// Print stats every second.
	go func() {
		tw := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
		ticker := time.NewTicker(time.Second)
		defer ticker.Stop()
		prevSent := int64(0)
		fmt.Fprintln(tw, "ELAPSED\tACTIVE\tMSG/S\tTOTAL SENT\tTOTAL RECV\tERRORS")
		start := time.Now()
		for {
			select {
			case <-ticker.C:
				cur := sent.Load()
				fmt.Fprintf(tw, "%s\t%d\t%d\t%d\t%d\t%d\n",
					time.Since(start).Round(time.Second),
					active.Load(),
					cur-prevSent,
					cur,
					received.Load(),
					errors.Load(),
				)
				tw.Flush()
				prevSent = cur
			case <-stop:
				return
			}
		}
	}()

	time.Sleep(*duration)
	close(stop)
	wg.Wait()

	fmt.Printf("\n─── Final ───────────────────────────────────────\n")
	fmt.Printf("  Sent:     %d\n", sent.Load())
	fmt.Printf("  Received: %d\n", received.Load())
	fmt.Printf("  Errors:   %d\n", errors.Load())
	loss := float64(sent.Load()-received.Load()) / float64(max(sent.Load(), int64(1))) * 100
	fmt.Printf("  Loss:     %.2f%%\n", loss)
}

func runConn(addr string, stop <-chan struct{}, throttle <-chan time.Time, sent, received, errors, active *atomic.Int64) {
	for {
		select {
		case <-stop:
			return
		default:
		}

		conn, err := net.DialTimeout("tcp", addr, 2*time.Second)
		if err != nil {
			errors.Add(1)
			time.Sleep(100 * time.Millisecond)
			continue
		}

		active.Add(1)
		runSession(conn, stop, throttle, sent, received, errors)
		active.Add(-1)
		conn.Close()
	}
}

func runSession(conn net.Conn, stop <-chan struct{}, throttle <-chan time.Time, sent, received, errors *atomic.Int64) {
	reader := bufio.NewReader(conn)
	for {
		// Honour rate limit before each send.
		if throttle != nil {
			select {
			case <-throttle:
			case <-stop:
				return
			}
		} else {
			select {
			case <-stop:
				return
			default:
			}
		}

		_, err := fmt.Fprintf(conn, "Hello from stress test\n")
		if err != nil {
			errors.Add(1)
			return
		}
		sent.Add(1)

		_, err = reader.ReadString('\n')
		if err != nil {
			errors.Add(1)
			return
		}
		received.Add(1)
	}
}
