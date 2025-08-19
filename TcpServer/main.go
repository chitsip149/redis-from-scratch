package main

import (
	"log"
	"net"
	"time"
)

func handleConnection(conn net.Conn) {
	//receive request and reply response

	for {
		var buf []byte = make([]byte, 1000)

		//read from socket and write onto the buffer
		n, err := conn.Read(buf)

		if err != nil {
			log.Fatal(err)
		}

		//pretend to process request
		time.Sleep(time.Second * 1)

		//reply
		_, err = conn.Write([]byte(string(buf[:n])))
		if err != nil {
			log.Fatal(err)
		}
	}
}

func main() {
	listener, error := net.Listen("tcp", "localhost:3000")
	if error != nil {
		log.Fatal(error)
	}
	log.Println("listening at port 3000")

	//conn == socket == communication channel
	for {
		conn, error := listener.Accept()
		if error != nil {
			log.Fatal(error)
		}
		log.Println("handle conn from", conn.RemoteAddr())

		//create a goroutine to handle this conn
		go handleConnection(conn)
	}

}
