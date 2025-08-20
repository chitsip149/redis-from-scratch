package main

import (
	"log"
	"net"
	"time"
)

// element in the queue
type Job struct {
	conn net.Conn
}

// thread in the pool
type Worker struct {
	id       int
	jobQueue chan Job
}

type Pool struct {
	//queue
	jobQueue chan Job
	workers  []*Worker
}

func NewPool(n int) *Pool {
	return &Pool{
		jobQueue: make(chan Job),
		workers:  make([]*Worker, n),
	}
}

func (p *Pool) Start() {
	for i := 0; i < len(p.workers); i++ {
		worker := NewWorker(i, p.jobQueue)
		p.workers[i] = worker
		worker.Start()

	}
}

func (p *Pool) AddJob(conn net.Conn) {
	p.jobQueue <- Job{conn: conn}
}

func NewWorker(id int, jobQueue chan Job) *Worker {
	return &Worker{
		id:       id,
		jobQueue: jobQueue,
	}
}

func (w *Worker) Start() {
	go func() {
		for job := range w.jobQueue {
			log.Printf("worker %d is processing from %s\n", w.id, job.conn.RemoteAddr())
			handleConnection(job.conn)
		}
	}()
}

func handleConnection(conn net.Conn) {
	defer conn.Close()
	var buf []byte = make([]byte, 1000)

	//read from socket and write onto the buffer
	_, err := conn.Read(buf)

	if err != nil {
		log.Fatal(err)
	}

	//pretend to process request
	time.Sleep(time.Second * 1)

	//reply
	conn.Write([]byte("HTTP/1.1 200 OK\r\n\r\nHello, world\r\n"))

}

func main() {
	listener, error := net.Listen("tcp", "localhost:3000")
	if error != nil {
		log.Fatal(error)
	}
	log.Println("listening at port 3000")

	pool := NewPool(2)
	pool.Start()

	//conn == socket == communication channel
	for {
		conn, error := listener.Accept()
		if error != nil {
			log.Fatal(error)
		}
		pool.AddJob(conn)
	}

}
